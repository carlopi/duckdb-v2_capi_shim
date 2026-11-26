cmake_minimum_required(VERSION 3.5...3.29)

# v2_capi_shim, built by DuckDB's own extension build:
#   duckdb_extension_load(v2_capi_shim SOURCE_DIR /path/to/v2_capi_shim)
# in that DuckDB's extension_config.cmake. Statically linked or loadable like any extension, for exactly that DuckDB.
set(TARGET_NAME v2_capi_shim)
project(${TARGET_NAME} C CXX)

# The contract: the release whose v2 C API the loaded extensions are built against (contracts/<version>); the latest
# release, bumped with each one
set(V2_CAPI_SHIM_CONTRACT
    "v2.0.1"
    CACHE STRING "DuckDB release whose v2 C API v2_capi_shim serves")
set(V2_CAPI_SHIM_CONTRACT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/contracts/${V2_CAPI_SHIM_CONTRACT})
if(NOT EXISTS ${V2_CAPI_SHIM_CONTRACT_DIR}/duckdb_v2.h)
  message(FATAL_ERROR "v2_capi_shim: no contract ${V2_CAPI_SHIM_CONTRACT} in ${CMAKE_CURRENT_SOURCE_DIR}/contracts")
endif()

# The target: the DuckDB this is built with
set(V2_CAPI_SHIM_TARGET_INCLUDE ${CMAKE_SOURCE_DIR}/src/include)
if(NOT EXISTS ${V2_CAPI_SHIM_TARGET_INCLUDE}/duckdb_v2.h)
  message(FATAL_ERROR "v2_capi_shim: ${V2_CAPI_SHIM_TARGET_INCLUDE} has no duckdb_v2.h: a DuckDB without the v2 C API")
endif()

# The adapters and the contract table, generated now with python3 (DuckDB's build needs it anyway), regenerated when
# either header changes
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(V2_CAPI_SHIM_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(V2_CAPI_SHIM_PAIR_DEFAULTS "")
file(GLOB V2_CAPI_SHIM_PAIRS ${CMAKE_CURRENT_SOURCE_DIR}/pairs/${V2_CAPI_SHIM_CONTRACT}_*.json)
foreach(pair ${V2_CAPI_SHIM_PAIRS})
  # pairs/<contract>_<target version>.json: defaults for parameters that target added
  get_filename_component(pair_name ${pair} NAME_WE)
  if(pair_name STREQUAL "${V2_CAPI_SHIM_CONTRACT}_v${DUCKDB_MAJOR_VERSION}.${DUCKDB_MINOR_VERSION}.${DUCKDB_PATCH_VERSION}")
    set(V2_CAPI_SHIM_PAIR_DEFAULTS --defaults ${pair})
  endif()
endforeach()
execute_process(
  COMMAND
    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate.py --contract ${V2_CAPI_SHIM_CONTRACT_DIR} --target
    ${V2_CAPI_SHIM_TARGET_INCLUDE} --out ${V2_CAPI_SHIM_GEN} ${V2_CAPI_SHIM_PAIR_DEFAULTS}
  RESULT_VARIABLE V2_CAPI_SHIM_GEN_RESULT
  OUTPUT_VARIABLE V2_CAPI_SHIM_GEN_OUTPUT
  ERROR_VARIABLE V2_CAPI_SHIM_GEN_ERROR)
if(NOT V2_CAPI_SHIM_GEN_RESULT EQUAL 0)
  message(FATAL_ERROR "v2_capi_shim: no bridge from ${V2_CAPI_SHIM_CONTRACT} to this DuckDB:\n"
                      "${V2_CAPI_SHIM_GEN_OUTPUT}${V2_CAPI_SHIM_GEN_ERROR}")
endif()
message(STATUS "v2_capi_shim: contract ${V2_CAPI_SHIM_CONTRACT}: ${V2_CAPI_SHIM_GEN_OUTPUT}")
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS ${V2_CAPI_SHIM_TARGET_INCLUDE}/duckdb_v2.h ${V2_CAPI_SHIM_CONTRACT_DIR}/duckdb_v2.h
           ${CMAKE_CURRENT_SOURCE_DIR}/gen_shim.py ${CMAKE_CURRENT_SOURCE_DIR}/parse_header.py
           ${CMAKE_CURRENT_SOURCE_DIR}/generate.py)

# the adapters call the running DuckDB's v2 functions through the table it hands out (src/target_api.c), as any v2
# extension does: a loadable extension carries its own copy of DuckDB, whose code must not run on the host's objects
# for these calls (an exception the host throws would not be recognized by the copy). The contract's table is laid out
# by the contract's headers
add_library(v2_capi_shim_bridge OBJECT ${V2_CAPI_SHIM_GEN}/shim.c ${CMAKE_CURRENT_SOURCE_DIR}/src/target_api.c)
target_include_directories(v2_capi_shim_bridge PRIVATE ${V2_CAPI_SHIM_TARGET_INCLUDE} ${V2_CAPI_SHIM_GEN})
target_compile_options(v2_capi_shim_bridge PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/src/target_prelude.h)
# the hand-written adapters this pair needs (generate.py says which)
file(STRINGS ${V2_CAPI_SHIM_GEN}/manual_sources.txt V2_CAPI_SHIM_MANUAL_SOURCES)
foreach(src ${V2_CAPI_SHIM_MANUAL_SOURCES})
  target_sources(v2_capi_shim_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${src})
endforeach()
add_library(v2_capi_shim_table OBJECT ${V2_CAPI_SHIM_GEN}/compat_table.c)
target_include_directories(v2_capi_shim_table BEFORE PRIVATE ${V2_CAPI_SHIM_CONTRACT_DIR})
target_compile_definitions(v2_capi_shim_table PRIVATE DUCKDB_BUILD_STATIC_EXTENSION)
foreach(lib v2_capi_shim_bridge v2_capi_shim_table)
  target_compile_definitions(${lib} PRIVATE DUCKDB_V2_API_ALLOW_UNSTABLE=1)
  set_target_properties(${lib} PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)
endforeach()

include_directories(src/include ${V2_CAPI_SHIM_GEN})
add_definitions(-DV2_CAPI_SHIM_CONTRACT="${V2_CAPI_SHIM_CONTRACT}" -DDUCKDB_V2_API_ALLOW_UNSTABLE=1)
set(EXTENSION_SOURCES src/v2_capi_shim_extension.cpp $<TARGET_OBJECTS:v2_capi_shim_bridge>
                      $<TARGET_OBJECTS:v2_capi_shim_table>)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
if(UNIX)
  target_link_libraries(${TARGET_NAME}_loadable_extension ${CMAKE_DL_LIBS})
endif()

install(
  TARGETS ${TARGET_NAME}_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
