#include "core/device/irq/gicv3.h"
#include "core/vmm/types.h"
#include <algorithm>
#include <cstring>

namespace gicv3 {

// ===========================================================================
// SoftGic
// ===========================================================================

SoftGic::SoftGic(uint32_t cpu_count)
    : cpu_count_(std::min(cpu_count, kMaxCpus)) {

    per_cpu_.resize(cpu_count_);
    for (auto& pc : per_cpu_) {
        pc.icc.pmr = 0;
        pc.icc.bpr1 = 0;
        pc.icc.grp1_en = false;
        pc.waker_sleep = true;
        for (auto& irq : pc.irq) {
            irq.priority = kPriorityDefault;
            irq.group = 1;
        }
    }
    for (auto& spi : spi_state_) {
        spi.priority = kPriorityDefault;
        spi.group = 1;
    }

    dist_dev_.parent_ = this;

    redist_devs_.resize(cpu_count_);
    for (uint32_t i = 0; i < cpu_count_; i++) {
        redist_devs_[i].parent_ = this;
        redist_devs_[i].index_ = i;
    }

    kick_fns_.resize(cpu_count_);
}

void SoftGic::RegisterVCpuKick(uint32_t cpu, VCpuKickFn fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cpu < cpu_count_) {
        kick_fns_[cpu] = std::move(fn);
    }
}

void SoftGic::RegisterDevices(AddressSpace& addr_space,
                              uint64_t dist_base, uint64_t redist_base) {
    redist_base_ = redist_base;
    addr_space.AddMmioDevice(dist_base, kGicdSize, &dist_dev_);
    for (uint32_t i = 0; i < cpu_count_; i++) {
        uint64_t base = redist_base + static_cast<uint64_t>(i) * kGicrSizePerCpu;
        addr_space.AddMmioDevice(base, kGicrSizePerCpu, &redist_devs_[i]);
    }
}

IrqState* SoftGic::GetIrqState(uint32_t cpu, uint32_t intid) {
    if (intid < kSgiPpiCount) {
        if (cpu < cpu_count_)
            return &per_cpu_[cpu].irq[intid];
        return nullptr;
    }
    if (intid >= kSpiBase && intid < kMaxSpi)
        return &spi_state_[intid];
    return nullptr;
}

void SoftGic::SetSpiLevel(uint32_t intid, bool level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (intid < kSpiBase || intid >= kMaxSpi) return;

    auto& st = spi_state_[intid];
    bool old_level = st.level;
    st.level = level;

    if (st.edge_cfg) {
        if (!old_level && level)
            st.pending = true;
    } else {
        st.pending = level;
    }

    if (level) {
        // Route to target CPU and evaluate
        uint32_t target = st.route & 0xFF;
        if (target < cpu_count_) UpdateCpu(target);
    }
}

void SoftGic::SetPpiLevel(uint32_t cpu, uint32_t intid, bool level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cpu >= cpu_count_ || intid >= kSgiPpiCount) return;

    auto& st = per_cpu_[cpu].irq[intid];
    bool old_level = st.level;
    st.level = level;

    if (st.edge_cfg) {
        if (!old_level && level)
            st.pending = true;
    } else {
        st.pending = level;
    }

    if (level) UpdateCpu(cpu);
}

uint32_t SoftGic::HighestPendingIntid(uint32_t cpu) const {
    if (cpu >= cpu_count_) return kIntidSpurious;

    const auto& icc = per_cpu_[cpu].icc;
    uint8_t running = icc.RunningPriority();
    uint8_t mask = icc.pmr;

    uint32_t best_intid = kIntidSpurious;
    uint8_t  best_prio  = 0xFF;

    // Check SGI/PPI (INTID 0..31)
    for (uint32_t i = 0; i < kSgiPpiCount; i++) {
        const auto& st = per_cpu_[cpu].irq[i];
        if (!st.enabled || !st.pending || st.active) continue;
        if (st.group == 1 && !icc.grp1_en) continue;
        if (st.group == 0 && !icc.grp0_en) continue;
        if (st.priority >= mask || st.priority >= running) continue;
        if (st.priority < best_prio) {
            best_prio = st.priority;
            best_intid = i;
        }
    }

    // Check SPI (INTID 32..kMaxSpi-1)
    if (dist_enabled_) {
        for (uint32_t i = kSpiBase; i < kMaxSpi; i++) {
            const auto& st = spi_state_[i];
            if (!st.enabled || !st.pending || st.active) continue;
            if (st.group == 1 && !icc.grp1_en) continue;
            if (st.group == 0 && !icc.grp0_en) continue;
            uint32_t target = st.route & 0xFF;
            if (target != cpu) continue;
            if (st.priority >= mask || st.priority >= running) continue;
            if (st.priority < best_prio) {
                best_prio = st.priority;
                best_intid = i;
            }
        }
    }

    return best_intid;
}

