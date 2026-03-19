#include "core/disk/qcow2.h"
#include <cstring>
#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <intrin.h>
#include <io.h>       // _commit, _fileno
#else
#include <unistd.h>   // fsync, fileno
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

// zlib for compressed cluster support
#include <zlib.h>
// zstd for qcow2 zstd compressed cluster support
#include <zstd.h>

// ---------- byte-swap helpers (big-endian on disk) ----------

uint16_t Qcow2DiskImage::Be16(uint16_t v) {
#ifdef _WIN32
    return _byteswap_ushort(v);
#else
    return __builtin_bswap16(v);
#endif
}

uint32_t Qcow2DiskImage::Be32(uint32_t v) {
#ifdef _WIN32
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}

uint64_t Qcow2DiskImage::Be64(uint64_t v) {
#ifdef _WIN32
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}

// ---------- lifecycle ----------

Qcow2DiskImage::~Qcow2DiskImage() {
    if (file_) {
        Flush();
        fclose(file_);
        file_ = nullptr;
    }
}

bool Qcow2DiskImage::Open(const std::string& path) {
    file_ = fopen(path.c_str(), "r+b");
    if (!file_) {
        LOG_ERROR("Qcow2: failed to open %s", path.c_str());
        return false;
    }

    if (!ReadHeader()) {
        fclose(file_);
        file_ = nullptr;
        return false;
    }

    if (!ReadL1Table()) {
        fclose(file_);
        file_ = nullptr;
        return false;
    }

    if (!ReadRefcountTable()) {
        fclose(file_);
        file_ = nullptr;
        return false;
    }

    // Determine file end for physical file extension tracking
    _fseeki64(file_, 0, SEEK_END);
    file_end_ = static_cast<uint64_t>(_ftelli64(file_));
    // Align to cluster boundary
    file_end_ = (file_end_ + cluster_size_ - 1) & ~(static_cast<uint64_t>(cluster_size_) - 1);

    LOG_INFO("Qcow2: %s, version %u, cluster_size %u, virtual_size %llu MB, "
             "l1_size %u, refcount_table 0x%llX (%u clusters), "
             "file_end 0x%llX, compression %s",
             path.c_str(), version_, cluster_size_,
             virtual_size_ / (1024 * 1024), l1_size_,
             refcount_table_offset_, refcount_table_clusters_,
             file_end_,
             compression_type_ == 1 ? "zstd" : "zlib");
    return true;
}

// ---------- header parsing ----------

bool Qcow2DiskImage::ReadHeader() {
    Qcow2Header hdr{};
    _fseeki64(file_, 0, SEEK_SET);
    if (fread(&hdr, 1, sizeof(hdr), file_) < 72) {
        LOG_ERROR("Qcow2: header too short");
        return false;
    }

    if (Be32(hdr.magic) != kQcow2Magic) {
        LOG_ERROR("Qcow2: bad magic 0x%08X", Be32(hdr.magic));
        return false;
    }

    version_ = Be32(hdr.version);
    if (version_ != 2 && version_ != 3) {
        LOG_ERROR("Qcow2: unsupported version %u", version_);
        return false;
    }

    if (Be64(hdr.backing_file_offset) != 0) {
        LOG_ERROR("Qcow2: backing files not supported");
        return false;
    }

    if (Be32(hdr.crypt_method) != 0) {
        LOG_ERROR("Qcow2: encrypted images not supported");
        return false;
    }

    cluster_bits_ = Be32(hdr.cluster_bits);
    if (cluster_bits_ < 9 || cluster_bits_ > 21) {
        LOG_ERROR("Qcow2: invalid cluster_bits %u", cluster_bits_);
        return false;
    }

    cluster_size_ = 1u << cluster_bits_;
    l2_entries_ = cluster_size_ / 8;  // each L2 entry is 8 bytes
    virtual_size_ = Be64(hdr.size);
    l1_size_ = Be32(hdr.l1_size);
    l1_table_offset_ = Be64(hdr.l1_table_offset);

    refcount_table_offset_ = Be64(hdr.refcount_table_offset);
    refcount_table_clusters_ = Be32(hdr.refcount_table_clusters);
    rfb_entries_ = cluster_size_ * 8 / 16;  // 16-bit refcounts

    // Read compression_type for qcow2 v3 (offset 104, requires header_length >= 108)
    compression_type_ = 0;  // default: zlib (DEFLATE)
    if (version_ == 3) {
        uint32_t header_length = Be32(hdr.header_length);
        if (header_length >= 108) {
            uint8_t comp_type = 0;
            _fseeki64(file_, 104, SEEK_SET);
            if (fread(&comp_type, 1, 1, file_) == 1) {
                compression_type_ = comp_type;
                if (compression_type_ > 1) {
                    LOG_ERROR("Qcow2: unsupported compression_type %u", compression_type_);
                    return false;
                }
            }
        }
    }

    return true;
}

