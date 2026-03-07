#pragma once

#include "core/vmm/machine_model.h"
#include "core/arch/aarch64/pl011.h"
#include "core/arch/aarch64/boot.h"

// ARM64 virt machine model (Apple Hypervisor.framework).
// Uses GICv3, PL011 UART, FDT boot, and VirtIO MMIO.
class Aarch64Machine final : public MachineModel {
public:
    Aarch64Machine() = default;

    bool SetupPlatformDevices(
        AddressSpace& addr_space,
        GuestMemMap& mem,
        HypervisorVm* hv_vm,
        std::shared_ptr<ConsolePort> console_port,
        std::function<void()> shutdown_cb,
        std::function<void()> reboot_cb) override;

    bool LoadKernel(
        const VmConfig& config,
        GuestMemMap& mem,
        const std::vector<VirtioDeviceSlot>& virtio_slots) override;

    bool SetupBootVCpu(HypervisorVCpu* vcpu, uint8_t* ram) override;

    void InjectIrq(HypervisorVm* hv_vm, uint8_t irq) override;
    void SetIrqLevel(HypervisorVm* hv_vm, uint8_t irq, bool asserted) override;

    void InjectConsoleInput(const uint8_t* data, size_t size) override;

    void TriggerPowerButton() override;

    std::vector<VirtioDeviceSlot> GetVirtioSlots() const override;

    GPA RamBase() const override { return aarch64::Layout::kRamBase; }
    GPA MmioGapStart() const override { return 0; }
    GPA MmioGapEnd() const override { return 0; }

private:
    Pl011 uart_;
    GPA kernel_entry_ = 0;
    GPA fdt_gpa_ = 0;

    HypervisorVm* hv_vm_ = nullptr;
    std::function<void()> shutdown_cb_;

    // PL011 UART base address (QEMU virt convention)
    static constexpr GPA kUartBase = 0x09000000;
    static constexpr uint8_t kUartIrq = 1;  // SPI 1 → actual GIC IRQ 33

    // VirtIO MMIO starts at 0x0A000000 with 0x200 stride
    static constexpr GPA kVirtioMmioBase  = 0x0A000000;
    static constexpr uint64_t kVirtioStride = 0x200;
    static constexpr uint8_t kVirtioBaseIrq = 16;  // SPI 16+

    // GIC addresses (must match hvf_vm.cpp)
    static constexpr GPA kGicDistBase   = 0x08000000;
    static constexpr GPA kGicRedistBase = 0x080A0000;
};
