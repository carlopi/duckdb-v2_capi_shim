#!/usr/bin/env bash
# The shim's own test, against a DuckDB built with it: a v2 extension built against the contract (tests/child.c,
# compiled with contracts/<version>/ and stamped for that version) loaded through the shim, plus what the shim says
# about itself and what it refuses.
#   tests/run.sh <duckdb binary> <v2_capi_shim.duckdb_extension>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DUCKDB="$1"; SHIM="$2"; CONTRACT="${CONTRACT:-v2.0.1}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
CC="${CC:-cc}"
PLATFORM="$("$DUCKDB" -csv -noheader -c "PRAGMA platform")"
case "$(uname)" in Darwin) SHARED="-dynamiclib" ;; *) SHARED="-shared" ;; esac
"$CC" -fPIC $SHARED -std=c11 -DDUCKDB_V2_API_ALLOW_UNSTABLE=1 -Wall -Werror -I "$ROOT/contracts/$CONTRACT" \
	"$ROOT/tests/child.c" -o "$W/child.raw"
mkdir -p "$W/stamped"
python3 "$ROOT/extension-ci-tools/scripts/append_extension_metadata.py" -l "$W/child.raw" -n child -o "$W/stamped/child.duckdb_extension" \
	-p "$PLATFORM" -dv "$CONTRACT" -ev v0.0.1 --abi-type C_STRUCT_UNSTABLE > /dev/null
cp "$W/child.raw" "$W/unstamped.duckdb_extension"
fail=0
check() { # check <name> <expected output> <sql>
	local got
	got="$("$DUCKDB" -csv -noheader -unsigned -c "LOAD '$SHIM'; $3" 2>&1 | tr '\n' '|' || true)"
	if [ "$got" = "$2" ]; then echo "ok   $1"; else echo "FAIL $1"; echo "     expected: $2"; echo "     got:      $got"; fail=1; fi
}
check "info: contract and platform" "$CONTRACT|$PLATFORM|" \
	"SELECT contract FROM v2_capi_shim_info(); SELECT platform FROM v2_capi_shim_info();"
check "capabilities" "connections|interrupts|" "SELECT name FROM v2_capi_shim_capabilities() ORDER BY name;"
check "a $CONTRACT extension loads and runs" "child|\"hello ci, from an extension built for v2.0.1\"|d|u|c|k|" \
	"SELECT * FROM load_extension('$W/stamped/child.duckdb_extension'); SELECT child_greet('ci'); SELECT * FROM child_letters('duck');"
check "it is listed as loaded, with its own version" "child|child,v0.0.1|" \
	"SELECT * FROM load_extension('$W/stamped/child.duckdb_extension'); SELECT extension_name, extension_version FROM duckdb_extensions() WHERE extension_name = 'child';"
got="$("$DUCKDB" -csv -noheader -unsigned -c "LOAD '$SHIM'; SET allow_unsigned_extensions = false; SELECT * FROM load_extension('$W/stamped/child.duckdb_extension', repo := 'community');" 2>&1 || true)"
case "$got" in *"not signed by repository 'community'"*) echo "ok   an unsigned file is refused for repo community" ;; *) echo "FAIL unsigned refusal: $got"; fail=1 ;; esac
got="$("$DUCKDB" -csv -noheader -unsigned -c "LOAD '$SHIM'; SELECT * FROM load_extension('$W/unstamped.duckdb_extension');" 2>&1 || true)"
case "$got" in *"no DuckDB extension footer"*) echo "ok   a file without a footer is refused" ;; *) echo "FAIL footer refusal: $got"; fail=1 ;; esac
got="$("$DUCKDB" -csv -noheader -unsigned -c "LOAD '$W/stamped/child.duckdb_extension';" 2>&1 || true)"
case "$("$DUCKDB" -csv -noheader -c "SELECT library_version FROM pragma_version()")" in
	"$CONTRACT") echo "ok   (the target is the contract: the extension also loads natively)" ;;
	*) case "$got" in *"built specifically for DuckDB version"*) echo "ok   loaded directly, the target refuses it" ;; *) echo "FAIL direct load: $got"; fail=1 ;; esac ;;
esac
exit $fail
