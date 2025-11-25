#include "sstable.h"
#include "buffer_pool.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <cassert>

// Helper: append file extension to a base path.
// Example: "sst_000001" + ".sst" -> "sst_000001.sst"
static std::string with_ext(const std::string& base, const char* ext) {
    return base + ext;
}

// Helper functions used only inside this file
namespace {

inline void buf_append_u32(std::string& buf, uint32_t v) {
    char tmp[4];
    std::memcpy(tmp, &v, sizeof(v));  // little-endian / host order
    buf.append(tmp, sizeof(v));
}

inline void buf_append_u64(std::string& buf, uint64_t v) {
    char tmp[8];
    std::memcpy(tmp, &v, sizeof(v));
    buf.append(tmp, sizeof(v));
}

// Flush a buffer to fd once; throw exception on failure
inline void flush_buf(int fd, const std::string& buf, const char* what) {
    if (buf.empty()) return;
    ssize_t n = ::write(fd, buf.data(), buf.size());
    if (n != (ssize_t)buf.size()) {
        throw std::runtime_error(what);
    }
}

// Threshold (8 MB) for temporary build buffers
constexpr size_t FLUSH_THRESHOLD = 8 * 1024 * 1024; // 8MB

} // namespace

// Constructor: initialize data and (legacy) index file paths.
SSTable::SSTable(std::string base_no_ext)
    : data_path(with_ext(base_no_ext, ".sst")),
      index_path(with_ext(base_no_ext, ".idx")) // kept only to not break callers
{}

// ----------------------------------------------------------------------
// Low-level page helpers
// ----------------------------------------------------------------------

bool SSTable::read_page(uint32_t page_id, std::string& out) const {
    if (data_fd < 0) return false;
    out.resize(PAGE_SIZE);

    uint64_t offset = static_cast<uint64_t>(page_id) * PAGE_SIZE;

    try {
        g_buffer_pool.read_bytes(data_fd, offset, out.data(), PAGE_SIZE);
    } catch (const std::exception& e) {
        return false;
    }

    return true;
}

// Read only the first key of a leaf page (for binary search over leaves).
bool SSTable::read_first_key_of_leaf(uint32_t page_id,
                                     std::string& first_key_out) const {
    std::string page;
    if (!read_page(page_id, page)) return false;
    const char* p = page.data();
    if (static_cast<uint8_t>(p[0]) != static_cast<uint8_t>(PageType::Leaf)) {
        return false;
    }
    // [0] type, [1..4] num_records
    uint32_t num_rec = 0;
    std::memcpy(&num_rec, p + 1, sizeof(num_rec));
    if (num_rec == 0) {
        first_key_out.clear();
        return false;
    }
    size_t pos = 1 + sizeof(uint32_t);
    if (pos + 8 > PAGE_SIZE) return false;

    uint32_t klen = 0, vlen = 0;
    std::memcpy(&klen, p + pos, 4);
    std::memcpy(&vlen, p + pos + 4, 4);
    pos += 8;
    if (pos + klen + vlen > PAGE_SIZE) return false;

    first_key_out.assign(p + pos, p + pos + klen);
    return true;
}

// Search a given key inside a leaf page; keys inside leaf are sorted.
bool SSTable::search_leaf_page(uint32_t page_id,
                               const std::string& key,
                               std::string& value_out) const {
    std::string page;
    if (!read_page(page_id, page)) return false;
    const char* p = page.data();

    if (static_cast<uint8_t>(p[0]) != static_cast<uint8_t>(PageType::Leaf)) {
        return false;
    }

    uint32_t num_rec = 0;
    std::memcpy(&num_rec, p + 1, sizeof(num_rec));

    size_t pos = 1 + sizeof(uint32_t);
    for (uint32_t i = 0; i < num_rec; ++i) {
        if (pos + 8 > PAGE_SIZE) break;
        uint32_t klen = 0, vlen = 0;
        std::memcpy(&klen, p + pos, 4);
        std::memcpy(&vlen, p + pos + 4, 4);
        pos += 8;
        if (pos + klen + vlen > PAGE_SIZE) break;

        std::string k(p + pos, p + pos + klen);
        pos += klen;
        std::string v(p + pos, p + pos + vlen);
        pos += vlen;

        if (k == key) {
            value_out = std::move(v);
            return true;
        }
        if (k > key) {
            // keys are sorted; no need to continue in this page
            return false;
        }
    }
    return false;
}

