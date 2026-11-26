#!/usr/bin/env bash
# The bridge plus extras: v2_capi_shim built with duckdb_shim_extra/1 (shim_extra.cpp, compiled against the target's
# own C++ headers and resolved against the running DuckDB's exported symbols), and child2, a v2.0.1 extension that
# uses the extras when offered. Needs tools/capi-compat/run.sh first. Target sources in $TARGET_SRC (default
# .cache/duckdb-main), its version id in $TARGET_VERSION.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)" && cd "$ROOT"
X=tools/capi-compat/extras; R=tools/capi-compat/runtime; O=.cache/capi-compat; B=$O/extras; mkdir -p "$B"
TS="${TARGET_SRC:-.cache/duckdb-main}"; T="$TS/src/include"; C=duckdb/src/include
TV="${TARGET_VERSION:-v2.1.0-dev0}"; U=-DDUCKDB_V2_API_ALLOW_UNSTABLE=1
clang -fPIC -c -std=c11 $U -Wall -Werror -Wno-unused-function -I "$T" -I $O -include $R/target_prelude.h $O/shim_v201_on_main.c -o $B/shim.o
clang -fPIC -c -std=c11 $U -Wall -Werror -I "$T" -include $R/target_prelude.h tools/capi-compat/compat_manual.c -o $B/manual.o
clang -fPIC -c -std=c11 $U -DDUCKDB_BUILD_STATIC_EXTENSION -Wall -Werror -I $C $O/compat_table.c -o $B/table.o
clang -fPIC -c -std=c11 $U -Wall -Werror -I "$T" -I $X $X/shim_entry.c -o $B/entry.o
# the target's own build flags for its C++ (release, C++17, its third_party includes)
clang++ -fPIC -c -std=c++17 -O2 -DNDEBUG -DDUCKDB_MAJOR_VERSION=2 -DDUCKDB_MINOR_VERSION=1 -DDUCKDB_PATCH_VERSION=0 \
	-I "$T" -I "$TS/third_party/fmt/include" -I "$TS/third_party/fast_float" -I "$TS/third_party/re2" -I $X $X/shim_extra.cpp -o $B/extra.o
clang++ -shared -undefined dynamic_lookup -o $B/v2_capi_shim.dylib $B/entry.o $B/shim.o $B/manual.o $B/table.o $B/extra.o
clang -fPIC -shared -std=c11 $U -Wall -Werror -I $C -I $X $X/child2.c -o $B/child2.duckdb_extension
python3 extension-ci-tools/scripts/append_extension_metadata.py -l $B/v2_capi_shim.dylib -n v2_capi_shim \
	-o $B/v2_capi_shim.duckdb_extension -p osx_arm64 -dv "$TV" -ev v0.0.1 --abi-type C_STRUCT_UNSTABLE > /dev/null
mkdir -p $B/native && python3 extension-ci-tools/scripts/append_extension_metadata.py -l $B/child2.duckdb_extension -n child2 \
	-o $B/native/child2.duckdb_extension -p osx_arm64 -dv v2.0.1 -ev v0.0.1 --abi-type C_STRUCT_UNSTABLE > /dev/null
ls -la $B/v2_capi_shim.duckdb_extension $B/child2.duckdb_extension
echo "duckdb:: symbols defined in the shim: $(nm -U $B/v2_capi_shim.dylib | grep -c '6duckdb' || true)"
