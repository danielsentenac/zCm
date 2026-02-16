cmake_minimum_required(VERSION 3.14)

project(zCm
  VERSION 0.1.0
  LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ZCM_BUILD_TESTS "Build zCm tests" ON)
option(ZCM_ENABLE_ZMQ "Enable ZeroMQ transport backend" ON)
option(ZCM_BUILD_EXAMPLES "Build zCm examples" ON)
option(ZCM_BUILD_TOOLS "Build zCm tools" ON)
set(ZCM_ZMQ_ROOT "$ENV{ZCM_ZMQ_ROOT}" CACHE PATH "ZeroMQ installation prefix (contains include/ and lib/)")
set(ZCM_ZMQ_INCLUDE_DIR "$ENV{ZCM_ZMQ_INCLUDE_DIR}" CACHE PATH "ZeroMQ include directory (contains zmq.h)")
set(ZCM_ZMQ_LIBRARY "$ENV{ZCM_ZMQ_LIBRARY}" CACHE FILEPATH "Absolute path to libzmq shared/static library")

add_library(zcm_lib
  src/low-level/zcm_context.c
  src/high-level/zcm_broker.c
  src/low-level/zcm_node.c
  src/high-level/zcm_proc.c
  src/high-level/zcm_proc_runtime.c
  src/low-level/zcm_msg.c
)

target_compile_definitions(zcm_lib
  PRIVATE
    ZCM_PROC_CONFIG_SCHEMA_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/config/schema/proc-config.xsd"
)

