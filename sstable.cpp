#include "sstable.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdexcept>
#include <algorithm>
#include <cstring>   // std::memcpy

// Helper: append file extension to a base path.
// Example: "sst_000001" + ".sst" -> "sst_000001.sst"
static std::string with_ext(const std::string& base, const char* ext) {
    return base + ext;
}

// Helper functions used only inside this file
namespace {

inline void buf_append_u32(std::string& buf, uint32_t v) {
    char tmp[4];
    std::memcpy(tmp, &v, sizeof(v));  // little-endian / host order, same as original write_u32
    buf.append(tmp, sizeof(v));
}

inline void buf_append_u64(std::string& buf, uint64_t v) {
    char tmp[8];
    std::memcpy(tmp, &v, sizeof(v));
    buf.append(tmp, sizeof(v));
}

// Flush a buffer to fd once; throw exception on failure
inline void flush_buf(int fd, std::string& buf,
                      int ifd, int dfd, const char* what) {
    if (buf.empty()) return;
    ssize_t n = ::write(fd, buf.data(), buf.size());
    if (n != (ssize_t)buf.size()) {
        ::close(ifd);
        ::close(dfd);
        throw std::runtime_error(what);
    }
    buf.clear();
}

// Threshold (8 MB) for flushing buffers to avoid excessive memory use
constexpr size_t FLUSH_THRESHOLD = 8 * 1024 * 1024; // 8MB, adjustable
} // namespace

// Constructor: initialize data and index file paths.
SSTable::SSTable(std::string base_no_ext) {
    data_path  = with_ext(base_no_ext, ".sst");
    index_path = with_ext(base_no_ext, ".idx");
}

// Write a 32-bit unsigned integer (binary form) into file
void SSTable::write_u32(int fd, uint32_t v) {
    if (::write(fd, &v, sizeof(v)) != (ssize_t)sizeof(v)) {
        throw std::runtime_error("write_u32");
    }
}

// Write a 64-bit unsigned integer (binary form) into file.
void SSTable::write_u64(int fd, uint64_t v) {
    if (::write(fd, &v, sizeof(v)) != (ssize_t)sizeof(v)) {
        throw std::runtime_error("write_u64");
    }
}

// Read a 32-bit unsigned integer from file at given offset.
uint32_t SSTable::read_u32_at(int fd, uint64_t off) {
    uint32_t v{};
    if (::pread(fd, &v, sizeof(v), off) != (ssize_t)sizeof(v)) {
        throw std::runtime_error("pread u32");
    }
    return v;
}

// Read a 64-bit unsigned integer from file at given offset.
uint64_t SSTable::read_u64_at(int fd, uint64_t off) {
    uint64_t v{};
    if (::pread(fd, &v, sizeof(v), off) != (ssize_t)sizeof(v)) {
        throw std::runtime_error("pread u64");
    }
    return v;
}

