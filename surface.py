import json,sys
def walk(n,out,inc):
    k=n.get('kind'); name=n.get('name','')
    loc=n.get('loc',{}); f=loc.get('file') or loc.get('includedFrom',{}).get('file')
    if k=='FunctionDecl' and name.startswith('duckdb_v2'):
        out['fn'][name]=n['type']['qualType']
    elif k=='TypedefDecl' and name.lower().startswith('duckdb_v2'):
        out['typedef'][name]=n['type']['qualType']
    elif k=='RecordDecl' and n.get('completeDefinition'):
        rname=name or 'anon@'+str(loc.get('line'))
        fields=[(c.get('name'),c['type']['qualType']) for c in n.get('inner',[]) if c.get('kind')=='FieldDecl']
        if rname.lower().lstrip('_').startswith('duckdb_v2'): out['record'][rname]=fields
        else: out['_anon'][n.get('id')]=fields
    elif k=='EnumDecl':
        for c in n.get('inner',[]):
            if c.get('kind')=='EnumConstantDecl':
                val=None
                def find(x):
                    nonlocal val
                    if 'value' in x and val is None: val=x['value']
                    for y in x.get('inner',[]): find(y)
                find(c)
                out['enum'][c['name']]=val
    for c in n.get('inner',[]): walk(c,out,inc)
def load(p):
    out={'fn':{},'typedef':{},'record':{},'enum':{},'_anon':{}}
    walk(json.load(open(p)),out,None); return out
o=load(sys.argv[1]); n=load(sys.argv[2])
# typedef'd anonymous structs: map typedef name -> anon fields via qualType 'struct (unnamed ...)' is awkward; report typedefs whose type is a struct
import re
for cat in ['fn','typedef','record','enum']:
    a,b=o[cat],n[cat]
    same=sum(1 for k in a if k in b and a[k]==b[k]); ch=[k for k in a if k in b and a[k]!=b[k]]
    rm=[k for k in a if k not in b]; ad=[k for k in b if k not in a]
    print(f"{cat:8} old={len(a):4} new={len(b):4} same={same:4} changed={len(ch):3} removed={len(rm):3} added={len(ad):3}")
pass
