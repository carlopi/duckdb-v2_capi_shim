#!/usr/bin/env python3
"""The build-time generator: everything the loader needs for one (contract, target) pair, with nothing but Python.

    generate.py --contract contracts/v2.0.1 --target <duckdb>/src/include --out <dir> [--defaults pairs/x.json]

writes into <dir>:
    shim.c          one compat_<function> per contract function, implemented on the target's (gen_shim.py)
    compat_table.c  the contract-layout duckdb_ext_api_v2, filled with them (compiled against the contract's headers)
    shim_target.h   what differs in the target itself that the loader's own code must know
    manual.txt      the contract functions left to compat_manual.c (empty: compat_manual.c is not needed)
Exits non-zero, saying which, when a contract function matches no rule: that pair needs a rule or an adapter."""
import argparse, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import parse_header  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('--contract', required=True)
ap.add_argument('--target', required=True, help='the target DuckDB include directory (with duckdb_v2.h)')
ap.add_argument('--out', required=True)
ap.add_argument('--defaults')
a = ap.parse_args()
for k in ("contract", "target", "out", "defaults"):
    if getattr(a, k):
        setattr(a, k, os.path.abspath(getattr(a, k)))
os.makedirs(a.out, exist_ok=True)
for stale in ('ast_old.json', 'ast_new.json', 'defaults.json'):
    if os.path.exists(os.path.join(a.out, stale)):
        os.remove(os.path.join(a.out, stale))
if a.defaults:
    shutil.copy(a.defaults, os.path.join(a.out, 'defaults.json'))
env = dict(os.environ, CONTRACT_HEADER=os.path.join(a.contract, 'duckdb_v2.h'),
           TARGET_HEADER=os.path.join(a.target, 'duckdb_v2.h'))
r = subprocess.run([sys.executable, os.path.join(HERE, 'gen_shim.py')], cwd=a.out, env=env, capture_output=True, text=True)
sys.stdout.write(r.stdout)
if r.returncode != 0 or "unmatched: []" not in r.stdout:
    sys.stderr.write(r.stderr + "v2_capi_shim: some contract functions match no rule for this target (see above)\n")
    sys.exit(1)

# the contract's function table, slot by slot, filled with the adapters
ext = open(os.path.join(a.contract, 'duckdb_extension_v2.h')).read()
m = re.search(r'typedef struct \{(.*?)\} duckdb_ext_api_v2;', ext, re.S)
slots = re.findall(r'\(\*(\w+)\)', m.group(1))
with open(os.path.join(a.out, 'compat_table.c'), 'w') as f:
    f.write('/* generated: the contract-layout function table, compiled against the contract\'s headers */\n')
    f.write('#include "duckdb_extension_v2.h"\n')
    f.write('#define S(f) extern __typeof__(*((duckdb_ext_api_v2 *)0)->f) compat_##f;\n')
    f.writelines('S(%s)\n' % s for s in slots)
    f.write('const duckdb_ext_api_v2 compat_table = {\n')
    f.writelines('    .%s = compat_%s,\n' % (s, s) for s in slots)
    f.write('};\n')

# the target's own flavour, for the loader's code
_, target_td, _ = parse_header.parse(os.path.join(a.target, 'duckdb_v2.h'))
get_api = target_td.get('duckdb_v2_extension_get_api_fn', '')
with open(os.path.join(a.out, 'shim_target.h'), 'w') as f:
    f.write('/* generated: what the loader must know about the target */\n#pragma once\n')
    f.write('#define SHIM_TARGET_GET_API_HAS_LENGTH %d\n' % (1 if 'idx_t' in get_api else 0))
print('slots: %d' % len(slots))
