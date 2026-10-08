"""Fits TinyCC's -mfpu to the machine's float instructions (docs/fpu.md, docs/cc.md). Idempotent.

The machine has single precision only: with the hook a float is instructions and a double
stays the library call it was, slow and not illegal.
"""
import sys

path = sys.argv[1] + '/riscv32-gen.c'
s = open(path).read()
if 'ShaderEmu' in s:
    sys.exit(0)
edits = [
    ('''    if (tcc_state->fpu) {
        gen_opf_fpu(op);
        return;
    }''', '''    /* ShaderEmu: the machine has F without D */
    if (tcc_state->fpu && (vtop[0].type.t & VT_BTYPE) == VT_FLOAT) {
        gen_opf_fpu(op);
        return;
    }'''),
    ('''    if (tcc_state->fpu && !l) {
        /* Inline FPU: int32 → float/double */''', '''    if (tcc_state->fpu && !l && t == VT_FLOAT) {
        /* Inline FPU: int32 → float (ShaderEmu: not double) */'''),
    ('''    if (tcc_state->fpu && !l) {
        /* Inline FPU: float/double → int32 */''', '''    if (tcc_state->fpu && !l && ft == VT_FLOAT) {
        /* Inline FPU: float → int32 (ShaderEmu: not double) */'''),
    ('''    if (tcc_state->fpu) {
        /* Inline FPU: float↔double conversion */''', '''    if (0 && tcc_state->fpu) {
        /* Inline FPU: float↔double conversion (ShaderEmu: left to the library) */'''),
]
for old, new in edits:
    assert s.count(old) == 1, old
    s = s.replace(old, new)
open(path, 'w').write(s)
print('riscv32-gen.c: -mfpu makes float instructions only')
