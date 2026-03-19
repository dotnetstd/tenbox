#pragma once

#include "core/device/device.h"
#include "core/device/irq/gicv3_regs.h"
#include "core/vmm/address_space.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace gicv3 {

// Callback to signal a virtual IRQ to a specific vCPU.
// Parameter: assert — when true the IRQ line is raised, when false it is lowered.
using VCpuKickFn = std::function<void(bool assert)>;

// Per-interrupt state shared between Distributor (SPI) and Redistributor (SGI/PPI).
struct IrqState {
    bool     enabled  = false;
    bool     pending  = false;
    bool     active   = false;
    bool     level    = false;   // current input line level (for level-triggered)
    bool     edge_cfg = false;   // true = edge, false = level
    uint8_t  priority = kPriorityDefault;
    uint32_t route    = 0;       // target CPU affinity (SPI only)
    uint8_t  group    = 1;       // 0 = Group 0, 1 = Group 1
};

// Per-CPU state for the ICC (CPU Interface) emulation.
struct CpuInterface {
    uint8_t  pmr      = 0;       // Priority Mask Register
    uint8_t  bpr1     = 0;       // Binary Point Register (Group 1)
    uint8_t  bpr0     = 0;       // Binary Point Register (Group 0)
    uint32_t ctlr     = 0;       // ICC_CTLR_EL1
    bool     grp1_en  = false;   // ICC_IGRPEN1_EL1
    bool     grp0_en  = false;   // ICC_IGRPEN0_EL1

    // Running priority stack (max nesting depth for preemption)
    static constexpr int kApMaxDepth = 4;
    uint8_t  running_priority[kApMaxDepth] = {};
    int      ap_depth = 0;

    uint8_t RunningPriority() const {
        return ap_depth > 0 ? running_priority[ap_depth - 1] : kPriorityIdle;
    }

    void PushPriority(uint8_t prio) {
        if (ap_depth < kApMaxDepth) {
            running_priority[ap_depth++] = prio;
        }
    }

    void PopPriority() {
        if (ap_depth > 0) --ap_depth;
    }
};

// ---------------------------------------------------------------------------
// SoftGicDistributor — MMIO device at GICD base, manages SPI state.
// ---------------------------------------------------------------------------
class SoftGic;

class SoftGicDistributor : public Device {
public:
    void MmioRead(uint64_t offset, uint8_t size, uint64_t* value) override;
    void MmioWrite(uint64_t offset, uint8_t size, uint64_t value) override;

private:
    friend class SoftGic;
    SoftGic* parent_ = nullptr;
};

// ---------------------------------------------------------------------------
// SoftGicRedistributor — MMIO device for one CPU, manages SGI/PPI state.
// Each instance covers 0x20000 bytes (RD_base 0x10000 + SGI_base 0x10000).
// ---------------------------------------------------------------------------
class SoftGicRedistributor : public Device {
public:
    void MmioRead(uint64_t offset, uint8_t size, uint64_t* value) override;
    void MmioWrite(uint64_t offset, uint8_t size, uint64_t value) override;

    uint32_t Index() const { return index_; }

private:
    friend class SoftGic;
    SoftGic*  parent_ = nullptr;
    uint32_t  index_  = 0;
};

// ---------------------------------------------------------------------------
// SoftGic — top-level software GICv3 emulation.
// ---------------------------------------------------------------------------
class SoftGic {
public:
    explicit SoftGic(uint32_t cpu_count);

    // Register GICD and GICR MMIO devices into the address space.
    void RegisterDevices(AddressSpace& addr_space,
                         uint64_t dist_base, uint64_t redist_base);

    // Register per-vCPU kick callback (called when an interrupt becomes pending).
    // Must be called once per vCPU after the vCPU is created.
    void RegisterVCpuKick(uint32_t cpu, VCpuKickFn fn);

    // Assert/deassert an SPI line (intid = 32..kMaxSpi-1).
    void SetSpiLevel(uint32_t intid, bool level);

