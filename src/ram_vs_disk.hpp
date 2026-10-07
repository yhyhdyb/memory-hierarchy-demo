#pragma once
// ============================================================================
// ram_vs_disk.hpp —— 实验2：内存 vs 磁盘（带宽与随机访问延迟）
// ============================================================================
#include <cstddef>

struct MemDiskResult {
    // 内存
    double ram_write_gbps = 0;  // 顺序写带宽
    double ram_read_gbps  = 0;  // 顺序读带宽
    double ram_copy_gbps  = 0;  // 拷贝带宽（读+写双向流量）
    double ram_random_ns  = 0;  // 随机访问延迟（复用指针追逐法）

    // 磁盘（普通文件，测试后删除）
    double disk_write_gbps = 0; // 顺序写带宽（含落盘 flush）
    double disk_read_gbps  = 0; // 顺序读带宽
    double disk_random_us  = 0; // 随机读延迟（µs）
};

MemDiskResult run_mem_vs_disk(size_t mem_bytes, size_t disk_bytes);
