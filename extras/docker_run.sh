#!/usr/bin/env bash
# Linux/Docker: the loader with capabilities and signature checks, built for linux_arm64 against DuckDB main as built
# in a container (.cache/docker-spike/build-main, linked statically), loading rusty_quack (a community extension built
# for v2.0.1, installed by the official duckdb/duckdb:2.0.1 image) with repo := 'community'. Output compared with the
# same queries on v2.0.1 natively. Third-party binaries only run inside containers.
set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)" && cd "$ROOT"
S=.cache/docker-spike
docker run --rm -v "$ROOT":/w -w /w debian:bookworm-slim bash -c '
set -e
apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends gcc g++ libc6-dev python3 >/dev/null 2>&1
S='"$S"'; O=.cache/capi-compat; X=tools/capi-compat/extras; R=tools/capi-compat/runtime; B=$S/loader; mkdir -p $B
TS=.cache/duckdb-main; T=$TS/src/include; C=duckdb/src/include; L=$S/build-main; U=-DDUCKDB_V2_API_ALLOW_UNSTABLE=1
gcc -fPIC -c -std=c11 $U -Wall -Werror -Wno-unused-function -I $T -I $O -include $R/target_prelude.h $O/shim.c -o $B/shim.o
gcc -fPIC -c -std=c11 $U -Wall -Werror -I $T -include $R/target_prelude.h tools/capi-compat/compat_manual.c -o $B/manual.o
gcc -fPIC -c -std=c11 $U -DDUCKDB_BUILD_STATIC_EXTENSION -Wall -Werror -I $C $O/compat_table.c -o $B/table.o
TLV=$($L/duckdb -csv -noheader -c "SELECT library_version FROM pragma_version()"); TSID=$($L/duckdb -csv -noheader -c "SELECT source_id FROM pragma_version()")
gcc -fPIC -c -std=gnu11 $U -Wall -Werror -DSHIM_TARGET_VERSION="\"$TLV\"" -DSHIM_TARGET_SOURCE_ID="\"$TSID\"" -DSHIM_PLATFORM='"linux_arm64"' -I $T -I $X -I $R $X/shim_entry.c -o $B/entry.o
g++ -fPIC -c -std=c++17 -O2 -DNDEBUG -DDUCKDB_MAJOR_VERSION=2 -DDUCKDB_MINOR_VERSION=1 -DDUCKDB_PATCH_VERSION=0 \
	-I $T -I $TS/third_party/fmt/include -I $TS/third_party/fast_float -I $TS/third_party/re2 -I $X $X/shim_extra.cpp -o $B/extra.o
echo "{ global: v2_capi_shim_init_c_api_v2; local: *; };" > $B/exports.map
g++ -shared -o $B/v2_capi_shim.so $B/entry.o $B/shim.o $B/manual.o $B/table.o $B/extra.o -Wl,--version-script=$B/exports.map \
	-Wl,--gc-sections -Wl,--start-group $L/src/libduckdb_static.a $L/src/main/extension/libduckdb_loadable_extensions.a \
	$L/src/main/http/libduckdb_httplib.a $L/third_party/*/libduckdb_*.a -Wl,--end-group -ldl -lpthread
V=$($L/duckdb -csv -noheader -c "SELECT source_id FROM pragma_version()")
python3 extension-ci-tools/scripts/append_extension_metadata.py -l $B/v2_capi_shim.so -n v2_capi_shim -o $B/v2_capi_shim.duckdb_extension -p linux_arm64 -dv "$V" -ev "v2.0.1-on-$V" --abi-type C_STRUCT_UNSTABLE >/dev/null
EXT=$S/v201-home/extensions/v2.0.1/linux_arm64/rusty_quack.duckdb_extension
echo "target: $($L/duckdb -csv -noheader -c "SELECT library_version || chr(32) || source_id FROM pragma_version()")  loader: $(stat -c %s $B/v2_capi_shim.duckdb_extension) bytes"
echo "== rusty_quack on main, directly:"
$L/duckdb -no-agent -unsigned -c "LOAD '"'"'$EXT'"'"';" 2>&1 | head -1 || true
LD="LOAD '"'"'$B/v2_capi_shim.duckdb_extension'"'"'; SET allow_unsigned_extensions = false"
echo "== signed by core? (it is not):"
$L/duckdb -no-agent -unsigned -c "$LD; SELECT * FROM load_extension('"'"'$EXT'"'"', repo := '"'"'core'"'"');" 2>&1 | head -1 || true
echo "== signed by community, through the loader:"
$L/duckdb -no-agent -unsigned -csv -cmd "$LD; SELECT * FROM load_extension('"'"'$EXT'"'"', repo := '"'"'community'"'"');" -f $S/rusty_queries.sql > $S/actual_main.csv 2>&1 || true
cat $S/actual_main.csv
'
echo "== diff against v2.0.1 natively:"
diff <(sed '1,3d' $S/expected_v201.csv | sed '1s/^\x1b\[00m//') <(sed '1,2d' $S/actual_main.csv) && echo IDENTICAL
