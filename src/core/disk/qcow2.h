#pragma once

#include "core/disk/disk_image.h"
#include <cstdio>
#include <vector>
#include <list>
#include <unordered_map>
#include <mutex>

#pragma pack(push, 1)
struct Qcow2Header {
    uint32_t magic;
    uint32_t version;
    uint64_t backing_file_offset;
    uint32_t backing_file_size;
    uint32_t cluster_bits;
    uint64_t size;                   // virtual disk size
    uint32_t crypt_method;
    uint32_t l1_size;
    uint64_t l1_table_offset;
    uint64_t refcount_table_offset;
    uint32_t refcount_table_clusters;
    uint32_t nb_snapshots;
    uint64_t snapshots_offset;
    // v3 extended fields (not used, but present for alignment)
    uint64_t incompatible_features;
    uint64_t compatible_features;
    uint64_t autoclear_features;
    uint32_t refcount_order;         // v3
    uint32_t header_length;          // v3
};
#pragma pack(pop)

class Qcow2DiskImage : public DiskImage {
public:
    ~Qcow2DiskImage() override;

    bool Open(const std::string& path) override;
    uint64_t GetSize() const override { return virtual_size_; }
    bool Read(uint64_t offset, void* buf, uint32_t len) override;
    bool Write(uint64_t offset, const void* buf, uint32_t len) override;
    bool Flush() override;
    bool Discard(uint64_t offset, uint64_t len) override;
    bool WriteZeros(uint64_t offset, uint64_t len) override;

    // Scan image integrity and optionally repair leaked clusters.
    // When fix=true, corrects refcounts for leaked clusters in-place.
    // Returns the number of leaked clusters found (or fixed), or -1 on error.
    int RepairLeaks(bool fix);

private:
    static constexpr uint32_t kQcow2Magic   = 0x514649FB;
    static constexpr uint64_t kCompressedBit = 1ULL << 62;
    static constexpr uint64_t kCopiedBit     = 1ULL << 63;
    // Mask to extract the host offset from L1/L2 entries (bits 9..55)
    static constexpr uint64_t kOffsetMask    = 0x00FFFFFFFFFFFE00ULL;
    // Bit 0 of standard cluster descriptor: cluster reads as all zeros (v3)
    static constexpr uint64_t kZeroFlag      = 1ULL;
    // Mask for refcount table entries (bits 9-63, per spec bits 0-8 reserved)
    static constexpr uint64_t kReftOffsetMask = 0xFFFFFFFFFFFFFE00ULL;
    static constexpr size_t   kL2CacheMax    = 64;
    static constexpr size_t   kRfbCacheMax   = 64;

    static uint16_t Be16(uint16_t v);
    static uint32_t Be32(uint32_t v);
    static uint64_t Be64(uint64_t v);

    bool ReadHeader();
    bool ReadL1Table();
    bool ReadRefcountTable();
    void SetDirtyBit();
    void ClearDirtyBit();

    // L2 cache: returns pointer to cached L2 table entries (host byte order).
    // The returned pointer is valid until the next L2 lookup (LRU eviction).
    uint64_t* GetL2Table(uint64_t l2_offset);
    void EvictL2Cache();

    // Refcount block cache
    uint16_t* GetRefcountBlock(uint64_t cluster_index, uint32_t* rfb_index,
                               bool allocate);
    void EvictRfbCache();
    void FlushRefcountTable();
    bool GrowRefcountTable(uint64_t min_cluster_index);

    // Resolve a virtual offset to a host file offset. Returns 0 if unallocated.
    // Sets `compressed` and `comp_size` if the cluster is compressed.
    uint64_t ResolveOffset(uint64_t virt_offset, bool* compressed,
                           uint64_t* comp_host_off, uint32_t* comp_size);

    // Allocate a cluster with refcount=1. Returns host file offset, or 0 on error.
    uint64_t AllocateCluster();
    // Decrement refcount of the cluster at the given host offset.
    void FreeCluster(uint64_t host_offset);
    // Decrement refcounts for all physical clusters spanned by a compressed L2 entry.
    void FreeCompressedCluster(uint64_t l2_entry);

    // Ensure L2 table is allocated for the given L1 index.
    uint64_t* EnsureL2Table(uint32_t l1_idx);

    bool ReadCluster(uint64_t host_off, uint64_t in_cluster_off,
                     void* buf, uint32_t len);
    bool ReadCompressedCluster(uint64_t comp_host_off, uint32_t comp_size,
                               uint64_t in_cluster_off, void* buf, uint32_t len);
    bool WriteCluster(uint64_t host_off, uint64_t in_cluster_off,
                      const void* buf, uint32_t len);
    bool CheckMetadataOverlap(uint64_t offset, uint64_t size);

    FILE* file_ = nullptr;
    uint64_t virtual_size_ = 0;
    uint32_t cluster_bits_ = 0;
    uint32_t cluster_size_ = 0;
    uint32_t l2_entries_ = 0;        // entries per L2 table
    uint32_t l1_size_ = 0;
    uint64_t l1_table_offset_ = 0;
    uint32_t version_ = 0;

    std::vector<uint64_t> l1_table_;  // in host byte order
    uint64_t file_end_ = 0;          // current end of file (for append allocations)
    uint8_t compression_type_ = 0;   // 0=zlib (deflate), 1=zstd
    uint32_t refcount_order_ = 4;    // log2(refcount_bits), default 4 => 16-bit
    uint32_t refcount_bits_ = 16;

    // Refcount management
    std::vector<uint64_t> refcount_table_;  // host byte order
    uint64_t refcount_table_offset_ = 0;
    uint32_t refcount_table_clusters_ = 0;
    uint32_t rfb_entries_ = 0;              // entries per refcount block
    uint64_t free_cluster_index_ = 0;
    bool refcount_table_dirty_ = false;

    // Refcount block LRU cache
    struct RfbCacheEntry {
        uint64_t offset_in_file;
        std::vector<uint16_t> data;   // host byte order
        bool dirty;
    };
    std::list<RfbCacheEntry> rfb_lru_;
    std::unordered_map<uint64_t, std::list<RfbCacheEntry>::iterator> rfb_map_;

    // L2 LRU cache
    struct L2CacheEntry {
        uint64_t l2_offset;           // file offset of this L2 table
        std::vector<uint64_t> data;   // entries in host byte order
        bool dirty;
    };
    std::list<L2CacheEntry> l2_lru_;
    std::unordered_map<uint64_t, std::list<L2CacheEntry>::iterator> l2_map_;
};