    // Assert a PPI for a specific CPU (intid = 16..31).
    void SetPpiLevel(uint32_t cpu, uint32_t intid, bool level);

    // Handle ICC system register access (called from vCPU SysReg trap).
    // Returns true if the register was recognized and handled.
    // |syndrome| is the ESR_EL2 ISS field for EC=0x18.
    // For reads, *reg_value receives the value; for writes, *reg_value is the value to write.
    bool HandleIccSysReg(uint32_t cpu, uint32_t syndrome,
                         uint64_t* reg_value);

    // Query state
    uint32_t CpuCount() const { return cpu_count_; }
    uint64_t RedistBase() const { return redist_base_; }
    uint64_t RedistSizePerCpu() const { return kGicrSizePerCpu; }

    // Check if a CPU has a deliverable pending interrupt (thread-safe).
    bool HasPendingInterrupt(uint32_t cpu);

private:
    friend class SoftGicDistributor;
    friend class SoftGicRedistributor;

    uint32_t cpu_count_;

    // SPI state (INTID 32..kMaxSpi-1)
    std::array<IrqState, kMaxSpi> spi_state_{};

    // Per-CPU SGI/PPI state (INTID 0..31)
    struct PerCpu {
        std::array<IrqState, kSgiPpiCount> irq{};
        CpuInterface icc{};
        bool waker_sleep = true;
    };
    std::vector<PerCpu> per_cpu_;

    // Distributor global state
    bool     dist_enabled_ = false;
    bool     dist_are_     = true;  // Affinity Routing Enable (always 1 for GICv3)

    // MMIO devices
    SoftGicDistributor dist_dev_;
    std::vector<SoftGicRedistributor> redist_devs_;

    uint64_t redist_base_ = 0;

    std::vector<VCpuKickFn> kick_fns_;
    mutable std::mutex mutex_;

    // --- Internal helpers ---

    // Find the highest-priority pending & enabled interrupt for a CPU.
    // Returns INTID or kIntidSpurious if none.
    uint32_t HighestPendingIntid(uint32_t cpu) const;

    // Evaluate whether a CPU has a deliverable interrupt and kick it.
    void UpdateCpu(uint32_t cpu);

    // Acknowledge the highest-priority interrupt for a CPU (ICC_IAR read).
    uint32_t AcknowledgeInterrupt(uint32_t cpu);

    // End-of-interrupt (ICC_EOIR write).
    void EndOfInterrupt(uint32_t cpu, uint32_t intid);

    // Deactivate interrupt (ICC_DIR write).
    void DeactivateInterrupt(uint32_t cpu, uint32_t intid);

    // Get/set interrupt state by absolute INTID for a given CPU context.
    IrqState* GetIrqState(uint32_t cpu, uint32_t intid);

    // Distributor MMIO handlers
    void DistRead(uint64_t offset, uint8_t size, uint64_t* value);
    void DistWrite(uint64_t offset, uint8_t size, uint64_t value);

    // Redistributor MMIO handlers
    void RedistRead(uint32_t cpu, uint64_t offset, uint8_t size, uint64_t* value);
    void RedistWrite(uint32_t cpu, uint64_t offset, uint8_t size, uint64_t value);

    // Bitmap helpers for ISENABLER/ICENABLER/ISPENDR/ICPENDR/ISACTIVER/ICACTIVER
    uint32_t ReadBitmapReg(uint32_t cpu, uint64_t base_offset, uint64_t offset) const;
    void WriteBitmapSetReg(uint32_t cpu, uint64_t base_offset, uint64_t offset, uint32_t value);
    void WriteBitmapClearReg(uint32_t cpu, uint64_t base_offset, uint64_t offset, uint32_t value);

    // Priority register helpers
    uint32_t ReadPriorityReg(uint32_t cpu, uint64_t offset) const;
    void WritePriorityReg(uint32_t cpu, uint64_t offset, uint32_t value);
};

} // namespace gicv3