bool Qcow2DiskImage::ReadL1Table() {
    l1_table_.resize(l1_size_);
    _fseeki64(file_, l1_table_offset_, SEEK_SET);
    size_t bytes = l1_size_ * sizeof(uint64_t);
    if (fread(l1_table_.data(), 1, bytes, file_) != bytes) {
        LOG_ERROR("Qcow2: failed to read L1 table (%u entries at 0x%llX)",
                  l1_size_, l1_table_offset_);
        return false;
    }

    // Convert from big-endian to host byte order
    for (uint32_t i = 0; i < l1_size_; i++) {
        l1_table_[i] = Be64(l1_table_[i]);
    }
    return true;
}

bool Qcow2DiskImage::ReadRefcountTable() {
    size_t table_bytes = static_cast<size_t>(refcount_table_clusters_) * cluster_size_;
    size_t table_entries = table_bytes / sizeof(uint64_t);
    refcount_table_.resize(table_entries);

    _fseeki64(file_, refcount_table_offset_, SEEK_SET);
    if (fread(refcount_table_.data(), 1, table_bytes, file_) != table_bytes) {
        LOG_ERROR("Qcow2: failed to read refcount table at 0x%llX (%zu bytes)",
                  refcount_table_offset_, table_bytes);
        return false;
    }

    for (size_t i = 0; i < table_entries; i++) {
        refcount_table_[i] = Be64(refcount_table_[i]);
    }
    return true;
}

// ---------- L2 cache ----------

uint64_t* Qcow2DiskImage::GetL2Table(uint64_t l2_offset) {
    auto it = l2_map_.find(l2_offset);
    if (it != l2_map_.end()) {
        // Move to front (most recently used)
        l2_lru_.splice(l2_lru_.begin(), l2_lru_, it->second);
        return it->second->data.data();
    }

    // Evict if cache full
    while (l2_lru_.size() >= kL2CacheMax) {
        EvictL2Cache();
    }

    // Read from disk
    L2CacheEntry entry;
    entry.l2_offset = l2_offset;
    entry.data.resize(l2_entries_);
    entry.dirty = false;

    _fseeki64(file_, l2_offset, SEEK_SET);
    size_t bytes = l2_entries_ * sizeof(uint64_t);
    if (fread(entry.data.data(), 1, bytes, file_) != bytes) {
        LOG_ERROR("Qcow2: failed to read L2 table at 0x%llX", l2_offset);
        return nullptr;
    }

    // Convert to host byte order
    for (uint32_t i = 0; i < l2_entries_; i++) {
        entry.data[i] = Be64(entry.data[i]);
    }

    l2_lru_.push_front(std::move(entry));
    l2_map_[l2_offset] = l2_lru_.begin();
    return l2_lru_.front().data.data();
}

void Qcow2DiskImage::EvictL2Cache() {
    if (l2_lru_.empty()) return;

    auto& victim = l2_lru_.back();
    if (victim.dirty) {
        // Write back to disk in big-endian
        std::vector<uint64_t> be_data(l2_entries_);
        for (uint32_t i = 0; i < l2_entries_; i++) {
            be_data[i] = Be64(victim.data[i]);
        }
        _fseeki64(file_, victim.l2_offset, SEEK_SET);
        fwrite(be_data.data(), 1, l2_entries_ * sizeof(uint64_t), file_);
    }

    l2_map_.erase(victim.l2_offset);
    l2_lru_.pop_back();
}

// ---------- refcount block cache ----------