bool SoftGic::HasPendingInterrupt(uint32_t cpu) {
    std::lock_guard<std::mutex> lock(mutex_);
    return HighestPendingIntid(cpu) != kIntidSpurious;
}

void SoftGic::UpdateCpu(uint32_t cpu) {
    // Must be called with mutex_ held.
    if (cpu >= cpu_count_) return;
    uint32_t intid = HighestPendingIntid(cpu);
    bool has_pending = (intid != kIntidSpurious);
    if (cpu < kick_fns_.size() && kick_fns_[cpu]) {
        kick_fns_[cpu](has_pending);
    }
}

uint32_t SoftGic::AcknowledgeInterrupt(uint32_t cpu) {
    // Must be called with mutex_ held.
    uint32_t intid = HighestPendingIntid(cpu);
    if (intid == kIntidSpurious) return kIntidSpurious;

    IrqState* st = GetIrqState(cpu, intid);
    if (!st) return kIntidSpurious;

    st->active = true;
    if (st->edge_cfg || intid < kSgiPpiCount) {
        st->pending = false;
    }
    // For level-triggered SPIs, pending stays asserted while line is high.

    per_cpu_[cpu].icc.PushPriority(st->priority);

    return intid;
}

void SoftGic::EndOfInterrupt(uint32_t cpu, uint32_t intid) {
    // Must be called with mutex_ held.
    if (cpu >= cpu_count_) return;

    IrqState* st = GetIrqState(cpu, intid);
    if (st) {
        st->active = false;
        // For level-triggered: if line still high, re-pend
        if (!st->edge_cfg && st->level) {
            st->pending = true;
        }
    }

    per_cpu_[cpu].icc.PopPriority();
    UpdateCpu(cpu);
}

void SoftGic::DeactivateInterrupt(uint32_t cpu, uint32_t intid) {
    // Must be called with mutex_ held.
    if (cpu >= cpu_count_) return;
    IrqState* st = GetIrqState(cpu, intid);
    if (st) st->active = false;
}

// ===========================================================================
// ICC System Register access
// ===========================================================================

bool SoftGic::HandleIccSysReg(uint32_t cpu, uint32_t syndrome,
                               uint64_t* reg_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cpu >= cpu_count_) return false;

    bool is_write = !((syndrome >> 0) & 1); // ISS bit 0: direction (0=write, 1=read)
    uint32_t reg_enc = syndrome & kIccSysRegMask;

    auto& icc = per_cpu_[cpu].icc;

    if (!is_write) {
        // MRS — read from system register
        uint64_t val = 0;
        switch (reg_enc) {
        case kIccIar1El1:
            val = AcknowledgeInterrupt(cpu);
            UpdateCpu(cpu);
            break;
        case kIccIar0El1:
            val = kIntidSpurious; // Group 0 not used
            break;
        case kIccHppir1El1:
            val = HighestPendingIntid(cpu);
            break;
        case kIccHppir0El1:
            val = kIntidSpurious;
            break;
        case kIccPmrEl1:
            val = icc.pmr;
            break;
        case kIccBpr1El1:
            val = icc.bpr1;
            break;
        case kIccBpr0El1:
            val = icc.bpr0;
            break;
        case kIccCtlrEl1:
            val = icc.ctlr;
            break;
        case kIccSreEl1:
            val = 0x7; // SRE=1, DFB=1, DIB=1 (system register interface always enabled)
            break;
        case kIccIgrpen0El1:
            val = icc.grp0_en ? 1u : 0u;
            break;
        case kIccIgrpen1El1:
            val = icc.grp1_en ? 1u : 0u;
            break;
        case kIccRprEl1:
            val = icc.RunningPriority();
            break;
        default:
            return false;
        }
        *reg_value = val;
    } else {
        // MSR — write to system register
        uint64_t val = *reg_value;
        switch (reg_enc) {
        case kIccEoir1El1:
            EndOfInterrupt(cpu, static_cast<uint32_t>(val & 0x3FF));
            UpdateCpu(cpu);
            break;
        case kIccEoir0El1:
            EndOfInterrupt(cpu, static_cast<uint32_t>(val & 0x3FF));
            UpdateCpu(cpu);
            break;
        case kIccDirEl1:
            DeactivateInterrupt(cpu, static_cast<uint32_t>(val & 0x3FF));
            UpdateCpu(cpu);
            break;
        case kIccPmrEl1:
            icc.pmr = static_cast<uint8_t>(val & 0xFF);
            UpdateCpu(cpu);
            break;
        case kIccBpr1El1:
            icc.bpr1 = static_cast<uint8_t>(val & 0x7);
            break;
        case kIccBpr0El1:
            icc.bpr0 = static_cast<uint8_t>(val & 0x7);
            break;
        case kIccCtlrEl1:
            icc.ctlr = static_cast<uint32_t>(val);
            break;
        case kIccSreEl1:
            // Read-only in our implementation (always SRE=1)
            break;
        case kIccIgrpen0El1:
            icc.grp0_en = (val & 1) != 0;
            UpdateCpu(cpu);
            break;
        case kIccIgrpen1El1:
            icc.grp1_en = (val & 1) != 0;
            UpdateCpu(cpu);
            break;
        case kIccSgi1rEl1:
            // SGI generation — not needed for our use case, but handle to avoid trapping
            break;
        default:
            return false;
        }
    }
    return true;
}

