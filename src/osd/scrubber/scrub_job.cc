// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "./scrub_job.h"

#include "pg_scrubber.h"

using must_scrub_t = Scrub::must_scrub_t;
using sched_params_t = Scrub::sched_params_t;
using OSDRestrictions = Scrub::OSDRestrictions;
using sched_conf_t = Scrub::sched_conf_t;
using scrub_schedule_t = Scrub::scrub_schedule_t;
using ScrubJob = Scrub::ScrubJob;
using delay_ready_t = Scrub::delay_ready_t;

namespace {
utime_t add_double(utime_t t, double d)
{
  double int_part;
  double frac_as_ns = 1'000'000'000 * std::modf(d, &int_part);
  return utime_t{
      t.sec() + static_cast<int>(int_part),
      static_cast<int>(t.nsec() + frac_as_ns)};
}
}  // namespace

using SchedEntry = Scrub::SchedEntry;

// ////////////////////////////////////////////////////////////////////////// //
// SchedTarget

using SchedTarget = Scrub::SchedTarget;

void SchedTarget::reset()
{
  // a bit convoluted, but the standard way to guarantee we keep the
  // same set of member defaults as the constructor
  *this = SchedTarget{sched_info.pgid, sched_info.level};
}

void SchedTarget::up_urgency_to(urgency_t u)
{
  sched_info.urgency = std::max(sched_info.urgency, u);
}


// ////////////////////////////////////////////////////////////////////////// //
// ScrubJob

#define dout_subsys ceph_subsys_osd
#undef dout_context
#define dout_context (cct)
#undef dout_prefix
#define dout_prefix _prefix_fn(_dout, this, __func__)

template <class T>
static std::ostream& _prefix_fn(std::ostream* _dout, T* t, std::string fn = "")
{
  return t->gen_prefix(*_dout, fn);
}

ScrubJob::ScrubJob(CephContext* cct, const spg_t& pg, int node_id)
    : pgid{pg}
    , whoami{node_id}
    , shallow_target{pg, scrub_level_t::shallow}
    , deep_target{pg, scrub_level_t::deep}
    , cct{cct}
    , log_msg_prefix{fmt::format("osd.{} scrub-job:pg[{}]:", node_id, pgid)}
{}

// debug usage only
namespace std {
ostream& operator<<(ostream& out, const ScrubJob& sjob)
{
  return out << fmt::format("{}", sjob);
}
}  // namespace std


SchedTarget& ScrubJob::get_target(scrub_level_t s_or_d)
{
  return (s_or_d == scrub_level_t::deep) ? deep_target : shallow_target;
}


bool ScrubJob::is_queued() const
{
  return shallow_target.queued || deep_target.queued;
}


void ScrubJob::clear_both_targets_queued()
{
  shallow_target.queued = false;
  deep_target.queued = false;
}


void ScrubJob::set_both_targets_queued()
{
  shallow_target.queued = true;
  deep_target.queued = true;
}


void ScrubJob::adjust_shallow_schedule(
    utime_t last_scrub,
    const Scrub::sched_conf_t& app_conf,
    utime_t scrub_clock_now,
    delay_ready_t modify_ready_targets)
{
  dout(10) << fmt::format(
		  "at entry: shallow target:{}, conf:{}, last-stamp:{:s} "
		  "also-ready?{:c}",
		  shallow_target, app_conf, last_scrub,
		  (modify_ready_targets == delay_ready_t::delay_ready) ? 'y'
								       : 'n')
	   << dendl;

  auto& sh_times = shallow_target.sched_info.schedule;	// shorthand

  if (!ScrubJob::requires_randomization(shallow_target.urgency())) {
    // the target time is already set. Make sure to reset the n.b. and
    // the (irrelevant) deadline
    sh_times.not_before = sh_times.scheduled_at;
    sh_times.deadline = sh_times.scheduled_at;

  } else {
    utime_t adj_not_before = last_scrub;
    utime_t adj_target = last_scrub;
    sh_times.deadline = adj_target;

    // add a random delay to the proposed scheduled time - but only for periodic
    // scrubs that are not already eligible for scrubbing.
    if ((modify_ready_targets == delay_ready_t::delay_ready) ||
	adj_not_before > scrub_clock_now) {
      adj_target += app_conf.shallow_interval;
      double r = rand() / (double)RAND_MAX;
      adj_target +=
	  app_conf.shallow_interval * app_conf.interval_randomize_ratio * r;
    }

    // the deadline can be updated directly into the scrub-job
    if (app_conf.max_shallow) {
      sh_times.deadline += *app_conf.max_shallow;
    } else {
      sh_times.deadline = utime_t{};
    }
    if (adj_not_before < adj_target) {
      adj_not_before = adj_target;
    }
    sh_times.scheduled_at = adj_target;
    sh_times.not_before = adj_not_before;
  }

  dout(10) << fmt::format(
		  "adjusted: nb:{:s} target:{:s} deadline:{:s} ({})",
		  sh_times.not_before, sh_times.scheduled_at, sh_times.deadline,
		  state_desc())
	   << dendl;
}


