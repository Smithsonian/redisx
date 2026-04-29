# Additional CMake configuration file for building the xchange library from
# source
#
# To invoke simply configure the cmake build in the xchange root directory.
#
# Author: Attila Kovacs


set(C_SOURCES
  redisx.c
  resp.c
  redisx-net.c 
  redisx-hooks.c
  redisx-client.c
  redisx-sentinel.c 
  redisx-cluster.c
  redisx-tab.c
  redisx-sub.c
  redisx-script.c 
  redisx-tls.c
)

add_library(core ${C_SOURCES})
add_library(redisx::core ALIAS core)

set_target_properties(core PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME redisx
)

target_include_directories(core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${xchange_INCLUDE_DIRS}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Link with math libs as necessary
target_link_libraries(core PUBLIC xchange::core ${MATHLIB} Threads::Threads)

if(POPT_LIBRARY AND READLINE_LIBRARY AND BSD_LIBRARY)
  add_executable(redisx-cli redisx-cli.c)
  target_link_libraries(redisx-cli PRIVATE core ${POPT_LIBRARY} ${READLINE_LIBRARY} ${BSD_LIBRARY})
endif()

if(ENABLE_OPENMP)
    target_compile_definitions(core PRIVATE WITH_OPENMP=1)
    target_link_libraries(core PUBLIC OpenMP::OpenMP_C)
    if(POPT_LIBRARY)
       target_link_libraries(redisx-cli PRIVATE OpenMP::OpenMP_C)
    endif()
endif()

if(ENABLE_TLS)
    target_compile_definitions(core PRIVATE WITH_TLS=1)
    target_link_libraries(core PUBLIC OpenSSL::SSL)
    if(POPT_LIBRARY)
       target_link_libraries(redisx-cli PRIVATE OpenSSL::SSL)
    endif()
endif()

#set_property(TARGET core PROPERTY C_STANDARD 99)



  