// Given an INTERNAL page and key, choose which child to descend into.
bool SSTable::find_child_in_internal_page(uint32_t page_id,
                                          const std::string& key,
                                          uint32_t& child_page_id_out) const {
    std::string page;
    if (!read_page(page_id, page)) return false;
    const char* p = page.data();

    if (static_cast<uint8_t>(p[0]) != static_cast<uint8_t>(PageType::Internal)) {
        return false;
    }

    uint32_t num_entries = 0;
    std::memcpy(&num_entries, p + 1, sizeof(num_entries));

    size_t pos = 1 + sizeof(uint32_t);
    bool have_candidate = false;
    uint32_t candidate_child = 0;

    for (uint32_t i = 0; i < num_entries; ++i) {
        if (pos + 8 > PAGE_SIZE) break;

        uint32_t child_page_id = 0;
        uint32_t klen = 0;
        std::memcpy(&child_page_id, p + pos, 4);
        std::memcpy(&klen,          p + pos + 4, 4);
        pos += 8;

        if (pos + klen > PAGE_SIZE) break;

        std::string k(p + pos, p + pos + klen);
        pos += klen;

        if (!have_candidate) {
            candidate_child = child_page_id;
            have_candidate = true;
        }

        if (k > key) {
            // key is in the previous child subtree (candidate_child)
            break;
        } else {
            // this subtree's first key <= target; update candidate
            candidate_child = child_page_id;
        }
    }

    if (!have_candidate) return false;
    child_page_id_out = candidate_child;
    return true;
}

// ----------------------------------------------------------------------
// SSTable::build()  ——  build a static on-disk B-tree SST
// ----------------------------------------------------------------------

