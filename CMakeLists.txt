cmake_minimum_required(VERSION 3.20)

project(FacilityObservatory
  VERSION 1.0.0
  DESCRIPTION "Read-only evidence observatory for Data Center Control Plane facility state"
  HOMEPAGE_URL "https://github.com/summonlabs/Facility-Observatory"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language settings
# ---------------------------------------------------------------------------
if(NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED CMAKE_POSITION_INDEPENDENT_CODE)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if(MSVC)
  # Deterministic, conformance-oriented toolchain behaviour.
  add_compile_options(/utf-8 /Zc:__cplusplus /Zc:preprocessor /permissive- /EHsc /diagnostics:caret)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(FACILITY_OBSERVATORY_BUILD_TESTS "Build the Facility Observatory test suite" ${BUILD_TESTING})
option(FACILITY_OBSERVATORY_BUILD_CLI "Build the facility-observatory command line tool" ON)
option(FACILITY_OBSERVATORY_BUILD_BENCHMARKS "Build Facility Observatory benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(FACILITY_OBSERVATORY_WARNINGS_AS_ERRORS "Promote first-party compiler warnings to errors" ON)
option(FACILITY_OBSERVATORY_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(FACILITY_OBSERVATORY_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer (GCC/Clang only)" OFF)

# ---------------------------------------------------------------------------
# First-party compile policy
#
# Applied per target with PRIVATE scope rather than through an exported
# INTERFACE target, so consumers of the installed package never inherit this
# repository's warning or sanitizer policy.
# ---------------------------------------------------------------------------
function(fo_apply_project_options target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(FACILITY_OBSERVATORY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
    if(FACILITY_OBSERVATORY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(FACILITY_OBSERVATORY_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
  if(FACILITY_OBSERVATORY_ENABLE_UBSAN AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined)
    target_link_options(${target} PRIVATE -fsanitize=undefined)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(facility_observatory
  src/authority.cpp
  src/codec.cpp
  src/contracts.cpp
  src/divergence.cpp
  src/evidence.cpp
  src/integrity.cpp
  src/journal.cpp
  src/json.cpp
  src/lock_order.cpp
  src/observatory.cpp
  src/outcome.cpp
  src/platform.cpp
  src/policy.cpp
  src/render.cpp
  src/store.cpp
  src/strong.cpp
  src/time.cpp
  src/version.cpp
)
add_library(FacilityObservatory::facility_observatory ALIAS facility_observatory)

target_include_directories(facility_observatory
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
fo_apply_project_options(facility_observatory)

# The runtime owns threads and a kernel-enforced writer lock, so the dependency
# is declared PUBLIC and re-found by the installed package config.
find_package(Threads REQUIRED)
target_link_libraries(facility_observatory PUBLIC Threads::Threads)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(facility_observatory PUBLIC FO_SHARED_BUILD PRIVATE FO_EXPORTS)
  set_target_properties(facility_observatory PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)
endif()

set_target_properties(facility_observatory PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME facility_observatory)

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
set(FO_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/FacilityObservatory" CACHE STRING
    "Directory (relative to the install prefix) holding the CMake package files")

install(TARGETS facility_observatory
  EXPORT FacilityObservatoryTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT FacilityObservatoryTargets
  FILE FacilityObservatoryTargets.cmake
  NAMESPACE FacilityObservatory::
  DESTINATION ${FO_INSTALL_CMAKEDIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FacilityObservatoryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/FacilityObservatoryConfig.cmake
  INSTALL_DESTINATION ${FO_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/FacilityObservatoryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/FacilityObservatoryConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/FacilityObservatoryConfigVersion.cmake
  DESTINATION ${FO_INSTALL_CMAKEDIR})

install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/FacilityObservatory)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(FACILITY_OBSERVATORY_BUILD_CLI)
  add_subdirectory(apps)
endif()

if(FACILITY_OBSERVATORY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(FACILITY_OBSERVATORY_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
