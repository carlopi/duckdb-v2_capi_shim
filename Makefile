PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# The extension template's build: DuckDB (the duckdb submodule, or the version CI checks out) with this extension
EXT_NAME=v2_capi_shim
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile

# The shim's own test against the build: a v2.0.1 extension (tests/child.c) loaded through it
test_shim: release
	tests/run.sh build/release/duckdb build/release/extension/v2_capi_shim/v2_capi_shim.duckdb_extension
