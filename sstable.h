#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// Represents one entry in the SSTable index file.
// Each entry maps a key to its byte offset in the data file.
struct SSTIndexEntry {
    std::string key;   // Key string
    uint64_t offset;   // Offset in data file where this key-value pair starts
};

// SSTable: Sorted String Table
// ----------------------------------------
// This class represents an immutable, sorted key-value store file.
// It usually comes from flushing a MemTable (in-memory structure) to disk.
// The SSTable consists of two parts:
//  - Data file: stores serialized key-value pairs (.sst)
//  - Index file: stores key -> offset mappings (.idx)
class SSTable {
public:
    std::string data_path;     // Path to data file (e.g., "sst_000001.sst")
    std::string index_path;    // Path to index file (e.g., "sst_000001.idx")
    std::vector<SSTIndexEntry> index;  // In-memory index (loaded from index file)
    int data_fd{-1};           // File descriptor for data file

    SSTable() = default;

    // Constructor: given the base name (without extension), initialize paths.
    // Example: base_no_ext = "sst_000001" -> data_path = "sst_000001.sst"
    explicit SSTable(std::string base_no_ext);

    // Static builder function:
    // Given sorted key-value pairs, create both data and index files on disk.
    // Returns an SSTable object representing the files created.
    static SSTable build(const std::string& base_no_ext,
                         const std::vector<std::pair<std::string, std::string>>& sorted_kv);

    // Open the data/index files for reading
    void open();

    // Close the data file if open
    void close();

    // Get the value associated with a key.
    // Returns true if found, false if not.
    bool get(const std::string& key, std::string& value_out) const;

    // Perform a range scan between 'start' and 'end'.
    // For each key-value pair, call the 'visit' function.
    void scan(const std::string& start, const std::string& end,
              const std::function<void(const std::string&, const std::string&)>& visit) const;

private:
    // Helper functions for low-level binary I/O:

    // Write a 32-bit unsigned integer to file at current position
    static void write_u32(int fd, uint32_t v);

    // Write a 64-bit unsigned integer to file at current position
    static void write_u64(int fd, uint64_t v);

    // Read a 32-bit unsigned integer from a specific file offset
    static uint32_t read_u32_at(int fd, uint64_t off);

    // Read a 64-bit unsigned integer from a specific file offset
    static uint64_t read_u64_at(int fd, uint64_t off);

    // Read one complete key-value record at a given offset in the data file
    // Returns false if read fails
    static bool read_record_at(int fd, uint64_t off, std::string& k, std::string& v);
};