// ===========================================================================
// Distributor MMIO
// ===========================================================================

void SoftGicDistributor::MmioRead(uint64_t offset, uint8_t size, uint64_t* value) {
    parent_->DistRead(offset, size, value);
}

void SoftGicDistributor::MmioWrite(uint64_t offset, uint8_t size, uint64_t value) {
    parent_->DistWrite(offset, size, value);
}

void SoftGic::DistRead(uint64_t offset, uint8_t size, uint64_t* value) {
    std::lock_guard<std::mutex> lock(mutex_);
    *value = 0;

    switch (offset) {
    case kGicdCtlr:
        *value = (dist_enabled_ ? kGicdCtlrEnGrp1Ns : 0) | kGicdCtlrAre;
        return;
    case kGicdTyper: {
        uint32_t it_lines = (kMaxSpi / 32) - 1;
        *value = kGicdTyperItLines(it_lines) | kGicdTyperCpuNum(cpu_count_ - 1);
        return;
    }
    case kGicdIidr:
        *value = 0x0100143B; // JEP106 for ARM (placeholder)
        return;
    case kGicdStatusr:
        *value = 0;
        return;
    case kGicdPidr2:
        *value = kPidr2ArchGicv3;
        return;
    default:
        break;
    }

    // ISENABLER (0x100 .. 0x17C) — read enabled bits for SPI
    if (offset >= kGicdIsenabler && offset < kGicdIsenabler + (kMaxSpi / 8)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIsenabler) * 8);
        uint32_t val = 0;
        for (uint32_t b = 0; b < 32 && (base_intid + b) < kMaxSpi; b++) {
            uint32_t intid = base_intid + b;
            const IrqState* st = (intid >= kSpiBase) ? &spi_state_[intid] : nullptr;
            if (st && st->enabled) val |= (1u << b);
        }
        *value = val;
        return;
    }

    // ICENABLER — same layout as ISENABLER, just reads the same bits
    if (offset >= kGicdIcenabler && offset < kGicdIcenabler + (kMaxSpi / 8)) {
        uint64_t rebased = (offset - kGicdIcenabler) + kGicdIsenabler;
        DistRead(rebased, size, value);
        return;
    }

    // ISPENDR
    if (offset >= kGicdIspendr && offset < kGicdIspendr + (kMaxSpi / 8)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIspendr) * 8);
        uint32_t val = 0;
        for (uint32_t b = 0; b < 32 && (base_intid + b) < kMaxSpi; b++) {
            uint32_t intid = base_intid + b;
            const IrqState* st = (intid >= kSpiBase) ? &spi_state_[intid] : nullptr;
            if (st && st->pending) val |= (1u << b);
        }
        *value = val;
        return;
    }

    // ICPENDR — reads same as ISPENDR
    if (offset >= kGicdIcpendr && offset < kGicdIcpendr + (kMaxSpi / 8)) {
        uint64_t rebased = (offset - kGicdIcpendr) + kGicdIspendr;
        DistRead(rebased, size, value);
        return;
    }

    // ISACTIVER
    if (offset >= kGicdIsactiver && offset < kGicdIsactiver + (kMaxSpi / 8)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIsactiver) * 8);
        uint32_t val = 0;
        for (uint32_t b = 0; b < 32 && (base_intid + b) < kMaxSpi; b++) {
            uint32_t intid = base_intid + b;
            const IrqState* st = (intid >= kSpiBase) ? &spi_state_[intid] : nullptr;
            if (st && st->active) val |= (1u << b);
        }
        *value = val;
        return;
    }

    // ICACTIVER — reads same as ISACTIVER
    if (offset >= kGicdIcactiver && offset < kGicdIcactiver + (kMaxSpi / 8)) {
        uint64_t rebased = (offset - kGicdIcactiver) + kGicdIsactiver;
        DistRead(rebased, size, value);
        return;
    }

    // IPRIORITYR (0x400 .. 0x7FC) — byte-accessible priority for each INTID
    if (offset >= kGicdIpriorityr && offset < kGicdIpriorityr + kMaxSpi) {
        uint32_t base_intid = static_cast<uint32_t>(offset - kGicdIpriorityr);
        uint32_t val = 0;
        for (uint8_t b = 0; b < size && (base_intid + b) < kMaxSpi; b++) {
            uint32_t intid = base_intid + b;
            uint8_t prio = (intid >= kSpiBase) ? spi_state_[intid].priority : 0;
            val |= static_cast<uint32_t>(prio) << (b * 8);
        }
        *value = val;
        return;
    }

    // ICFGR (0xC00 .. 0xCFC) — 2 bits per interrupt
    if (offset >= kGicdIcfgr && offset < kGicdIcfgr + (kMaxSpi / 4)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIcfgr) * 4);
        uint32_t val = 0;
        for (uint32_t b = 0; b < 16 && (base_intid + b) < kMaxSpi; b++) {
            uint32_t intid = base_intid + b;
            if (intid >= kSpiBase && spi_state_[intid].edge_cfg)
                val |= (2u << (b * 2));
        }
        *value = val;
        return;
    }

    // IGROUPR (0x080 .. 0x0FC)
    if (offset >= kGicdIgroupBase && offset < kGicdIgroupBase + (kMaxSpi / 8)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIgroupBase) * 8);
        uint32_t val = 0;
        for (uint32_t b = 0; b < 32 && (base_intid + b) < kMaxSpi; b++) {
            uint32_t intid = base_intid + b;
            if (intid >= kSpiBase && spi_state_[intid].group == 1)
                val |= (1u << b);
        }
        *value = val;
        return;
    }

    // IROUTER (0x6000 .. ) — 64-bit per SPI
    if (offset >= kGicdIrouter && offset < kGicdIrouter + (kMaxSpi - kSpiBase) * 8) {
        uint32_t intid = kSpiBase + static_cast<uint32_t>((offset - kGicdIrouter) / 8);
        if (intid < kMaxSpi) {
            *value = spi_state_[intid].route;
        }
        return;
    }

    // IGRPMODR — return 0 (all NS Group 1)
    if (offset >= kGicdIgrpmodr && offset < kGicdIgrpmodr + (kMaxSpi / 8)) {
        *value = 0;
        return;
    }

    // Unhandled — return 0
}

