#!/usr/bin/env bash
# Spike: a compat layer serving the v2 C API of the contract (the duckdb submodule, v2.0.1) on top of the v2 C API of a
# target DuckDB (default: upstream main). Generates the adapters, compiles them against the target's headers with type
# checking enforced, builds the contract-layout function table against the contract's headers, and links the two.
# Usage: tools/capi-compat/run.sh [target-git-ref]   (output in .cache/capi-compat/)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLS="$ROOT/tools/capi-compat"
OUT="$ROOT/.cache/capi-compat"
REF="${1:-main}"
CONTRACT="$ROOT/duckdb/src/include"
mkdir -p "$OUT" && cd "$OUT"
if [ ! -d target ]; then
	git clone -q --depth 1 --branch "$REF" --filter=blob:none --sparse https://github.com/duckdb/duckdb target
	git -C target sparse-checkout set --no-cone /src/include/duckdb_v2.h /src/include/duckdb_extension_v2.h /api_spec/v2
fi
TARGET="$OUT/target/src/include"
echo "contract: $(git -C "$ROOT/duckdb" describe --tags 2>/dev/null || git -C "$ROOT/duckdb" rev-parse --short HEAD)  target: $REF $(git -C target rev-parse --short HEAD)"
echo '#include "duckdb_v2.h"' > probe.c
clang -fsyntax-only -DDUCKDB_V2_API_ALLOW_UNSTABLE=1 -Xclang -ast-dump=json -I "$CONTRACT" probe.c > ast_old.json
clang -fsyntax-only -DDUCKDB_V2_API_ALLOW_UNSTABLE=1 -Xclang -ast-dump=json -I "$TARGET" probe.c > ast_new.json
python3 "$TOOLS/surface.py" ast_old.json ast_new.json
cp "$TOOLS/compat_manual.h" .
python3 "$TOOLS/gen_shim.py"
F=(-std=c11 -DDUCKDB_V2_API_ALLOW_UNSTABLE=1 -Wall -Werror -Wincompatible-pointer-types -Wconversion -Wno-sign-conversion -Wno-unused-function)
clang -c "${F[@]}" -I "$TARGET" -I . shim_v201_on_main.c -o shim.o
clang -c "${F[@]}" -I "$TARGET" "$TOOLS/compat_manual.c" -o manual.o
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
ld -r table.o shim.o manual.o -o compat.o
echo "slots: $(wc -l < slots.txt)  unresolved adapters: $(nm -u compat.o | grep -c ' _\?compat_' || true)  target imports: $(nm -u compat.o | grep -c duckdb_v2_)  other imports: $(nm -u compat.o | grep -v duckdb_v2_ | tr -d ' ' | tr '\n' ' ')"
ls -la compat.o
