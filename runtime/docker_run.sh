#!/usr/bin/env bash
# Linux/Docker run of the plain bridge: DuckDB main built in a container (.cache/docker-spike/build-main), the bridge
# built there for linux_arm64, and a published community extension built for v2.0.1 (rusty_quack, installed and
# signature-checked by the official duckdb/duckdb:2.0.1 image into .cache/docker-spike/v201-home) loaded through it.
# Third-party binaries only ever run inside containers. Output compared with the same queries on v2.0.1, natively.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)" && cd "$ROOT"
S=.cache/docker-spike
EXT=/w/$S/v201-home/extensions/v2.0.1/linux_arm64/rusty_quack.duckdb_extension
docker run --rm -v "$ROOT":/w -w /w debian:bookworm-slim bash -c '
set -e
apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends gcc libc6-dev python3 >/dev/null
O=.cache/capi-compat; B='"$S"'/bridge; mkdir -p $B; T=.cache/duckdb-main/src/include; C=duckdb/src/include; U=-DDUCKDB_V2_API_ALLOW_UNSTABLE=1
gcc -fPIC -c -std=c11 $U -Wall -Werror -Wno-unused-function -I $T -I $O -include tools/capi-compat/runtime/target_prelude.h $O/shim.c -o $B/shim.o
gcc -fPIC -c -std=c11 $U -Wall -Werror -I $T -include tools/capi-compat/runtime/target_prelude.h tools/capi-compat/compat_manual.c -o $B/manual.o
gcc -fPIC -c -std=c11 $U -DDUCKDB_BUILD_STATIC_EXTENSION -Wall -Werror -I $C $O/compat_table.c -o $B/table.o
gcc -fPIC -c -std=gnu11 $U -Wall -Werror -I $T tools/capi-compat/runtime/shim_entry.c -o $B/entry.o
gcc -shared -o $B/v2_capi_shim.so $B/entry.o $B/shim.o $B/manual.o $B/table.o -ldl -lpthread
V=$('"$S"'/build-main/duckdb -csv -noheader -c "SELECT source_id FROM pragma_version()")
python3 extension-ci-tools/scripts/append_extension_metadata.py -l $B/v2_capi_shim.so -n v2_capi_shim -o $B/v2_capi_shim.duckdb_extension -p linux_arm64 -dv "$V" -ev "v2.0.1-on-$V" --abi-type C_STRUCT_UNSTABLE >/dev/null
echo "target: $('"$S"'/build-main/duckdb -csv -noheader -c "SELECT library_version || chr(32) || source_id FROM pragma_version()")  bridge: $(stat -c %s $B/v2_capi_shim.duckdb_extension) bytes"
echo "== directly:"
'"$S"'/build-main/duckdb -unsigned -c "LOAD '"'$EXT'"';" 2>&1 | head -2 || true
echo "== through the bridge:"
V2_CAPI_SHIM_LOAD='"$EXT"' '"$S"'/build-main/duckdb -unsigned -csv -cmd "LOAD '"'"'$B/v2_capi_shim.duckdb_extension'"'"';" -f '"$S"'/rusty_queries.sql > '"$S"'/actual_main.csv 2>&1
cat '"$S"'/actual_main.csv
'
echo "== diff against v2.0.1 natively (warning lines aside):"
diff <(grep -v -e 'WARNING' -e deprecated -e '^\s*$' $S/expected_v201.csv | sed 's/^\x1b\[00m//') <(grep -v -e '^\s*$' $S/actual_main.csv) && echo IDENTICAL