uint16_t* Qcow2DiskImage::GetRefcountBlock(uint64_t cluster_index,
                                             uint32_t* rfb_index,
                                             bool allocate) {
    uint64_t rft_index = cluster_index / rfb_entries_;
    *rfb_index = static_cast<uint32_t>(cluster_index % rfb_entries_);

    if (rft_index >= refcount_table_.size()) {
        return nullptr;
    }

    uint64_t block_offset = refcount_table_[rft_index];

    if (block_offset == 0) {
        if (!allocate) return nullptr;

        // Use this cluster position as the refcount block itself
        block_offset = static_cast<uint64_t>(cluster_index) << cluster_bits_;

        // Extend file if needed
        if (block_offset + cluster_size_ > file_end_) {
            file_end_ = block_offset + cluster_size_;
        }
        std::vector<uint8_t> zeros(cluster_size_, 0);
        _fseeki64(file_, block_offset, SEEK_SET);
        fwrite(zeros.data(), 1, cluster_size_, file_);

        while (rfb_lru_.size() >= kRfbCacheMax) {
            EvictRfbCache();
        }

        RfbCacheEntry entry;
        entry.offset_in_file = block_offset;
        entry.data.resize(rfb_entries_, 0);
        entry.data[*rfb_index] = 1;  // self-referencing: the block itself uses this cluster
        entry.dirty = true;

        rfb_lru_.push_front(std::move(entry));
        rfb_map_[block_offset] = rfb_lru_.begin();

        refcount_table_[rft_index] = block_offset;
        refcount_table_dirty_ = true;

        return rfb_lru_.front().data.data();
    }

    // Look up in cache
    auto it = rfb_map_.find(block_offset);
    if (it != rfb_map_.end()) {
        rfb_lru_.splice(rfb_lru_.begin(), rfb_lru_, it->second);
        return it->second->data.data();
    }

    while (rfb_lru_.size() >= kRfbCacheMax) {
        EvictRfbCache();
    }

    RfbCacheEntry entry;
    entry.offset_in_file = block_offset;
    entry.data.resize(rfb_entries_);
    entry.dirty = false;

    _fseeki64(file_, block_offset, SEEK_SET);
    size_t bytes = rfb_entries_ * sizeof(uint16_t);
    if (fread(entry.data.data(), 1, bytes, file_) != bytes) {
        LOG_ERROR("Qcow2: failed to read refcount block at 0x%llX", block_offset);
        return nullptr;
    }

    for (uint32_t i = 0; i < rfb_entries_; i++) {
        entry.data[i] = Be16(entry.data[i]);
    }

    rfb_lru_.push_front(std::move(entry));
    rfb_map_[block_offset] = rfb_lru_.begin();
    return rfb_lru_.front().data.data();
}

void Qcow2DiskImage::EvictRfbCache() {
    if (rfb_lru_.empty()) return;

    auto& victim = rfb_lru_.back();
    if (victim.dirty) {
        std::vector<uint16_t> be_data(rfb_entries_);
        for (uint32_t i = 0; i < rfb_entries_; i++) {
            be_data[i] = Be16(victim.data[i]);
        }
        _fseeki64(file_, victim.offset_in_file, SEEK_SET);
        fwrite(be_data.data(), 1, rfb_entries_ * sizeof(uint16_t), file_);
    }

    rfb_map_.erase(victim.offset_in_file);
    rfb_lru_.pop_back();
}

void Qcow2DiskImage::FlushRefcountTable() {
    std::vector<uint64_t> be_table(refcount_table_.size());
    for (size_t i = 0; i < refcount_table_.size(); i++) {
        be_table[i] = Be64(refcount_table_[i]);
    }
    _fseeki64(file_, refcount_table_offset_, SEEK_SET);
    fwrite(be_table.data(), 1, be_table.size() * sizeof(uint64_t), file_);
    refcount_table_dirty_ = false;
}

// ---------- offset resolution ----------

