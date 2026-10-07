#pragma once
// ============================================================================
// bench_common.hpp —— 公共工具：计时、物理内存探测、CPU 缓存信息探测
// 跨平台：Windows (MSVC / MinGW) + Linux / macOS
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

inline double seconds_between(TimePoint a, TimePoint b) { return std::chrono::duration<double>(b - a).count(); }
inline double ms_between(TimePoint a, TimePoint b)      { return seconds_between(a, b) * 1e3; }
inline double ns_between(TimePoint a, TimePoint b)      { return seconds_between(a, b) * 1e9; }

// ---------------- 物理内存总量 ----------------
inline uint64_t total_physical_memory_bytes() {
#ifdef _WIN32
    MEMORYSTATUSEX st;
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st)) return st.ullTotalPhys;
    return 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGE_SIZE);
    return (pages > 0 && psize > 0) ? (uint64_t)pages * (uint64_t)psize : 0;
#endif
}

// ---------------- CPU 缓存信息 ----------------
struct CacheLevelInfo {
    int    level;      // 1 / 2 / 3
    size_t size_bytes;
    size_t line_bytes;
};

inline std::vector<CacheLevelInfo> detect_cache_levels() {
    std::vector<CacheLevelInfo> out;
#ifdef _WIN32
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (len == 0) return out;
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buf(
        len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!GetLogicalProcessorInformation(buf.data(), &len)) return out;
    for (const auto& e : buf) {
        if (e.Relationship != RelationCache) continue;
        int    lvl  = (int)e.Cache.Level;
        size_t sz   = (size_t)e.Cache.Size;
        size_t line = (size_t)e.Cache.LineSize;
        bool dup = false;
        for (const auto& x : out)
            if (x.level == lvl && x.size_bytes == sz) { dup = true; break; }
        if (!dup) out.push_back({lvl, sz, line});
    }
    std::sort(out.begin(), out.end(),
              [](const CacheLevelInfo& a, const CacheLevelInfo& b) { return a.level < b.level; });
#else
    struct Probe { int lvl; long size; long line; };
    const Probe probes[] = {
        {1, _SC_LEVEL1_DCACHE_SIZE,  _SC_LEVEL1_DCACHE_LINESIZE},
        {2, _SC_LEVEL2_CACHE_SIZE,   _SC_LEVEL2_CACHE_LINESIZE},
        {3, _SC_LEVEL3_CACHE_SIZE,   _SC_LEVEL3_CACHE_LINESIZE},
    };
    for (const auto& p : probes) {
        long sz = sysconf(p.size);
        if (sz > 0) out.push_back({p.lvl, (size_t)sz, (size_t)sysconf(p.line)});
    }
#endif
    return out;
}

inline void print_cache_info() {
    auto c = detect_cache_levels();
    std::printf("  检测到的 CPU 缓存: ");
    if (c.empty()) { std::printf("未能自动检测（将按常见 CPU 容量给出提示）\n"); return; }
    for (size_t i = 0; i < c.size(); ++i)
        std::printf("%sL%d=%zuKB(行%zuB)", i ? ", " : "",
                    c[i].level, c[i].size_bytes / 1024, c[i].line_bytes);
    std::printf("\n");
}

// ---------------- 字节数格式化 ----------------
inline std::string fmt_bytes(size_t bytes) {
    char buf[64];
    if      (bytes >= 1024ull * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024)        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024ull)               std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else                                     std::snprintf(buf, sizeof(buf), "%zu B", bytes);
    return buf;
}
