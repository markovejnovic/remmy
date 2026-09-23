# See LICENSE in the repository root.

# Test-only instrumentation for the cutils suite.
# CUTILS_TEST_TSAN selects TSan over ASan. There is no unsanitized test mode.
set(cutils_sanitizer address)
if(CUTILS_TEST_TSAN)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
    message(FATAL_ERROR "The TSan test run requires Clang")
  endif()
  set(cutils_sanitizer thread)
endif()
set(cutils_sanitizer_flags "-fsanitize=${cutils_sanitizer},undefined,float-divide-by-zero,vptr")
if(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
  string(APPEND cutils_sanitizer_flags ",integer,local-bounds,nullability")
  list(APPEND cutils_sanitizer_ignorelist
    "-fsanitize-ignorelist=${CMAKE_CURRENT_LIST_DIR}/sanitizer-ignorelist.txt")
endif()

add_library(cutils_test_options INTERFACE)
target_compile_options(cutils_test_options INTERFACE ${cutils_sanitizer_flags}
  ${cutils_sanitizer_ignorelist} -O1 -g -UNDEBUG -fno-omit-frame-pointer -fno-optimize-sibling-calls
  -fno-sanitize-recover=all)
target_link_options(cutils_test_options INTERFACE
  ${cutils_sanitizer_flags} -fno-sanitize-recover=all)
target_compile_definitions(cutils_test_options INTERFACE
  _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG _GLIBCXX_DEBUG=1)
if(NOT CUTILS_TEST_TSAN)
  target_compile_options(cutils_test_options INTERFACE -fsanitize-address-use-after-scope)
  if(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
    target_compile_options(cutils_test_options INTERFACE -fsanitize-address-use-after-return=always)
  endif()
endif()

# Recompile the implementation for tests without changing cutils::cutils.
get_target_property(cutils_sources cutils SOURCES)
add_library(cutils_test STATIC ${cutils_sources})
target_compile_features(cutils_test PUBLIC cxx_std_26)
target_include_directories(cutils_test PUBLIC "$<TARGET_PROPERTY:cutils,INTERFACE_INCLUDE_DIRECTORIES>")
target_link_libraries(cutils_test PUBLIC Threads::Threads cutils_test_options)
cutils_harden(cutils_test)
set_property(TARGET cutils_test PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)

function(cutils_test_environment output)
  set(detect_leaks 1)
  if(APPLE)
    # LeakSanitizer is unsupported on Apple arm64.
    set(detect_leaks 0)
  endif()
  set(${output} "ASAN_OPTIONS=detect_leaks=${detect_leaks}:detect_stack_use_after_return=1:strict_string_checks=1:alloc_dealloc_mismatch=1:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;TSAN_OPTIONS=halt_on_error=1:detect_deadlocks=1:second_deadlock_stack=1" PARENT_SCOPE)
endfunction()

function(cutils_configure_tests)
  cutils_test_environment(environment)
  get_property(tests DIRECTORY PROPERTY TESTS)
  set_tests_properties(${tests} PROPERTIES TIMEOUT 90
    ENVIRONMENT "${environment}")
endfunction()
