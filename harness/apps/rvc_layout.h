#pragma once
// The machine's physical layout, as experiments/rvc_opt/src/layout.h has it (docs/system/nommu.md, "Memory"):
// the devices' 32 MB from 0x80000000 (the GPU's memory, the mailboxes, the control words, the network's and
// the console's rows, the sound card's memory, the device words), then RAM in one range to 0x92000000, where
// the kernel starts. Until 28 October 2026 the devices were at 0x86000000 (RAM below and above them); a
// snapshot or a kernel of that layout does not run on this one.
#include <cstdint>

namespace rvlayout {
constexpr uint32_t kDevAt = 0x80000000u;       // the devices' first byte
constexpr uint32_t kDevEnd = 0x82000000u;      // and their end: RAM from here
constexpr uint32_t kGpuMemEnd = 0x81e00000u;   // the end of what a GPU program may own (then the device words)
constexpr uint32_t kKernelAt = 0x82000000u;    // where the kernel is linked and started
constexpr uint32_t kTreeAt = 0x83e00000u;      // the device tree, after the kernel in the boot image
// a device's byte at offset o from kDevAt, and texel t of the devices as an index of RAM's texels
constexpr uint32_t dev(uint32_t o) { return kDevAt + o; }
constexpr uint32_t devT(uint32_t t) { return t + ((kDevAt & 0x7fffffffu) >> 4); }
// the places the host reads and writes (docs/machine/gpu.md, console.md, lan.md, fetch.md; programs/mc/mc.h)
constexpr uint32_t kCtrl = dev(0x1000000u);         // the control words
constexpr uint32_t kMbox = dev(0xc00000u);          // the worker cores' mailboxes
constexpr uint32_t kNet = dev(0x16b8000u);          // the network's row
constexpr uint32_t kCon = dev(0x16b8280u);          // the console's words: the host's, the guest's, then the ring
constexpr uint32_t kStatsAll = dev(0x16ba000u);     // every core's count (MC_STATS_ALL)
constexpr uint32_t kFetchData = dev(0x16c0000u);    // what the host fetched (FETCH_DATA)
constexpr uint32_t kCountTotal = dev(0xc10000u);    // the counters' totals (MC_COUNT_PHYS)
// A snapshot of this layout is marked so (rvc_harness --save-state): the state texture in rows of 8,192 texels
// (docs/machine/gpu.md, "Rows of 8192"). One marked SX86SNP2 is of this address layout in rows of 2,048 and is
// laid out anew as it loads; one marked SX86SNAP is of the layout before (devices at 0x86000000) and is refused.
constexpr char kSnapshotMagic[9] = "SX86SNP3";
constexpr char kSnapshotMagicRows2048[9] = "SX86SNP2";
}  // namespace rvlayout