std::optional<std::reference_wrapper<SchedTarget>> ScrubJob::earliest_eligible(
    utime_t scrub_clock_now)
{
  std::weak_ordering compr = cmp_entries(
      scrub_clock_now, shallow_target.queued_element(),
      deep_target.queued_element());

  auto poss_ret = (compr == std::weak_ordering::less)
		      ? std::ref<SchedTarget>(shallow_target)
		      : std::ref<SchedTarget>(deep_target);
  if (poss_ret.get().sched_info.schedule.not_before <= scrub_clock_now) {
    return poss_ret;
  }
  return std::nullopt;
}

std::optional<std::reference_wrapper<const SchedTarget>>
ScrubJob::earliest_eligible(utime_t scrub_clock_now) const
{
  std::weak_ordering compr = cmp_entries(
      scrub_clock_now, shallow_target.queued_element(),
      deep_target.queued_element());

  auto poss_ret = (compr == std::weak_ordering::less)
		      ? std::cref<SchedTarget>(shallow_target)
		      : std::cref<SchedTarget>(deep_target);
  if (poss_ret.get().sched_info.schedule.not_before <= scrub_clock_now) {
    return poss_ret;
  }
  return std::nullopt;
}


SchedTarget& ScrubJob::earliest_target()
{
  std::weak_ordering compr = cmp_future_entries(
      shallow_target.queued_element(), deep_target.queued_element());
  return (compr == std::weak_ordering::less) ? shallow_target : deep_target;
}

const SchedTarget& ScrubJob::earliest_target() const
{
  std::weak_ordering compr = cmp_future_entries(
      shallow_target.queued_element(), deep_target.queued_element());
  return (compr == std::weak_ordering::less) ? shallow_target : deep_target;
}

utime_t ScrubJob::get_sched_time() const
{
  return earliest_target().sched_info.schedule.not_before;
}

void ScrubJob::adjust_deep_schedule(
    utime_t last_deep,
    const Scrub::sched_conf_t& app_conf,
    utime_t scrub_clock_now,
    delay_ready_t modify_ready_targets)
{
  dout(10) << fmt::format(
		  "at entry: deep target:{}, conf:{}, last-stamp:{:s} "
		  "also-ready?{:c}",
		  deep_target, app_conf, last_deep,
		  (modify_ready_targets == delay_ready_t::delay_ready) ? 'y'
								       : 'n')
	   << dendl;

  auto& dp_times = deep_target.sched_info.schedule;  // shorthand

  if (!ScrubJob::requires_randomization(deep_target.urgency())) {
    // the target time is already set. Make sure to reset the n.b. and
    // the (irrelevant) deadline
    dp_times.not_before = dp_times.scheduled_at;
    dp_times.deadline = dp_times.scheduled_at;

  } else {
    utime_t adj_not_before = last_deep;
    utime_t adj_target = last_deep;
    dp_times.deadline = adj_target;

    // add a random delay to the proposed scheduled time - but only for periodic
    // scrubs that are not already eligible for scrubbing.
    if ((modify_ready_targets == delay_ready_t::delay_ready) ||
	adj_not_before > scrub_clock_now) {
      adj_target += app_conf.deep_interval;
      double r = rand() / (double)RAND_MAX;
      adj_target += app_conf.deep_interval * app_conf.interval_randomize_ratio *
		    r;	// RRR fix
    }

    // the deadline can be updated directly into the scrub-job
    if (app_conf.max_shallow) {
      dp_times.deadline += *app_conf.max_shallow;  // RRR fix
    } else {
      dp_times.deadline = utime_t{};
    }
    if (adj_not_before < adj_target) {
      adj_not_before = adj_target;
    }
    dp_times.scheduled_at = adj_target;
    dp_times.not_before = adj_not_before;
  }

  dout(10) << fmt::format(
		  "adjusted: nb:{:s} target:{:s} deadline:{:s} ({})",
		  dp_times.not_before, dp_times.scheduled_at, dp_times.deadline,
		  state_desc())
	   << dendl;
}


