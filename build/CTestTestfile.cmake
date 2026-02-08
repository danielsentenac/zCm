# CMake generated Testfile for 
# Source directory: /home/sentenac/ZCM
# Build directory: /home/sentenac/ZCM/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(zcm_smoke "/home/sentenac/ZCM/build/tests/zcm_smoke")
set_tests_properties(zcm_smoke PROPERTIES  _BACKTRACE_TRIPLES "/home/sentenac/ZCM/CMakeLists.txt;53;add_test;/home/sentenac/ZCM/CMakeLists.txt;0;")
add_test(zcm_msg_roundtrip "/home/sentenac/ZCM/build/tests/zcm_msg_roundtrip")
set_tests_properties(zcm_msg_roundtrip PROPERTIES  _BACKTRACE_TRIPLES "/home/sentenac/ZCM/CMakeLists.txt;60;add_test;/home/sentenac/ZCM/CMakeLists.txt;0;")
add_test(zcm_msg_fuzz "/home/sentenac/ZCM/build/tests/zcm_msg_fuzz")
set_tests_properties(zcm_msg_fuzz PROPERTIES  _BACKTRACE_TRIPLES "/home/sentenac/ZCM/CMakeLists.txt;67;add_test;/home/sentenac/ZCM/CMakeLists.txt;0;")
add_test(zcm_msg_vectors "/home/sentenac/ZCM/build/tests/zcm_msg_vectors")
set_tests_properties(zcm_msg_vectors PROPERTIES  _BACKTRACE_TRIPLES "/home/sentenac/ZCM/CMakeLists.txt;74;add_test;/home/sentenac/ZCM/CMakeLists.txt;0;")
add_test(zcm_node_list "/home/sentenac/ZCM/build/tests/zcm_node_list")
set_tests_properties(zcm_node_list PROPERTIES  _BACKTRACE_TRIPLES "/home/sentenac/ZCM/CMakeLists.txt;81;add_test;/home/sentenac/ZCM/CMakeLists.txt;0;")
