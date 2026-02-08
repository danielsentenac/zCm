# CMake generated Testfile for 
# Source directory: /home/sentenac/ZCM
# Build directory: /home/sentenac/ZCM
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(zcm_smoke "/home/sentenac/ZCM/zcm_smoke")
set_tests_properties(zcm_smoke PROPERTIES  _BACKTRACE_TRIPLES "/home/sentenac/ZCM/CMakeLists.txt;46;add_test;/home/sentenac/ZCM/CMakeLists.txt;0;")
add_test(zcm_msg_roundtrip "/home/sentenac/ZCM/zcm_msg_roundtrip")
set_tests_properties(zcm_msg_roundtrip PROPERTIES  _BACKTRACE_TRIPLES "/home/sentenac/ZCM/CMakeLists.txt;50;add_test;/home/sentenac/ZCM/CMakeLists.txt;0;")
add_test(zcm_msg_fuzz "/home/sentenac/ZCM/zcm_msg_fuzz")
set_tests_properties(zcm_msg_fuzz PROPERTIES  _BACKTRACE_TRIPLES "/home/sentenac/ZCM/CMakeLists.txt;54;add_test;/home/sentenac/ZCM/CMakeLists.txt;0;")
add_test(zcm_msg_vectors "/home/sentenac/ZCM/zcm_msg_vectors")
set_tests_properties(zcm_msg_vectors PROPERTIES  _BACKTRACE_TRIPLES "/home/sentenac/ZCM/CMakeLists.txt;58;add_test;/home/sentenac/ZCM/CMakeLists.txt;0;")
