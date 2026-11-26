#!/usr/bin/env bash
# The v2.0.0 -> v2.0.1 bridge, in Docker: community extensions built for v2.0.0 (fakeit, units) but published under
# v2.0.1, on the official duckdb/duckdb:2.0.1 image, natively (control) and through the bridge. The extensions are
# installed by that image (signature checked); third-party binaries only run inside containers.
set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)" && cd "$ROOT"
S=.cache/docker-spike; O=.cache/capi-compat-v200-v201; B=$S/bridge-v200
mkdir -p "$B"
docker run --rm -v "$ROOT":/w -w /w debian:bookworm-slim bash -c '
set -e
apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends gcc libc6-dev python3 >/dev/null 2>&1
O='"$O"'; B='"$B"'; T=duckdb/src/include; C=.cache/duckdb-v200/src/include; U=-DDUCKDB_V2_API_ALLOW_UNSTABLE=1
gcc -fPIC -c -std=c11 $U -Wall -Werror -Wno-unused-function -I $T -I $O -include tools/capi-compat/runtime/target_prelude.h $O/shim.c -o $B/shim.o
gcc -fPIC -c -std=c11 $U -DDUCKDB_BUILD_STATIC_EXTENSION -Wall -Werror -I $C $O/compat_table.c -o $B/table.o
gcc -fPIC -c -std=gnu11 $U -DSHIM_TARGET_STR_STRUCT -DDUCKDB_EXTENSION_NAME=v2_capi_shim -DSHIM_CONTRACT_VERSION=\"v2.0.0\" -DSHIM_TARGET_VERSION=\"v2.0.1\" -DSHIM_TARGET_SOURCE_ID=\"v2.0.1\" -DSHIM_PLATFORM=\"linux_arm64\" -Wall -Werror -I $T tools/capi-compat/runtime/shim_entry.c -o $B/entry.o
gcc -shared -o $B/v2_capi_shim.so $B/entry.o $B/shim.o $B/table.o -ldl
python3 extension-ci-tools/scripts/append_extension_metadata.py -l $B/v2_capi_shim.so -n v2_capi_shim -o $B/v2_capi_shim.duckdb_extension -p linux_arm64 -dv v2.0.1 -ev v2.0.0-on-v2.0.1 --abi-type C_STRUCT_UNSTABLE >/dev/null
echo "bridge v2.0.0 -> v2.0.1: $(stat -c %s $B/v2_capi_shim.duckdb_extension) bytes"
'
# The extensions: the v2.0.0 builds, installed from community by the official 2.0.0 image (signature checked). The
# copies published under the v2.0.1 path are different builds of the same versions, which 2.0.1 refuses to INSTALL
H=/w/$S/v200-home; X=$H/extensions/v2.0.0/linux_arm64
docker run --rm -v "$ROOT":/w -e HOME=$H --entrypoint /duckdb duckdb/duckdb:2.0.0 -no-agent -csv -c "SET extension_directory='$H/extensions'; INSTALL fakeit FROM community; INSTALL units FROM community;" >/dev/null 2>&1
for e in units fakeit; do
	docker run --rm -v "$ROOT":/w -e HOME=$H --entrypoint /duckdb duckdb/duckdb:2.0.0 -no-agent -csv -cmd "SET extension_directory='$H/extensions'; LOAD $e;" -f /w/$S/${e}_queries.sql 2>&1 | grep -v -e WARNING -e "'extension_directory' setting" -e '^.\[00m$' | sed 's/^\x1b\[00m//' | sed '/^$/d' > $S/${e}_v200_native.out || true
	echo "== $e on 2.0.1, directly:"
	docker run --rm -v "$ROOT":/w --entrypoint /duckdb duckdb/duckdb:2.0.1 -no-agent -unsigned -c "LOAD '$X/$e.duckdb_extension';" 2>&1 | head -1 || true
	docker run --rm -v "$ROOT":/w -e V2_CAPI_SHIM_LOAD=$X/$e.duckdb_extension --entrypoint /duckdb duckdb/duckdb:2.0.1 -no-agent -csv -unsigned -cmd "LOAD '/w/$B/v2_capi_shim.duckdb_extension';" -f /w/$S/${e}_queries.sql > $S/${e}_v201_bridge.out 2>&1 || true
	echo "== $e on 2.0.1 through the bridge vs on 2.0.0 natively:"
	diff $S/${e}_v200_native.out $S/${e}_v201_bridge.out && echo IDENTICAL
done
