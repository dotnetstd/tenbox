#pragma once

#include "core/vmm/types.h"
#include <string>

namespace aarch64 {

// ARM64 Linux Image header (first 64 bytes of the Image file).
// See Documentation/arm64/booting.rst in the Linux source.
struct LinuxImageHeader {
    uint32_t code0;          // Executable code
    uint32_t code1;          // Executable code
    uint64_t text_offset;    // Image load offset from start of RAM
    uint64_t image_size;     // Effective image size (0 means unknown)
    uint64_t flags;          // Kernel flags
    uint64_t res2;
    uint64_t res3;
    uint64_t res4;
    uint32_t magic;          // Magic number: 0x644d5241 ("ARM\x64")
    uint32_t res5;
};

static constexpr uint32_t kArmImageMagic = 0x644d5241;  // "ARM\x64"

// Memory layout constants for the QEMU virt-style machine
namespace Layout {
    constexpr GPA kRamBase       = 0x40000000;   // 1 GiB — standard QEMU virt

    // FDT is placed at the start of RAM
    constexpr GPA kFdtBase       = kRamBase;
    constexpr uint64_t kFdtMaxSize = 0x100000;   // 1 MiB reserved for FDT

    // Kernel loaded at RAM base + 2 MiB (per ARM64 boot protocol)
    constexpr GPA kKernelBase    = kRamBase + 0x200000;  // text_offset default

    // Initrd placed after kernel (rounded up to page boundary)
    // Actual address computed at load time based on kernel size.
}

struct BootConfig {
    std::string kernel_path;
    std::string initrd_path;
    std::string cmdline;
    GuestMemMap mem;
    uint32_t cpu_count = 1;
};

// Load ARM64 Linux kernel Image and optional initrd into guest RAM.
// Returns the entry point GPA, or 0 on failure.
GPA LoadLinuxImage(BootConfig& config);

} // namespace aarch64
