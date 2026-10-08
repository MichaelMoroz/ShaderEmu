# The tick as a compute shader (an experiment)

A copy of `experiments/rvc_opt` whose `CPUTick` pass has a second entry, `tick_cs`: the same
emulation as one thread of a compute shader instead of the pixels of the CPU's zone. It is
here to answer one question, how much the fragment shader costs the emulator, and is not
what the machine runs: the D3D11 harness only, and a VRChat world cannot dispatch a compute
shader at all.

    set RVC11_COMPUTE=1
    bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_compute --image linux-net ...

`gpu.shader` and `sound.shader` are unchanged copies (the harness looks for them beside
`main.shader`), and the harness treats this folder as it does `rvc_opt` (machine sizes,
`SBI_HLE`). Init frames still use the fragment entry.

| Define | |
|---|---|
| (none) | the tick as it is, write cache and all; it writes the pass's texels into a 64 x 64 texture that the harness copies over the CPU zone |
| `L1_SHARED` | the write cache and the second-level TLB in thread group shared memory instead of local arrays |
| `XR_SHARED` | the registers too |
| `RAM_BUFFER` | RAM is a byte address buffer of the tick's own, filled from the state texture once (`ram_import_cs`): a load is one word or four, a store one word, no write cache. The machine's memory copy and fill are done in the tick. Nothing else sees that RAM, so what runs is what needs no device but the console and the ROM: upstream's Linux image, not ours with its GPU and display |
| `RAM_DIRECT` | the whole state texture is the shader's UAV, read and written in place: a store goes to RAM there and then. No write cache, so no frame ends on a full one; the Commit pass still runs for what else it does (memory operations, the UART, the devices) and copies the bands that were written into the other buffer |

## What it measures

RTX 5070 Laptop, D3D11, fxc2, 9 October 2026, 16,384 instructions a frame unless it says
otherwise. Emulation is the same in every column: the raytracer ends with the same RAM and
after the same 73,669,730 instructions, both Linux images boot, and the checksum run prints
what the fragment tick's prints.

| | fragment | compute | compute, `L1_SHARED` `XR_SHARED` | compute, `RAM_DIRECT` |
|---|---|---|---|---|
| raytracer, tick pass a frame | 2.94 ms | 2.70 to 2.76 ms | 2.61 to 2.70 ms | 2.63 ms |
| our Linux to its login shell | 8.2 s | 8.2 s | | 6.9 s |
| boot, checksums of three programs, `ls -lR`, 300 KB of `dd` | 191.5 s, 3,158k IPS | | | 160.9 s, 3,758k IPS |
| the same at 65,536 instructions a frame | | | | 138.4 s, 4,206k IPS |

- One thread instead of some 1,500 pixels that each run the whole tick is worth 7%.
- Shared memory instead of local arrays is worth nothing that can be told from noise.
- Writing RAM directly is worth 19% on the store-heavy run, and a third with longer frames,
  which it makes possible: nothing ends a frame early but the guest and the devices.

So not an order of magnitude. An emulated instruction is some 120 operations of decode and
execution and four or five branches wherever it runs, and those are what take the 160 to
250 ns; the memory it touches is a small part (about one texture or array access in eight
instructions for the raytracer).

## Linux

Upstream's image (`--payload rvc\_Nixvc\data-net`), which runs on all four; same session,
runs interleaved. With RAM written directly a run is the same frames and instructions whether
RAM is the texture or the buffer.

| | fragment | compute | `RAM_DIRECT` | `RAM_BUFFER` |
|---|---|---|---|---|
| to the login shell, 16,384 a frame | 11.7 s, 3,211k IPS | 11.7 s, 3,210k | 9.7 s, 3,872k | 9.1 s, 4,105k |
| to the login shell, 65,536 a frame | 11.3 s, 3,293k IPS | 10.9 s, 3,410k | 8.7 s, 4,266k | 8.3 s, 4,454k |
| boot, then `ls -lR`, 300 KB of `dd` and a shell loop of 30,000 turns (1.18 billion instructions), 65,536 a frame | 3,329k IPS | 3,660k IPS | 267.7 s, 4,414k IPS | 252.5 s, 4,680k IPS |

(The first two columns of the last row had not finished at the 300 s the run was given.)

So for Linux: the tick as a compute shader as it is, 0 to 10%; with RAM written directly,
1.2 to 1.33 times the fragment tick; and a byte address buffer is another 4 to 6% over the
texture, 1.28 to 1.4 times in all. On the raytracer the buffer and the texture are the same
speed (2.42 to 2.52 ms a frame against 2.46 to 2.51).

The compute shader compiles in 1 to 3 seconds under fxc2. `RAM_DIRECT` loads from a
four-component UAV, which D3D11 allows only on hardware with
`TypedUAVLoadAdditionalFormats`.
