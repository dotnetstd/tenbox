#pragma once

#include <cstdint>

// ARM GICv3 register definitions.
// References: ARM IHI 0069H (GIC Architecture Specification v3/v4).

namespace gicv3 {

// ---------------------------------------------------------------------------
// GICD — Distributor registers (manages SPIs, INTID 32–1019)
// ---------------------------------------------------------------------------
static constexpr uint64_t kGicdCtlr          = 0x0000;
static constexpr uint64_t kGicdTyper         = 0x0004;
static constexpr uint64_t kGicdIidr          = 0x0008;
static constexpr uint64_t kGicdStatusr       = 0x0010;
static constexpr uint64_t kGicdIgroupBase    = 0x0080;
static constexpr uint64_t kGicdIsenabler     = 0x0100;
static constexpr uint64_t kGicdIcenabler     = 0x0180;
static constexpr uint64_t kGicdIspendr       = 0x0200;
static constexpr uint64_t kGicdIcpendr       = 0x0280;
static constexpr uint64_t kGicdIsactiver     = 0x0300;
static constexpr uint64_t kGicdIcactiver     = 0x0380;
static constexpr uint64_t kGicdIpriorityr    = 0x0400;
static constexpr uint64_t kGicdIcfgr         = 0x0C00;
static constexpr uint64_t kGicdIgrpmodr      = 0x0D00;
static constexpr uint64_t kGicdIrouter       = 0x6000;

// Peripheral ID registers (used by Linux driver to detect GIC version)
static constexpr uint64_t kGicdPidr2         = 0xFFE8;
// PIDR2 value: ArchRev[7:4]=0x3 means GICv3
static constexpr uint32_t kPidr2ArchGicv3    = 0x30;

// GICD_CTLR bits
static constexpr uint32_t kGicdCtlrEnGrp1Ns  = 1u << 1;
static constexpr uint32_t kGicdCtlrAre       = 1u << 4;  // ARE_NS (affinity routing)
static constexpr uint32_t kGicdCtlrRwp       = 1u << 31; // register write pending

// GICD_TYPER fields
static constexpr uint32_t kGicdTyperItLines(uint32_t n) {
    return (n & 0x1F);
}
static constexpr uint32_t kGicdTyperCpuNum(uint32_t n) {
    return ((n & 0x7) << 5);
}

// Max SPI count we support (INTID 32..127 → 96 SPIs, i.e. ITLinesNumber = 2)
static constexpr uint32_t kMaxSpi      = 128;
static constexpr uint32_t kSpiBase     = 32;
static constexpr uint32_t kMaxCpus     = 8;

// Distributor MMIO size
static constexpr uint64_t kGicdSize    = 0x10000;

// ---------------------------------------------------------------------------
// GICR — Redistributor registers (per-CPU, manages SGIs/PPIs INTID 0–31)
// ---------------------------------------------------------------------------

// RD_base frame (first 64 KiB of each redistributor)
static constexpr uint64_t kGicrCtlr         = 0x0000;
static constexpr uint64_t kGicrIidr         = 0x0004;
static constexpr uint64_t kGicrTyper        = 0x0008;  // 64-bit
static constexpr uint64_t kGicrStatusr      = 0x0010;
static constexpr uint64_t kGicrWaker        = 0x0014;
static constexpr uint64_t kGicrPropbaser    = 0x0070;
static constexpr uint64_t kGicrPendbaser    = 0x0078;

// GICR_WAKER bits
static constexpr uint32_t kGicrWakerProcessorSleep = 1u << 1;
static constexpr uint32_t kGicrWakerChildrenAsleep = 1u << 2;

// GICR_TYPER bits
static constexpr uint64_t kGicrTyperLast    = 1ull << 4;

static constexpr uint64_t kGicrPidr2        = 0xFFE8;

// SGI_base frame (second 64 KiB, offset 0x10000 from redistributor base)
static constexpr uint64_t kGicrSgiOffset    = 0x10000;
static constexpr uint64_t kGicrIgroupr0     = kGicrSgiOffset + 0x0080;
static constexpr uint64_t kGicrIsenabler0   = kGicrSgiOffset + 0x0100;
static constexpr uint64_t kGicrIcenabler0   = kGicrSgiOffset + 0x0180;
static constexpr uint64_t kGicrIspendr0     = kGicrSgiOffset + 0x0200;
static constexpr uint64_t kGicrIcpendr0     = kGicrSgiOffset + 0x0280;
static constexpr uint64_t kGicrIsactiver0   = kGicrSgiOffset + 0x0300;
static constexpr uint64_t kGicrIcactiver0   = kGicrSgiOffset + 0x0380;
static constexpr uint64_t kGicrIpriorityr   = kGicrSgiOffset + 0x0400;
static constexpr uint64_t kGicrIcfgr0       = kGicrSgiOffset + 0x0C00;
static constexpr uint64_t kGicrIcfgr1       = kGicrSgiOffset + 0x0C04;
static constexpr uint64_t kGicrIgrpmodr0    = kGicrSgiOffset + 0x0D00;

// Each redistributor occupies 2 x 64 KiB frames (RD_base + SGI_base)
static constexpr uint64_t kGicrSizePerCpu   = 0x20000;

// Number of SGI/PPI interrupts per CPU
static constexpr uint32_t kSgiPpiCount      = 32;

// ---------------------------------------------------------------------------
// ICC — CPU Interface system registers (accessed via MRS/MSR, trapped EC=0x18)
// ---------------------------------------------------------------------------
// Encoding: Op0=3, Op1, CRn, CRm, Op2  →  packed as (Op0<<16|Op1<<14|CRn<<10|CRm<<1|Op2)
// But we match against the ISS encoding from ESR_EL2 syndrome for EC=0x18:
//   ISS[21:20] = Op0, ISS[19:17] = Op1 (3 bits), ISS[16:14] = Op2 (3 bits),
//   ISS[13:10] = CRn, ISS[4:1] = CRm
// We define a helper to encode the (Op0, Op1, CRn, CRm, Op2) tuple.

static constexpr uint32_t IccSysRegEncode(
    uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) {
    // ESR_EL2 ISS for MSR/MRS (EC=0x18), per ARM DDI 0487:
    //   bits [21:20] = Op0
    //   bits [19:17] = Op2
    //   bits [16:14] = Op1
    //   bits [13:10] = CRn
    //   bits  [4:1]  = CRm
    return ((op0 & 3) << 20) | ((op2 & 7) << 17) | ((op1 & 7) << 14) |
           ((crn & 0xF) << 10) | ((crm & 0xF) << 1);
}

// Mask for the register encoding fields in ESR ISS (exclude Rt and direction)
static constexpr uint32_t kIccSysRegMask = 0x003FFC1E;

// ICC register encodings (Op0=3 for all)
static constexpr uint32_t kIccIar0El1     = IccSysRegEncode(3, 0, 12, 8, 0);
static constexpr uint32_t kIccIar1El1     = IccSysRegEncode(3, 0, 12, 12, 0);
static constexpr uint32_t kIccEoir0El1    = IccSysRegEncode(3, 0, 12, 8, 1);
static constexpr uint32_t kIccEoir1El1    = IccSysRegEncode(3, 0, 12, 12, 1);
static constexpr uint32_t kIccHppir0El1   = IccSysRegEncode(3, 0, 12, 8, 2);
static constexpr uint32_t kIccHppir1El1   = IccSysRegEncode(3, 0, 12, 12, 2);
static constexpr uint32_t kIccBpr0El1     = IccSysRegEncode(3, 0, 12, 8, 3);
static constexpr uint32_t kIccBpr1El1     = IccSysRegEncode(3, 0, 12, 12, 3);
static constexpr uint32_t kIccCtlrEl1     = IccSysRegEncode(3, 0, 12, 12, 4);
static constexpr uint32_t kIccSreEl1      = IccSysRegEncode(3, 0, 12, 12, 5);
static constexpr uint32_t kIccIgrpen0El1  = IccSysRegEncode(3, 0, 12, 12, 6);
static constexpr uint32_t kIccIgrpen1El1  = IccSysRegEncode(3, 0, 12, 12, 7);
static constexpr uint32_t kIccPmrEl1      = IccSysRegEncode(3, 0, 4, 6, 0);
static constexpr uint32_t kIccDirEl1      = IccSysRegEncode(3, 0, 12, 11, 1);
static constexpr uint32_t kIccRprEl1      = IccSysRegEncode(3, 0, 12, 11, 3);
static constexpr uint32_t kIccSgi1rEl1    = IccSysRegEncode(3, 0, 12, 11, 5);

// Special INTID values
static constexpr uint32_t kIntidSpurious  = 1023;

// Default priority (lower value = higher priority)
static constexpr uint8_t kPriorityDefault = 0xA0;
static constexpr uint8_t kPriorityIdle    = 0xFF;

} // namespace gicv3
