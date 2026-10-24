# A call of an undefined weak function, which the code tests first (libc++abi's __cxa_thread_atexit_impl on musl):
# ld.lld points the call at its own bundle (0 is beyond a call's +-2 MB from 0x02000000). Exits with 5.
    .text
    .weak missing
    .globl _start
_start:
    la    a0, missing
    bnei  a0, 0, have
    li    a7, 93
    li    a0, 5
    syscall
have:
    call  missing
    li    a7, 93
    li    a0, 9
    syscall
