#ifndef LAYOUT_H
#define LAYOUT_H

// The machine's physical layout, in one place (docs/system/nommu.md, "Memory"). The state texture
// holds 0x80000000 to 0x92000000 from row 64 on, a 16-byte texel a pixel: the devices' 32 MB first
// (the GPU's memory, the mailboxes, the control words, the network's and the console's rows, the
// sound card's memory and the device words at its end), then RAM in one range, where the kernel
// starts (supervisor mode, the device tree in a1). Until 28 October 2026 the devices were at
// 0x86000000, with RAM from the kernel at 0x80400000 below them and from 0x88000000 above: a
// snapshot or a kernel of that layout does not run on this one.
#define DEV_AT    0x80000000u   // the devices' first byte (the GPU's memory)
#define DEV_END   0x82000000u   // their end: RAM from here
#define GPU_MEM_END 0x81e00000u // the end of what a GPU program may own (then the device words)
#define KERNEL_AT 0x82000000u   // where the kernel is linked and the machine starts it
#define TREE_AT   0x83e00000u   // the device tree, in the boot image after the kernel (tools/make_linux_image.py)
// texel t of the devices (an offset from DEV_AT in texels) as an index of RAM's texels
#define DEV_T(t) ((t) + ((DEV_AT & 0x7fffffffu) >> 4))

#endif