uint64_t Qcow2DiskImage::ResolveOffset(uint64_t virt_offset, bool* compressed,
                                         uint64_t* comp_host_off,
                                         uint32_t* comp_size) {
    *compressed = false;
    *comp_host_off = 0;
    *comp_size = 0;

    uint32_t l1_idx = static_cast<uint32_t>(
        virt_offset / (static_cast<uint64_t>(l2_entries_) * cluster_size_));
    uint32_t l2_idx = static_cast<uint32_t>(
        (virt_offset / cluster_size_) % l2_entries_);

    if (l1_idx >= l1_size_) return 0;

    uint64_t l1_entry = l1_table_[l1_idx];
    if (l1_entry == 0) return 0;

    uint64_t l2_table_off = l1_entry & kOffsetMask;
    uint64_t* l2 = GetL2Table(l2_table_off);
    if (!l2) return 0;

    uint64_t l2_entry = l2[l2_idx];
    if (l2_entry == 0) return 0;

    if (l2_entry & kCompressedBit) {
        *compressed = true;
        // For compressed clusters (QEMU qcow2 format):
        // csize_shift = 62 - (cluster_bits - 8)
        // Bits 0 to (csize_shift - 1): host offset
        // Bits csize_shift to 61: compressed sectors - 1
        // Bit 62: compressed flag
        // Bit 63: copied flag (unused for compressed)
        uint32_t csize_shift = 62 - (cluster_bits_ - 8);
        uint64_t csize_mask = (1ULL << (cluster_bits_ - 8)) - 1;
        uint64_t offset_mask = (1ULL << csize_shift) - 1;

        uint32_t nb_csectors = static_cast<uint32_t>(
            ((l2_entry >> csize_shift) & csize_mask) + 1);
        uint64_t host_off = l2_entry & offset_mask;

        *comp_host_off = host_off;
        *comp_size = nb_csectors * 512;
        return 0;  // caller must use compressed path
    }

    return l2_entry & kOffsetMask;
}

// ---------- cluster I/O ----------

bool Qcow2DiskImage::ReadCluster(uint64_t host_off, uint64_t in_cluster_off,
                                   void* buf, uint32_t len) {
    _fseeki64(file_, host_off + in_cluster_off, SEEK_SET);
    return fread(buf, 1, len, file_) == len;
}

bool Qcow2DiskImage::ReadCompressedCluster(uint64_t comp_host_off,
                                             uint32_t comp_size,
                                             uint64_t in_cluster_off,
                                             void* buf, uint32_t len) {
    // Read compressed data
    std::vector<uint8_t> comp_buf(comp_size);
    _fseeki64(file_, comp_host_off, SEEK_SET);
    if (fread(comp_buf.data(), 1, comp_size, file_) != comp_size) {
        LOG_ERROR("Qcow2: failed to read compressed data at 0x%llX (%u bytes)",
                  comp_host_off, comp_size);
        return false;
    }

    // Decompress full cluster
    std::vector<uint8_t> decompressed(cluster_size_);

    if (compression_type_ == 1) {
        // zstd streaming decompression (handles multiple frames)
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        if (!dctx) {
            LOG_ERROR("Qcow2: ZSTD_createDCtx failed");
            return false;
        }
        ZSTD_inBuffer input = { comp_buf.data(), comp_size, 0 };
        ZSTD_outBuffer output = { decompressed.data(), cluster_size_, 0 };

        while (output.pos < output.size) {
            size_t ret = ZSTD_decompressStream(dctx, &output, &input);
            if (ZSTD_isError(ret)) {
                LOG_ERROR("Qcow2: ZSTD_decompressStream failed: %s",
                          ZSTD_getErrorName(ret));
                ZSTD_freeDCtx(dctx);
                return false;
            }
            if (ret == 0 && output.pos < output.size) {
                break;  // no more input data
            }
        }
        ZSTD_freeDCtx(dctx);
    } else if (compression_type_ == 0) {
        // zlib (DEFLATE) decompression
        uLongf dest_len = cluster_size_;
        int ret = uncompress(decompressed.data(), &dest_len,
                             comp_buf.data(), comp_size);
        if (ret != Z_OK) {
            // Try raw inflate (no zlib header) as qcow2 uses raw deflate
            z_stream strm{};
            strm.avail_in = comp_size;
            strm.next_in = comp_buf.data();
            strm.avail_out = cluster_size_;
            strm.next_out = decompressed.data();

            ret = inflateInit2(&strm, -15);  // raw deflate
            if (ret != Z_OK) {
                LOG_ERROR("Qcow2: inflateInit2 failed (%d)", ret);
                return false;
            }
            ret = inflate(&strm, Z_FINISH);
            inflateEnd(&strm);

            if (ret != Z_STREAM_END && ret != Z_OK) {
                LOG_ERROR("Qcow2: inflate failed (%d) for cluster at 0x%llX",
                          ret, comp_host_off);
                return false;
            }
        }
    } else {
        LOG_ERROR("Qcow2: unknown compression_type %u", compression_type_);
        return false;
    }

    if (in_cluster_off + len > cluster_size_) {
        LOG_ERROR("Qcow2: read past cluster boundary");
        return false;
    }

    memcpy(buf, decompressed.data() + in_cluster_off, len);
    return true;
}

