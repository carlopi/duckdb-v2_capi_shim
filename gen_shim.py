#!/usr/bin/env python3
"""Generates the compat adapters for one (contract, target) pair from clang's AST of both duckdb_v2.h.

Reads ast_old.json (contract) and ast_new.json (target) in the current directory, and optionally
defaults.json: {"<function>": {"<new parameter>": "<C expression>"}} for parameters the target added, with the value
that keeps the contract's behaviour. Writes shim.c: one compat_<function> per contract function, with the contract's
signature, implemented on the target's functions. Types the target changed or dropped are declared under compat_
names; everything else is the target's own (identical) type. Functions needing hand-written adapters are listed and
left to compat_manual.c."""
import json, os, re

def load(p):
    fns, typedefs, records = {}, {}, {}
    def walk(n):
        k, name = n.get('kind'), n.get('name', '')
        if k == 'FunctionDecl' and name.startswith('duckdb_v2'):
            ps = [(c.get('name', ''), c['type']['qualType']) for c in n.get('inner', []) if c.get('kind') == 'ParmVarDecl']
            fns[name] = (n['type']['qualType'].split('(')[0].strip(), ps)
        elif k == 'TypedefDecl' and name.lower().startswith('duckdb_v2'):
            typedefs[name] = n['type']['qualType']
        elif k == 'RecordDecl' and n.get('completeDefinition') and name.startswith('duckdb_v2'):
            # anonymous members are named after their source location: compare their shape only
            records[name] = [(c.get('name'), re.sub(r'\(unnamed (union|struct) at [^)]*\)', r'(\1)', c['type']['qualType']))
                             for c in n.get('inner', []) if c.get('kind') == 'FieldDecl']
        for c in n.get('inner', []):
            walk(c)
    walk(json.load(open(p)))
    return fns, typedefs, records

old, old_td, old_rec = load('ast_old.json')
new, new_td, new_rec = load('ast_new.json')
defaults = json.load(open('defaults.json')) if os.path.exists('defaults.json') else {}

# the contract's string struct, if the target no longer has it (or has it differently)
STR_TYPES = [t for t in ('duckdb_v2_str', 'duckdb_v2_identifier_t') if t in old_td]
str_gone = 'duckdb_v2_str' in old_rec and old_rec.get('duckdb_v2_str') != new_rec.get('duckdb_v2_str')
# callback typedefs whose definition changed: the contract's spelled as compat_
changed_cb = {t for t in old_td if t in new_td and old_td[t] != new_td[t] and '(*)' in old_td[t]}

def oldtype(t):
    for cb in changed_cb:
        t = re.sub(r'\b%s\b' % cb, 'compat_' + cb[len('duckdb_'):], t)
    if str_gone:
        t = re.sub(r'\bduckdb_v2_(identifier_t|str)\b', 'compat_v2_str', t)
    return t

STR_IN = tuple('const %s *' % s for s in STR_TYPES)
STR_OUT = tuple('%s *' % s for s in STR_TYPES)
SYNC_CB = {'duckdb_v2_text_sink_fn': 'compat_text_sink_tramp',
           'duckdb_v2_file_system_path_glob_callback_fn': 'compat_glob_tramp'}
MANUAL = {'duckdb_v2_log_sink_set_write_callback', 'duckdb_v2_log_sink_set_flush_callback',
          'duckdb_v2_log_sink_set_user_data'} if 'duckdb_v2_log_sink_write_callback_fn' in changed_cb else set()

