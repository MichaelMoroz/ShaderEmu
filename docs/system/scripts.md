# Scripts: JavaScript and Python

Two interpreters are in the image in place of upstream's, which are dynamic ELF programs and
do not run on the machine without an MMU: QuickJS's `qjs` (and `qjsc`) and MicroPython's
`micropython`. The IDE writes, runs and checks both, and debugs Python (`docs/apps/ide.md`).

    / # qjs /examples/pi.js 100              # upstream's examples are in /examples
    / # qjs --std -e 'print(std.getenv("HOME"))'
    / # micropython -c 'import json; print(json.dumps([1, 2]))'

    wsl -- bash linux/nommu/build.sh quickjs/build.sh
    wsl -- bash linux/nommu/build.sh micropython/build.sh

Both scripts take their source at a fixed commit, are among `linux/nommu/build.sh`'s, and put
the program's licence in `/usr/share/quickjs/LICENSE` and `/usr/share/micropython/LICENSE`.

## QuickJS

Fabrice Bellard's engine (MIT), at `3d5e064e`: ES2023 with BigInt, modules, and the `std` and
`os` modules (`qjs --std` makes them globals). `qjsc` is built twice: for this computer, to make
the REPL's C from `repl.js`, and for the guest, where `qjsc -c FILE` checks a script and compiles
it to bytecode (C output wants a C compiler with QuickJS's library, which the image does not
have). Left out: `os.Worker` and `Atomics`, which want threads and the A extension's atomic
instructions, neither of which the machine has. A script's stack is 192 KB
(`JS_DEFAULT_STACK_SIZE`), under the 256 KB a flat program gets.

On `rvc_cpu` (31 October 2026), `qjs -e 0` is 1.46 million instructions, and `/examples/pi.js`
is 2.95 million for 100 digits and 8.79 million for 1,000.

## MicroPython

MicroPython 1.29.0 (MIT), its Unix port's standard variant. `mpy-cross` is built first for this
computer, then the port with the guest's compiler. Left out: threads, `ffi`, TLS, `btree`, the
FAT and littlefs filesystems, and the native code emitters (they write compressed instructions,
which the machine does not have). `asyncio` is frozen into the program.

- Its path is `.frozen` and `~/.micropython/lib`, not upstream's `/usr/lib/micropython`: that
  library is micropython-lib of 2017, much of it built on `ffi`, and a file there would come
  before the built-in module of the same name.
- Its random seed and `os.urandom` read `/dev/urandom`: musl's build would read `/dev/random`,
  which waits for an entropy pool that never fills here.
- `sys.settrace` is built in for the IDE's debugger, changed so that a call with no trace set
  makes no frame object: otherwise every call of every program pays for one.

`micropython -c pass` is 0.27 million instructions; a loop that does nothing costs about 370
instructions a turn (`rvc_cpu`, 31 October 2026).

## Checking it

`python tools/test_all.py qjs micropython` (the `programs` tag): pi to 50 digits and a line of
`std`; JSON, `re`, `os.urandom` and a coroutine run by `asyncio`. The IDE's cases are
`python tools/test_all.py ide`.
