#include "buffer_pool.h"
#include <unistd.h>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <new>

BufferPool g_buffer_pool{};  // Global buffer pool instance: by default 1024 pages and 4096 buckets

// ------------ Frame implementation: 4KB-aligned buffer, suitable for O_DIRECT-friendly I/O ------------

BufferPool::Frame::Frame() {
    void* p = nullptr;
    // Allocate a buffer of size PAGE_SIZE, aligned to PAGE_SIZE
    int rc = ::posix_memalign(&p, PAGE_SIZE, PAGE_SIZE);
    if (rc != 0 || !p) {
        throw std::bad_alloc();
    }
    data  = static_cast<char*>(p);
    valid = 0;
}

BufferPool::Frame::~Frame() {
    if (data) {
        std::free(data);
        data = nullptr;
    }
}

BufferPool::Frame::Frame(Frame&& other) noexcept {
    id    = other.id;
    data  = other.data;
    valid = other.valid;
    other.data  = nullptr;
    other.valid = 0;
}

BufferPool::Frame& BufferPool::Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        if (data) std::free(data);
        id    = other.id;
        data  = other.data;
        valid = other.valid;
        other.data  = nullptr;
        other.valid = 0;
    }
    return *this;
}

// ------------ BufferPool core ------------

BufferPool::BufferPool(std::size_t capacity_pages, std::size_t num_buckets)
    : capacity_(capacity_pages),
      table_(std::max<std::size_t>(num_buckets, 16))   // At least 16 buckets
{
    if (capacity_ == 0) capacity_ = 1;  // Prevent zero capacity
}

uint64_t BufferPool::splitmix64(uint64_t x) {
    // Classic splitmix64 hash function, public domain
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

std::size_t BufferPool::bucket_index(const PageId& id) const {
    // Combine fd and page_no into a single 64-bit key, then hash it
    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(id.fd)) << 32)
                   ^ id.page_no;
    uint64_t h = splitmix64(key);
    return static_cast<std::size_t>(h % table_.size());
}

BufferPool::Frame& BufferPool::get_frame(int fd, uint64_t page_no) {
    PageId id{fd, page_no};
    std::size_t b = bucket_index(id);
    Bucket& bucket = table_[b];

    // 1) Look up in the bucket's linked list (separate chaining)
    for (auto it = bucket.begin(); it != bucket.end(); ++it) {
        FrameIterator fit = *it;
        if (fit->id == id) {
            // Cache hit: move the frame to the front of the LRU list (most recently used)
            lru_.splice(lru_.begin(), lru_, fit);
            return *lru_.begin();
        }
    }

    // 2) Cache miss: if the pool is full, evict the LRU frame (tail of the LRU list)
    if (size_ >= capacity_ && !lru_.empty()) {
        FrameIterator victim = std::prev(lru_.end());
        PageId vid = victim->id;
        std::size_t vb = bucket_index(vid);
        Bucket& vbucket = table_[vb];

        // Remove this iterator from the corresponding bucket's list
        vbucket.remove(victim);

        // Remove the frame from the LRU list
        lru_.erase(victim);
        --size_;
    }

    // 3) Read a new page from disk and insert it into the LRU head and bucket list
    Frame frame;            // Constructor allocates a 4KB aligned buffer
    frame.id = id;
    frame.valid = 0;

    off_t off = static_cast<off_t>(page_no * PAGE_SIZE);
    ssize_t n = ::pread(fd, frame.data, PAGE_SIZE, off);
    if (n < 0) {
        throw std::runtime_error("BufferPool: pread failed");
    }
    frame.valid = static_cast<std::size_t>(n);

    // Insert into the head of the LRU list
    lru_.push_front(std::move(frame));
    FrameIterator new_it = lru_.begin();
    ++size_;

    // Insert into the bucket's linked list (chaining)
    bucket.push_front(new_it);

    return *new_it;
}

void BufferPool::read_bytes(int fd, uint64_t offset, void* dst, std::size_t len) {
    char* out = static_cast<char*>(dst);
    std::size_t remaining = len;

    while (remaining > 0) {
        uint64_t page_no  = offset / PAGE_SIZE;
        std::size_t page_off = static_cast<std::size_t>(offset % PAGE_SIZE);

        Frame& frame = get_frame(fd, page_no);

        if (page_off >= frame.valid) {
            throw std::runtime_error("BufferPool: read beyond file end");
        }

        std::size_t can_copy = std::min(remaining, frame.valid - page_off);
        std::memcpy(out, frame.data + page_off, can_copy);

        out       += can_copy;
        offset    += can_copy;
        remaining -= can_copy;
    }
}
