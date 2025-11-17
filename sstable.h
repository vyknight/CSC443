class SSTable {
public:
    // Which algorithm to use for point queries
    enum class QueryMode {
        BinarySearch,  // use std::lower_bound over the full index
        BTree          // use a 2-level static B-tree index (root -> leaf)
    };

private:
    std::string data_path;
    std::string index_path;
    int data_fd = -1;

    struct SSTIndexEntry {
        std::string key;
        uint64_t offset;
    };
    std::vector<SSTIndexEntry> index;

    // --------- B-tree like top-level index (root node) ---------
    struct TopIndexEntry {
        std::string key;   // first key in this block
        std::size_t start; // inclusive index into `index`
        std::size_t end;   // exclusive index into `index`
    };

    QueryMode query_mode_ = QueryMode::BinarySearch;
    std::vector<TopIndexEntry> top_index_;  // root node
    std::size_t fanout_ = 128;              // desired number of blocks (can tune)

    // existing private helpers...
    static void write_u32(int fd, uint32_t v);
    static void write_u64(int fd, uint64_t v);
    static uint32_t read_u32_at(int fd, uint64_t off);
    static uint64_t read_u64_at(int fd, uint64_t off);
    static bool read_record_at(int fd, uint64_t off,
                               std::string& k, std::string& v);

    // New helpers for B-tree index
    void build_btree_index(std::size_t fanout);
    bool get_binary(const std::string& key, std::string& value_out) const;
    bool get_btree(const std::string& key, std::string& value_out) const;

public:
    explicit SSTable(std::string base_no_ext);
    static SSTable build(const std::string& base_no_ext,
                         const std::vector<std::pair<std::string,std::string>>& sorted_kv);

    void open();
    void close();

    bool get(const std::string& key, std::string& value_out) const;
    void scan(const std::string& start, const std::string& end,
              const std::function<void(const std::string&, const std::string&)>& visit) const;

    // Allow the user / KVDatabase to change the query mode
    void set_query_mode(QueryMode mode) {
        query_mode_ = mode;
    }
};
