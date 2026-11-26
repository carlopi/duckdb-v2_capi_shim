#!/usr/bin/env python3
"""Reads the v2 C API surface from a duckdb_v2.h with nothing but Python: the header is generated and regular, so its
function declarations, typedefs and structs are found by pattern. Returns what the generator needs, spelled the way
clang spells types (so the clang-based check and this agree): functions name -> (return type, [(param, type)]),
typedefs name -> type (function pointers as 'ret (*)(types)'), structs name -> [(field, type)]."""
import re


def _strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', text)


def norm_type(t):
    t = re.sub(r'\s+', ' ', t).strip()
    t = re.sub(r'\s*\*\s*', ' *', t)          # every star: one space before, none after
    while '* *' in t:
        t = t.replace('* *', '**')            # 'char **', as clang prints it
    t = re.sub(r'\*(const|volatile)\b', r'*\1', t)
    t = t.replace('( ', '(').replace(' )', ')')
    return t.strip()


def _split_params(text):
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch == ',' and depth == 0:
            out.append(cur); cur = ''
            continue
        depth += ch in '(['
        depth -= ch in ')]'
        cur += ch
    if cur.strip():
        out.append(cur)
    return [p.strip() for p in out]


def _param(p):
    """'const char *const *names' -> ('names', 'const char *const *')"""
    m = re.match(r'^(.*?)([A-Za-z_][A-Za-z0-9_]*)\s*$', p, re.S)
    if not m or not m.group(1).strip():
        return ('', norm_type(p))
    return (m.group(2), norm_type(m.group(1)))


def parse(path):
    text = _strip_comments(open(path).read())
    fns, typedefs, records = {}, {}, {}
    for m in re.finditer(r'DUCKDB_C_API\s+([^;{}]*?)\b(duckdb_v2_\w+)\s*\(([^;{}]*?)\)\s*;', text, re.S):
        ret = norm_type(m.group(1))
        params = [] if m.group(3).strip() in ('', 'void') else [_param(p) for p in _split_params(m.group(3))]
        fns[m.group(2)] = (ret, params)
    for m in re.finditer(r'typedef\s+([^;{}]*?)\(\s*\*\s*(\w+)\s*\)\s*\(([^;{}]*?)\)\s*;', text, re.S):
        params = [] if m.group(3).strip() in ('', 'void') else [_param(p)[1] for p in _split_params(m.group(3))]
        ret = norm_type(m.group(1))
        typedefs[m.group(2)] = '%s%s(*)(%s)' % (ret, '' if ret.endswith('*') else ' ', ', '.join(params) if params else 'void')
    for m in re.finditer(r'typedef\s+((?:struct|enum|union)\s+)?(\w+)\s+(\w+)\s*;', text):
        typedefs.setdefault(m.group(3), ((m.group(1) or '') + m.group(2)).strip())
    for m in re.finditer(r'\bstruct\s+(duckdb_v2_\w+)\s*\{', text):
        # the body, balanced; nested unions and structs count as one field of shape '(union)' / '(struct)'
        i, depth = m.end(), 1
        while depth:
            depth += text[i] == '{'
            depth -= text[i] == '}'
            i += 1
        body = text[m.end():i - 1]
        flat = re.sub(r'\b(union|struct)\s*\{[^{}]*(\{[^{}]*\}[^{}]*)*\}', r'(\1)', body)
        fields = []
        for decl in flat.split(';'):
            decl = decl.strip()
            if decl:
                fields.append(_param(decl)[::1])
        records[m.group(1)] = [(n, t) for n, t in fields]
    return fns, typedefs, records
