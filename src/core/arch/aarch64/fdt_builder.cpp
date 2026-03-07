#include "core/arch/aarch64/fdt_builder.h"

// FDT magic and token constants
static constexpr uint32_t kFdtMagic       = 0xD00DFEED;
static constexpr uint32_t kFdtBeginNode   = 0x00000001;
static constexpr uint32_t kFdtEndNode     = 0x00000002;
static constexpr uint32_t kFdtProp        = 0x00000003;
static constexpr uint32_t kFdtEnd         = 0x00000009;
static constexpr uint32_t kFdtVersion     = 17;
static constexpr uint32_t kFdtLastCompat  = 16;

static void PushU32BE(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >>  8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >>  0) & 0xFF));
}

FdtBuilder::FdtBuilder() {
    struct_buf_.reserve(4096);
    strings_buf_.reserve(512);
}

void FdtBuilder::BeginNode(const std::string& name) {
    PushU32BE(struct_buf_, kFdtBeginNode);
    // Node name is null-terminated and padded to 4-byte boundary
    for (char c : name) {
        struct_buf_.push_back(static_cast<uint8_t>(c));
    }
    struct_buf_.push_back(0);
    PadTo4(struct_buf_);
}

void FdtBuilder::EndNode() {
    PushU32BE(struct_buf_, kFdtEndNode);
}

void FdtBuilder::AddPropertyU32(const std::string& name, uint32_t value) {
    PushU32BE(struct_buf_, kFdtProp);
    PushU32BE(struct_buf_, 4);              // len
    PushU32BE(struct_buf_, AddString(name));
    PushU32BE(struct_buf_, value);
}

void FdtBuilder::AddPropertyU64(const std::string& name, uint64_t value) {
    PushU32BE(struct_buf_, kFdtProp);
    PushU32BE(struct_buf_, 8);
    PushU32BE(struct_buf_, AddString(name));
    PushU32BE(struct_buf_, static_cast<uint32_t>(value >> 32));
    PushU32BE(struct_buf_, static_cast<uint32_t>(value & 0xFFFFFFFF));
}

void FdtBuilder::AddPropertyString(const std::string& name, const std::string& value) {
    uint32_t len = static_cast<uint32_t>(value.size()) + 1;  // include NUL
    PushU32BE(struct_buf_, kFdtProp);
    PushU32BE(struct_buf_, len);
    PushU32BE(struct_buf_, AddString(name));
    for (char c : value) {
        struct_buf_.push_back(static_cast<uint8_t>(c));
    }
    struct_buf_.push_back(0);
    PadTo4(struct_buf_);
}

void FdtBuilder::AddPropertyEmpty(const std::string& name) {
    PushU32BE(struct_buf_, kFdtProp);
    PushU32BE(struct_buf_, 0);
    PushU32BE(struct_buf_, AddString(name));
}

void FdtBuilder::AddPropertyCells(const std::string& name, const std::vector<uint32_t>& cells) {
    uint32_t len = static_cast<uint32_t>(cells.size()) * 4;
    PushU32BE(struct_buf_, kFdtProp);
    PushU32BE(struct_buf_, len);
    PushU32BE(struct_buf_, AddString(name));
    for (uint32_t cell : cells) {
        PushU32BE(struct_buf_, cell);
    }
}

void FdtBuilder::AddPropertyBytes(const std::string& name, const uint8_t* data, size_t len) {
    PushU32BE(struct_buf_, kFdtProp);
    PushU32BE(struct_buf_, static_cast<uint32_t>(len));
    PushU32BE(struct_buf_, AddString(name));
    struct_buf_.insert(struct_buf_.end(), data, data + len);
    PadTo4(struct_buf_);
}

std::vector<uint8_t> FdtBuilder::Finish() {
    PushU32BE(struct_buf_, kFdtEnd);

    // FDT header is 40 bytes
    constexpr uint32_t kHeaderSize = 40;
    // Memory reservation block is empty (just a terminating 0,0 entry)
    constexpr uint32_t kMemRsvMapSize = 16;

    uint32_t off_dt_struct = kHeaderSize + kMemRsvMapSize;
    uint32_t off_dt_strings = off_dt_struct + static_cast<uint32_t>(struct_buf_.size());
    uint32_t total_size = off_dt_strings + static_cast<uint32_t>(strings_buf_.size());

    std::vector<uint8_t> dtb;
    dtb.reserve(total_size);

    // FDT header
    PushU32BE(dtb, kFdtMagic);
    PushU32BE(dtb, total_size);
    PushU32BE(dtb, off_dt_struct);
    PushU32BE(dtb, off_dt_strings);
    PushU32BE(dtb, kHeaderSize);  // off_mem_rsvmap
    PushU32BE(dtb, kFdtVersion);
    PushU32BE(dtb, kFdtLastCompat);
    PushU32BE(dtb, 0);            // boot_cpuid_phys
    PushU32BE(dtb, static_cast<uint32_t>(strings_buf_.size()));
    PushU32BE(dtb, static_cast<uint32_t>(struct_buf_.size()));

    // Memory reservation map (empty — just terminator)
    PushU32BE(dtb, 0); PushU32BE(dtb, 0);  // address = 0
    PushU32BE(dtb, 0); PushU32BE(dtb, 0);  // size = 0

    // Structure block
    dtb.insert(dtb.end(), struct_buf_.begin(), struct_buf_.end());

    // Strings block
    dtb.insert(dtb.end(), strings_buf_.begin(), strings_buf_.end());

    return dtb;
}

void FdtBuilder::PadTo4(std::vector<uint8_t>& buf) {
    while (buf.size() % 4 != 0) {
        buf.push_back(0);
    }
}

uint32_t FdtBuilder::AddString(const std::string& s) {
    // Check if this string already exists in the strings buffer
    size_t existing = 0;
    while (existing < strings_buf_.size()) {
        const char* candidate = reinterpret_cast<const char*>(&strings_buf_[existing]);
        if (s == candidate) {
            return static_cast<uint32_t>(existing);
        }
        existing += strlen(candidate) + 1;
    }

    uint32_t offset = static_cast<uint32_t>(strings_buf_.size());
    for (char c : s) {
        strings_buf_.push_back(static_cast<uint8_t>(c));
    }
    strings_buf_.push_back(0);
    return offset;
}