bool Qcow2DiskImage::WriteCluster(uint64_t host_off, uint64_t in_cluster_off,
                                    const void* buf, uint32_t len) {
    _fseeki64(file_, host_off + in_cluster_off, SEEK_SET);
    return fwrite(buf, 1, len, file_) == len;
}

// ---------- allocation ----------

uint64_t Qcow2DiskImage::AllocateCluster() {
    uint32_t rfb_index;
    uint64_t cluster_index = free_cluster_index_++;
    uint16_t* rfb = GetRefcountBlock(cluster_index, &rfb_index, true);

    while (rfb) {
        if (rfb[rfb_index] == 0) {
            break;
        }
        cluster_index = free_cluster_index_++;
        if (++rfb_index >= rfb_entries_) {
            rfb = GetRefcountBlock(cluster_index, &rfb_index, true);
        }
    }

    if (!rfb) {
        LOG_ERROR("Qcow2: failed to allocate cluster (refcount table exhausted)");
        return 0;
    }

    rfb[rfb_index] = 1;

    // Mark the refcount block dirty
    uint64_t rft_index = cluster_index / rfb_entries_;
    uint64_t block_offset = refcount_table_[rft_index];
    auto it = rfb_map_.find(block_offset);
    if (it != rfb_map_.end()) {
        it->second->dirty = true;
    }

    uint64_t offset = static_cast<uint64_t>(cluster_index) << cluster_bits_;

    // Extend file and zero-fill the new cluster
    if (offset + cluster_size_ > file_end_) {
        file_end_ = offset + cluster_size_;
    }
    std::vector<uint8_t> zeros(cluster_size_, 0);
    _fseeki64(file_, offset, SEEK_SET);
    fwrite(zeros.data(), 1, cluster_size_, file_);

    return offset;
}

void Qcow2DiskImage::FreeCluster(uint64_t host_offset) {
    uint64_t cluster_index = host_offset >> cluster_bits_;
    uint32_t rfb_index;
    uint16_t* rfb = GetRefcountBlock(cluster_index, &rfb_index, false);
    if (!rfb) {
        LOG_ERROR("Qcow2: FreeCluster: no refcount block for offset 0x%llX",
                  host_offset);
        return;
    }

    if (rfb[rfb_index] == 0) {
        LOG_ERROR("Qcow2: FreeCluster: refcount already 0 for offset 0x%llX",
                  host_offset);
        return;
    }

    rfb[rfb_index]--;

    uint64_t rft_index = cluster_index / rfb_entries_;
    uint64_t block_offset = refcount_table_[rft_index];
    auto it = rfb_map_.find(block_offset);
    if (it != rfb_map_.end()) {
        it->second->dirty = true;
    }

    if (rfb[rfb_index] == 0 && cluster_index < free_cluster_index_) {
        free_cluster_index_ = cluster_index;
    }
}

uint64_t* Qcow2DiskImage::EnsureL2Table(uint32_t l1_idx) {
    if (l1_idx >= l1_size_) return nullptr;

    uint64_t l1_entry = l1_table_[l1_idx];
    if (l1_entry != 0) {
        uint64_t l2_off = l1_entry & kOffsetMask;
        return GetL2Table(l2_off);
    }

    // Allocate new L2 table
    uint64_t new_l2_off = AllocateCluster();
    if (new_l2_off == 0) {
        LOG_ERROR("Qcow2: failed to allocate L2 table for l1_idx %u", l1_idx);
        return nullptr;
    }

    // Update L1 entry (set COPIED bit)
    l1_table_[l1_idx] = new_l2_off | kCopiedBit;

    // Write L1 entry back to disk (big-endian)
    uint64_t be_entry = Be64(l1_table_[l1_idx]);
    _fseeki64(file_, l1_table_offset_ + l1_idx * sizeof(uint64_t), SEEK_SET);
    fwrite(&be_entry, sizeof(be_entry), 1, file_);

    return GetL2Table(new_l2_off);
}