void SoftGic::DistWrite(uint64_t offset, uint8_t size, uint64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t val32 = static_cast<uint32_t>(value);

    switch (offset) {
    case kGicdCtlr:
        dist_enabled_ = (val32 & kGicdCtlrEnGrp1Ns) != 0;
        // ARE is always 1, ignore writes to it
        for (uint32_t c = 0; c < cpu_count_; c++) UpdateCpu(c);
        return;
    case kGicdStatusr:
        return; // W1C, ignore
    default:
        break;
    }

    // ISENABLER — set enable bits
    if (offset >= kGicdIsenabler && offset < kGicdIsenabler + (kMaxSpi / 8)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIsenabler) * 8);
        for (uint32_t b = 0; b < 32 && (base_intid + b) < kMaxSpi; b++) {
            if (!(val32 & (1u << b))) continue;
            uint32_t intid = base_intid + b;
            if (intid >= kSpiBase) spi_state_[intid].enabled = true;
        }
        for (uint32_t c = 0; c < cpu_count_; c++) UpdateCpu(c);
        return;
    }

    // ICENABLER — clear enable bits
    if (offset >= kGicdIcenabler && offset < kGicdIcenabler + (kMaxSpi / 8)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIcenabler) * 8);
        for (uint32_t b = 0; b < 32 && (base_intid + b) < kMaxSpi; b++) {
            if (!(val32 & (1u << b))) continue;
            uint32_t intid = base_intid + b;
            if (intid >= kSpiBase) spi_state_[intid].enabled = false;
        }
        return;
    }

    // ISPENDR — set pending
    if (offset >= kGicdIspendr && offset < kGicdIspendr + (kMaxSpi / 8)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIspendr) * 8);
        for (uint32_t b = 0; b < 32 && (base_intid + b) < kMaxSpi; b++) {
            if (!(val32 & (1u << b))) continue;
            uint32_t intid = base_intid + b;
            if (intid >= kSpiBase) spi_state_[intid].pending = true;
        }
        for (uint32_t c = 0; c < cpu_count_; c++) UpdateCpu(c);
        return;
    }

    // ICPENDR — clear pending
    if (offset >= kGicdIcpendr && offset < kGicdIcpendr + (kMaxSpi / 8)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIcpendr) * 8);
        for (uint32_t b = 0; b < 32 && (base_intid + b) < kMaxSpi; b++) {
            if (!(val32 & (1u << b))) continue;
            uint32_t intid = base_intid + b;
            if (intid >= kSpiBase) spi_state_[intid].pending = false;
        }
        return;
    }

    // ISACTIVER — set active
    if (offset >= kGicdIsactiver && offset < kGicdIsactiver + (kMaxSpi / 8)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIsactiver) * 8);
        for (uint32_t b = 0; b < 32 && (base_intid + b) < kMaxSpi; b++) {
            if (!(val32 & (1u << b))) continue;
            uint32_t intid = base_intid + b;
            if (intid >= kSpiBase) spi_state_[intid].active = true;
        }
        return;
    }

    // ICACTIVER — clear active
    if (offset >= kGicdIcactiver && offset < kGicdIcactiver + (kMaxSpi / 8)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIcactiver) * 8);
        for (uint32_t b = 0; b < 32 && (base_intid + b) < kMaxSpi; b++) {
            if (!(val32 & (1u << b))) continue;
            uint32_t intid = base_intid + b;
            if (intid >= kSpiBase) spi_state_[intid].active = false;
        }
        for (uint32_t c = 0; c < cpu_count_; c++) UpdateCpu(c);
        return;
    }

    // IPRIORITYR
    if (offset >= kGicdIpriorityr && offset < kGicdIpriorityr + kMaxSpi) {
        uint32_t base_intid = static_cast<uint32_t>(offset - kGicdIpriorityr);
        for (uint8_t b = 0; b < size && (base_intid + b) < kMaxSpi; b++) {
            uint32_t intid = base_intid + b;
            if (intid >= kSpiBase) {
                spi_state_[intid].priority = static_cast<uint8_t>((value >> (b * 8)) & 0xFF);
            }
        }
        return;
    }

    // ICFGR — 2 bits per interrupt, bit 1 = edge(1)/level(0)
    if (offset >= kGicdIcfgr && offset < kGicdIcfgr + (kMaxSpi / 4)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIcfgr) * 4);
        for (uint32_t b = 0; b < 16 && (base_intid + b) < kMaxSpi; b++) {
            uint32_t intid = base_intid + b;
            if (intid >= kSpiBase) {
                spi_state_[intid].edge_cfg = (val32 >> (b * 2 + 1)) & 1;
            }
        }
        return;
    }

    // IGROUPR
    if (offset >= kGicdIgroupBase && offset < kGicdIgroupBase + (kMaxSpi / 8)) {
        uint32_t base_intid = static_cast<uint32_t>((offset - kGicdIgroupBase) * 8);
        for (uint32_t b = 0; b < 32 && (base_intid + b) < kMaxSpi; b++) {
            uint32_t intid = base_intid + b;
            if (intid >= kSpiBase) {
                spi_state_[intid].group = (val32 & (1u << b)) ? 1 : 0;
            }
        }
        return;
    }

    // IROUTER — 64-bit per SPI
    if (offset >= kGicdIrouter && offset < kGicdIrouter + (kMaxSpi - kSpiBase) * 8) {
        uint32_t intid = kSpiBase + static_cast<uint32_t>((offset - kGicdIrouter) / 8);
        if (intid < kMaxSpi) {
            spi_state_[intid].route = static_cast<uint32_t>(value & 0xFF);
        }
        return;
    }

    // IGRPMODR — ignore (we treat everything as NS Group 1)
    if (offset >= kGicdIgrpmodr && offset < kGicdIgrpmodr + (kMaxSpi / 8)) {
        return;
    }
}

