#include "ram_vs_disk.hpp"
#include "bench_common.hpp"
#include "cache_hierarchy.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <malloc.h>   // _aligned_malloc / _aligned_free（无缓冲IO）
#include <random>
#include <vector>

#ifdef _WIN32
#include <io.h>   // _commit
#else
#include <unistd.h> // fsync
#endif

// ---------------- 内存 ----------------

static double mem_write_gbps(size_t bytes) {
    std::vector<char> buf(bytes);
    double best = 0;
    for (int r = 0; r < 3; ++r) {
        auto t0 = Clock::now();
        std::memset(buf.data(), 0xAB, bytes);
        auto t1 = Clock::now();
        best = std::max(best, bytes / 1e9 / seconds_between(t0, t1));
    }
    return best;
}

static double mem_read_gbps(size_t bytes) {
    std::vector<uint64_t> buf(bytes / 8, 0x123456789ABCDEF0ull);
    volatile uint64_t sink = 0;
    double best = 0;
    for (int r = 0; r < 3; ++r) {
        auto t0 = Clock::now();
        // 每次读满 64B 一个缓存行（8×uint64），volatile 防止整段被优化掉
        for (size_t i = 0; i + 8 <= buf.size(); i += 8)
            sink += buf[i] + buf[i + 1] + buf[i + 2] + buf[i + 3] +
                    buf[i + 4] + buf[i + 5] + buf[i + 6] + buf[i + 7];
        auto t1 = Clock::now();
        best = std::max(best, bytes / 1e9 / seconds_between(t0, t1));
    }
    return best;
}

static double mem_copy_gbps(size_t bytes) {
    std::vector<char> src(bytes, 0x11), dst(bytes, 0x22);
    double best = 0;
    for (int r = 0; r < 3; ++r) {
        auto t0 = Clock::now();
        std::memcpy(dst.data(), src.data(), bytes);
        auto t1 = Clock::now();
        // memcpy 同时产生读流量和写流量，统计双向总流量
        best = std::max(best, 2.0 * bytes / 1e9 / seconds_between(t0, t1));
    }
    return best;
}

// ---------------- 磁盘（普通文件） ----------------

static void flush_to_disk(FILE* f) {
#ifdef _WIN32
    _commit(_fileno(f));   // 强制写穿到物理磁盘（等价于 fsync）
#else
    fsync(fileno(f));
#endif
}

static double disk_write_gbps(const std::string& path, size_t bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::perror("fopen(write)"); return 0; }
    std::vector<char> buf(1u << 20, 0x5A);   // 1MB 用户态缓冲
    auto t0 = Clock::now();
    size_t done = 0;
    while (done < bytes) {
        size_t chunk = std::min(buf.size(), bytes - done);
        if (std::fwrite(buf.data(), 1, chunk, f) != chunk) break;
        done += chunk;
    }
    std::fflush(f);
    flush_to_disk(f);                        // 计入真实落盘时间
    auto t1 = Clock::now();
    std::fclose(f);
    return done ? done / 1e9 / seconds_between(t0, t1) : 0;
}

static double disk_read_gbps(const std::string& path, size_t bytes) {
#ifdef _WIN32
    // 无缓冲读：绕开 OS 页缓存，测真实磁盘顺序读带宽。
    // 要求偏移与长度均为物理扇区大小整数倍（此处按 1MB 块读，天然满足）。
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        const size_t chunk = 1u << 20;                 // 1MB，4096 的整数倍
        void* buf = _aligned_malloc(chunk, 4096);      // 无缓冲 IO 要求缓冲区对齐
        auto t0 = Clock::now();
        size_t done = 0;
        while (done < bytes) {
            DWORD want = (DWORD)std::min(chunk, bytes - done);
            DWORD got  = 0;
            if (!ReadFile(h, buf, want, &got, nullptr) || got == 0) break;
            done += got;
        }
        auto t1 = Clock::now();
        _aligned_free(buf);
        CloseHandle(h);
        return done ? done / 1e9 / seconds_between(t0, t1) : 0;
    }
    std::perror("CreateFileA(read)");                  // 失败则回退普通读
