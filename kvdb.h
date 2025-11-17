#pragma once

#include "newsstable.h"
#include "avl_tree.h"
#include <string>
#include <vector>

class KVDatabase {
    std::string db_dir_;
    int mem_cap_;     // maximum number of records allowed in the memtable
    AVLTree mem_;     // actual MemTable implemented using an AVL tree

    std::vector<SSTable> ssts_; // index 0 = newest SST, last = oldest

    // Find the next SST base filename (without extension)
    std::string next_sst_basename() const;

    // Flush mem_ into a new SST and then clear mem_
    void flush_to_sst();

public:
    KVDatabase(std::string db_dir, int memtable_size);

    void open();   // create directory if needed and load existing SSTs
    void close();  // flush mem_ and close all SSTs

    void put(const std::string& key, const std::string& value);
    bool get(const std::string& key, std::string& value_out) const;

    // Scan keys in the closed interval [start, end]
    std::vector<std::pair<std::string,std::string>>
    scan(const std::string& start, const std::string& end) const;
};
