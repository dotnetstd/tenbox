#include "platform/macos/hypervisor/hvf_mmio_decode.h"
#include "core/vmm/types.h"

namespace hvf {

// ARM64 instruction encoding helpers
static inline uint32_t ExtractBits(uint32_t insn, int hi, int lo) {
    return (insn >> lo) & ((1u << (hi - lo + 1)) - 1);
}

static uint64_t ReadReg(const uint64_t* regs, uint8_t reg) {
    if (reg == 31) return 0;  // XZR
    return regs[reg];
}

bool DecodeMmioInstruction(uint32_t insn, uint64_t syndrome,
                           const uint64_t* regs, MmioDecodeResult* result) {
    // The ISS field of ESR_EL2 for DABT tells us if the syndrome is valid.
    // ISV (bit 24) indicates if the instruction syndrome is valid.
    bool isv = (syndrome >> 24) & 1;

    if (isv) {
        // ISS-based fast path: extract access info directly from syndrome
        result->is_write = (syndrome >> 6) & 1;   // WnR
        uint8_t sas = (syndrome >> 22) & 3;        // SAS: access size
        result->access_size = 1u << sas;            // 0->1, 1->2, 2->4, 3->8
        result->reg = (syndrome >> 16) & 0x1F;     // SRT: transfer register
        result->is_pair = false;
        result->reg2 = 0;
        result->write_value2 = 0;

        if (result->is_write) {
            result->write_value = ReadReg(regs, result->reg);
        }
        return true;
    }

    // ISV not set — must decode the instruction manually.
    // This happens for LDP/STP and some other complex encodings.

    result->is_pair = false;
    result->reg2 = 0;
    result->write_value2 = 0;

    // LDP/STP (Load/Store Pair) — opc=x0, bits [31:30][29:27]=x01, bit 26=0
    uint32_t op0 = ExtractBits(insn, 31, 30);
    uint32_t bits_29_27 = ExtractBits(insn, 29, 27);

    if (bits_29_27 == 0b101 && ((insn >> 26) & 1) == 0) {
        // Load/Store Pair: C4.1.1 encoding
        bool is_load = (insn >> 22) & 1;   // L bit
        uint8_t rt = insn & 0x1F;
        uint8_t rt2 = ExtractBits(insn, 14, 10);

        uint8_t size;
        if ((op0 & 2) == 0) {
            size = 4;  // 32-bit pair
        } else {
            size = 8;  // 64-bit pair
        }

        result->is_write = !is_load;
        result->access_size = size;
        result->reg = rt;
        result->is_pair = true;
        result->reg2 = rt2;

        if (result->is_write) {
            result->write_value = ReadReg(regs, rt);
            result->write_value2 = ReadReg(regs, rt2);
        }
        return true;
    }

    // LDR/STR (immediate, unsigned offset) — bits [31:30]=size, [29:27]=111,
    // [26]=0, [25:24]=01, [23:22]=opc
    uint32_t bits_29_24 = ExtractBits(insn, 29, 24);
    if ((bits_29_24 & 0b111011) == 0b111001) {
        // LDR/STR unsigned immediate
        uint8_t size_bits = ExtractBits(insn, 31, 30);
        uint8_t opc = ExtractBits(insn, 23, 22);
        uint8_t rt = insn & 0x1F;

        result->access_size = 1u << size_bits;
        result->is_write = (opc == 0);  // opc=00 is STR
        result->reg = rt;

        if (result->is_write) {
            result->write_value = ReadReg(regs, rt);
        }
        return true;
    }

    // LDR/STR (register, pre/post-index, unscaled) — bits [29:27]=111,
    // [26]=0, [25:24]=00
    if ((bits_29_24 & 0b111011) == 0b111000) {
        uint8_t size_bits = ExtractBits(insn, 31, 30);
        uint8_t opc = ExtractBits(insn, 23, 22);
        uint8_t rt = insn & 0x1F;

        result->access_size = 1u << size_bits;
        result->is_write = (opc == 0);
        result->reg = rt;

        if (result->is_write) {
            result->write_value = ReadReg(regs, rt);
        }
        return true;
    }

    LOG_WARN("MMIO decode: unsupported instruction 0x%08x, syndrome 0x%llx",
             insn, (unsigned long long)syndrome);
    return false;
}

} // namespace hvf
