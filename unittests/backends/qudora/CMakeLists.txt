# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

add_executable(test_qudora QudoraTester.cpp)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
  target_link_options(test_qudora PRIVATE -Wl,--no-as-needed)
endif()
target_compile_definitions(test_qudora PRIVATE -DNVQIR_BACKEND_NAME=qudora)
target_include_directories(test_qudora PRIVATE ../..)
target_link_libraries(test_qudora
  PRIVATE fmt::fmt-header-only 
  cudaq-common 
  cudaq
  cudaq-builder
  cudaq-mlir-runtime
  cudaq-rest-qpu
  cudaq-spin 
  cudaq-platform-default 
  gtest_main)


configure_file("QudoraStartServerAndTest.sh.in" "${CMAKE_BINARY_DIR}/unittests/backends/qudora/QudoraStartServerAndTest.sh" @ONLY)
add_test(NAME qudora-tests COMMAND bash QudoraStartServerAndTest.sh WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/unittests/backends/qudora/)
#add_test(NAME my-tests COMMAND cmd.sh)
