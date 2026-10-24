# libunwind for gpu_vm (gpu_vm/userland/cxx.sh): Registers_gpuvm, its context save and restore, the config. Idempotent:
#   python3 libunwind_gpuvm.py [~/gpu_vm/llvm-project/libunwind]
import os, sys
L = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else '~/gpu_vm/llvm-project/libunwind')

def edit(path, old, new, count=1):
    p = os.path.join(L, path)
    s = open(p).read()
    if new in s:
        return
    assert s.count(old) == count, (path, old[:60])
    s = s.replace(old, new)
    open(p, 'w').write(s)

edit('include/__libunwind_config.h', '#define _LIBUNWIND_HIGHEST_DWARF_REGISTER_RISCV     64\n',
     '#define _LIBUNWIND_HIGHEST_DWARF_REGISTER_RISCV     64\n#define _LIBUNWIND_HIGHEST_DWARF_REGISTER_GPUVM     63\n')
edit('include/__libunwind_config.h', '''#elif defined(__wasm__)
// Unused''', '''#elif defined(__gpuvm__)
// gpu_vm (gpu_vm/GPU_VM.md): r0-r63, the pc in r0's word
#define _LIBUNWIND_TARGET_GPUVM 1
#define _LIBUNWIND_CONTEXT_SIZE 32
#define _LIBUNWIND_CURSOR_SIZE 39
#define _LIBUNWIND_HIGHEST_DWARF_REGISTER _LIBUNWIND_HIGHEST_DWARF_REGISTER_GPUVM
#elif defined(__wasm__)
// Unused''')
edit('src/config.h', '''    defined(__sparc__) || defined(__s390x__) || defined(__loongarch__)
#if !defined(_LIBUNWIND_BUILD_SJLJ_APIS)''', '''    defined(__sparc__) || defined(__s390x__) || defined(__loongarch__) ||     \
    defined(__gpuvm__)
#if !defined(_LIBUNWIND_BUILD_SJLJ_APIS)''')
edit('src/libunwind.cpp', '''#elif defined(__riscv)
# define REGISTER_KIND Registers_riscv''', '''#elif defined(__riscv)
# define REGISTER_KIND Registers_riscv
#elif defined(__gpuvm__)
# define REGISTER_KIND Registers_gpuvm''')
edit('src/Registers.hpp', '''  REGISTERS_LOONGARCH,
};''', '''  REGISTERS_LOONGARCH,
  REGISTERS_GPUVM,
};''')
edit('src/Registers.hpp', '''#if defined(_LIBUNWIND_TARGET_LOONGARCH)
/// Registers_loongarch''', '''#if defined(_LIBUNWIND_TARGET_GPUVM)
/// Registers_gpuvm holds a gpu_vm thread's registers: r1-r63 in their words,
/// the pc in r0's (as the kernel's NT_PRSTATUS). DWARF's register n is rn;
/// the return address is ra (r1), sp r2.
class _LIBUNWIND_HIDDEN Registers_gpuvm {
public:
  Registers_gpuvm() { memset(&_r, 0, sizeof(_r)); }
  Registers_gpuvm(const void *registers) {
    static_assert((check_fit<Registers_gpuvm, unw_context_t>::does_fit),
                  "gpu_vm registers do not fit into unw_context_t");
    memcpy(&_r, registers, sizeof(_r));
  }

  typedef uint32_t reg_t;
  typedef uint32_t link_reg_t;
  typedef const link_reg_t &link_hardened_reg_arg_t;

  bool validRegister(int num) const {
    return num == UNW_REG_IP || num == UNW_REG_SP || (num >= 1 && num <= 63);
  }
  uint32_t getRegister(int num) const {
    if (num == UNW_REG_IP)
      return _r[0];
    if (num == UNW_REG_SP)
      return _r[2];
    if (num >= 1 && num <= 63)
      return _r[num];
    _LIBUNWIND_ABORT("unsupported gpu_vm register");
  }
  void setRegister(int num, uint32_t value) {
    if (num == UNW_REG_IP)
      _r[0] = value;
    else if (num == UNW_REG_SP)
      _r[2] = value;
    else if (num >= 1 && num <= 63)
      _r[num] = value;
    else
      _LIBUNWIND_ABORT("unsupported gpu_vm register");
  }
  bool validFloatRegister(int) const { return false; }
  double getFloatRegister(int) const {
    _LIBUNWIND_ABORT("gpu_vm: floats are in the integer registers");
  }
  void setFloatRegister(int, double) {
    _LIBUNWIND_ABORT("gpu_vm: floats are in the integer registers");
  }
  bool validVectorRegister(int) const { return false; }
  v128 getVectorRegister(int) const {
    _LIBUNWIND_ABORT("gpu_vm has no vector registers");
  }
  void setVectorRegister(int, v128) {
    _LIBUNWIND_ABORT("gpu_vm has no vector registers");
  }
  static const char *getRegisterName(int num) {
    static const char *const names[64] = {
        "pc",  "ra",  "sp",  "gp",  "tp",  "a0",  "a1",  "a2",  "a3",  "a4",
        "a5",  "a6",  "a7",  "t0",  "t1",  "t2",  "t3",  "t4",  "t5",  "t6",
        "t7",  "t8",  "t9",  "t10", "t11", "t12", "t13", "t14", "t15", "t16",
        "t17", "t18", "s0",  "s1",  "s2",  "s3",  "s4",  "s5",  "s6",  "s7",
        "s8",  "s9",  "s10", "s11", "s12", "s13", "s14", "s15", "s16", "s17",
        "s18", "s19", "s20", "s21", "s22", "s23", "t19", "t20", "t21", "t22",
        "t23", "t24", "t25", "t26"};
    if (num == UNW_REG_IP)
      return "pc";
    if (num == UNW_REG_SP)
      return "sp";
    return num >= 0 && num <= 63 ? names[num] : "unknown register";
  }
  void jumpto() { __libunwind_Registers_gpuvm_jumpto(this); }
  static constexpr int lastDwarfRegNum() {
    return _LIBUNWIND_HIGHEST_DWARF_REGISTER_GPUVM;
  }
  static int getArch() { return REGISTERS_GPUVM; }

  uint64_t getSP() const { return _r[2]; }
  void setSP(uint32_t value) { _r[2] = value; }
  uint64_t getIP() const { return _r[0]; }
  void setIP(uint32_t value) { _r[0] = value; }

private:
  uint32_t _r[64];
};
#endif // _LIBUNWIND_TARGET_GPUVM

#if defined(_LIBUNWIND_TARGET_LOONGARCH)
/// Registers_loongarch''')
# the restore's declaration beside the others' (Registers.hpp's top)
edit('src/Registers.hpp', '''#if defined(_LIBUNWIND_TARGET_I386)
class _LIBUNWIND_HIDDEN Registers_x86;''', '''#if defined(_LIBUNWIND_TARGET_GPUVM)
class _LIBUNWIND_HIDDEN Registers_gpuvm;
extern "C" void __libunwind_Registers_gpuvm_jumpto(Registers_gpuvm *);
#endif

#if defined(_LIBUNWIND_TARGET_I386)
class _LIBUNWIND_HIDDEN Registers_x86;''')
edit('src/UnwindRegistersSave.S', '''#elif defined(__riscv)

#
# extern int __unw_getcontext(unw_context_t* thread_state)''', '''#elif defined(__gpuvm__)

#
# extern int __unw_getcontext(unw_context_t* thread_state)
#
# a0 the context: r1-r63 in their words, the pc (the return address) in r0's
#
DEFINE_LIBUNWIND_FUNCTION(__unw_getcontext)
  sq r0, 0(a0)
  .irp i,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60
    sq r\\i, (4 * \\i)(a0)
  .endr
  sw ra, 0(a0)
  addi a0, zero, 0
  jr ra

#elif defined(__riscv)

#
# extern int __unw_getcontext(unw_context_t* thread_state)''')
edit('src/UnwindRegistersRestore.S', '''#elif defined(__riscv)

//
// void libunwind::Registers_riscv::jumpto()''', '''#elif defined(__gpuvm__)

//
// extern "C" void __libunwind_Registers_gpuvm_jumpto(Registers_gpuvm *);
//
// a0 the registers: the pc into ra (r0's word: r0 takes no write), a0's four
// last, then to the pc
//
DEFINE_LIBUNWIND_FUNCTION(__libunwind_Registers_gpuvm_jumpto)
  .irp i,8,12,16,20,24,28,32,36,40,44,48,52,56,60
    lq r\\i, (4 * \\i)(a0)
  .endr
  lw ra, 0(a0)
  lw sp, 8(a0)
  lw gp, 12(a0)
  lq r4, 16(a0)
  jr ra

#elif defined(__riscv)

//
// void libunwind::Registers_riscv::jumpto()''')
# the cursor's DWARF-only encoding, as RISC-V's
edit('src/UnwindCursor.hpp', """  compact_unwind_encoding_t dwarfEncoding(Registers_riscv &) const {
    return 0;
  }
#endif""", """  compact_unwind_encoding_t dwarfEncoding(Registers_riscv &) const {
    return 0;
  }
#endif

#if defined(_LIBUNWIND_TARGET_GPUVM)
  compact_unwind_encoding_t dwarfEncoding(Registers_gpuvm &) const {
    return 0;
  }
#endif""")
print('libunwind: gpu_vm in')
