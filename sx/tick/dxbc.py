# The tick's fragment shader as fxc2 compiles it (tools/fxc2/d3dcompiler_47.dll), with no card: its DXBC listing,
# or the count of its instructions, for a step and its switches.
#   python sx/tick/dxbc.py sx/tick/steps/x13.hlsl [-D NAME=V]... [--entry frag] [--list FILE]
import ctypes, os, sys

here = os.path.dirname(os.path.abspath(__file__))
dll = ctypes.WinDLL(os.path.join(here, '..', '..', 'tools', 'fxc2', 'd3dcompiler_47.dll'))

class Macro(ctypes.Structure):
    _fields_ = [('Name', ctypes.c_char_p), ('Definition', ctypes.c_char_p)]

class Blob:   # ID3DBlob: GetBufferPointer is vtable slot 3, GetBufferSize 4
    def __init__(self, p): self.p = p
    def _fn(self, i, res):
        vt = ctypes.cast(ctypes.cast(self.p, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        return ctypes.WINFUNCTYPE(res, ctypes.c_void_p)(vt[i])(self.p)
    def bytes(self): return ctypes.string_at(self._fn(3, ctypes.c_void_p), self._fn(4, ctypes.c_size_t))

def main():
    args = sys.argv[1:]
    path, defs, entry, out = None, [], 'frag', None
    i = 0
    while i < len(args):
        a = args[i]
        if a == '-D': defs.append(args[i + 1]); i += 2
        elif a == '--entry': entry = args[i + 1]; i += 2
        elif a == '--list': out = args[i + 1]; i += 2
        else: path = a; i += 1
    src = open(path, 'rb').read()
    ms = (Macro * (len(defs) + 1))()
    for k, d in enumerate(defs):
        n, _, v = d.partition('=')
        ms[k].Name, ms[k].Definition = n.encode(), (v or '1').encode()
    D3D_COMPILE_STANDARD_FILE_INCLUDE = ctypes.c_void_p(1)
    code, err = ctypes.c_void_p(), ctypes.c_void_p()
    hr = dll.D3DCompile(src, len(src), os.path.abspath(path).encode(), ms, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                        entry.encode(), b'ps_5_0', 0, 0, ctypes.byref(code), ctypes.byref(err))
    if err.value: sys.stderr.write(Blob(err).bytes().decode(errors='replace'))
    if hr != 0: sys.exit('compile failed: %08x' % (hr & 0xffffffff))
    dis = ctypes.c_void_p()
    blob = Blob(code).bytes()
    hr = dll.D3DDisassemble(blob, len(blob), 0, None, ctypes.byref(dis))
    if hr != 0: sys.exit('disassemble failed: %08x' % (hr & 0xffffffff))
    text = Blob(dis).bytes().decode(errors='replace')
    if out: open(out, 'w').write(text)
    lines = [l for l in text.splitlines() if l.strip() and not l.strip().startswith(('//', 'dcl_', 'ps_'))]
    print('%s: %d DXBC instructions, %d bytes' % (path, len(lines), len(blob)))

main()