stats = {'forward': 0, 'generated': 0, 'manual': 0}
manual, unmatched, out = [], [], []
for name, (ret, ops) in old.items():
    if name not in new:
        unmatched.append((name, 'removed in the target'))
        continue
    nret, nps = new[name]
    sig = "%s compat_%s(%s)" % (oldtype(ret), name,
                                ', '.join(oldtype(t) + ' ' + (n or 'a%d' % i) for i, (n, t) in enumerate(ops)) or 'void')
    if name in MANUAL:
        manual.append(name)
        continue
    if [oldtype(t) for _, t in ops] == [t for _, t in nps] and ret == nret:
        stats['forward'] += 1
        if ret == 'DUCKDB_V2_ERROR':
            out.append("%s { DUCKDB_V2_ERROR r_ = %s(%s); COMPAT_TRACE(\"%s\", r_); return r_; }" % (sig, name, ', '.join(n for n, _ in ops), name))
        else:
            out.append("%s { %s%s(%s); }" % (sig, 'return ' if ret != 'void' else '', name, ', '.join(n for n, _ in ops)))
        continue
    pre, post, args = [], [], []
    i = j = 0
    ok = True
    while ok and (i < len(ops) or j < len(nps)):
        if i >= len(ops):  # target parameters past the contract's: only with a declared default
            pname = nps[j][0]
            if pname in defaults.get(name, {}):
                args.append(defaults[name][pname]); j += 1; continue
            ok = False; break
        n, t = ops[i]
        nt = nps[j][1] if j < len(nps) else None
        nname = nps[j][0] if j < len(nps) else None
        if nname and nname != n and nname in defaults.get(name, {}):  # a parameter the target inserted
            args.append(defaults[name][nname]); j += 1; continue
        if t == 'const char *' and nt == 'const char *' and j + 1 < len(nps) and nps[j + 1][1] == 'idx_t' \
                and (i + 1 >= len(ops) or ops[i + 1][1] != 'idx_t'):
            args += [n, "%s ? strlen(%s) : 0" % (n, n)]; i += 1; j += 2; continue
        if t in SYNC_CB and t in changed_cb and i + 1 < len(ops) and ops[i + 1][1] == 'void *':
            pre.append("compat_cb_ctx %s_c = { (void (*)(void))(%s), %s };" % (n, n, ops[i + 1][0]))
            args += [SYNC_CB[t], "&%s_c" % n]; i += 2; j += 2; continue
        if oldtype(t) == nt:
            args.append(n); i += 1; j += 1; continue
        if str_gone and t in STR_IN + STR_OUT and nt == 'const char *' and j + 1 < len(nps) and nps[j + 1][1] == 'idx_t':
            args += ["%s ? %s->ptr : NULL" % (n, n), "%s ? %s->len : 0" % (n, n)]; i += 1; j += 2; continue
        if str_gone and t in STR_OUT and nt == 'const char **' and nps[j + 1][1] == 'idx_t *':
            pre.append("const char *%s_p = NULL; idx_t %s_l = 0;" % (n, n))
            args += ["%s ? &%s_p : NULL" % (n, n), "%s ? &%s_l : NULL" % (n, n)]
            post.append("if (%s) { %s->ptr = %s_p; %s->len = %s_l; }" % (n, n, n, n, n)); i += 1; j += 2; continue
        if str_gone and t in STR_IN and nt == 'const char *const *' and nps[j + 1][1] == 'const idx_t *':
            cnt = next((m for m, u in ops[i + 1:] if u == 'idx_t'), None)
            if not cnt:
                ok = False; break
            pre.append("COMPAT_UNPACK(%s, %s);" % (n, cnt)); args += ["%s_p" % n, "%s_l" % n]
            post.append("COMPAT_FREE(%s);" % n); i += 1; j += 2; continue
        if t == 'uint8_t' and nt == 'uint32_t':
            args.append("(uint32_t)%s" % n); i += 1; j += 1; continue
        if t == 'uint8_t *' and nt == 'uint32_t *':
            pre.append("uint32_t %s_w = 0;" % n); args.append("%s ? &%s_w : NULL" % (n, n))
            post.append("if (%s) *%s = (uint8_t)%s_w;" % (n, n, n)); i += 1; j += 1; continue
        if nt == 'const ' + t:
            args.append(n); i += 1; j += 1; continue
        ok = False
    if not ok or ret != nret:
        unmatched.append((name, 'no rule matches'))
        continue
    stats['generated'] += 1
    r = "%s r_ = " % ret if ret != 'void' else ''
    trace = ' COMPAT_TRACE("%s", r_);' % name if ret == 'DUCKDB_V2_ERROR' else ''
    out.append("%s { %s %s%s(%s); %s%s %s }" % (sig, ' '.join(pre), r, name, ', '.join(args), ' '.join(post), trace,
                                                'return r_;' if ret != 'void' else ''))
stats['manual'] = len(manual)

hdr = ['#include <stdlib.h>', '#include <string.h>', '#include "duckdb_v2.h"',
       '/* -DCOMPAT_TRACE: every call that returns an error is reported on stderr */',
       '#ifdef COMPAT_TRACE', '#include <stdio.h>',
       '#undef COMPAT_TRACE', '#define COMPAT_TRACE(f, r) do { if (r) fprintf(stderr, "compat: %s -> %d\\n", f, (int)(r)); } while (0)',
       '#else', '#define COMPAT_TRACE(f, r) (void)0', '#endif']
if str_gone:
    hdr += ['/* the contract\'s string struct, which the target no longer has */',
            'typedef struct compat_v2_str { const char *ptr; idx_t len; } compat_v2_str;']
for cb in sorted(changed_cb):
    # the contract's callback type, its string parameters spelled with the contract's struct
    proto = oldtype(old_td[cb]).replace('(*)', '(*compat_%s)' % cb[len('duckdb_'):], 1)
    hdr.append('typedef %s;' % proto)
hdr += ['typedef struct compat_cb_ctx { void (*cb)(void); void *ud; } compat_cb_ctx;']
if 'duckdb_v2_text_sink_fn' in changed_cb:
    hdr.append('static void compat_text_sink_tramp(const char *p, idx_t l, void *c, duckdb_v2_error_info_handle *e) { compat_v2_str s = {p, l}; ((compat_v2_text_sink_fn)((compat_cb_ctx *)c)->cb)(&s, ((compat_cb_ctx *)c)->ud, e); }')
if 'duckdb_v2_file_system_path_glob_callback_fn' in changed_cb:
    hdr.append('static void compat_glob_tramp(const char *p, idx_t l, void *c, duckdb_v2_error_info_handle *e) { compat_v2_str s = {p, l}; ((compat_v2_file_system_path_glob_callback_fn)((compat_cb_ctx *)c)->cb)(&s, ((compat_cb_ctx *)c)->ud, e); }')
hdr += ['#define COMPAT_UNPACK(a, n) const char **a##_p = (const char **)malloc(sizeof(char *) * ((n) + 1)); idx_t *a##_l = (idx_t *)malloc(sizeof(idx_t) * ((n) + 1)); for (idx_t k_ = 0; a && k_ < (n); k_++) { a##_p[k_] = a[k_].ptr; a##_l[k_] = a[k_].len; }',
        '#define COMPAT_FREE(a) free((void *)a##_p); free(a##_l);']
open('shim.c', 'w').write('\n'.join(hdr) + '\n' + '\n'.join(out) + '\n')
open('manual.txt', 'w').write('\n'.join(manual) + ('\n' if manual else ''))
print(stats)
print('hand-written:', manual)
print('unmatched:', unmatched)