SSTable SSTable::build(const std::string& base_no_ext,
                       const std::vector<std::pair<std::string, std::string>>& sorted_kv) {
    SSTable t(base_no_ext);

    const uint32_t PAGE_SIZE = SSTable::PAGE_SIZE;

    // ---------------- Leaf partitioning ----------------
    struct LeafPartition {
        size_t begin;        // index into sorted_kv
        size_t end;          // one past last
        std::string first_key;
    };

    std::vector<LeafPartition> leaves;
    const size_t n = sorted_kv.size();

    const uint32_t LEAF_HDR = 1 + sizeof(uint32_t); // type + num_records

    size_t i = 0;
    while (i < n) {
        size_t begin = i;
        uint32_t used = LEAF_HDR;

        while (i < n) {
            const auto& k = sorted_kv[i].first;
            const auto& v = sorted_kv[i].second;
            uint32_t klen = static_cast<uint32_t>(k.size());
            uint32_t vlen = static_cast<uint32_t>(v.size());
            uint32_t rec_sz = 8 + klen + vlen; // klen, vlen, k, v

            if (rec_sz > PAGE_SIZE - LEAF_HDR) {
                throw std::runtime_error("record too large to fit in a leaf page");
            }
            if (used + rec_sz > PAGE_SIZE) break;

            used += rec_sz;
            ++i;
        }

        LeafPartition lp;
        lp.begin = begin;
        lp.end   = i;
        lp.first_key = sorted_kv[begin].first;
        leaves.push_back(std::move(lp));
    }

    uint32_t leaf_page_count = static_cast<uint32_t>(leaves.size());

    // ---------------- Build internal levels (bottom-up) ----------------

    struct ChildRef {
        uint32_t child_index;      // index in its level (leaf index or node index)
        std::string first_key;     // first key of that subtree
    };

    struct InternalNodeDesc {
        size_t begin_child;        // range in level_children[l]
        size_t end_child;          // [begin_child, end_child)
        std::string first_key;     // = first_key of first child
    };

    // level_children[0] = leaves; level_children[1] = first internal level's nodes, etc.
    std::vector<std::vector<ChildRef>> level_children;
    level_children.emplace_back();
    auto& leaf_children = level_children.back();
    leaf_children.reserve(leaf_page_count);
    for (uint32_t idx = 0; idx < leaf_page_count; ++idx) {
        leaf_children.push_back({idx, leaves[idx].first_key});
    }

    std::vector<std::vector<InternalNodeDesc>> internal_levels; // 0 = just above leaves

    const uint32_t INTERNAL_HDR = 1 + sizeof(uint32_t); // type + num_entries

    while (true) {
        auto& cur_children = level_children.back();
        if (cur_children.size() <= 1) break; // current top level is root

        std::vector<InternalNodeDesc> nodes;

        size_t j = 0;
        while (j < cur_children.size()) {
            size_t begin_child = j;
            uint32_t used = INTERNAL_HDR;

            while (j < cur_children.size()) {
                const auto& ch = cur_children[j];
                uint32_t klen = static_cast<uint32_t>(ch.first_key.size());
                uint32_t entry_sz = 4 + 4 + klen; // child_page_id + key_len + key

                if (entry_sz > PAGE_SIZE - INTERNAL_HDR) {
                    throw std::runtime_error("internal entry too large for page");
                }
                if (used + entry_sz > PAGE_SIZE) break;

                used += entry_sz;
                ++j;
            }

            InternalNodeDesc nd;
            nd.begin_child = begin_child;
            nd.end_child   = j;
            nd.first_key   = cur_children[begin_child].first_key;
            nodes.push_back(std::move(nd));
        }

        internal_levels.push_back(nodes);

        // Build next (higher) level's children
        std::vector<ChildRef> parent_children;
        parent_children.reserve(nodes.size());
        for (uint32_t node_idx = 0; node_idx < nodes.size(); ++node_idx) {
            parent_children.push_back({node_idx, nodes[node_idx].first_key});
        }
        level_children.push_back(std::move(parent_children));
    }

    const size_t internal_level_count = internal_levels.size();

    // ---------------- Assign page ids ----------------
    // page 0 is header
    std::vector<uint32_t> level_base_page(internal_level_count, 0);
    uint32_t next_page = 1;

    for (int lvl = static_cast<int>(internal_level_count) - 1; lvl >= 0; --lvl) {
        level_base_page[lvl] = next_page;
        next_page += static_cast<uint32_t>(internal_levels[lvl].size());
    }

    uint32_t leaf_start_page = next_page;
    uint32_t root_page_id;

    if (internal_level_count == 0) {
        // No internal levels: root is the first leaf page
        root_page_id = leaf_start_page;
    } else {
        // root is the only node in the highest internal level (last one we built)
        size_t root_lvl = internal_level_count - 1;
        root_page_id = level_base_page[root_lvl]; // index 0 in that level
    }

    // ---------------- Actually write pages to disk ----------------

    int dfd = ::open(t.data_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (dfd < 0) {
        throw std::runtime_error("open data for write failed");
    }

    // 1. Header page (page 0)
    {
        std::string page;
        page.reserve(PAGE_SIZE);
        page.push_back(static_cast<char>(SSTable::PageType::Header)); // type
        buf_append_u32(page, PAGE_SIZE);
        buf_append_u32(page, root_page_id);
        buf_append_u32(page, leaf_start_page);
        buf_append_u32(page, leaf_page_count);
        buf_append_u64(page, static_cast<uint64_t>(sorted_kv.size()));
        if (page.size() > PAGE_SIZE) {
            ::close(dfd);
            throw std::runtime_error("header page overflow");
        }
        page.resize(PAGE_SIZE, 0);
        flush_buf(dfd, page, "write header");
    }

    // 2. Internal node pages: from root level down to lowest internal level
    for (int lvl = static_cast<int>(internal_level_count) - 1; lvl >= 0; --lvl) {
        const auto& nodes = internal_levels[lvl];
        const auto& children = level_children[lvl]; // children of this level

        for (size_t node_idx = 0; node_idx < nodes.size(); ++node_idx) {
            const auto& nd = nodes[node_idx];

            std::string page;
            page.reserve(PAGE_SIZE);
            page.push_back(static_cast<char>(SSTable::PageType::Internal));
            uint32_t num_entries = static_cast<uint32_t>(nd.end_child - nd.begin_child);
            buf_append_u32(page, num_entries);

            for (size_t ci = nd.begin_child; ci < nd.end_child; ++ci) {
                const auto& ch = children[ci];

                uint32_t child_page_id = 0;
                if (lvl == 0) {
                    // children are leaves
                    child_page_id = leaf_start_page + ch.child_index;
                } else {
                    // children are internal nodes in level lvl-1
                    child_page_id = level_base_page[lvl - 1] + ch.child_index;
                }

                uint32_t klen = static_cast<uint32_t>(ch.first_key.size());
                buf_append_u32(page, child_page_id);
                buf_append_u32(page, klen);
                page.append(ch.first_key.data(), ch.first_key.size());
            }

            if (page.size() > PAGE_SIZE) {
                ::close(dfd);
                throw std::runtime_error("internal page overflow");
            }
            page.resize(PAGE_SIZE, 0);
            flush_buf(dfd, page, "write internal page");
        }
    }

    // 3. Leaf pages: contiguous, in key order
    for (uint32_t leaf_idx = 0; leaf_idx < leaf_page_count; ++leaf_idx) {
        const auto& lp = leaves[leaf_idx];

        std::string page;
        page.reserve(PAGE_SIZE);
        page.push_back(static_cast<char>(SSTable::PageType::Leaf));

        uint32_t num_recs = static_cast<uint32_t>(lp.end - lp.begin);
        buf_append_u32(page, num_recs);

        for (size_t idx = lp.begin; idx < lp.end; ++idx) {
            const auto& kv = sorted_kv[idx];
            const std::string& k = kv.first;
            const std::string& v = kv.second;
            uint32_t klen = static_cast<uint32_t>(k.size());
            uint32_t vlen = static_cast<uint32_t>(v.size());
            buf_append_u32(page, klen);
            buf_append_u32(page, vlen);
            page.append(k.data(), klen);
            page.append(v.data(), vlen);
        }

        if (page.size() > PAGE_SIZE) {
            ::close(dfd);
            throw std::runtime_error("leaf page overflow");
        }
        page.resize(PAGE_SIZE, 0);
        flush_buf(dfd, page, "write leaf page");
    }

    ::close(dfd);

    // Open for reading and populate in-memory metadata
    t.open();
    return t;
}

// ----------------------------------------------------------------------
// open / close
// ----------------------------------------------------------------------

void SSTable::open() {
    if (data_fd >= 0) return;

    data_fd = ::open(data_path.c_str(), O_RDONLY);
    if (data_fd < 0) {
        throw std::runtime_error("open data for read failed");
    }

    // read header page (page 0)
    std::string page;
    if (!read_page(0, page)) {
        ::close(data_fd);
        data_fd = -1;
        throw std::runtime_error("read header failed");
    }

    const char* p = page.data();
    if (static_cast<uint8_t>(p[0]) != static_cast<uint8_t>(PageType::Header)) {
        ::close(data_fd);
        data_fd = -1;
        throw std::runtime_error("invalid header page type");
    }

    uint32_t page_size = 0;
    std::memcpy(&page_size,      p + 1, 4);
    std::memcpy(&root_page_id_,  p + 5, 4);
    std::memcpy(&leaf_start_page_, p + 9, 4);
    std::memcpy(&leaf_page_count_, p + 13, 4);
    std::memcpy(&num_records_,     p + 17, 8);

    if (page_size != PAGE_SIZE) {
        ::close(data_fd);
        data_fd = -1;
        throw std::runtime_error("PAGE_SIZE mismatch in SST header");
    }
}

void SSTable::close() {
    if (data_fd >= 0) {
        ::close(data_fd);
        data_fd = -1;
    }
}

// ----------------------------------------------------------------------
// Point queries: get()
// ----------------------------------------------------------------------

bool SSTable::get(const std::string& key, std::string& value_out) const {
    if (data_fd < 0 || leaf_page_count_ == 0) return false;

    switch (query_mode_) {
        case QueryMode::BinarySearch:
            return get_binary(key, value_out);
        case QueryMode::BTree:
            return get_btree(key, value_out);
        default:
            return get_binary(key, value_out);
    }
}

// Binary search over leaf pages based on each leaf's first key.
// Complexity: O(log #leaves) page reads.
bool SSTable::get_binary(const std::string& key, std::string& value_out) const {
    if (leaf_page_count_ == 0) return false;

    // binary search smallest i s.t. first_key(i) > key
    int64_t lo = 0;
    int64_t hi = static_cast<int64_t>(leaf_page_count_);

    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        uint32_t page_id = leaf_start_page_ + static_cast<uint32_t>(mid);

        std::string first_key;
        if (!read_first_key_of_leaf(page_id, first_key)) {
            return false;
        }

        if (first_key > key) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }

    int64_t idx;
    if (lo == 0) {
        idx = 0;
    } else {
        idx = lo - 1;
    }

    uint32_t target_page = leaf_start_page_ + static_cast<uint32_t>(idx);
    return search_leaf_page(target_page, key, value_out);
}