target_include_directories(zcm_lib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(ZCM_ENABLE_ZMQ)
  set(_zcm_zmq_include_dirs "")
  set(_zcm_zmq_link_libs "")
  set(_zcm_zmq_link_opts "")
  set(_zcm_zmq_origin "")

  if(ZCM_ZMQ_INCLUDE_DIR AND ZCM_ZMQ_LIBRARY)
    if(NOT EXISTS "${ZCM_ZMQ_INCLUDE_DIR}/zmq.h")
      message(FATAL_ERROR
        "ZCM_ZMQ_INCLUDE_DIR is set but zmq.h is missing:\n"
        "  ${ZCM_ZMQ_INCLUDE_DIR}/zmq.h")
    endif()
    if(NOT EXISTS "${ZCM_ZMQ_LIBRARY}")
      message(FATAL_ERROR
        "ZCM_ZMQ_LIBRARY does not exist:\n"
        "  ${ZCM_ZMQ_LIBRARY}\n"
        "Build/install ZeroMQ first, or pass an existing library path "
        "(for example libzmq.so.5.x.y or libzmq.a).")
    endif()
    list(APPEND _zcm_zmq_include_dirs "${ZCM_ZMQ_INCLUDE_DIR}")
    list(APPEND _zcm_zmq_link_libs "${ZCM_ZMQ_LIBRARY}")
    set(_zcm_zmq_origin "explicit ZCM_ZMQ_INCLUDE_DIR/ZCM_ZMQ_LIBRARY")
  elseif(ZCM_ZMQ_ROOT)
    find_path(_zcm_zmq_inc_from_root
      NAMES zmq.h
      HINTS "${ZCM_ZMQ_ROOT}"
      PATH_SUFFIXES include
      NO_DEFAULT_PATH
    )
    find_library(_zcm_zmq_lib_from_root
      NAMES zmq libzmq
      HINTS "${ZCM_ZMQ_ROOT}"
      PATH_SUFFIXES lib lib64 src/.libs
      NO_DEFAULT_PATH
    )
    if(_zcm_zmq_inc_from_root AND _zcm_zmq_lib_from_root)
      list(APPEND _zcm_zmq_include_dirs "${_zcm_zmq_inc_from_root}")
      list(APPEND _zcm_zmq_link_libs "${_zcm_zmq_lib_from_root}")
      set(_zcm_zmq_origin "ZCM_ZMQ_ROOT")
    endif()
  endif()

  if(NOT _zcm_zmq_include_dirs OR NOT _zcm_zmq_link_libs)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(PC_ZMQ QUIET libzmq)
    endif()
    if(PC_ZMQ_FOUND)
      list(APPEND _zcm_zmq_include_dirs ${PC_ZMQ_INCLUDE_DIRS})
      if(PC_ZMQ_LINK_LIBRARIES)
        list(APPEND _zcm_zmq_link_libs ${PC_ZMQ_LINK_LIBRARIES})
      else()
        list(APPEND _zcm_zmq_link_libs ${PC_ZMQ_LIBRARIES})
      endif()
      list(APPEND _zcm_zmq_link_opts ${PC_ZMQ_LDFLAGS_OTHER})
      set(_zcm_zmq_origin "pkg-config(libzmq)")
    endif()
  endif()

  if(NOT _zcm_zmq_include_dirs OR NOT _zcm_zmq_link_libs)
    set(_zcm_zmq_include_dirs "")
    set(_zcm_zmq_link_libs "")
    set(_zcm_zmq_link_opts "")
    find_path(_zcm_zmq_inc_default NAMES zmq.h)
    find_library(_zcm_zmq_lib_default NAMES zmq libzmq)
    if(_zcm_zmq_inc_default AND _zcm_zmq_lib_default)
      list(APPEND _zcm_zmq_include_dirs "${_zcm_zmq_inc_default}")
      list(APPEND _zcm_zmq_link_libs "${_zcm_zmq_lib_default}")
      set(_zcm_zmq_origin "system search path")
    endif()
  endif()

  if(NOT _zcm_zmq_include_dirs OR NOT _zcm_zmq_link_libs)
    message(FATAL_ERROR
      "ZeroMQ not found. Provide one of:\n"
      "  -DZCM_ZMQ_ROOT=/path/to/zeromq\n"
      "  -DZCM_ZMQ_INCLUDE_DIR=/path/include -DZCM_ZMQ_LIBRARY=/path/lib/libzmq.so\n"
      "or set PKG_CONFIG_PATH so pkg-config can resolve libzmq."
    )
  endif()

  target_include_directories(zcm_lib PRIVATE ${_zcm_zmq_include_dirs})
  target_link_libraries(zcm_lib PRIVATE ${_zcm_zmq_link_libs})
  if(_zcm_zmq_link_opts)
    target_link_options(zcm_lib PRIVATE ${_zcm_zmq_link_opts})
  endif()

  set(_zcm_zmq_rpath_dirs "")
  foreach(_zcm_lib IN LISTS _zcm_zmq_link_libs)
    if(IS_ABSOLUTE "${_zcm_lib}")
      get_filename_component(_zcm_lib_dir "${_zcm_lib}" DIRECTORY)
      list(APPEND _zcm_zmq_rpath_dirs "${_zcm_lib_dir}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _zcm_zmq_rpath_dirs)
  if(_zcm_zmq_rpath_dirs)
    set_property(TARGET zcm_lib APPEND PROPERTY BUILD_RPATH "${_zcm_zmq_rpath_dirs}")
    set_property(TARGET zcm_lib APPEND PROPERTY INSTALL_RPATH "${_zcm_zmq_rpath_dirs}")
  endif()

  message(STATUS "ZeroMQ source: ${_zcm_zmq_origin}")
  message(STATUS "ZeroMQ include: ${_zcm_zmq_include_dirs}")
  message(STATUS "ZeroMQ libs: ${_zcm_zmq_link_libs}")
  target_compile_definitions(zcm_lib PRIVATE ZCM_HAS_ZMQ=1)
endif()

find_package(Threads REQUIRED)
if(Threads_FOUND)
  target_link_libraries(zcm_lib PRIVATE Threads::Threads)
endif()

set(CTEST_OUTPUT_ON_FAILURE ON)

set(ZCM_TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests)
set(ZCM_EXAMPLE_OUTPUT_DIR ${CMAKE_BINARY_DIR}/examples)
set(ZCM_TOOL_OUTPUT_DIR ${CMAKE_BINARY_DIR}/tools)
set(ZCM_LIB_OUTPUT_DIR ${CMAKE_BINARY_DIR}/lib)

set_target_properties(zcm_lib PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY ${ZCM_LIB_OUTPUT_DIR}
  LIBRARY_OUTPUT_DIRECTORY ${ZCM_LIB_OUTPUT_DIR}
  RUNTIME_OUTPUT_DIRECTORY ${ZCM_LIB_OUTPUT_DIR}
)

if(ZCM_BUILD_TESTS)
  enable_testing()
  add_executable(zcm_smoke tests/smoke/zcm_smoke.c)
  target_link_libraries(zcm_smoke PRIVATE zcm_lib)
  add_test(NAME zcm_smoke COMMAND zcm_smoke)
  set_target_properties(zcm_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TEST_OUTPUT_DIR}
  )

  add_executable(zcm_msg_roundtrip tests/msg/zcm_msg_roundtrip.c)
  target_link_libraries(zcm_msg_roundtrip PRIVATE zcm_lib)
  add_test(NAME zcm_msg_roundtrip COMMAND zcm_msg_roundtrip)
  set_target_properties(zcm_msg_roundtrip PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TEST_OUTPUT_DIR}
  )

  add_executable(zcm_msg_fuzz tests/msg/zcm_msg_fuzz.c)
  target_link_libraries(zcm_msg_fuzz PRIVATE zcm_lib)
  add_test(NAME zcm_msg_fuzz COMMAND zcm_msg_fuzz)
  set_target_properties(zcm_msg_fuzz PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TEST_OUTPUT_DIR}
  )

  add_executable(zcm_msg_vectors tests/msg/zcm_msg_vectors.c)
  target_link_libraries(zcm_msg_vectors PRIVATE zcm_lib)
  add_test(NAME zcm_msg_vectors COMMAND zcm_msg_vectors)
  set_target_properties(zcm_msg_vectors PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TEST_OUTPUT_DIR}
  )

  add_executable(zcm_node_list tests/node/zcm_node_list.c)
  target_link_libraries(zcm_node_list PRIVATE zcm_lib)
  add_test(NAME zcm_node_list COMMAND zcm_node_list)
  set_target_properties(zcm_node_list PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TEST_OUTPUT_DIR}
  )

  add_executable(zcm_node_unique_name tests/node/zcm_node_unique_name.c)
  target_link_libraries(zcm_node_unique_name PRIVATE zcm_lib)
  add_test(NAME zcm_node_unique_name COMMAND zcm_node_unique_name)
  set_target_properties(zcm_node_unique_name PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TEST_OUTPUT_DIR}
  )

  add_executable(zcm_node_prune_dead tests/node/zcm_node_prune_dead.c)
  target_link_libraries(zcm_node_prune_dead PRIVATE zcm_lib)
  add_test(NAME zcm_node_prune_dead COMMAND zcm_node_prune_dead)
  set_target_properties(zcm_node_prune_dead PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TEST_OUTPUT_DIR}
  )

  add_executable(zcm_node_ctrl_fallback tests/node/zcm_node_ctrl_fallback.c)
  target_link_libraries(zcm_node_ctrl_fallback PRIVATE zcm_lib)
  add_test(NAME zcm_node_ctrl_fallback COMMAND zcm_node_ctrl_fallback)
  set_target_properties(zcm_node_ctrl_fallback PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TEST_OUTPUT_DIR}
  )

  add_executable(zcm_proc_reannounce tests/node/zcm_proc_reannounce.c)
  target_link_libraries(zcm_proc_reannounce PRIVATE zcm_lib)
  add_test(NAME zcm_proc_reannounce COMMAND zcm_proc_reannounce)
  set_target_properties(zcm_proc_reannounce PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TEST_OUTPUT_DIR}
  )

  add_executable(zcm_cli_workflow tests/node/zcm_cli_workflow.c)
  target_link_libraries(zcm_cli_workflow PRIVATE zcm_lib)
  target_compile_definitions(zcm_cli_workflow
    PRIVATE
      ZCM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  add_test(NAME zcm_cli_workflow COMMAND zcm_cli_workflow)
  set_target_properties(zcm_cli_workflow PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TEST_OUTPUT_DIR}
  )

  add_executable(zcm_cli_ping_fallback tests/node/zcm_cli_ping_fallback.c)
  target_link_libraries(zcm_cli_ping_fallback PRIVATE zcm_lib)
  add_test(NAME zcm_cli_ping_fallback COMMAND zcm_cli_ping_fallback)
  set_target_properties(zcm_cli_ping_fallback PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TEST_OUTPUT_DIR}
  )
