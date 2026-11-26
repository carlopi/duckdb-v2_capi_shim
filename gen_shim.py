import json,re,sys
def fns(p):
    out={}
    def walk(n):
        if n.get('kind')=='FunctionDecl' and n.get('name','').startswith('duckdb_v2'):
            ps=[(c.get('name',''),c['type']['qualType']) for c in n.get('inner',[]) if c.get('kind')=='ParmVarDecl']
            ret=n['type']['qualType'].split('(')[0].strip()
            out[n['name']]=(ret,ps)
        for c in n.get('inner',[]): walk(c)
    walk(json.load(open(p))); return out
old=fns('ast_old.json'); new=fns('ast_new.json')
STR_IN=('const duckdb_v2_str *','const duckdb_v2_identifier_t *')
STR_OUT=('duckdb_v2_str *','duckdb_v2_identifier_t *')
OLD_CB={'duckdb_v2_text_sink_fn','duckdb_v2_file_system_path_glob_callback_fn','duckdb_v2_log_sink_write_callback_fn','duckdb_v2_extension_get_api_fn'}
def ctype(t): return t.replace('duckdb_v2_identifier_t','compat_v2_str').replace('duckdb_v2_str','compat_v2_str')
def oldtype(t):
    for cb in OLD_CB: t=t.replace(cb,'compat_'+cb[len('duckdb_'):])
    return ctype(t)
stats={'forward':0,'generated':0,'manual':0}; manual=[]; out=[]
for name,(ret,ops) in old.items():
    if name not in new: manual.append((name,'removed')); continue
    nret,nps=new[name]
    sig=f"{oldtype(ret)} compat_{name}({', '.join(oldtype(t)+' '+(n or f'a{i}') for i,(n,t) in enumerate(ops)) or 'void'})"
    if name in ('duckdb_v2_log_sink_set_write_callback','duckdb_v2_log_sink_set_flush_callback','duckdb_v2_log_sink_set_user_data'):
        manual.append((name,'hand-written: compat_manual.c')); continue
    if [t for _,t in ops]==[t for _,t in nps] and ret==nret:
        stats['forward']+=1
        out.append(f"{sig} {{ {'return ' if ret!='void' else ''}{name}({', '.join(n for n,_ in ops)}); }}"); continue
    pre=[];post=[];args=[];j=0;ok=True
    for i,(n,t) in enumerate(ops):
        if j>=len(nps): ok=False;break
        nt=nps[j][1]
        SYNC_CB={'duckdb_v2_text_sink_fn':'compat_text_sink_tramp','duckdb_v2_file_system_path_glob_callback_fn':'compat_glob_tramp'}
        if t=='const char *' and nt=='const char *' and j+1<len(nps) and nps[j+1][1]=='idx_t' and (i+1>=len(ops) or ops[i+1][1]!='idx_t'):
            args += [n, f"{n} ? strlen({n}) : 0"]; j+=2; continue
        if t in SYNC_CB and nt==t and i+1<len(ops) and ops[i+1][1]=='void *':
            pre.append(f"compat_cb_ctx {n}_c = {{ (void (*)(void))({n}), {ops[i+1][0]} }};")
            args += [SYNC_CB[t], f"&{n}_c"]; j+=2; skip_next=True; continue
        if 'skip_next' in dir() and skip_next: skip_next=False; continue
        if t==nt: args.append(n); j+=1
        elif t in STR_IN+STR_OUT and nt=='const char *' and j+1<len(nps) and nps[j+1][1]=='idx_t':
            args += [f"{n} ? {n}->ptr : NULL", f"{n} ? {n}->len : 0"]; j+=2
        elif t in STR_OUT and nt=='const char **' and nps[j+1][1]=='idx_t *':
            pre.append(f"const char *{n}_p = NULL; idx_t {n}_l = 0;"); args += [f"{n} ? &{n}_p : NULL", f"{n} ? &{n}_l : NULL"]
            post.append(f"if ({n}) {{ {n}->ptr = {n}_p; {n}->len = {n}_l; }}"); j+=2
        elif t in STR_IN and nt=='const char *const *' and nps[j+1][1]=='const idx_t *':
            # array: the count is the next idx_t parameter of the old signature
            cnt=next((m for m,u in ops[i+1:] if u=='idx_t'),None)
            if not cnt: ok=False;break
            pre.append(f"COMPAT_UNPACK({n}, {cnt});"); args += [f"{n}_p", f"{n}_l"]; post.append(f"COMPAT_FREE({n});"); j+=2
        elif t=='const char *' and nt=='const char *' and j+1<len(nps) and nps[j+1][1]=='idx_t' and (len(ops)<=i+1 or ops[i+1][1]!='idx_t'):
            args += [n, f"{n} ? strlen({n}) : 0"]; j+=2
        elif t in ('uint8_t',) and nt=='uint32_t': args.append(f"(uint32_t){n}"); j+=1
        elif t=='uint8_t *' and nt=='uint32_t *':
            pre.append(f"uint32_t {n}_w = 0;"); args.append(f"{n} ? &{n}_w : NULL"); post.append(f"if ({n}) *{n} = (uint8_t){n}_w;"); j+=1
        elif nt=='const '+t: args.append(n); j+=1
        else: ok=False;break
    if name in ('duckdb_v2_log_sink_set_write_callback','duckdb_v2_log_sink_set_flush_callback','duckdb_v2_log_sink_set_user_data'): manual.append((name,'stateful: hand-written')); continue
    if not ok or j!=len(nps) or ret!=nret:
        manual.append((name,'unmatched')); continue
    stats['generated']+=1
    r = f"{ret} r_ = " if ret!='void' else ''
    out.append(f"{sig} {{ {' '.join(pre)} {r}{name}({', '.join(args)}); {' '.join(post)} {'return r_;' if ret!='void' else ''} }}")
