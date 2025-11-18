#include <gtest/gtest.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include "../buffer_pool.h"

// ---- helper: create a temp file with repeated pattern ----
int create_temp_file(const char* path, const char* pattern, size_t repeat_MB) {
    int fd = ::open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) return -1;

    size_t len = strlen(pattern);
    size_t total = repeat_MB * 1024 * 1024;

    std::vector<char> buf(len);
    memcpy(buf.data(), pattern, len);

    size_t written = 0;
    while (written < total) {
        ::write(fd, buf.data(), len);
        written += len;
    }
    ::lseek(fd, 0, SEEK_SET);
    return fd;
}

// ---- helper: log wrapper for read_bytes ----
void log_read(BufferPool& pool, int fd, uint64_t off, void* dst, size_t len) {
    printf("[TEST] read(fd=%d, off=%lu, len=%lu)\n",
           fd, (unsigned long)off, (unsigned long)len);
    fflush(stdout);

    pool.read_bytes(fd, off, dst, len);
}


// ---------------- TEST 1: basic single-page read -------------------------
TEST(BufferPoolTest, FirstReadMissThenHit) {
    BufferPool pool(4 /*capacity*/, 8 /*buckets*/);

    int fd = create_temp_file("tmp1.bin", "ABCDEFGH", 1);
    ASSERT_GT(fd, 0);

    char buf1[16];
    char buf2[16];

    std::cout << "\n=== TEST 1: FirstReadMissThenHit ===\n";

    // First read → MISS
    log_read(pool, fd, 0, buf1, 8);
    ASSERT_EQ(memcmp(buf1, "ABCDEFGH", 8), 0);

    // Second read → HIT
    log_read(pool, fd, 0, buf2, 8);
    ASSERT_EQ(memcmp(buf2, "ABCDEFGH", 8), 0);

    ::close(fd);
}


// ---------------- TEST 2: cross-page read -------------------------
TEST(BufferPoolTest, CrossPageRead) {
    BufferPool pool(8, 16);

    int fd = create_temp_file("tmp2.bin", "XYZ12345", 2);
    ASSERT_GT(fd, 0);

    std::cout << "\n=== TEST 2: CrossPageRead ===\n";

    const size_t off = BufferPool::PAGE_SIZE - 4;
    char out[16];

    log_read(pool, fd, off, out, 8);

    ASSERT_EQ(memcmp(out, "2345XYZ1", 8), 0);

    ::close(fd);
}


// ---------------- TEST 3: LRU eviction -------------------------
TEST(BufferPoolTest, LRUEviction) {
    BufferPool pool(2 /*capacity*/, 8);

    int fd = create_temp_file("tmp3.bin", "ABCDEFGH", 1);
    ASSERT_GT(fd, 0);

    char tmp[8];

    std::cout << "\n=== TEST 3: LRUEviction ===\n";

    // Access page 0
    log_read(pool, fd, 0 * BufferPool::PAGE_SIZE, tmp, 8);

    // Access page 1
    log_read(pool, fd, 1 * BufferPool::PAGE_SIZE, tmp, 8);

    // Access page 2 → evict page 0
    log_read(pool, fd, 2 * BufferPool::PAGE_SIZE, tmp, 8);

    // Access page 0 again → re-load after eviction
    log_read(pool, fd, 0 * BufferPool::PAGE_SIZE, tmp, 8);
    ASSERT_EQ(memcmp(tmp, "ABCDEFGH", 8), 0);

    ::close(fd);
}


// ---------------- TEST 4: multiple file descriptors -------------------------
TEST(BufferPoolTest, MultiFDIsolation) {
    BufferPool pool(4, 8);

    int fd1 = create_temp_file("fileA.bin", "AAAAAAA1", 1);
    int fd2 = create_temp_file("fileB.bin", "BBBBBBB2", 1);

    ASSERT_GT(fd1, 0);
    ASSERT_GT(fd2, 0);

    char a[8], b[8];

    std::cout << "\n=== TEST 4: MultiFDIsolation ===\n";

    log_read(pool, fd1, 0, a, 8);
    log_read(pool, fd2, 0, b, 8);

    ASSERT_EQ(memcmp(a, "AAAAAAA1", 8), 0);
    ASSERT_EQ(memcmp(b, "BBBBBBB2", 8), 0);

    ::close(fd1);
    ::close(fd2);
}