endif()

if(ZCM_BUILD_EXAMPLES)
  add_executable(zcm_proc examples/proc/zcm_proc_main.c)
  target_link_libraries(zcm_proc PRIVATE zcm_lib)
  target_compile_definitions(zcm_proc
    PRIVATE
      ZCM_PROC_CONFIG_SCHEMA_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/config/schema/proc-config.xsd"
  )
  set_target_properties(zcm_proc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_EXAMPLE_OUTPUT_DIR}
  )

endif()

if(ZCM_BUILD_TOOLS)
  set(CM_BRIDGE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tools/cm-bridge/cm_bridge.c")
  if(EXISTS "${CM_BRIDGE_SRC}")
    add_executable(cm_bridge "${CM_BRIDGE_SRC}")
    target_link_libraries(cm_bridge PRIVATE zcm_lib)
    set_target_properties(cm_bridge PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${ZCM_TOOL_OUTPUT_DIR}
    )
  else()
    message(STATUS "Skipping cm_bridge: source file not found at ${CM_BRIDGE_SRC}")
  endif()

  add_executable(zcm tools/command-line/zcm_cli.c)
  target_link_libraries(zcm PRIVATE zcm_lib pthread)
  set_target_properties(zcm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TOOL_OUTPUT_DIR}
  )

  add_executable(zcm_broker tools/broker/zcm_broker.c)
  target_link_libraries(zcm_broker PRIVATE zcm_lib)
  set_target_properties(zcm_broker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ZCM_TOOL_OUTPUT_DIR}
  )
endif()

add_custom_target(test_results
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/test_results
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --verbose --output-on-failure --output-log ${CMAKE_BINARY_DIR}/test_results/ctest.log
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(test-verbose
  DEPENDS test_results
)