// ---------- public Read/Write ----------

bool Qcow2DiskImage::Read(uint64_t offset, void* buf, uint32_t len) {
    uint8_t* dst = static_cast<uint8_t*>(buf);

    while (len > 0) {
        uint64_t in_cluster_off = offset & (cluster_size_ - 1);
        uint32_t chunk = std::min(len,
            static_cast<uint32_t>(cluster_size_ - in_cluster_off));

        if (offset + chunk > virtual_size_) {
            LOG_ERROR("Qcow2: read past virtual disk end");
            return false;
        }

        bool compressed = false;
        uint64_t comp_host_off = 0;
        uint32_t comp_size = 0;
        uint64_t host_off = ResolveOffset(offset, &compressed,
                                           &comp_host_off, &comp_size);

        if (compressed) {
            if (!ReadCompressedCluster(comp_host_off, comp_size,
                                        in_cluster_off, dst, chunk)) {
                return false;
            }
        } else if (host_off == 0) {
            memset(dst, 0, chunk);
        } else {
            if (!ReadCluster(host_off, in_cluster_off, dst, chunk)) {
                return false;
            }
        }

        offset += chunk;
        dst += chunk;
        len -= chunk;
    }
    return true;
}

bool Qcow2DiskImage::Write(uint64_t offset, const void* buf, uint32_t len) {
    const uint8_t* src = static_cast<const uint8_t*>(buf);

    while (len > 0) {
        uint64_t in_cluster_off = offset & (cluster_size_ - 1);
        uint32_t chunk = std::min(len,
            static_cast<uint32_t>(cluster_size_ - in_cluster_off));

        if (offset + chunk > virtual_size_) {
            LOG_ERROR("Qcow2: write past virtual disk end");
            return false;
        }

        uint32_t l1_idx = static_cast<uint32_t>(
            offset / (static_cast<uint64_t>(l2_entries_) * cluster_size_));
        uint32_t l2_idx = static_cast<uint32_t>(
            (offset / cluster_size_) % l2_entries_);

        uint64_t* l2 = EnsureL2Table(l1_idx);
        if (!l2) return false;

        uint64_t l2_entry = l2[l2_idx];
        uint64_t data_off = 0;

        bool need_cow = false;
        if (l2_entry == 0) {
            need_cow = true;
        } else if (l2_entry & kCompressedBit) {
            need_cow = true;
        }

        if (need_cow) {
            data_off = AllocateCluster();
            if (data_off == 0) {
                LOG_ERROR("Qcow2: failed to allocate data cluster");
                return false;
            }

            // If writing a partial cluster, read old data first
            if (chunk < cluster_size_ && l2_entry != 0) {
                // Read existing cluster data (possibly compressed)
                std::vector<uint8_t> old_data(cluster_size_, 0);
                bool comp = false;
                uint64_t comp_off = 0;
                uint32_t comp_sz = 0;
                uint64_t old_host = ResolveOffset(
                    offset & ~(static_cast<uint64_t>(cluster_size_) - 1),
                    &comp, &comp_off, &comp_sz);

                if (comp) {
                    ReadCompressedCluster(comp_off, comp_sz, 0,
                                          old_data.data(), cluster_size_);
                } else if (old_host != 0) {
                    ReadCluster(old_host, 0, old_data.data(), cluster_size_);
                }

                // Write old data to new cluster
                WriteCluster(data_off, 0, old_data.data(), cluster_size_);
            }

            // Update L2 entry (set COPIED bit)
            l2[l2_idx] = data_off | kCopiedBit;

            // Mark L2 cache entry dirty
            uint64_t l2_table_off = l1_table_[l1_idx] & kOffsetMask;
            auto it = l2_map_.find(l2_table_off);
            if (it != l2_map_.end()) {
                it->second->dirty = true;
            }
        } else {
            data_off = l2_entry & kOffsetMask;
        }

        if (!WriteCluster(data_off, in_cluster_off, src, chunk)) {
            return false;
        }

        offset += chunk;
        src += chunk;
        len -= chunk;
    }
    return true;
}