#endif
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::perror("fopen(read)"); return 0; }
    std::vector<char> buf(1u << 20);
    auto t0 = Clock::now();
    size_t done = 0;
    while (done < bytes) {
        size_t chunk = std::min(buf.size(), bytes - done);
        size_t got = std::fread(buf.data(), 1, chunk, f);
        if (got == 0) break;
        done += got;
    }
    auto t1 = Clock::now();
    std::fclose(f);
    return done ? done / 1e9 / seconds_between(t0, t1) : 0;
}

static double disk_random_read_us(const std::string& path, size_t file_bytes) {
    const int    reads = 64;                 // 随机读 64 次
    const size_t chunk = 4096;               // 每次 4KB（扇区对齐）
#ifdef _WIN32
    // 无缓冲随机读：绕开页缓存，测真实磁盘随机 IO 延迟
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        void* buf = _aligned_malloc(chunk, 4096);
        std::mt19937_64 rng(0xD15Cu);
        std::uniform_int_distribution<size_t> dist(0, file_bytes - chunk);
        LARGE_INTEGER li;
        double total = 0;
        int    n = 0;
        for (int i = 0; i < reads; ++i) {
            size_t off = dist(rng) & ~(size_t)(chunk - 1);
            li.QuadPart = (LONGLONG)off;
            auto t0 = Clock::now();
            if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) continue;
            DWORD got = 0;
            if (!ReadFile(h, buf, (DWORD)chunk, &got, nullptr) || got != chunk) continue;
            auto t1 = Clock::now();
            total += ms_between(t0, t1) * 1000.0;   // ms → µs
            ++n;
        }
        _aligned_free(buf);
        CloseHandle(h);
        if (n) return total / n;
    }
    std::perror("CreateFileA(random read)");         // 失败则回退普通读
#endif
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    std::vector<char> buf(chunk);
    std::mt19937_64 rng(0xD15Cu);
    std::uniform_int_distribution<size_t> dist(0, file_bytes - chunk);
    double total = 0;
    int    n = 0;
    for (int i = 0; i < reads; ++i) {
        size_t off = dist(rng) & ~(size_t)(chunk - 1);
        auto t0 = Clock::now();
        if (std::fseek(f, (long)off, SEEK_SET) != 0) continue;
        if (std::fread(buf.data(), 1, chunk, f) != chunk) continue;
        auto t1 = Clock::now();
        total += ms_between(t0, t1) * 1000.0;
        ++n;
    }
    std::fclose(f);
    return n ? total / n : 0;
}

MemDiskResult run_mem_vs_disk(size_t mem_bytes, size_t disk_bytes) {
    MemDiskResult r;

    std::printf("  内存顺序写 (memset, %s, 3轮取最优) ...\n", fmt_bytes(mem_bytes).c_str());
    r.ram_write_gbps = mem_write_gbps(mem_bytes);
    std::printf("  内存顺序读 (逐缓存行累加, %s) ...\n", fmt_bytes(mem_bytes).c_str());
    r.ram_read_gbps = mem_read_gbps(mem_bytes);
    std::printf("  内存拷贝 (memcpy, %s) ...\n", fmt_bytes(mem_bytes).c_str());
    r.ram_copy_gbps = mem_copy_gbps(mem_bytes);
    std::printf("  内存随机访问延迟 (指针追逐, %s) ...\n",
                fmt_bytes(std::min<size_t>(mem_bytes, 64ull * 1024 * 1024)).c_str());
    r.ram_random_ns = measure_random_access_latency_ns(std::min<size_t>(mem_bytes, 64ull * 1024 * 1024));

    const std::string path = "disk_bench.tmp";
    std::printf("  磁盘顺序写 (%s → %s, 含落盘) ...\n", fmt_bytes(disk_bytes).c_str(), path.c_str());
    r.disk_write_gbps = disk_write_gbps(path, disk_bytes);
    std::printf("  磁盘顺序读 (%s) ...\n", path.c_str());
    r.disk_read_gbps = disk_read_gbps(path, disk_bytes);
    std::printf("  磁盘随机读延迟 (4KB × 64 次) ...\n");
    r.disk_random_us = disk_random_read_us(path, disk_bytes);
    std::remove(path.c_str());
    return r;
}