SchedTarget& ScrubJob::delay_on_failure(
    scrub_level_t level,
    std::chrono::seconds delay,
    Scrub::delay_cause_t delay_cause,
    utime_t scrub_clock_now)
{
  auto& delayed_target =
      (level == scrub_level_t::deep) ? deep_target : shallow_target;
  delayed_target.sched_info.schedule.not_before =
      std::max(scrub_clock_now, delayed_target.sched_info.schedule.not_before) +
      utime_t{delay};
  delayed_target.sched_info.last_issue = delay_cause;
  return delayed_target;
}


std::string ScrubJob::scheduling_state(utime_t now_is, bool is_deep_expected)
    const
{
  // if not registered, not a candidate for scrubbing on this OSD (or at all)
  if (!registered) {
    return "not registered for scrubbing";
  }
  if (!is_queued()) {
    // if not currently queued - we are being scrubbed
    return "scrubbing";
  }

  const auto first_ready = earliest_eligible(now_is);
  if (first_ready) {
    // the target is ready to be scrubbed
    return fmt::format(
	"queued for {}scrub at {:s} (debug RRR: {})",
	(first_ready->get().is_deep() ? "deep " : ""),
	first_ready->get().sched_info.schedule.scheduled_at,
	(is_deep_expected ? "deep " : ""));
  } else {
    // both targets are in the future
    const auto& nearest = earliest_target();
    return fmt::format(
	"{}scrub scheduled @ {:s} ({:s})", (nearest.is_deep() ? "deep " : ""),
	nearest.sched_info.schedule.not_before,
	nearest.sched_info.schedule.scheduled_at);
  }
}

std::ostream& ScrubJob::gen_prefix(std::ostream& out, std::string_view fn) const
{
  return out << log_msg_prefix << fn << ": ";
}

void ScrubJob::dump(ceph::Formatter* f) const
{
  const auto& entry = earliest_target().sched_info;
  const auto& sch = entry.schedule;
  f->open_object_section("scrub");
  f->dump_stream("pgid") << pgid;
  f->dump_stream("sched_time") << get_sched_time();
  f->dump_stream("orig_sched_time") << sch.scheduled_at;
  f->dump_stream("deadline") << sch.deadline;
  f->dump_bool("forced", entry.urgency >= urgency_t::operator_requested);
  f->close_section();
}

// a set of static functions to determine, given a scheduling target's urgency,
// what restrictions apply to that target (and what exemptions it has).

bool ScrubJob::observes_noscrub_flags(urgency_t urgency)
{
  return urgency < urgency_t::after_repair;
}

bool ScrubJob::observes_allowed_hours(urgency_t urgency)
{
  return urgency < urgency_t::operator_requested;
}

bool ScrubJob::observes_load_limit(urgency_t urgency)
{
  return urgency < urgency_t::after_repair;
}

bool ScrubJob::requires_reservation(urgency_t urgency)
{
  return urgency < urgency_t::after_repair;
}

bool ScrubJob::requires_randomization(urgency_t urgency)
{
  return urgency == urgency_t::periodic_regular;
}

bool ScrubJob::observes_max_concurrency(urgency_t urgency)
{
  return urgency < urgency_t::operator_requested;
}
