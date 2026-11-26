#!/usr/bin/env bash
# The bridge plus capabilities: v2_capi_shim_v201 built with shim_extra.cpp (compiled against the target's own C++ headers
# and linked with the target's libduckdb_static.a, as a C++ extension is: the loader carries its own copy of that
# version's code and exports nothing but its entrypoint), and child2, a v2.0.1 extension requiring 'connections/1'. Needs tools/capi-compat/run.sh first. Target sources in $TARGET_SRC (default
# .cache/duckdb-main), its version id in $TARGET_VERSION.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)" && cd "$ROOT"
X=tools/capi-compat/extras; R=tools/capi-compat/runtime; O=.cache/capi-compat; B=$O/extras; mkdir -p "$B"
TS="${TARGET_SRC:-.cache/duckdb-main}"; T="$TS/src/include"; C=duckdb/src/include
TV="${TARGET_VERSION:-v2.1.0-dev0}"; U=-DDUCKDB_V2_API_ALLOW_UNSTABLE=1
clang -fPIC -c -std=c11 $U -Wall -Werror -Wno-unused-function -I "$T" -I $O -include $R/target_prelude.h $O/shim.c -o $B/shim.o
clang -fPIC -c -std=c11 $U -Wall -Werror -I "$T" -include $R/target_prelude.h tools/capi-compat/compat_manual.c -o $B/manual.o
clang -fPIC -c -std=c11 $U -DDUCKDB_BUILD_STATIC_EXTENSION -Wall -Werror -I $C $O/compat_table.c -o $B/table.o
clang -fPIC -c -std=c11 $U -Wall -Werror -I "$T" -I $X $X/shim_entry.c -o $B/entry.o
# the target's own build flags for its C++ (release, C++17, its third_party includes)
clang++ -fPIC -c -std=c++17 -O2 -DNDEBUG -DDUCKDB_MAJOR_VERSION=2 -DDUCKDB_MINOR_VERSION=1 -DDUCKDB_PATCH_VERSION=0 \
	-I "$T" -I "$TS/third_party/fmt/include" -I "$TS/third_party/fast_float" -I "$TS/third_party/re2" -I $X $X/shim_extra.cpp -o $B/extra.o
L="$TS/build/release"
clang++ -shared -o $B/v2_capi_shim_v201.dylib $B/entry.o $B/shim.o $B/manual.o $B/table.o $B/extra.o \
	-Wl,-exported_symbol,_v2_capi_shim_v201_init_c_api_v2 -Wl,-dead_strip \
	"$L/src/libduckdb_static.a" "$L/src/main/extension/libduckdb_loadable_extensions.a" "$L/src/main/http/libduckdb_httplib.a" \
	"$L"/third_party/*/libduckdb_*.a
clang -fPIC -shared -std=c11 $U -Wall -Werror -I $C -I $X $X/child2.c -o $B/child2.duckdb_extension
python3 extension-ci-tools/scripts/append_extension_metadata.py -l $B/v2_capi_shim_v201.dylib -n v2_capi_shim_v201 \
	-o $B/v2_capi_shim_v201.duckdb_extension -p osx_arm64 -dv "$TV" -ev "v2.0.1-on-$TV" --abi-type C_STRUCT_UNSTABLE > /dev/null
mkdir -p $B/native && python3 extension-ci-tools/scripts/append_extension_metadata.py -l $B/child2.duckdb_extension -n child2 \
	-o $B/native/child2.duckdb_extension -p osx_arm64 -dv v2.0.1 -ev v0.0.1 --abi-type C_STRUCT_UNSTABLE > /dev/null
ls -la $B/v2_capi_shim_v201.duckdb_extension $B/child2.duckdb_extension
echo "duckdb:: symbols defined in the shim: $(nm -U $B/v2_capi_shim_v201.dylib | grep -c '6duckdb' || true); undefined duckdb:: symbols: $(nm -u $B/v2_capi_shim_v201.dylib | grep -c '6duckdb' || true); exported: $(nm -gU $B/v2_capi_shim_v201.dylib | awk '{print $3}' | tr '\n' ' ')"
