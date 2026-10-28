"""docs/plans/riscv-profile.md's tables from prof_summary.py's lines (one JSON a part): the summary across the parts and
a section a part. Prints Markdown.

    python tools/rv_compare/prof_doc.py SUMMARY.jsonl [--section] [--summary]"""
import argparse, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from parts import PARTS  # noqa: E402
from prof_parts import WINDOWS  # noqa: E402

KINDS = ['program', 'libc copy/fill/str', 'libc other', 'libm', 'soft float, 64-bit', 'kernel', 'window server', 'window client',
         'unknown']
FORMS = {}


def short(f):
    return f.replace('vmlinux:', 'kernel:')


def window(p):
    P, W = PARTS[p], WINDOWS.get(p, {})
    if 'after' in W:
        return f"{W['after']} s after the command to {W['span']} s later"
    frm = W.get('frm') or (P.get('pace') if P.get('pace') != 'steady' else None) or 'the command'
    return f"`{frm}` to `{P.get('until') or 'the wall (50 s of the host)'}`"


def summary_table(rs):
    print('| part | guest s profiled (s) | core 0 (M instructions / guest s) | all cores (M instructions / guest s) | core 0 passes ended full (/ guest s) '
          '| workers\' passes ended full (/ guest s) | core 0 texels new a busy pass (texels) | top three of core 0 (% of core 0) '
          '| libc copy/str (%) | libc other (%) | libm + soft float (%) | kernel (%) | window server (%) |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for r in rs:
        if 'error' in r:
            print(f"| {r['part']} | {r['error'][:60]} |" + ' |' * 11)
            continue
        g = r['guest_s']
        c0, w = r['core0'], r['workers']
        k = c0['kinds']
        top = '; '.join(f'{short(f)} {v}' for f, v in c0['top'][:3])
        print(f"| {r['part']} | {g:.1f} | {c0['instr'] / g / 1e6:.2f} | {r['all'] / g / 1e6:.1f} | {c0['full'] / g:.1f} | {w['full'] / g:.1f} "
              f"| {r['core0_distinct_busy_mean']:.0f} | {top} | {k.get('libc copy/fill/str', 0)} | {k.get('libc other', 0)} "
              f"| {k.get('libm', 0) + k.get('soft float, 64-bit', 0):.1f} | {k.get('kernel', 0)} | {k.get('window server', 0)} |")


def section(r):
    p = r['part']
    P = PARTS[p]
    print(f"### {p}\n")
    print(f"{P['what']}. `{P['cmd'][:160]}`{'...' if len(P['cmd']) > 160 else ''}\n")
    if 'error' in r:
        print(f"No profile: {r['error'][:200]}\n")
        return
    g = r['guest_s']
    c0, w = r['core0'], r['workers']
    print(f"Window: {window(p)}: {g:.1f} s of the guest, {r['profiled_passes']:,} passes; core 0 {c0['instr'] / 1e6:.1f} M "
          f"instructions ({c0['instr'] / g / 1e6:.2f} M a guest second, {r['core0_instr_pass_mean']:,.0f} a pass), all cores "
          f"{r['all'] / 1e6:.1f} M, {r['cores_ran']} cores ran. Core 0 took {c0['newtex']:,} write-cache entries "
          f"({r['core0_distinct_busy_mean']:.0f} distinct texels a pass that stored), and {c0['full']:,} of its passes ended full "
          f"({c0['full'] / g:.1f} a guest second); the workers' {w['full']:,} ({w['full'] / g:.1f} a guest second).\n")
    print('| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers\' instructions (%) |')
    print('|---|---|---|---|')
    for k in KINDS:
        a, b, c = c0['kinds'].get(k, 0), c0['kinds_tex'].get(k, 0), w['kinds'].get(k, 0)
        if a or b or c:
            print(f'| {k} | {a} | {b} | {c} |')
    print()
    n = max(len(c0['top']), len(c0['top_tex']))
    print('| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0\'s) |')
    print('|---|---|---|---|')
    for i in range(n):
        a = c0['top'][i] if i < len(c0['top']) else ('', '')
        b = c0['top_tex'][i] if i < len(c0['top_tex']) else ('', '')
        print(f'| {short(a[0])} | {a[1]} | {short(b[0])} | {b[1]} |')
    print()
    if c0['top_full']:
        print('Core 0\'s passes ended full, where (passes): ' + ', '.join(f'{short(f)} {v}' for f, v in c0['top_full']) + '.\n')
    if w['instr']:
        n = max(len(w['top']), len(w['top_tex']))
        print('| workers: function | instructions (% of the workers\') | workers: function | write-cache entries (% of the workers\') |')
        print('|---|---|---|---|')
        for i in range(min(n, 6)):
            a = w['top'][i] if i < len(w['top']) else ('', '')
            b = w['top_tex'][i] if i < len(w['top_tex']) else ('', '')
            print(f'| {short(a[0])} | {a[1]} | {short(b[0])} | {b[1]} |')
        print()
        if w['top_full']:
            print('The workers\' passes ended full, where (passes): ' + ', '.join(f'{short(f)} {v}' for f, v in w['top_full']) + '.\n')
    got = ', '.join(f"{k} {v['loads']:,} ({v['pct_core0']}% of core 0, {v['sites']} places)" for k, v in r['got'].items() if v['loads'] >= 1000)
    if got:
        print(f'GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): {got}.\n')
    pats = [l.strip() for l in r['patterns'][1:] if l.strip() and l.strip()[0].isdigit()]
    if pats:
        print('Slow forms prof.py finds (% of core 0): ' + '; '.join(pats[:5]) + '.\n')
    f = FORMS.get(p)
    if f and 'error' not in f:
        pc, km = f['prog_copies'], f['kernel_memcpy']
        by = sorted(pc['by'].items(), key=lambda x: -x[1][0] * x[1][1])[:3]
        print(f"Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset {pc['instr_pct']}% of core 0's "
              f"instructions and {pc['tex_pct']}% of its write-cache entries, calls of about 2 KB or more {pc['big_calls_instr_pct']}%"
              f" ({'; '.join(f'{short(k)} {v[0]:,} calls of {v[1]:,} instructions' for k, v in by)}). The kernel's `__memcpy` "
              f"{km['instr_pct']}% (byte paths {km['byte_paths_pct']}%, 64-byte word loop {km['word_loop_pct']}%).")
        if f['soft_callers'] and f['soft_callers'][0][1] >= 0.5:
            print(' Soft floats and 64-bit divides, inclusive, by caller (% of core 0): '
                  + ', '.join(f'{short(a)} {b}' for a, b in f['soft_callers'] if b >= 0.1) + '.')
        if f['big_fills'] and f['big_fills'][0][2] >= 0.3:
            print(' Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): '
                  + ', '.join(f'{short(a)} {n} {v}' for a, n, v in f['big_fills'] if v >= 0.1) + '.')
        print()
    # the shell's waits and the programs' mains hold everything below them: not jobs
    jobs = [j for j in r.get('jobs') or [] if not j['fn'].split(':')[0].startswith('busybox') and 'main' not in j['fn'].split(':')[-1]]
    if jobs:
        print('| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |')
        print('|---|---|---|---|')
        for j in jobs[:6]:
            print(f"| {short(j['fn'])} | {j['calls']:,} | {j['per']:,} | {j['most']:,} |")
        print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('summary')
    ap.add_argument('--sections', action='store_true')
    ap.add_argument('--forms', help="prof_forms.py's lines, added to the sections")
    a = ap.parse_args()
    if a.forms:
        for line in open(a.forms):
            r = json.loads(line)
            FORMS[r['part']] = r
    rs = {}
    for line in open(a.summary):
        r = json.loads(line)
        rs[r['part']] = r
    order = [p for p in PARTS if p in rs]
    if a.sections:
        for p in order:
            section(rs[p])
    else:
        summary_table([rs[p] for p in order])


if __name__ == '__main__':
    main()
