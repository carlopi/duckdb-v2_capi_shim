#!/usr/bin/env python3
"""The same shim, across DuckDB versions: for each ref, read that commit's duckdb_v2.h and duckdb_extension_v2.h from
git (no checkout, no build), generate the adapters against the contract, and compile them with the hand-written
sources they need. Says, per ref, whether the shim covers it and, when not, which contract functions match no rule.

    tests/sweep.py <duckdb git dir> [ref ...]

Without refs: every v2.x release tag, and every first-parent commit of origin/main that changed duckdb_v2.h since the
oldest of them. The git dir can be a blobless clone (git clone --filter=blob:none --no-checkout); its origin/main and
tags must be fetched."""
import os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CONTRACT = os.environ.get('CONTRACT', 'v2.0.1')
CC = os.environ.get('CC', 'cc')


def git(repo, *args):
    return subprocess.run(['git', '-C', repo] + list(args), capture_output=True, text=True, check=True).stdout


def refs_to_sweep(repo):
    tags = [t for t in git(repo, 'tag', '--list', 'v2.*').split() if re.fullmatch(r'v2\.\d+\.\d+', t)]
    tags.sort(key=lambda t: [int(x) for x in t[1:].split('.')])
    base = tags[0] if tags else 'origin/main~200'
    refs = list(tags)
    for branch in ('origin/v2.0-cyanoptera', 'origin/main'):
        try:
            git(repo, 'rev-parse', '--verify', '-q', branch)
        except subprocess.CalledProcessError:
            continue
        refs += git(repo, 'log', '--first-parent', '--reverse', '--format=%H', base + '..' + branch, '--',
                    'src/include/duckdb_v2.h').split()
        refs.append(branch)  # its head, whatever it touched
    seen = set()
    return [r for r in refs if not (r in seen or seen.add(r))]


def sweep_one(repo, ref):
    work = tempfile.mkdtemp()
    try:
        inc = os.path.join(work, 'include')
        os.makedirs(inc)
        for h in ('duckdb_v2.h', 'duckdb_extension_v2.h'):
            try:
                text = git(repo, 'show', '%s:src/include/%s' % (ref, h))
            except subprocess.CalledProcessError:
                return 'no-v2', '', 'no %s at this commit' % h
            open(os.path.join(inc, h), 'w').write(text)
        gen = os.path.join(work, 'gen')
        r = subprocess.run([sys.executable, os.path.join(ROOT, 'generate.py'), '--contract',
                            os.path.join(ROOT, 'contracts', CONTRACT), '--target', inc, '--out', gen],
                           capture_output=True, text=True)
        stats = (re.search(r"\{'forward'.*?\}", r.stdout) or re.search('', '')).group(0)
        if r.returncode != 0:
            unmatched = re.search(r"unmatched: (\[.*\])", r.stdout)
            names = re.findall(r"'(duckdb_v2_\w+)'", unmatched.group(1)) if unmatched else []
            return 'no rule', stats, '%d unmatched: %s' % (len(names), ', '.join(n[len('duckdb_v2_'):] for n in names[:6]))
        sources = [os.path.join(gen, 'shim.c')] + [os.path.join(ROOT, s.strip()) for s in
                                                  open(os.path.join(gen, 'manual_sources.txt')) if s.strip()]
        sources.append(os.path.join(ROOT, 'src', 'target_api.c'))
        flags = ['-std=c11', '-DDUCKDB_V2_API_ALLOW_UNSTABLE=1', '-Wall', '-Werror', '-Wno-unused-function',
                 '-fsyntax-only', '-I', inc, '-I', gen, '-include', os.path.join(ROOT, 'src', 'target_prelude.h')]
        for src in sources:
            c = subprocess.run([CC] + flags + [src], capture_output=True, text=True)
            if c.returncode != 0:
                first = next((l for l in c.stderr.splitlines() if 'error' in l), c.stderr[:200])
                return 'compile', stats, '%s: %s' % (os.path.basename(src), first.split('error:')[-1].strip()[:160])
        return 'ok', stats, ''
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    repo = sys.argv[1]
    refs = sys.argv[2:] or refs_to_sweep(repo)
    failures = 0
    print('%-16s %-10s %-26s %-9s %s' % ('ref', 'date', 'contract ' + CONTRACT + ' served by', 'result', 'detail'))
    for ref in refs:
        date = git(repo, 'log', '-1', '--format=%cs', ref).strip()
        result, stats, detail = sweep_one(repo, ref)
        nums = re.findall(r'\d+', stats)
        counts = '%s fwd %s gen %s hand' % tuple(nums) if len(nums) == 3 else ''
        name = ref if ref.startswith('v') else ref.replace('origin/', '') if ref.startswith('origin/') else ref[:10]
        print('%-16s %-10s %-26s %-9s %s' % (name, date, counts, result, detail), flush=True)
        failures += result not in ('ok', 'no-v2')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