stats['manual']=len(manual)
hdr='''#include "compat_manual.h"
#include <stdlib.h>
#include <string.h>
#include "duckdb_v2.h"
/* the v2.0.1 contract types that main no longer has */
typedef struct compat_v2_str { const char *ptr; idx_t len; } compat_v2_str;
typedef void (*compat_v2_text_sink_fn)(const compat_v2_str *, void *, duckdb_v2_error_info_handle *);
typedef void (*compat_v2_file_system_path_glob_callback_fn)(const compat_v2_str *, void *, duckdb_v2_error_info_handle *);
typedef void (*compat_v2_log_sink_write_callback_fn)(duckdb_v2_log_sink_write_info_handle, void *, int64_t, DUCKDB_V2_LOG_LEVEL, DUCKDB_V2_LOG_SCOPE, const compat_v2_str *, const compat_v2_str *, duckdb_v2_error_info_handle *);
typedef struct compat_cb_ctx { void (*cb)(void); void *ud; } compat_cb_ctx;
static void compat_text_sink_tramp(const char *p, idx_t l, void *c, duckdb_v2_error_info_handle *e) { compat_v2_str s = {p, l}; ((compat_v2_text_sink_fn)((compat_cb_ctx *)c)->cb)(&s, ((compat_cb_ctx *)c)->ud, e); }
static void compat_glob_tramp(const char *p, idx_t l, void *c, duckdb_v2_error_info_handle *e) { compat_v2_str s = {p, l}; ((compat_v2_file_system_path_glob_callback_fn)((compat_cb_ctx *)c)->cb)(&s, ((compat_cb_ctx *)c)->ud, e); }
#define COMPAT_UNPACK(a, n) const char **a##_p = (const char **)malloc(sizeof(char *) * ((n) + 1)); idx_t *a##_l = (idx_t *)malloc(sizeof(idx_t) * ((n) + 1)); for (idx_t k_ = 0; a && k_ < (n); k_++) { a##_p[k_] = a[k_].ptr; a##_l[k_] = a[k_].len; }
#define COMPAT_FREE(a) free((void *)a##_p); free(a##_l);
'''
open('shim_v201_on_main.c','w').write(hdr+'\n'.join(out)+'\n')
print(stats); print('manual:',manual)
