#include "core/arch/aarch64/pl011.h"

void Pl011::MmioRead(uint64_t offset, uint8_t /*size*/, uint64_t* value) {
    switch (offset) {
    case kDR: {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        if (rx_count_ > 0) {
            *value = PopRx();
            if (rx_count_ == 0) {
                ris_ &= ~kIrqRx;
            }
        } else {
            *value = 0;
        }
        UpdateIrq();
        break;
    }
    case kFR: {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        uint32_t flags = kFrTxfe;  // TX always empty (instant TX)
        if (rx_count_ == 0) flags |= kFrRxfe;
        if (rx_count_ >= kFifoSize) flags |= kFrRxff;
        *value = flags;
        break;
    }
    case kCR:      *value = cr_; break;
    case kLCRH:    *value = lcr_h_; break;
    case kIBRD:    *value = ibrd_; break;
    case kFBRD:    *value = fbrd_; break;
    case kIFLS:    *value = ifls_; break;
    case kIMSC:    *value = imsc_; break;
    case kRIS:     *value = ris_; break;
    case kMIS:     *value = ris_ & imsc_; break;

    // PrimeCell identification
    case kPERIPH_ID0: *value = 0x11; break;
    case kPERIPH_ID1: *value = 0x10; break;
    case kPERIPH_ID2: *value = 0x14; break;  // r1p4
    case kPERIPH_ID3: *value = 0x00; break;
    case kCELL_ID0:   *value = 0x0D; break;
    case kCELL_ID1:   *value = 0xF0; break;
    case kCELL_ID2:   *value = 0x05; break;
    case kCELL_ID3:   *value = 0xB1; break;

    default:
        *value = 0;
        break;
    }
}

void Pl011::MmioWrite(uint64_t offset, uint8_t /*size*/, uint64_t value) {
    switch (offset) {
    case kDR:
        if (tx_callback_) {
            tx_callback_(static_cast<uint8_t>(value & 0xFF));
        }
        ris_ |= kIrqTx;
        UpdateIrq();
        break;
    case kCR:    cr_ = static_cast<uint32_t>(value); break;
    case kLCRH:  lcr_h_ = static_cast<uint32_t>(value); break;
    case kIBRD:  ibrd_ = static_cast<uint32_t>(value); break;
    case kFBRD:  fbrd_ = static_cast<uint32_t>(value); break;
    case kIFLS:  ifls_ = static_cast<uint32_t>(value); break;
    case kIMSC:
        imsc_ = static_cast<uint32_t>(value);
        UpdateIrq();
        break;
    case kICR:
        ris_ &= ~static_cast<uint32_t>(value);
        UpdateIrq();
        break;
    default:
        break;
    }
}

void Pl011::PushInput(uint8_t byte) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_count_ >= kFifoSize) return;

    rx_buf_[rx_tail_] = byte;
    rx_tail_ = (rx_tail_ + 1) % kFifoSize;
    rx_count_++;

    ris_ |= kIrqRx;
    UpdateIrq();
}

void Pl011::CheckAndRaiseIrq() {
    UpdateIrq();
}

uint8_t Pl011::PopRx() {
    if (rx_count_ == 0) return 0;
    uint8_t byte = rx_buf_[rx_head_];
    rx_head_ = (rx_head_ + 1) % kFifoSize;
    rx_count_--;
    return byte;
}

void Pl011::UpdateIrq() {
    uint32_t mis = ris_ & imsc_;
    if (irq_level_callback_)
        irq_level_callback_(mis != 0);
    else if (mis && irq_callback_)
        irq_callback_();
}
