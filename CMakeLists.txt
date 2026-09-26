# See LICENSE in the repository root.

cmake_minimum_required(VERSION 3.28)

# Export an explicit SDK for non-Apple clang-tidy/clangd, including CMake 4.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_SYSROOT AND "$ENV{SDKROOT}" STREQUAL "")
  set(CMAKE_OSX_SYSROOT macosx CACHE STRING "macOS SDK" FORCE)
endif()

project(remmy LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(CheckCXXCompilerFlag)
include(CheckIPOSupported)
include(CTest)
find_package(Threads REQUIRED)

set(REMMY_CLANG_TIDY_DEFAULT OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
  set(REMMY_CLANG_TIDY_DEFAULT ON)
endif()
option(REMMY_ENABLE_CLANG_TIDY "Run strict clang-tidy during compilation" ${REMMY_CLANG_TIDY_DEFAULT})
if(REMMY_ENABLE_CLANG_TIDY)
  find_program(REMMY_CLANG_TIDY NAMES clang-tidy REQUIRED)
  # .clang-tidy owns the check list and warnings-as-errors policy.
  set(CMAKE_CXX_CLANG_TIDY "${REMMY_CLANG_TIDY}")
endif()

# Nothing here throws; keep exceptions off for every target, not per target.
add_compile_options(-fno-exceptions)

# cutils is a dependency here; its test suite has a separate standalone build.
set(CUTILS_BUILD_TESTING OFF)
add_subdirectory(cutils)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
  add_executable(remmy src/main.cpp src/dir_node.cpp)
  target_link_libraries(remmy PRIVATE cutils::cutils)
  target_compile_options(remmy PRIVATE -freflection)
  # ld64 flag; ELF executables export nothing without -rdynamic anyway.
  target_link_options(remmy PRIVATE -static-libstdc++ -static-libgcc
    $<$<PLATFORM_ID:Darwin>:-Wl,-exported_symbol,_main>)
  cutils_harden(remmy)
  # Clang tools cannot parse the reflecting entry point.
  set_property(TARGET remmy PROPERTY CXX_CLANG_TIDY "")
elseif(REMMY_ENABLE_CLANG_TIDY AND CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
  message(STATUS "Clang: linting cutils with clang-tidy, not building remmy")
else()
  message(FATAL_ERROR "remmy requires GCC 16 reflection; use the release or debug preset")
endif()

set(REMMY_LINT_FILES)
if(BUILD_TESTING)
  # End-to-end tests run the shipped executable; sanitizer coverage belongs to cutils.
  if(TARGET remmy)
    find_program(REMMY_UV NAMES uv)
    if(REMMY_UV)
      add_test(NAME e2e
        COMMAND "${REMMY_UV}" run --project "${CMAKE_CURRENT_SOURCE_DIR}"
          pytest "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p no:cacheprovider -n auto
          --remmy "$<TARGET_FILE:remmy>"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
      message(STATUS "uv not found; e2e test disabled")
    endif()
  endif()
endif()

if(TARGET remmy)
  # Optimization flags must never bleed into Debug or sanitizer builds.
  check_cxx_compiler_flag("-march=native" REMMY_HAS_MARCH_NATIVE)
  if(REMMY_HAS_MARCH_NATIVE)
    set(REMMY_NATIVE_FLAG -march=native)
  else()
    check_cxx_compiler_flag("-mcpu=native" REMMY_HAS_MCPU_NATIVE)
    if(REMMY_HAS_MCPU_NATIVE)
      set(REMMY_NATIVE_FLAG -mcpu=native)
    endif()
  endif()
  check_ipo_supported(RESULT REMMY_IPO_OK OUTPUT REMMY_IPO_ERR)
  if(NOT REMMY_IPO_OK)
    message(STATUS "Release LTO unavailable: ${REMMY_IPO_ERR}")
  endif()
  target_compile_options(remmy PRIVATE
    "$<$<CONFIG:Release>:-O3;-funroll-loops;${REMMY_NATIVE_FLAG};-fno-unwind-tables;-fno-asynchronous-unwind-tables>"
    "$<$<NOT:$<CONFIG:Release>>:-fno-omit-frame-pointer>")
  target_compile_definitions(remmy PRIVATE
    "_LIBCPP_HARDENING_MODE=$<IF:$<CONFIG:Release>,_LIBCPP_HARDENING_MODE_FAST,_LIBCPP_HARDENING_MODE_DEBUG>"
    "$<$<NOT:$<CONFIG:Release>>:_GLIBCXX_DEBUG=1>")
  set_property(TARGET remmy PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ${REMMY_IPO_OK})
endif()

if(REMMY_ENABLE_CLANG_TIDY AND REMMY_LINT_FILES)
  add_custom_target(tidy
    COMMAND "${REMMY_CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}" ${REMMY_LINT_FILES}
    VERBATIM)
endif()

# Formatting is explicit: never rewrite sources as a side effect of a build.
find_program(REMMY_CLANG_FORMAT NAMES clang-format)
if(REMMY_CLANG_FORMAT)
  file(GLOB_RECURSE REMMY_FORMAT_FILES CONFIGURE_DEPENDS
    cutils/*.hpp cutils/*.cpp
    src/*.[ch] src/*.cpp src/*.hpp)
  add_custom_target(format
    COMMAND "${REMMY_CLANG_FORMAT}" --style=file -i ${REMMY_FORMAT_FILES}
    VERBATIM)
  add_custom_target(format-check
    COMMAND "${REMMY_CLANG_FORMAT}" --style=file --dry-run --Werror ${REMMY_FORMAT_FILES}
    VERBATIM)
else()
  message(STATUS "clang-format not found; format/format-check targets unavailable")
endif()

option(REMMY_BUILD_DOCS "Enable Doxygen documentation targets" OFF)
if(REMMY_BUILD_DOCS)
  find_package(Doxygen 1.9.8 REQUIRED COMPONENTS doxygen OPTIONAL_COMPONENTS dot)
  set(DOXYGEN_GENERATE_HTML YES)
  set(DOXYGEN_GENERATE_LATEX NO)
  set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/docs")
  set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${CMAKE_CURRENT_SOURCE_DIR}/TESTING.md")
  set(DOXYGEN_EXTRACT_PRIVATE YES)
  set(DOXYGEN_EXTRACT_STATIC YES)
  set(DOXYGEN_EXTRACT_ANON_NSPACES YES)
  set(DOXYGEN_WARN_AS_ERROR FAIL_ON_WARNINGS)
  set(DOXYGEN_WARN_IF_DOC_ERROR YES)
  # Preview all code while writing. Malformed documentation still fails.
  set(DOXYGEN_EXTRACT_ALL YES)
  doxygen_add_docs(docs cutils/include cutils/src src TESTING.md COMMENT "Generate the API reference")

  # Separate completeness gate: undocumented API and parameters are errors.
  set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/docs-check")
  set(DOXYGEN_EXTRACT_ALL NO)
  set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
  set(DOXYGEN_WARN_IF_UNDOC_ENUM_VAL YES)
  set(DOXYGEN_WARN_NO_PARAMDOC YES)
  doxygen_add_docs(docs-check cutils/include cutils/src src TESTING.md COMMENT "Check API documentation completeness")
endif()