bool Qcow2DiskImage::Discard(uint64_t offset, uint64_t len) {
    while (len > 0) {
        uint64_t in_cluster_off = offset & (cluster_size_ - 1);
        uint64_t chunk = std::min(len,
            static_cast<uint64_t>(cluster_size_) - in_cluster_off);

        // Only discard whole clusters -- partial cluster discard is a no-op
        if (in_cluster_off == 0 && chunk >= cluster_size_) {
            uint32_t l1_idx = static_cast<uint32_t>(
                offset / (static_cast<uint64_t>(l2_entries_) * cluster_size_));
            uint32_t l2_idx = static_cast<uint32_t>(
                (offset / cluster_size_) % l2_entries_);

            if (l1_idx < l1_size_) {
                uint64_t l1_entry = l1_table_[l1_idx];
                if (l1_entry != 0) {
                    uint64_t l2_table_off = l1_entry & kOffsetMask;
                    uint64_t* l2 = GetL2Table(l2_table_off);
                    if (l2 && l2[l2_idx] != 0) {
                        uint64_t l2_entry = l2[l2_idx];
                        if (!(l2_entry & kCompressedBit)) {
                            uint64_t host_off = l2_entry & kOffsetMask;
                            if (host_off != 0) {
                                FreeCluster(host_off);
                            }
                        }
                        l2[l2_idx] = 0;
                        auto it = l2_map_.find(l2_table_off);
                        if (it != l2_map_.end())
                            it->second->dirty = true;
                    }
                }
            }
        }

        offset += chunk;
        len -= chunk;
    }
    return true;
}

bool Qcow2DiskImage::WriteZeros(uint64_t offset, uint64_t len) {
    while (len > 0) {
        uint64_t in_cluster_off = offset & (cluster_size_ - 1);
        uint64_t chunk = std::min(len,
            static_cast<uint64_t>(cluster_size_) - in_cluster_off);

        if (in_cluster_off == 0 && chunk >= cluster_size_) {
            // Whole cluster: discard (reads back as zeros)
            if (!Discard(offset, cluster_size_)) return false;
        } else {
            // Partial cluster: write actual zeros
            std::vector<uint8_t> zeros(static_cast<size_t>(chunk), 0);
            if (!Write(offset, zeros.data(), static_cast<uint32_t>(chunk))) {
                return false;
            }
        }

        offset += chunk;
        len -= chunk;
    }
    return true;
}

bool Qcow2DiskImage::Flush() {
    if (!file_) return false;

    // Flush all dirty L2 cache entries
    for (auto& entry : l2_lru_) {
        if (entry.dirty) {
            std::vector<uint64_t> be_data(l2_entries_);
            for (uint32_t i = 0; i < l2_entries_; i++) {
                be_data[i] = Be64(entry.data[i]);
            }
            _fseeki64(file_, entry.l2_offset, SEEK_SET);
            fwrite(be_data.data(), 1, l2_entries_ * sizeof(uint64_t), file_);
            entry.dirty = false;
        }
    }

    // Flush all dirty refcount block cache entries
    for (auto& entry : rfb_lru_) {
        if (entry.dirty) {
            std::vector<uint16_t> be_data(rfb_entries_);
            for (uint32_t i = 0; i < rfb_entries_; i++) {
                be_data[i] = Be16(entry.data[i]);
            }
            _fseeki64(file_, entry.offset_in_file, SEEK_SET);
            fwrite(be_data.data(), 1, rfb_entries_ * sizeof(uint16_t), file_);
            entry.dirty = false;
        }
    }

    // Flush refcount table if modified
    if (refcount_table_dirty_) {
        FlushRefcountTable();
    }

    fflush(file_);
#ifdef _WIN32
    _commit(_fileno(file_));
#else
    fsync(fileno(file_));
#endif
    return true;
}
