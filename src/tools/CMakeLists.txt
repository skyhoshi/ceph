set(rados_srcs
  rados/rados.cc
  RadosDump.cc
  rados/RadosImport.cc
  rados/PoolDump.cc
  ${PROJECT_SOURCE_DIR}/src/common/util.cc
  ${PROJECT_SOURCE_DIR}/src/common/obj_bencher.cc
  ${PROJECT_SOURCE_DIR}/src/osd/ECUtilL.cc)
if(WIN32)
  list(APPEND rados_srcs ../common/win32/code_page.rc)
endif()
add_executable(rados ${rados_srcs})

target_link_libraries(rados librados global ${BLKID_LIBRARIES} ${CMAKE_DL_LIBS})
if(WITH_LIBRADOSSTRIPER)
  target_link_libraries(rados radosstriper)
else()
  target_link_libraries(rados cls_lock_client)
endif()
install(TARGETS rados DESTINATION bin)

if(NOT WIN32)
  set(neorados_srcs
      neorados.cc)
  add_executable(neorados ${neorados_srcs})
  target_link_libraries(neorados libneorados ${FMT_LIB} ${CMAKE_DL_LIBS})
  add_dependencies(tests neorados)
  #install(TARGETS neorados DESTINATION bin)
endif()

if(WITH_TESTS)
add_executable(ceph_scratchtool scratchtool.c)
target_link_libraries(ceph_scratchtool librados global)
install(TARGETS ceph_scratchtool DESTINATION bin)

add_executable(ceph_scratchtoolpp scratchtoolpp.cc)
target_link_libraries(ceph_scratchtoolpp librados global)
install(TARGETS ceph_scratchtoolpp DESTINATION bin)

add_executable(ceph_radosacl radosacl.cc)
target_link_libraries(ceph_radosacl librados global)
install(TARGETS ceph_radosacl DESTINATION bin)

install(PROGRAMS
  ceph-monstore-update-crush.sh
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/ceph)
endif(WITH_TESTS)

add_executable(ceph-monstore-tool
  ceph_monstore_tool.cc
  ../auth/cephx/CephxKeyServer.cc
  ../mgr/mgr_commands.cc)
target_link_libraries(ceph-monstore-tool os global Boost::program_options)
install(TARGETS ceph-monstore-tool DESTINATION bin)

add_executable(ceph-objectstore-tool
  ceph_objectstore_tool.cc
  rebuild_mondb.cc
  RadosDump.cc)
target_link_libraries(ceph-objectstore-tool osd os global Boost::program_options ${CMAKE_DL_LIBS})
if(WITH_FUSE)
  target_link_libraries(ceph-objectstore-tool FUSE::FUSE)
endif(WITH_FUSE)
install(TARGETS ceph-objectstore-tool DESTINATION bin)

if(WITH_LIBCEPHFS)
if(WITH_TESTS)
  add_executable(ceph-client-debug ceph-client-debug.cc)
  target_link_libraries(ceph-client-debug cephfs global client)
  install(TARGETS ceph-client-debug DESTINATION bin)
endif(WITH_TESTS)
endif(WITH_LIBCEPHFS)

add_executable(ceph-kvstore-tool
  kvstore_tool.cc
  ceph_kvstore_tool.cc)
target_link_libraries(ceph-kvstore-tool
  legacy-option-headers
  os global)
install(TARGETS ceph-kvstore-tool DESTINATION bin)

set(ceph_conf_srcs ceph_conf.cc)
add_executable(ceph-conf ${ceph_conf_srcs})
target_link_libraries(ceph-conf global)
install(TARGETS ceph-conf DESTINATION bin)

set(crushtool_srcs crushtool.cc)
add_executable(crushtool ${crushtool_srcs})
target_link_libraries(crushtool
  legacy-option-headers
  global)
install(TARGETS crushtool DESTINATION bin)

set(monmaptool_srcs monmaptool.cc)
add_executable(monmaptool ${monmaptool_srcs})
target_link_libraries(monmaptool global)
install(TARGETS monmaptool DESTINATION bin)

set(osdomaptool_srcs osdmaptool.cc)
add_executable(osdmaptool ${osdomaptool_srcs})
target_link_libraries(osdmaptool global)
install(TARGETS osdmaptool DESTINATION bin)

install(PROGRAMS crushdiff DESTINATION bin)

set(ceph-diff-sorted_srcs ceph-diff-sorted.cc)
add_executable(ceph-diff-sorted ${ceph-diff-sorted_srcs})
set_target_properties(ceph-diff-sorted PROPERTIES
  SKIP_RPATH TRUE
  INSTALL_RPATH "")
install(TARGETS ceph-diff-sorted DESTINATION bin)

if(WITH_TESTS)
set(ceph_psim_srcs psim.cc)
add_executable(ceph_psim ${ceph_psim_srcs})
target_link_libraries(ceph_psim global)
install(TARGETS ceph_psim DESTINATION bin)
endif(WITH_TESTS)

set(ceph_authtool_srcs ceph_authtool.cc)
add_executable(ceph-authtool ${ceph_authtool_srcs})
target_link_libraries(ceph-authtool global ${EXTRALIBS} ${CRYPTO_LIBS})
install(TARGETS ceph-authtool DESTINATION bin)

if(WITH_CEPHFS)
  add_subdirectory(cephfs)
  add_subdirectory(cephfs_mirror)
endif(WITH_CEPHFS)

if(WITH_RBD)
  add_subdirectory(rbd)
  add_subdirectory(rbd_mirror)
  if(LINUX)
    add_subdirectory(rbd_nbd)
  endif()
  if(WIN32)
    add_subdirectory(rbd_wnbd)
  endif()
  if(FREEBSD)
    add_subdirectory(rbd_ggate)
  endif()
endif(WITH_RBD)

add_subdirectory(immutable_object_cache)
add_subdirectory(ceph-dencoder)
add_subdirectory(erasure-code)
add_subdirectory(ceph_dedup)
