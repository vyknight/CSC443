#include "kvdb.h"
#include <regex>
#include <stdexcept>
#include <algorithm>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

KVDatabase::KVDatabase(std::string db_dir, int memtable_size)
    : db_dir_(std::move(db_dir))
    , mem_cap_(memtable_size)
    , mem_(memtable_size)
{}

std::string KVDatabase::next_sst_basename() const {
    uint32_t max_id = 0;
    std::regex pat(R"(sst_(\d{6})\.sst)");

    if (fs::exists(db_dir_)) {
        for (auto& p : fs::directory_iterator(db_dir_)) {
            if (!p.is_regular_file()) continue;
            auto name = p.path().filename().string();
            std::smatch m;
            if (std::regex_match(name, m, pat)) {
                uint32_t id = static_cast<uint32_t>(std::stoul(m[1]));
                max_id = std::max(max_id, id);
            }
        }
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "sst_%06u", max_id + 1);
    return (fs::path(db_dir_) / buf).string();
}

void KVDatabase::flush_to_sst() {
    if (mem_.get_size() == 0) return;

    std::vector<std::pair<std::string,std::string>> sorted;
    sorted.reserve(mem_.get_size());

    // Export all (k,v) pairs from the memtable in ascending key order
    mem_.inorder_scan([&](const std::string& k, const std::string& v){
        sorted.emplace_back(k, v);
    });

    auto base  = next_sst_basename();
    SSTable t  = SSTable::build(base, sorted);

    //Insert the newest SST at the front
    ssts_.insert(ssts_.begin(), std::move(t));  

    mem_.clear();
}

void KVDatabase::open() {
    if (!fs::exists(db_dir_)) {
        fs::create_directories(db_dir_);
    }

    std::vector<std::string> bases;
    for (auto& p : fs::directory_iterator(db_dir_)) {
        if (p.is_regular_file() && p.path().extension() == ".sst") {
            // p.path().stem() = "sst_000001"
            bases.emplace_back((fs::path(db_dir_) / p.path().stem()).string());
        }
    }

    // Larger file name = newer SST = should appear earlier
    std::sort(bases.begin(), bases.end(), std::greater<>());

    ssts_.clear();
    for (auto& b : bases) {
        SSTable t(b);
        t.open();
        ssts_.push_back(std::move(t));
    }
}

void KVDatabase::close() {
    // Flush memtable into a new SST
    flush_to_sst();
    for (auto& t : ssts_) {
        t.close();
    }
}

void KVDatabase::put(const std::string& key, const std::string& value) {
    // If the key is new and memtable is full, flush first
    std::string dummy;
    bool exists_in_mem = mem_.get(key, dummy);

    if (!exists_in_mem && mem_.get_size() >= mem_cap_) {
        flush_to_sst();
    }

    // Insert or overwrite in memtable
    mem_.insert(key, value);
}

bool KVDatabase::get(const std::string& key, std::string& value_out) const {
    // 1) Check memtable first (newest)
    if (mem_.get(key, value_out)) return true;

    // 2) Check SSTs from newest to oldest
    for (const auto& t : ssts_) {
        if (t.get(key, value_out)) return true;
    }
    return false;
}

std::vector<std::pair<std::string,std::string>>
KVDatabase::scan(const std::string& start, const std::string& end) const {
    // Use a map to merge keys: sorted and deduplicated;
    // newer data (memtable & newer SSTs) override older data
    std::map<std::string,std::string> result;

    // 1) Memtable: newest values
    mem_.inorder_scan([&](const std::string& k, const std::string& v){
        if (k >= start && k <= end) {
            result[k] = v;
        }
    });

    // 2) SSTs from newest to oldest; only insert if not already present
    for (const auto& t : ssts_) {
        t.scan(start, end, [&](const std::string& k, const std::string& v){
            if (result.find(k) == result.end()) {
                result.emplace(k, v);
            }
        });
    }

    // 3) Convert map to vector (map is already sorted by key)
    std::vector<std::pair<std::string,std::string>> out;
    out.reserve(result.size());
    for (auto& kv : result) {
        out.emplace_back(kv.first, kv.second);
    }
    return out;
}