// ===========================================================================
// Redistributor MMIO
// ===========================================================================

void SoftGicRedistributor::MmioRead(uint64_t offset, uint8_t size, uint64_t* value) {
    parent_->RedistRead(index_, offset, size, value);
}

void SoftGicRedistributor::MmioWrite(uint64_t offset, uint8_t size, uint64_t value) {
    parent_->RedistWrite(index_, offset, size, value);
}

void SoftGic::RedistRead(uint32_t cpu, uint64_t offset, uint8_t size, uint64_t* value) {
    std::lock_guard<std::mutex> lock(mutex_);
    *value = 0;

    if (cpu >= cpu_count_) return;
    auto& pc = per_cpu_[cpu];

    // RD_base frame (0x0000 .. 0xFFFF)
    if (offset < kGicrSgiOffset) {
        switch (offset) {
        case kGicrCtlr:
            *value = 0;
            return;
        case kGicrIidr:
            *value = 0x0100143B;
            return;
        case kGicrTyper:
            // 64-bit read: Affinity_Value (cpu index in Aff0), Last bit
        {
            uint64_t typer = static_cast<uint64_t>(cpu) << 32; // Affinity in [39:32]
            if (cpu == cpu_count_ - 1) typer |= kGicrTyperLast;
            if (size >= 8) {
                *value = typer;
            } else {
                // 32-bit access to lower half
                *value = static_cast<uint32_t>(typer);
            }
        }
            return;
        case kGicrTyper + 4:
            // Upper 32 bits of TYPER
            *value = static_cast<uint32_t>(static_cast<uint64_t>(cpu) >> 0);
            return;
        case kGicrWaker:
            *value = pc.waker_sleep
                ? (kGicrWakerProcessorSleep | kGicrWakerChildrenAsleep)
                : 0;
            return;
        case kGicrStatusr:
            *value = 0;
            return;
        case kGicrPidr2:
            *value = kPidr2ArchGicv3;
            return;
        default:
            return;
        }
    }

    // SGI_base frame (0x10000 .. 0x1FFFF)

    // ISENABLER0
    if (offset == kGicrIsenabler0) {
        uint32_t val = 0;
        for (uint32_t b = 0; b < kSgiPpiCount; b++) {
            if (pc.irq[b].enabled) val |= (1u << b);
        }
        *value = val;
        return;
    }

    // ICENABLER0 — reads same as ISENABLER0
    if (offset == kGicrIcenabler0) {
        RedistRead(cpu, kGicrIsenabler0, size, value);
        return;
    }

    // ISPENDR0
    if (offset == kGicrIspendr0) {
        uint32_t val = 0;
        for (uint32_t b = 0; b < kSgiPpiCount; b++) {
            if (pc.irq[b].pending) val |= (1u << b);
        }
        *value = val;
        return;
    }

    // ICPENDR0
    if (offset == kGicrIcpendr0) {
        RedistRead(cpu, kGicrIspendr0, size, value);
        return;
    }

    // ISACTIVER0
    if (offset == kGicrIsactiver0) {
        uint32_t val = 0;
        for (uint32_t b = 0; b < kSgiPpiCount; b++) {
            if (pc.irq[b].active) val |= (1u << b);
        }
        *value = val;
        return;
    }

    // ICACTIVER0
    if (offset == kGicrIcactiver0) {
        RedistRead(cpu, kGicrIsactiver0, size, value);
        return;
    }

    // IGROUPR0
    if (offset == kGicrIgroupr0) {
        uint32_t val = 0;
        for (uint32_t b = 0; b < kSgiPpiCount; b++) {
            if (pc.irq[b].group == 1) val |= (1u << b);
        }
        *value = val;
        return;
    }

    // IPRIORITYR (0x10400 .. 0x1041F) — 32 bytes, one per SGI/PPI
    if (offset >= kGicrIpriorityr && offset < kGicrIpriorityr + kSgiPpiCount) {
        uint32_t base = static_cast<uint32_t>(offset - kGicrIpriorityr);
        uint32_t val = 0;
        for (uint8_t b = 0; b < size && (base + b) < kSgiPpiCount; b++) {
            val |= static_cast<uint32_t>(pc.irq[base + b].priority) << (b * 8);
        }
        *value = val;
        return;
    }

    // ICFGR0 (SGI config, read-only, always edge)
    if (offset == kGicrIcfgr0) {
        *value = 0xAAAAAAAA; // All SGIs edge-triggered
        return;
    }

    // ICFGR1 (PPI config)
    if (offset == kGicrIcfgr1) {
        uint32_t val = 0;
        for (uint32_t b = 0; b < 16; b++) {
            uint32_t intid = 16 + b;
            if (intid < kSgiPpiCount && pc.irq[intid].edge_cfg)
                val |= (2u << (b * 2));
        }
        *value = val;
        return;
    }

    // IGRPMODR0
    if (offset == kGicrIgrpmodr0) {
        *value = 0;
        return;
    }
}

