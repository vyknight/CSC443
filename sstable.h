#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <utility>

class SSTable {
public:
    // Which algorithm to use for point queries
    enum class QueryMode {
        BinarySearch,  // binary search over leaf pages
        BTree          // B-tree search from root page down to a leaf
    };

private:
    // On-disk page layout ------------------------------------------------
    static constexpr uint32_t PAGE_SIZE = 4096;

    enum class PageType : uint8_t {
        Header   = 1,
        Internal = 2,
        Leaf     = 3,
    };

    std::string data_path;
    // kept for backward compatibility; not used anymore
    std::string index_path;
    int data_fd = -1;

    // Global B-tree metadata loaded from header page
    uint32_t root_page_id_    = 0;  // page id of B-tree root
    uint32_t leaf_start_page_ = 0;  // first leaf page id
    uint32_t leaf_page_count_ = 0;  // number of leaf pages
    uint64_t num_records_     = 0;  // total #records in this SST

    QueryMode query_mode_ = QueryMode::BinarySearch;

    // ---- low-level helpers (all work in units of pages) ----
    bool read_page(uint32_t page_id, std::string& out) const;

    // Leaf helpers
    bool read_first_key_of_leaf(uint32_t page_id,
                                std::string& first_key_out) const;
    bool search_leaf_page(uint32_t page_id,
                          const std::string& key,
                          std::string& value_out) const;

    // Internal-node helper: choose child for given key
    bool find_child_in_internal_page(uint32_t page_id,
                                     const std::string& key,
                                     uint32_t& child_page_id_out) const;

    // Query-mode specific implementations
    bool get_binary(const std::string& key, std::string& value_out) const;
    bool get_btree (const std::string& key, std::string& value_out) const;

public:
    explicit SSTable(std::string base_no_ext);

    // Build a new SST as a static B-tree on disk from sorted key/value pairs.
    static SSTable build(const std::string& base_no_ext,
                         const std::vector<std::pair<std::string,std::string>>& sorted_kv);

    void open();
    void close();

    bool get(const std::string& key, std::string& value_out) const;

    // Range scan [start, end], inclusive, using leaf pages only
    void scan(const std::string& start, const std::string& end,
              const std::function<void(const std::string&, const std::string&)>& visit) const;

    // Allow the user / KVDatabase to change the query mode
    void set_query_mode(QueryMode mode) {
        query_mode_ = mode;
    }
};
