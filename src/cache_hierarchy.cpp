#include "cache_hierarchy.hpp"
#include "bench_common.hpp"

#include <algorithm>
#include <numeric>
#include <random>

double measure_random_access_latency_ns(size_t bytes) {
    const size_t n = bytes / sizeof(int);
    if (n < 16) return 0.0;

    // 构建一条随机排列的“链表”：next[i] = 下一个随机下标。
    // 从 idx 出发，每次 next[idx] 都跳到数组中的随机位置，
    // 硬件预取器无法预测，测到的就是真实的随机访问延迟。
    volatile int* next = new volatile int[n];
    {
        std::vector<int> perm(n);
        std::iota(perm.begin(), perm.end(), 0);
        std::mt19937 rng(0xC0FFEEu);
        std::shuffle(perm.begin(), perm.end(), rng);
        for (size_t i = 0; i + 1 < n; ++i) next[perm[i]] = perm[i + 1];
        next[perm[n - 1]] = perm[0];
    }

    // 迭代次数：按数组大小分配时间预算（每档约 10~30ms 的测量窗口）
    long long iters;
    if      (bytes <= 64ull * 1024)         iters = 4000000;
    else if (bytes <= 1ull * 1024 * 1024)   iters = 1500000;
    else if (bytes <= 16ull * 1024 * 1024)  iters = 400000;
    else if (bytes <= 128ull * 1024 * 1024) iters = 120000;
    else                                    iters = 50000;

    volatile size_t idx = 0;
    for (long long i = 0; i < 2000; ++i) idx = (size_t)next[idx];   // 预热，稳定状态

    auto t0 = Clock::now();
    for (long long i = 0; i < iters; ++i) idx = (size_t)next[idx];
    auto t1 = Clock::now();

    delete[] next;
    return ns_between(t0, t1) / (double)iters;
}

static std::string region_hint_for(size_t bytes, const std::vector<CacheLevelInfo>& caches) {
    for (const auto& c : caches)
        if (bytes <= c.size_bytes)
            return "L" + std::to_string(c.level) + " 内(命中率高)";
    return std::string("超出最后一级缓存 → 主存") +
           (bytes >= 128ull * 1024 * 1024 ? " + TLB 抖动" : "");
}

std::vector<LatencySample> run_cache_hierarchy_scan(size_t max_bytes) {
    static const long kb_list[] = {4,    8,    16,   32,   64,   128,  256,  512,
                                   1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144};
    auto caches = detect_cache_levels();
    std::vector<LatencySample> out;
    for (long kb : kb_list) {
        size_t bytes = (size_t)kb * 1024;
        if (bytes > max_bytes) break;
        std::printf("  测量 %s ... ", fmt_bytes(bytes).c_str());
        std::fflush(stdout);
        double ns = measure_random_access_latency_ns(bytes);
        std::printf("完成\n");
        out.push_back({bytes, ns, region_hint_for(bytes, caches)});
    }
    return out;
}
