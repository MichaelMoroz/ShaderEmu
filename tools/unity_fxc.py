"""Compile passes of the world's Unity shaders outside the editor, with the compiler the editor has (fxc2's
d3dcompiler_47.dll), to see that a change to shader_vm/src or unity/ShaderEmu/Shaders compiles before the editor does.

    python tools/unity_fxc.py SHADER PASS [PASS ...]      e.g. GpuDrawBlit.shader GPUDrawIds GPU0

The shaders are copied into a scratch folder with src/*.cginc made from shader_vm/src/*.h as "ShaderEmu/Sync shader
sources from the repository" makes them, and Unity's CGIncludes (the newest editor under Unity Hub, or UNITY_CGINCLUDES),
and each named pass's CGPROGRAM is compiled for every stage its pragmas name, with Unity's backwards compatibility flag
(D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY) and SHADER_API_D3D11 and UNITY_REVERSED_Z defined. D3DCOMPILER=DLL takes
another compiler (Windows' own refuses gpu_amp_geom's "potentially uninitialized" outputs, which fxc2 takes). Exits 1 if a
stage did not compile.
"""
import ctypes, glob, os, re, shutil, sys, tempfile
from ctypes import POINTER, byref, c_char_p, c_size_t, c_uint, c_void_p

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = os.environ.get('D3DCOMPILER', os.path.join(ROOT, 'tools', 'fxc2', 'bin', 'd3dcompiler_47.dll'))
STAGES = (('vertex', 'vs_5_0'), ('hull', 'hs_5_0'), ('domain', 'ds_5_0'), ('geometry', 'gs_5_0'), ('fragment', 'ps_5_0'))


def cg_includes():
    if os.environ.get('UNITY_CGINCLUDES'):
        return os.environ['UNITY_CGINCLUDES']
    found = sorted(glob.glob(r'C:\Program Files\Unity\Hub\Editor\*\Editor\Data\CGIncludes'))
    if not found:
        raise SystemExit("no Unity editor's CGIncludes under Unity Hub: set UNITY_CGINCLUDES")
    return found[-1]


def compile_stage(dll, path, entry, target):
    src = open(path, 'rb').read()
    code, errs = c_void_p(), c_void_p()
    dll.D3DCompile.argtypes = [c_char_p, c_size_t, c_char_p, c_void_p, c_void_p, c_char_p, c_char_p, c_uint, c_uint,
                               POINTER(c_void_p), POINTER(c_void_p)]
    # (ID3DInclude 1: the standard include, which looks beside the top file)
    hr = dll.D3DCompile(src, len(src), path.encode(), None, c_void_p(1), entry.encode(), target.encode(), 1 << 12, 0,
                        byref(code), byref(errs))

    def blob(p):   # an ID3DBlob's bytes: GetBufferPointer and GetBufferSize, its vtable's 4th and 5th
        if not p.value:
            return b''
        vt = ctypes.cast(ctypes.cast(p, POINTER(c_void_p))[0], POINTER(c_void_p))
        ptr = ctypes.WINFUNCTYPE(c_void_p, c_void_p)(vt[3])(p)
        n = ctypes.WINFUNCTYPE(c_size_t, c_void_p)(vt[4])(p)
        return ctypes.string_at(ptr, n)
    return hr == 0, blob(errs).decode('latin-1', 'replace'), len(blob(code))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    shader, names = sys.argv[1], sys.argv[2:]
    shaders = os.path.join(ROOT, 'unity', 'ShaderEmu', 'Shaders')
    work = tempfile.mkdtemp(prefix='unity_fxc_')
    cginc = cg_includes()
    for folder in (cginc, shaders):
        for f in os.listdir(folder):
            if os.path.isfile(os.path.join(folder, f)):
                shutil.copy(os.path.join(folder, f), work)
    for f in ('crt.cginc', 'helpers.cginc'):
        shutil.copy(os.path.join(ROOT, 'shader_vm', f), work)
    os.makedirs(os.path.join(work, 'src'))
    for f in os.listdir(os.path.join(ROOT, 'shader_vm', 'src')):
        if f.endswith('.h'):
            text = re.sub(r'(#include\s+"[^"]+)\.h"', r'\1.cginc"', open(os.path.join(ROOT, 'shader_vm', 'src', f)).read())
            for to in (os.path.join(work, 'src'), work):   # (an include inside one is found from the top file's folder)
                open(os.path.join(to, f[:-2] + '.cginc'), 'w').write(text)
    dll = ctypes.WinDLL(DLL)
    text = open(os.path.join(shaders, shader)).read()
    failed = 0
    for name in names:
        m = re.search(r'Name\s+"' + re.escape(name) + r'".*?CGPROGRAM(.*?)ENDCG', text, re.S)
        if not m:
            print(f'{name}: no such pass in {shader}')
            failed += 1
            continue
        body = m.group(1)
        src = os.path.join(work, name + '.hlsl')
        open(src, 'w').write('#define SHADER_API_D3D11 1\n#define UNITY_REVERSED_Z 1\n' + body)
        for pragma, profile in STAGES:
            e = re.search(r'#pragma\s+' + pragma + r'\s+(\w+)', body)
            if not e:
                continue
            ok, log, size = compile_stage(dll, src, e.group(1), profile)
            failed += not ok
            print(f'{name} {profile} {e.group(1)}: ' + (f'ok, {size} bytes' if ok else 'FAILED'))
            if not ok:
                print(log.strip()[-3000:])
    shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
