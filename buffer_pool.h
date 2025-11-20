#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <list>
#include <forward_list>

// A 4KB page is uniquely identified by (fd, page_no).
// - fd: file descriptor
// - page_no: page number (offset / 4096)
struct PageId {
    int fd;
    uint64_t page_no;

    bool operator==(const PageId& o) const noexcept {
        return fd == o.fd && page_no == o.page_no;
    }
};

// BufferPool
// ---------------------------------------------------------------------
// The buffer pool caches database pages (4KB each) in memory. It uses:
//   - a custom hash table with separate chaining for collision handling
//   - a global LRU list for eviction
//
// Each frame inside the buffer pool stores:
//   - the page identifier (fd, page_no)
//   - a 4KB aligned buffer allocated via posix_memalign
//     This allows reading pages using O_DIRECT safely.
//   - the actual valid byte count (for the last page of a file)
// ---------------------------------------------------------------------
class BufferPool {
public:
    static constexpr std::size_t PAGE_SIZE = 4096;

    explicit BufferPool(std::size_t capacity_pages = 1024,
                        std::size_t num_buckets   = 4096);

    // Read `len` bytes starting at (fd, offset) into `dst`.
    // All reads must go through the buffer pool:
    //   - First check if the page is cached
    //   - Otherwise load from disk, put into the buffer pool (LRU)
    void read_bytes(int fd, uint64_t offset, void* dst, std::size_t len);

private:
    // One frame = one cached 4KB page
    struct Frame {
        PageId id{};              // (fd, page_no)
        char* data{nullptr};      // 4KB buffer, PAGE_SIZE-aligned
        std::size_t valid{0};     // valid bytes read (<= PAGE_SIZE)

        Frame();
        ~Frame();

        Frame(const Frame&)            = delete;
        Frame& operator=(const Frame&) = delete;

        Frame(Frame&& other) noexcept;
        Frame& operator=(Frame&& other) noexcept;
    };

    using FrameList      = std::list<Frame>;         // LRU list
    using FrameIterator  = FrameList::iterator;      // iterator into LRU list
    using Bucket         = std::forward_list<FrameIterator>; // hash table chaining

    std::size_t capacity_;        // maximum number of cached pages
    std::size_t size_{0};         // current number of cached pages
    FrameList   lru_;             // LRU list: front = most recent, back = least recent
    std::vector<Bucket> table_;   // hash table (each bucket is a chain of iterators)

    // Hash function: map PageId → bucket index
    std::size_t bucket_index(const PageId& id) const;

    // Get a frame for (fd, page_no).
    // If cached: return it and update LRU order.
    // If not cached: read page from disk, insert into LRU/head, evict LRU tail if needed.
    Frame& get_frame(int fd, uint64_t page_no);

    // splitmix64 — fast non-cryptographic hash function used for hashing PageId.
    static uint64_t splitmix64(uint64_t x);
};

// Global buffer pool instance, shared by all SSTables.
extern BufferPool g_buffer_pool;
