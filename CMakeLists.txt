# CMake configuration file for the RedisX library
#
# Authors: Attila Kovacs
#
# Usage: 
#   
#  To build under a 'build/' directory:
#
#    $ cmake -B build [OPTIONS]
#    $ cmake --build build
#  
#  And then to install, e.g. under '/usr/local':
#    $ [sudo ] cmake --install --prefix=/usr/local
#
#  You may install just a specific component via the --component option, such
#  as 'Runtime' or 'Development'.
#

cmake_minimum_required(VERSION 3.24...4.3)

# Project definition
project(redisx
    VERSION 1.0.4
    DESCRIPTION "A completely free Redis / Valkey client library for C/C++"
    HOMEPAGE_URL "https://sigmyne.github.io/redisx/"
    LANGUAGES C
)

# Include required modules
include(CheckLibraryExists)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FeatureSummary)


# Build options
option(BUILD_SHARED_LIBS "Build as shared libraries instead of static" OFF)
option(BUILD_DOC "Build HTML documetation" OFF)
option(BUILD_TESTING "Build regression test suite" ON)
option(BUILD_EXAMPLES "Build example programs" OFF)
option(ENABLE_OPENMP "Build with OpenMP support" OFF)
option(ENABLE_TLS "Build with TLS support" OFF)

# Set feature descriptions for summary
add_feature_info(SharedLibs BUILD_SHARED_LIBS "Build as shared libraries")
add_feature_info(Documentation BUILD_DOC "Developer documentation (HTML)")
add_feature_info(Testing BUILD_TESTING "Run regression testing")
add_feature_info(Examples BUILD_EXAMPLES "Build and test example programs")
add_feature_info(OpenMP ENABLE_OPENMP "OpenMP support")
add_feature_info(TLS ENABLE_TLS "TLS support")


# ----------------------------------------------------------------------------
# pkg-config variables

# [.pc] Libs -- linker flags for apps
set(PC_LIBS "-lredisx ")

# [.pc] Libs.private -- linker flags for supernovas itself
set(PC_LIBS_PRIVATE "")

# ----------------------------------------------------------------------------
# Global build configuration

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Debug postfix for libraries
set(CMAKE_DEBUG_POSTFIX d)

# Build time locations for sub-configs
link_directories(${CMAKE_BINARY_DIR}/lib)
set(redisx_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include)

if(WIN32)
    # Windows-specific settings
    message("Configuring for Windows...")
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

check_library_exists(m exp "" HAS_MATHLIB)
if(HAS_MATHLIB)
    set(MATHLIB m)
    
    # [.pc] Link applications against math lib...
    string(APPEND PC_LIBS_PRIVATE -l${MATHLIB} " ")
endif()

find_library(POPT_LIBRARY popt OPTIONAL)
find_library(READLINE_LIBRARY readline OPTIONAL)
find_library(BSD_LIBRARY bsd OPTIONAL)

# xchange dependency
find_package(xchange REQUIRED)
set_package_properties(xchange PROPERTIES
    URL "https://sigmyne.github.io/xchange"
    DESCRIPTION  "structured data representation for C/C++"
    TYPE REQUIRED
    PURPOSE "Provides definitions for RedisX"
)
string(APPEND PC_LIBS_PRIVATE "-lxchange ")

find_package(Threads REQUIRED)
if(CMAKE_THREAD_LIBS_INIT)
    # [.pc] apps should be linked against threads libs also...
    string(APPEND PC_LIBS_PRIVATE ${CMAKE_THREAD_LIBS_INIT} " ")
endif()

# OpenMP dependency
if(ENABLE_OPENMP)
    find_package(OpenMP REQUIRED COMPONENTS C)
    string(APPEND PC_LIBS_PRIVATE "-lopenmp ")
endif()

# OpenSSL dependency
if(ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
    string(APPEND PC_LIBS_PRIVATE "-lopenssl ")
endif()


# ----------------------------------------------------------------------------
# Build libraries from source

add_subdirectory(src)


# ----------------------------------------------------------------------------
# Testing, examples, and benchmarks...

# Run tests for the enabled options below...
enable_testing()

# Examples
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Testing
if(BUILD_TESTING)
    add_subdirectory(test)
endif()

# Examples
if(BUILD_DOC)
    add_subdirectory(doc)
endif()


# ----------------------------------------------------------------------------
# Install Runtime component

install(FILES include/redisx.h
    TYPE INCLUDE
    COMPONENT Development
)

set(INSTALL_TARGETS core)
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME Runtime)

install(TARGETS ${INSTALL_TARGETS} EXPORT redisxTargets)

# Install markdown files
install(FILES CHANGELOG.md CONTRIBUTING.md
    TYPE DOC
    COMPONENT Development   
)

# For examples/ dir, list just the directory, not the individual files installed.
install(CODE "message(STATUS \"Installing: <prefix>/${CMAKE_INSTALL_DOCDIR}/examples/\")")
install(DIRECTORY ${PROJECT_SOURCE_DIR}/examples
    TYPE DOC
    MESSAGE_NEVER
    COMPONENT Development
    PATTERN .gitignore EXCLUDE
)

install(EXPORT redisxTargets
    COMPONENT Development
    FILE redisxTargets.cmake
    NAMESPACE redisx::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/redisx
)


# ------------------------------------------------------------------------
# Package config

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/redisxConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/redisxConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/redisx
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/redisxConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/redisxConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/redisxConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/redisx
    COMPONENT Development
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/redisx.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/redisx.pc
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/redisx.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    COMPONENT Development
)

# ----------------------------------------------------------------------------
# Uninstall target
if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        IMMEDIATE @ONLY
    )

    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    )
endif()

# ----------------------------------------------------------------------------
# Export the package for use from the build-tree
# (this registers the build-tree with a global CMake-registry)
export(PACKAGE redisx)
export(EXPORT redisxTargets NAMESPACE redisx:: FILE redisxTargets.cmake)

# ----------------------------------------------------------------------------
# Summary
feature_summary(WHAT ALL)

message(" - Compile flags added with CMAKE_BUILD_TYPE:")
message("   Debug:             ${CMAKE_C_FLAGS_DEBUG}")
message("   Release:           ${CMAKE_C_FLAGS_RELEASE}")
message("   RelWithDebInfo:    ${CMAKE_C_FLAGS_RELWITHDEBINFO}")
message("   MinSizeRel:        ${CMAKE_C_FLAGS_MINSIZEREL}")
message("")

message(" - RedisX Configuration Summary:")
message("   Version:           ${PROJECT_VERSION}")
if(CMAKE_BUILD_TYPE)
    message("   Build type:        ${CMAKE_BUILD_TYPE}")
else()
    message("   Build type:        (default) -- external CFLAGS only")
endif()
message("   Install prefix:    ${CMAKE_INSTALL_PREFIX}")
message("   Compiler:          ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message("")


