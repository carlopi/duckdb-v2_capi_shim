#!/usr/bin/env bash
# Builds the runtime proof: child (against the contract, the duckdb submodule's v2.0.1 headers) and v2_capi_shim
# (against the target's headers), after tools/capi-compat/run.sh generated the adapters. Target sources in $TARGET_SRC
# (default .cache/duckdb-main), its version in $TARGET_VERSION.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)" && cd "$ROOT"
R=tools/capi-compat/runtime; O=.cache/capi-compat; B=$O/runtime; mkdir -p "$B"
T="${TARGET_SRC:-.cache/duckdb-main}/src/include"; C=duckdb/src/include
TV="${TARGET_VERSION:-v2.1.0-dev0}"; U=-DDUCKDB_V2_API_ALLOW_UNSTABLE=1
clang -fPIC -c -std=c11 $U -Wall -Werror -Wno-unused-function -I "$T" -I $O -include $R/target_prelude.h $O/shim.c -o $B/shim.o
clang -fPIC -c -std=c11 $U -Wall -Werror -I "$T" -include $R/target_prelude.h tools/capi-compat/compat_manual.c -o $B/manual.o
clang -fPIC -c -std=c11 $U -DDUCKDB_BUILD_STATIC_EXTENSION -Wall -Werror -I $C $O/compat_table.c -o $B/table.o
clang -fPIC -c -std=c11 $U -Wall -Werror -I "$T" $R/shim_entry.c -o $B/entry.o
clang -shared -o $B/v2_capi_shim_v201.dylib $B/entry.o $B/shim.o $B/manual.o $B/table.o
clang -fPIC -shared -std=c11 $U -Wall -Werror -I $C $R/child.c -o $B/child.duckdb_extension
python3 extension-ci-tools/scripts/append_extension_metadata.py -l $B/v2_capi_shim_v201.dylib -n v2_capi_shim_v201 \
	-o $B/v2_capi_shim_v201.duckdb_extension -p osx_arm64 -dv "$TV" -ev "v2.0.1-on-$TV" --abi-type C_STRUCT_UNSTABLE > /dev/null
ls -la $B/v2_capi_shim_v201.duckdb_extension $B/child.duckdb_extension
