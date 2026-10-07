#pragma once
// ============================================================================
// cache_hierarchy.hpp —— 实验1：多级访存（L1/L2/L3 缓存与主存的延迟阶梯）
// ============================================================================
#include <cstddef>
#include <string>
#include <vector>

struct LatencySample {
    size_t size_bytes;       // 被测数组大小
    double ns_per_access;    // 每次随机访问的平均延迟（ns）
    std::string region_hint; // 依据检测到的缓存容量给出的预期层级
};

// 用“指针追逐（pointer chasing）”法测量 size_bytes 数组的随机访问延迟（ns/次）。
// 该方法令每次访问都跳到数组中随机位置，硬件预取器无法预测，
// 因此测到的是真实的随机访问延迟（由缓存未命中主导）。
double measure_random_access_latency_ns(size_t bytes);

// 从 4KB 到 max_bytes 逐档扫描，返回延迟曲线。
std::vector<LatencySample> run_cache_hierarchy_scan(size_t max_bytes);
