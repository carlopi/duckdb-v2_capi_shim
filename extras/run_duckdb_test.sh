#!/usr/bin/env bash
# DuckDB's own v2 extension test (test/extension/load_extension_capi_v2.test, the target's copy) with the extensions
# DuckDB built at v2.0.1 (the submodule's build/release/test/extension) loaded through the loader. Needs the target
# built with BUILD_UNITTESTS=1 and tools/capi-compat/extras/build.sh run.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)" && cd "$ROOT"
E=$ROOT/build/release/test/extension; B=$ROOT/.cache/capi-compat/extras; M="${TARGET_SRC:-.cache/duckdb-main}"
mkdir -p "$M/test/capi_compat"
sed -e "s#LOAD '{BUILD_DIRECTORY}/test/extension/\(cpp_api_demo[a-z_]*\).duckdb_extension';#LOAD '$B/v2_capi_shim_v201.duckdb_extension';\n\nstatement ok\nSELECT * FROM load_extension('$E/\1.duckdb_extension');#" \
	"$M/test/extension/load_extension_capi_v2.test" > "$M/test/capi_compat/via_bridge.test"
python3 - "$M/test/capi_compat/via_bridge.test" <<'PY'
import sys, re
p = sys.argv[1]; s = open(p).read()
# the failing load: the error now comes from load_extension, after the loader loaded fine
s = re.sub(r"statement error\nLOAD '([^']*v2_capi_shim_v201[^']*)';\n\nstatement ok\n(SELECT \* FROM load_extension\('[^']*cpp_api_demo_fail[^']*'\));",
           r"statement ok\nLOAD '\1';\n\nstatement error\n\2;", s)
open(p, 'w').write(s)
PY
cd "$M" && ./build/release/test/unittest test/capi_compat/via_bridge.test