// B-tree search: from root_page_id_ descend through INTERNAL nodes to a LEAF.
bool SSTable::get_btree(const std::string& key, std::string& value_out) const {
    if (leaf_page_count_ == 0) return false;

    uint32_t page_id = root_page_id_;
    std::string page;

    while (true) {
        if (!read_page(page_id, page)) return false;
        const char* p = page.data();
        auto type = static_cast<PageType>(static_cast<uint8_t>(p[0]));

        if (type == PageType::Leaf) {
            // We already have the page in memory; reuse parsing logic
            // by writing a tiny inline version here to避免重复 IO
            uint32_t num_rec = 0;
            std::memcpy(&num_rec, p + 1, sizeof(num_rec));

            size_t pos = 1 + sizeof(uint32_t);
            for (uint32_t i = 0; i < num_rec; ++i) {
                if (pos + 8 > PAGE_SIZE) break;
                uint32_t klen = 0, vlen = 0;
                std::memcpy(&klen, p + pos, 4);
                std::memcpy(&vlen, p + pos + 4, 4);
                pos += 8;
                if (pos + klen + vlen > PAGE_SIZE) break;

                std::string k(p + pos, p + pos + klen);
                pos += klen;
                std::string v(p + pos, p + pos + vlen);
                pos += vlen;

                if (k == key) {
                    value_out = std::move(v);
                    return true;
                }
                if (k > key) {
                    return false;
                }
            }
            return false;
        } else if (type == PageType::Internal) {
            uint32_t child = 0;
            if (!find_child_in_internal_page(page_id, key, child)) {
                return false;
            }
            page_id = child;
        } else {
            // invalid
            return false;
        }
    }
}

