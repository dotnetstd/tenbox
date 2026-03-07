#pragma once

#include "core/vmm/hypervisor_vm.h"
#include "common/ports.h"
#include <cstdint>
#include <memory>

// Platform abstraction for OS-specific operations used by the VM.
// Each platform (Windows, macOS) provides a single .cpp implementing these.
struct VmPlatform {
    static bool IsHypervisorPresent();
    static std::unique_ptr<HypervisorVm> CreateHypervisor(uint32_t cpu_count);
    static uint8_t* AllocateRam(uint64_t size);
    static void FreeRam(uint8_t* base, uint64_t size);
    static std::shared_ptr<ConsolePort> CreateConsolePort();
    static void YieldCpu();
    static void SleepMs(uint32_t ms);
};