// SSTable::build()
// --------------------------------------------------------------------
// Build an immutable SSTable on disk from sorted key-value pairs.
// Writes two files:
//   - data file (.sst): stores actual key/value records
//   - index file (.idx): stores key -> offset mappings
// Returns an SSTable object ready for reading.
SSTable SSTable::build(const std::string& base_no_ext,
                       const std::vector<std::pair<std::string, std::string>>& sorted_kv) {
    SSTable t(base_no_ext);

    // Open data and index files for writing
    int dfd = ::open(t.data_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (dfd < 0) {
        throw std::runtime_error("open data for write failed");
    }
    int ifd = ::open(t.index_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (ifd < 0) {
        ::close(dfd);
        throw std::runtime_error("open index for write failed");
    }

    // Prepare two in-memory buffers to reduce write calls
    std::string idx_buf;
    std::string data_buf;
    idx_buf.reserve(1 * 1024 * 1024);    // preallocate 1 MB
    data_buf.reserve(4 * 1024 * 1024);   // preallocate 4 MB

    // Write the number of entries at the beginning of the index file
    uint32_t n = static_cast<uint32_t>(sorted_kv.size());
    buf_append_u32(idx_buf, n);

    // Keep track of byte offset within the data file
    uint64_t off = 0;

    for (auto& kv : sorted_kv) {
        const std::string& k = kv.first;
        const std::string& v = kv.second;
        uint32_t klen = static_cast<uint32_t>(k.size());
        uint32_t vlen = static_cast<uint32_t>(v.size());

        // index format: [key_len][key_bytes][offset]
        buf_append_u32(idx_buf, klen);
        idx_buf.append(k.data(), klen);
        buf_append_u64(idx_buf, off);

        // data format: [key_len][val_len][key_bytes][val_bytes]
        buf_append_u32(data_buf, klen);
        buf_append_u32(data_buf, vlen);
        data_buf.append(k.data(), klen);
        data_buf.append(v.data(), vlen);

        // Update next offset in data file
        off += sizeof(uint32_t) * 2 + klen + vlen;

        // Flush buffers when too large to avoid excessive memory usage
        if (idx_buf.size() >= FLUSH_THRESHOLD) {
            flush_buf(ifd, idx_buf, ifd, dfd, "write idx_buf");
        }
        if (data_buf.size() >= FLUSH_THRESHOLD) {
            flush_buf(dfd, data_buf, ifd, dfd, "write data_buf");
        }
    }

    // Flush remaining buffers
    flush_buf(ifd, idx_buf, ifd, dfd, "final write idx_buf");
    flush_buf(dfd, data_buf, ifd, dfd, "final write data_buf");

    ::close(ifd);
    ::close(dfd);

    // Open the SSTable for reading (load index into memory)
    t.open();
    return t;
}

void SSTable::open() {
    int ifd = ::open(index_path.c_str(), O_RDONLY);
    if (ifd < 0) {
        throw std::runtime_error("open index for read failed");
    }

    // Read number of entries
    uint32_t n{};
    if (::read(ifd, &n, sizeof(n)) != (ssize_t)sizeof(n)) {
        ::close(ifd);
        throw std::runtime_error("read n");
    }

    // Read index entries one by one
    index.clear();
    index.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t klen{};
        if (::read(ifd, &klen, sizeof(klen)) != (ssize_t)sizeof(klen)) {
            ::close(ifd);
            throw std::runtime_error("read klen");
        }
        std::string k(klen, '\0');
        if (::read(ifd, k.data(), klen) != (ssize_t)klen) {
            ::close(ifd);
            throw std::runtime_error("read key");
        }
        uint64_t off{};
        if (::read(ifd, &off, sizeof(off)) != (ssize_t)sizeof(off)) {
            ::close(ifd);
            throw std::runtime_error("read off");
        }
        index.push_back({std::move(k), off});
    }
    ::close(ifd);

    // Open data file with O_DIRECT if possible; fall back to regular read if not supported
    data_fd = ::open(data_path.c_str(), O_RDONLY | O_DIRECT);
    if (data_fd < 0 && errno == EINVAL) {
        // File system or OS does not support O_DIRECT; fall back to buffered I/O
        data_fd = ::open(data_path.c_str(), O_RDONLY);
    }
    if (data_fd < 0) {
        throw std::runtime_error("open data for read failed");
    }

    // -------- Build the static B-tree-like top-level index --------
    build_btree_index(fanout_);
}

void SSTable::build_btree_index(std::size_t fanout) {
    top_index_.clear();
    fanout_ = fanout;

    std::size_t n = index.size();
    if (n == 0 || fanout_ == 0) return;

    // Number of entries per block (leaf)
    std::size_t block_size = (n + fanout_ - 1) / fanout_; // ceil(n / fanout_)

    std::size_t start = 0;
    while (start < n) {
        std::size_t end = std::min(start + block_size, n);
        const std::string& first_key = index[start].key;

        TopIndexEntry e;
        e.key   = first_key;
        e.start = start;
        e.end   = end;
        top_index_.push_back(std::move(e));

        start = end;
    }
}


void SSTable::close() {
    if (data_fd >= 0) {
        ::close(data_fd);
        data_fd = -1;
    }
    index.clear();
}

// SSTable::read_record_at()
// --------------------------------------------------------------------
// Given an offset, read one complete key-value record from data file.
// Record format: [key_len][val_len][key_bytes][val_bytes]
bool SSTable::read_record_at(int fd, uint64_t off, std::string& k, std::string& v) {
    uint32_t klen = read_u32_at(fd, off);
    uint32_t vlen = read_u32_at(fd, off + sizeof(uint32_t));
    k.resize(klen);
    v.resize(vlen);
    if (::pread(fd, k.data(), klen, off + sizeof(uint32_t) * 2) != (ssize_t)klen) {
        return false;
    }
    if (::pread(fd, v.data(), vlen, off + sizeof(uint32_t) * 2 + klen) != (ssize_t)vlen) {
        return false;
    }
    return true;
}

// SSTable::get()
// --------------------------------------------------------------------
// Binary-search the in-memory index to find key, then read the record
// from the data file using its offset. Returns true if found.
bool SSTable::get(const std::string& key, std::string& value_out) const {
    if (index.empty() || data_fd < 0) return false;

    switch (query_mode_) {
        case QueryMode::BinarySearch:
            return get_binary(key, value_out);
        case QueryMode::BTree:
            return get_btree(key, value_out);
        default:
            // Fallback: just use binary search
            return get_binary(key, value_out);
    }
}

bool SSTable::get_binary(const std::string& key, std::string& value_out) const {
    auto it = std::lower_bound(
        index.begin(), index.end(), key,
        [](const SSTIndexEntry& e, const std::string& k) {
            return e.key < k;
        }
    );
    if (it == index.end() || it->key != key) return false;

    std::string k, v;
    if (!read_record_at(data_fd, it->offset, k, v)) return false;
    if (k != key) return false;
    value_out = std::move(v);
    return true;
}

bool SSTable::get_btree(const std::string& key, std::string& value_out) const {
    if (top_index_.empty()) {
        // If the B-tree index has not been built, fall back to binary search
        return get_binary(key, value_out);
    }

    // Step 1: search the root node (top_index_) to find the block
    // We want the last block whose key <= target key.
    auto block_it = std::upper_bound(
        top_index_.begin(), top_index_.end(), key,
        [](const std::string& k, const TopIndexEntry& e) {
            return k < e.key;
        }
    );

    if (block_it == top_index_.begin()) {
        // All block first-keys are > key -> key may still be in the first block
        block_it = top_index_.begin();
    } else {
        // upper_bound returns first block with key > target; step back one block
        --block_it;
    }

    std::size_t start = block_it->start;
    std::size_t end   = block_it->end;
    if (start >= end || start >= index.size()) {
        return false;
    }
    end = std::min(end, index.size());

    // Step 2: binary search *within* the chosen block (leaf)
    auto it = std::lower_bound(
        index.begin() + start, index.begin() + end, key,
        [](const SSTIndexEntry& e, const std::string& k) {
            return e.key < k;
        }
    );
    if (it == index.begin() + end || it->key != key) return false;

    std::string k, v;
    if (!read_record_at(data_fd, it->offset, k, v)) return false;
    if (k != key) return false;
    value_out = std::move(v);
    return true;
}

// SSTable::scan()
// --------------------------------------------------------------------
// Range scan from 'start' to 'end' (inclusive).
// Iterates through index and invokes user-supplied callback 'visit'.
void SSTable::scan(const std::string& start, const std::string& end,
                   const std::function<void(const std::string&, const std::string&)>& visit) const {
    if (index.empty() || data_fd < 0) return;
    auto it = std::lower_bound(index.begin(), index.end(), start,
        [](const SSTIndexEntry& e, const std::string& k) { return e.key < k; });

    for (; it != index.end(); ++it) {
        if (it->key > end) break;
        std::string k, v;
        if (!read_record_at(data_fd, it->offset, k, v)) break;
        if (k < start) continue;
        if (k > end) break;
        visit(k, v);
    }
}