void SoftGic::RedistWrite(uint32_t cpu, uint64_t offset, uint8_t size, uint64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cpu >= cpu_count_) return;
    auto& pc = per_cpu_[cpu];
    uint32_t val32 = static_cast<uint32_t>(value);

    // RD_base frame
    if (offset < kGicrSgiOffset) {
        switch (offset) {
        case kGicrCtlr:
            return; // Ignore for now
        case kGicrWaker:
            pc.waker_sleep = (val32 & kGicrWakerProcessorSleep) != 0;
            return;
        default:
            return;
        }
    }

    // SGI_base frame

    // ISENABLER0
    if (offset == kGicrIsenabler0) {
        for (uint32_t b = 0; b < kSgiPpiCount; b++) {
            if (val32 & (1u << b)) pc.irq[b].enabled = true;
        }
        UpdateCpu(cpu);
        return;
    }

    // ICENABLER0
    if (offset == kGicrIcenabler0) {
        for (uint32_t b = 0; b < kSgiPpiCount; b++) {
            if (val32 & (1u << b)) pc.irq[b].enabled = false;
        }
        return;
    }

    // ISPENDR0
    if (offset == kGicrIspendr0) {
        for (uint32_t b = 0; b < kSgiPpiCount; b++) {
            if (val32 & (1u << b)) pc.irq[b].pending = true;
        }
        UpdateCpu(cpu);
        return;
    }

    // ICPENDR0
    if (offset == kGicrIcpendr0) {
        for (uint32_t b = 0; b < kSgiPpiCount; b++) {
            if (val32 & (1u << b)) pc.irq[b].pending = false;
        }
        return;
    }

    // ISACTIVER0
    if (offset == kGicrIsactiver0) {
        for (uint32_t b = 0; b < kSgiPpiCount; b++) {
            if (val32 & (1u << b)) pc.irq[b].active = true;
        }
        return;
    }

    // ICACTIVER0
    if (offset == kGicrIcactiver0) {
        for (uint32_t b = 0; b < kSgiPpiCount; b++) {
            if (val32 & (1u << b)) pc.irq[b].active = false;
        }
        UpdateCpu(cpu);
        return;
    }

    // IGROUPR0
    if (offset == kGicrIgroupr0) {
        for (uint32_t b = 0; b < kSgiPpiCount; b++) {
            pc.irq[b].group = (val32 & (1u << b)) ? 1 : 0;
        }
        return;
    }

    // IPRIORITYR
    if (offset >= kGicrIpriorityr && offset < kGicrIpriorityr + kSgiPpiCount) {
        uint32_t base = static_cast<uint32_t>(offset - kGicrIpriorityr);
        for (uint8_t b = 0; b < size && (base + b) < kSgiPpiCount; b++) {
            pc.irq[base + b].priority = static_cast<uint8_t>((value >> (b * 8)) & 0xFF);
        }
        return;
    }

    // ICFGR0 — SGI config is read-only
    if (offset == kGicrIcfgr0) return;

    // ICFGR1 — PPI config
    if (offset == kGicrIcfgr1) {
        for (uint32_t b = 0; b < 16; b++) {
            uint32_t intid = 16 + b;
            if (intid < kSgiPpiCount) {
                pc.irq[intid].edge_cfg = (val32 >> (b * 2 + 1)) & 1;
            }
        }
        return;
    }

    // IGRPMODR0 — ignore
    if (offset == kGicrIgrpmodr0) return;
}

} // namespace gicv3
