# Additional CMake configuration file for building the example programs of the 
# RedisX library. The examples are also added to the test suite so they 
# may be checked for functionality.
#
# To invoke simply configure the cmake build in the xchange root directory with
# the -DBUILD_EXAMPLES=ON option.
#
# Author: Attila Kovacs

cmake_minimum_required(VERSION 3.20...4.3)

# Configuration in case this is a standalone project....
if(NOT PROJECT_NAME)
    project(redisx-examples
        VERSION 1.0.4
        DESCRIPTION "RedisX example programs"
        HOMEPAGE_URL "https://sigmyne.github.io/redisx/"
        LANGUAGES C
    )
    
    include(CheckLibraryExists)
    include(GNUInstallDirs)
    
    find_package(redisx)
    
    check_library_exists(m exp "" HAS_MATHLIB)
    if(HAS_MATHLIB)
        set(MATHLIB m)
    endif()
endif()

# Build all examples
FILE(GLOB EXAMPLE_PROGRAMS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.c)

# Example programs, matching the sources
list(TRANSFORM EXAMPLE_PROGRAMS REPLACE "[.]c$" "")

message("include dirs is ${redisx_INCLUDE_DIRS}")

# Build each example
foreach(EXAMPLE ${EXAMPLE_PROGRAMS})
    set(EXAMPLE_SOURCE ${EXAMPLE}.c)

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${EXAMPLE_SOURCE})
        add_executable(${EXAMPLE} ${EXAMPLE_SOURCE})

        target_include_directories(${EXAMPLE} PRIVATE ${xchange_INCLUDE_DIRS})

        # Link against the xchange library
        target_link_libraries(${EXAMPLE} PRIVATE redisx::core xchange::core ${MATHLIB})

        add_test(NAME ${EXAMPLE} COMMAND ${EXAMPLE})
    else()
        message(WARNING "Source file ${EXAMPLE_SOURCE} not found - ${EXAMPLE} will not be built")
    endif()
endforeach()
