# See LICENSE in the repository root.

cmake_minimum_required(VERSION 3.28)
project(cutils VERSION 0.1.0 LANGUAGES CXX)

option(CUTILS_BUILD_TESTING "Build cutils tests" ${PROJECT_IS_TOP_LEVEL})
find_package(Threads REQUIRED)

# Set here as well as in remmy's root so standalone builds get it too.
add_compile_options(-fno-exceptions)

add_library(cutils STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/os/limits/fd.cpp")
add_library(cutils::cutils ALIAS cutils)
target_compile_features(cutils PUBLIC cxx_std_26)
target_link_libraries(cutils PUBLIC Threads::Threads)
target_include_directories(cutils PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

function(cutils_harden tgt)
  target_compile_options(${tgt} PRIVATE
    -fstack-protector-strong
    -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion
    -Wshadow -Wformat=2 -Wundef -Wdouble-promotion -Wimplicit-fallthrough
    -Wcast-align -Wnon-virtual-dtor -Woverloaded-virtual -Wold-style-cast)
  # No stable binary ABI is promised; keep the compiler's native layout choice.
  target_compile_options(${tgt} PRIVATE "$<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>")
  target_compile_definitions(${tgt} PRIVATE _GLIBCXX_ASSERTIONS=1)
endfunction()

cutils_harden(cutils)
target_compile_options(cutils PRIVATE
  "$<$<NOT:$<CONFIG:Release>>:-fno-omit-frame-pointer>")
target_compile_definitions(cutils PRIVATE
  "_LIBCPP_HARDENING_MODE=$<IF:$<CONFIG:Release>,_LIBCPP_HARDENING_MODE_FAST,_LIBCPP_HARDENING_MODE_DEBUG>"
  "$<$<NOT:$<CONFIG:Release>>:_GLIBCXX_DEBUG=1>")

if(CUTILS_BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()
