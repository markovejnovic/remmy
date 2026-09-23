# See LICENSE in the repository root.

option(CUTILS_TEST_TSAN "Use ThreadSanitizer instead of AddressSanitizer for tests" OFF)
include(cmake/TestSupport.cmake)

include(FetchContent)
set(CATCH_CONFIG_DISABLE_EXCEPTIONS ON CACHE BOOL "" FORCE)
set(CATCH_CONFIG_NO_COUNTER ON CACHE BOOL "" FORCE)
FetchContent_Declare(Catch2
  URL https://codeload.github.com/catchorg/Catch2/tar.gz/refs/tags/v3.16.0
  URL_HASH SHA256=0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34
  EXCLUDE_FROM_ALL SYSTEM)
FetchContent_MakeAvailable(Catch2)
foreach(target IN ITEMS Catch2 Catch2WithMain)
  target_link_libraries(${target} PRIVATE cutils_test_options)
  target_compile_options(${target} PRIVATE -fno-exceptions)
  target_compile_definitions(${target} PRIVATE _GLIBCXX_ASSERTIONS=1)
  set_target_properties(${target} PROPERTIES
    CXX_CLANG_TIDY "" INTERPROCEDURAL_OPTIMIZATION FALSE)
endforeach()
if(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
  set_property(SOURCE "${catch2_SOURCE_DIR}/src/catch2/internal/catch_random_number_generator.cpp"
    DIRECTORY "${catch2_SOURCE_DIR}/src" APPEND PROPERTY
    COMPILE_OPTIONS -fno-sanitize=unsigned-shift-base)
  set_property(SOURCE "${catch2_SOURCE_DIR}/src/catch2/internal/catch_run_context.cpp"
    DIRECTORY "${catch2_SOURCE_DIR}/src" APPEND PROPERTY
    COMPILE_OPTIONS -fno-sanitize=implicit-integer-sign-change)
  set_property(SOURCE "${catch2_SOURCE_DIR}/src/catch2/internal/catch_test_case_info_hasher.cpp"
    DIRECTORY "${catch2_SOURCE_DIR}/src" APPEND PROPERTY
    COMPILE_OPTIONS -fno-sanitize=unsigned-integer-overflow)
  set_property(SOURCE "${catch2_SOURCE_DIR}/src/catch2/internal/catch_test_case_tracker.cpp"
    DIRECTORY "${catch2_SOURCE_DIR}/src" APPEND PROPERTY
    COMPILE_OPTIONS -fno-sanitize=unsigned-integer-overflow)
endif()
include(Catch)

function(cutils_add_catch_test target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE cutils_test Catch2::Catch2WithMain)
  cutils_harden(${target})
  set_target_properties(${target} PROPERTIES
    CXX_CLANG_TIDY "" INTERPROCEDURAL_OPTIMIZATION FALSE)
  cutils_test_environment(environment)
  string(REPLACE ";" "\\\\\\\\\\\\\\\\;" environment "${environment}")
  catch_discover_tests(${target}
    PROPERTIES TIMEOUT 90 ENVIRONMENT "${environment}")
endfunction()

cutils_add_catch_test(heap_array_test collections/heap_array_test.cpp)
cutils_add_catch_test(env_test os/env_test.cpp)
cutils_add_catch_test(fd_test os/fd_test.cpp)
cutils_add_catch_test(limits_test os/limits/fd_test.cpp)
cutils_add_catch_test(task_scheduler_test task_scheduler/task_scheduler_test.cpp)
cutils_add_catch_test(work_stealing_queue_test workstealing_queue/chase_lev_test.cpp)

cutils_add_catch_test(clppap_core_test
  clppap/detail/arg_classifier_test.cpp
  clppap/detail/argument_set_test.cpp
  clppap/positional_iterator_test.cpp)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
  cutils_add_catch_test(clppap_reflection_test
    clppap/clppap_test.cpp clppap/detail/schema_test.cpp clppap/report_test.cpp)
  cutils_add_catch_test(clppap_allocation_test
    clppap/allocation_test.cpp support/allocation_probe.cpp)
  foreach(target IN ITEMS clppap_reflection_test clppap_allocation_test)
    target_compile_options(${target} PRIVATE -freflection)
  endforeach()
endif()

cutils_configure_tests()