// ----------------------------------------------------------------------
// Range scan
// ----------------------------------------------------------------------

void SSTable::scan(const std::string& start, const std::string& end,
                   const std::function<void(const std::string&, const std::string&)>& visit) const {
    if (data_fd < 0 || leaf_page_count_ == 0) return;

    // 1. binary-search leaf page to start from
    int64_t lo = 0;
    int64_t hi = static_cast<int64_t>(leaf_page_count_);

    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        uint32_t page_id = leaf_start_page_ + static_cast<uint32_t>(mid);

        std::string first_key;
        if (!read_first_key_of_leaf(page_id, first_key)) {
            return;
        }

        if (first_key >= start) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }

    int64_t leaf_idx = (lo == 0 ? 0 : lo - 1);
    if (leaf_idx < 0) leaf_idx = 0;

    // 2. sequentially scan leaf pages
    for (uint32_t lp = static_cast<uint32_t>(leaf_idx);
         lp < leaf_page_count_;
         ++lp) {
        uint32_t page_id = leaf_start_page_ + lp;
        std::string page;
        if (!read_page(page_id, page)) return;

        const char* p = page.data();
        if (static_cast<uint8_t>(p[0]) != static_cast<uint8_t>(PageType::Leaf)) {
            return;
        }

        uint32_t num_rec = 0;
        std::memcpy(&num_rec, p + 1, sizeof(num_rec));
        size_t pos = 1 + sizeof(uint32_t);

        for (uint32_t i = 0; i < num_rec; ++i) {
            if (pos + 8 > PAGE_SIZE) break;
            uint32_t klen = 0, vlen = 0;
            std::memcpy(&klen, p + pos, 4);
            std::memcpy(&vlen, p + pos + 4, 4);
            pos += 8;
            if (pos + klen + vlen > PAGE_SIZE) break;

            std::string k(p + pos, p + pos + klen);
            pos += klen;
            std::string v(p + pos, p + pos + vlen);
            pos += vlen;

            if (k < start) continue;
            if (k > end)  return;

            visit(k, v);
        }
    }
}
