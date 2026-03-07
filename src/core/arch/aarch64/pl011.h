#pragma once

#include "core/device/device.h"
#include <mutex>
#include <array>
#include <functional>

// ARM PrimeCell PL011 UART (MMIO-based serial port).
// Used as the console on aarch64 VMs (ttyAMA0).
class Pl011 : public Device {
public:
    static constexpr uint64_t kMmioSize = 0x1000;

    using IrqCallback = std::function<void()>;
    using IrqLevelCallback = std::function<void(bool asserted)>;
    using TxCallback = std::function<void(uint8_t)>;
    void SetIrqCallback(IrqCallback cb) { irq_callback_ = std::move(cb); }
    void SetIrqLevelCallback(IrqLevelCallback cb) { irq_level_callback_ = std::move(cb); }
    void SetTxCallback(TxCallback cb) { tx_callback_ = std::move(cb); }

    void MmioRead(uint64_t offset, uint8_t size, uint64_t* value) override;
    void MmioWrite(uint64_t offset, uint8_t size, uint64_t value) override;

    void PushInput(uint8_t byte);
    void CheckAndRaiseIrq();

private:
    // PL011 register offsets
    static constexpr uint64_t kDR    = 0x000;  // Data Register
    static constexpr uint64_t kFR    = 0x018;  // Flag Register
    static constexpr uint64_t kIBRD  = 0x024;  // Integer Baud Rate
    static constexpr uint64_t kFBRD  = 0x028;  // Fractional Baud Rate
    static constexpr uint64_t kLCRH  = 0x02C;  // Line Control
    static constexpr uint64_t kCR    = 0x030;  // Control Register
    static constexpr uint64_t kIFLS  = 0x034;  // Interrupt FIFO Level Select
    static constexpr uint64_t kIMSC  = 0x038;  // Interrupt Mask Set/Clear
    static constexpr uint64_t kRIS   = 0x03C;  // Raw Interrupt Status
    static constexpr uint64_t kMIS   = 0x040;  // Masked Interrupt Status
    static constexpr uint64_t kICR   = 0x044;  // Interrupt Clear
    static constexpr uint64_t kPERIPH_ID0 = 0xFE0;
    static constexpr uint64_t kPERIPH_ID1 = 0xFE4;
    static constexpr uint64_t kPERIPH_ID2 = 0xFE8;
    static constexpr uint64_t kPERIPH_ID3 = 0xFEC;
    static constexpr uint64_t kCELL_ID0   = 0xFF0;
    static constexpr uint64_t kCELL_ID1   = 0xFF4;
    static constexpr uint64_t kCELL_ID2   = 0xFF8;
    static constexpr uint64_t kCELL_ID3   = 0xFFC;

    // Flag Register bits
    static constexpr uint32_t kFrTxfe = (1 << 7);  // TX FIFO empty
    static constexpr uint32_t kFrRxff = (1 << 6);  // RX FIFO full
    static constexpr uint32_t kFrTxff = (1 << 5);  // TX FIFO full
    static constexpr uint32_t kFrRxfe = (1 << 4);  // RX FIFO empty
    static constexpr uint32_t kFrBusy = (1 << 3);  // UART busy

    // Interrupt bits
    static constexpr uint32_t kIrqRx = (1 << 4);   // RX interrupt
    static constexpr uint32_t kIrqTx = (1 << 5);   // TX interrupt

    static constexpr size_t kFifoSize = 256;

    uint32_t cr_ = 0x0300;   // UART enable, TX enable, RX enable
    uint32_t lcr_h_ = 0;
    uint32_t ibrd_ = 0;
    uint32_t fbrd_ = 0;
    uint32_t ifls_ = 0;
    uint32_t imsc_ = 0;      // Interrupt mask
    uint32_t ris_ = 0;       // Raw interrupt status

    IrqCallback irq_callback_;
    IrqLevelCallback irq_level_callback_;
    TxCallback tx_callback_;

    mutable std::mutex rx_mutex_;
    std::array<uint8_t, kFifoSize> rx_buf_{};
    size_t rx_head_ = 0;
    size_t rx_tail_ = 0;
    size_t rx_count_ = 0;

    uint8_t PopRx();
    void UpdateIrq();
};
