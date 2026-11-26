# For building this repository on its own (make, through extension-ci-tools); a DuckDB that wants it instead says, in
# its own extension_config.cmake:
#   duckdb_extension_load(v2_capi_shim SOURCE_DIR /path/to/v2_capi_shim)
duckdb_extension_load(v2_capi_shim SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR})
