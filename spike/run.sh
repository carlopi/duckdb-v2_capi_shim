#!/usr/bin/env bash
# Generates the compat layer for one pair: serves the v2 C API of a contract (contracts/<version>, default the
# loader's: v2.0.1) on top of the v2 C API of a target DuckDB (TARGET_SRC, a DuckDB source tree). Diffs the two
# surfaces from clang's AST, generates an adapter per contract function, compiles them against the target's headers
# with type checking enforced, builds the contract-layout function table against the contract's headers, links them.
# Usage: TARGET_SRC=/path/to/duckdb ./run.sh   (output in $OUT, default .cache/gen)
#   CONTRACT=v2.0.0 for another contract; DEFAULTS=pairs/<file>.json for values of parameters the target added
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
TOOLS="$ROOT"
OUT="${OUT:-$ROOT/.cache/gen}"
CONTRACT_NAME="${CONTRACT:-v2.0.1}"
CONTRACT="$ROOT/contracts/$CONTRACT_NAME"
[ -f "$CONTRACT/duckdb_v2.h" ] || { echo "no contract $CONTRACT_NAME in contracts/" >&2; exit 1; }
: "${TARGET_SRC:?set TARGET_SRC to a DuckDB source tree (the target)}"
mkdir -p "$OUT" && cd "$OUT"
rm -rf target && ln -s "$(cd "$TARGET_SRC" && pwd -P)" target
TARGET="$OUT/target/src/include"
echo "contract: $CONTRACT_NAME  target: $(git -C target/ describe --tags 2>/dev/null || git -C target/ rev-parse --short HEAD 2>/dev/null || echo "$TARGET_SRC")"
echo '#include "duckdb_v2.h"' > probe.c
clang -fsyntax-only -DDUCKDB_V2_API_ALLOW_UNSTABLE=1 -Xclang -ast-dump=json -I "$CONTRACT" probe.c > ast_old.json
clang -fsyntax-only -DDUCKDB_V2_API_ALLOW_UNSTABLE=1 -Xclang -ast-dump=json -I "$TARGET" probe.c > ast_new.json
python3 "$TOOLS/surface.py" ast_old.json ast_new.json
[ -n "${DEFAULTS:-}" ] && cp "$DEFAULTS" defaults.json || rm -f defaults.json
python3 "$TOOLS/gen_shim.py"
F=(-std=c11 -DDUCKDB_V2_API_ALLOW_UNSTABLE=1 -Wall -Werror -Wincompatible-pointer-types -Wconversion -Wno-sign-conversion -Wno-unused-function)
clang -c "${F[@]}" -I "$TARGET" -I . shim.c -o shim.o
MANUAL_O=; if [ -s manual.txt ]; then clang -c "${F[@]}" -I "$TARGET" "$TOOLS/compat_manual.c" -o manual.o; MANUAL_O=manual.o; fi
sed -n '/^typedef struct {/,/} duckdb_ext_api_v2;/p' "$CONTRACT/duckdb_extension_v2.h" | grep -oE '\(\*[a-z0-9_]+\)' | tr -d '(*)' > slots.txt
{
	echo '#include "duckdb_extension_v2.h"'
	echo '#define S(f) extern __typeof__(*((duckdb_ext_api_v2 *)0)->f) compat_##f;'
	while read -r s; do echo "S($s)"; done < slots.txt
	echo 'const duckdb_ext_api_v2 compat_table = {'
	while read -r s; do echo "  .$s = compat_$s,"; done < slots.txt
	echo '};'
} > compat_table.c
clang -c -std=c11 -DDUCKDB_BUILD_STATIC_EXTENSION -DDUCKDB_V2_API_ALLOW_UNSTABLE=1 -Wall -Werror -I "$CONTRACT" compat_table.c -o table.o
ld -r table.o shim.o $MANUAL_O -o compat.o
echo "slots: $(wc -l < slots.txt)  unresolved adapters: $(nm -u compat.o | grep -c ' _\?compat_' || true)  target imports: $(nm -u compat.o | grep -c duckdb_v2_)  other imports: $(nm -u compat.o | grep -v duckdb_v2_ | tr -d ' ' | tr '\n' ' ')"
ls -la compat.o
