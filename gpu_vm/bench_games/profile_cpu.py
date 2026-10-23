"""The interpreters' side of gpu_vm/PROFILE_Q3.md: where each core's bundles (gvm_cpu --profile with GVM_PROF_CORES=1) or
instructions (rvc_cpu RVC_PROF with RVC_PROF_PCS=1) went over a window of frames, by job and by kind.

    python gpu_vm/bench_games/profile_cpu.py gvm PROF FUNCS.tsv FRAMES [--csv FILE]
    python gpu_vm/bench_games/profile_cpu.py rv PCS.TXT FUNCS.tsv FRAMES [--csv FILE]

FUNCS.tsv is profile_syms.py's. Kinds: the kernel and firmware (pcs outside the program), polling and waiting (mcw_*,
the game's *Poll functions and its rests), the program. Jobs, by the function's name: the scene (the renderer's
tr_shaderemu.c and gpu_shaderemu.c functions, R_*), the client's game (CG_*, BG_*, PM_*), the items (CG_Item and the
SE_Side functions), the server (SV_*, G_*, the bots' AAS_*, Bot*, BotAI*, AI_*), the collision code both share (CM_*),
the C library and soft float (mem*, str*, __*), the rest of core 0's engine. Each line is a frame's mean.
"""
import argparse, bisect, collections, re

SCENE = re.compile(r'^(gpu|entity_|stage_|picture_|scene_|R_|RB_|list_|light_at|world_|sheet|way_|culled|matrix|SE_Scene|SE_Add|SE_Stretch|seg|gl)')
CLIENT = re.compile(r'^(CG_|BG_|PM_|cg|Pmove|SE_Client)')
ITEMS = re.compile(r'^(CG_Item|SE_Side|SE_SceneSide)')
SERVER = re.compile(r'^(SV_|G_|AAS_|Bot|BotAI|AI_|ClientThink|ClientEndFrame|Touch_|trap_|SE_Server|SE_Think|be_|Info_|Characteristic|ClientCommand|player_|G2)')
SHARED = re.compile(r'^(CM_|Q_|VectorNormalize|AngleVectors|MSG_|Com_|Huff|COM_)')
LIBC = re.compile(r'^(mem|str|__|sqrt|sin|cos|atan|floor|ceil|qsort|vsnprintf|printf|fmod|pow|acos|asin|exp|log)')
POLL = re.compile(r'^(mcw_|helpers_poll|serve$|.*Poll$|scene_rest|SE_.*Rest|wait$|Sys_Milliseconds|SE_WindowClock|NET_Sleep|Com_Frame$|gpuControl|gpuIdle)')


def kind_job(name, in_prog):
    if name is None:
        return ('kernel/firmware', 'kernel') if not in_prog else ('program', 'unknown')
    if POLL.match(name):
        return 'poll/wait', 'mcw'
    for j, r in (('items', ITEMS), ('client game', CLIENT), ('scene', SCENE), ('server', SERVER), ('shared CM_/Q_', SHARED), ('libc/soft float', LIBC)):
        if r.match(name):
            return 'program', j
    return 'program', 'engine/other'


def load_funcs(p):
    F = []
    base = None
    for l in open(p):
        if l.startswith('#'):
            continue
        a, b, n = l.rstrip('\n').split('\t')
        F.append((int(a, 16), int(b, 16), n))
    F.sort()
    return F, [f[0] for f in F]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what', choices=('gvm', 'rv'))
    ap.add_argument('prof')
    ap.add_argument('funcs')
    ap.add_argument('frames', type=float)
    ap.add_argument('--csv')
    a = ap.parse_args()
    F, S = load_funcs(a.funcs)
    lo, hi = F[0][0], F[-1][1]

    def fn(pc):
        i = bisect.bisect_right(S, pc) - 1
        return F[i][2] if i >= 0 and pc < F[i][1] else None
    per = collections.defaultdict(collections.Counter)   # core -> (kind, job) -> count
    funcs = collections.defaultdict(collections.Counter)
    ops = collections.defaultdict(int)
    for l in open(a.prof):
        if l.startswith('#'):
            continue
        p = l.split()
        if a.what == 'gvm':
            pc = int(p[0], 16)
            runs = {0: int(p[5])}
            for x in p[7:]:
                c, v = x.split(':')
                runs[int(c)] = int(v)
        else:
            if len(p) < 8:
                continue
            pc, runs = int(p[2], 16), {int(p[7]): int(p[4])}
        n = fn(pc)
        k = kind_job(n, lo <= pc < hi)
        for c, v in runs.items():
            if v:
                per[c][k] += v
                funcs[c][n or 'kernel/firmware'] += v
    unit = 'bundles' if a.what == 'gvm' else 'instructions'
    busy = sorted((c for c in per if sum(per[c].values()) / a.frames > 2000 or c == 0), key=lambda c: -sum(per[c].values()))
    print(f'{unit} a frame over {a.frames:.0f} frames; cores with more than 2,000 a frame')
    rows = []
    for grp, cs in (('core 0', [0]), ('workers', [c for c in busy if c])):
        tot = collections.Counter()
        for c in cs:
            tot.update(per[c])
        t = sum(tot.values())
        print(f'{grp}: {t / a.frames:,.0f} a frame')
        for (k, j), v in sorted(tot.items(), key=lambda kv: -kv[1]):
            print(f'    {k:16s} {j:16s} {v / a.frames:10,.0f}  {v / t * 100:5.1f}%')
            rows.append((grp, k, j, v / a.frames))
    print('each busy core: ' + '; '.join(f'{c} {sum(per[c].values()) / a.frames:,.0f} (' + ', '.join(f'{n} {v / sum(per[c].values()) * 100:.0f}%' for n, v in funcs[c].most_common(3)) + ')' for c in busy))
    if a.csv:
        with open(a.csv, 'w') as f:
            f.write('cores,kind,job,a_frame\n')
            for r in rows:
                f.write(f'{r[0]},{r[1]},{r[2]},{r[3]:.1f}\n')


if __name__ == '__main__':
    main()
