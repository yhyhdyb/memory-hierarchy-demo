#pragma once
// ============================================================================
// dma_sim.hpp —— 实验3：DMA 模拟（CPU 亲自搬运 vs 硬件搬运并行）
// ============================================================================
#include <cstddef>

struct DmaSimResult {
    size_t bytes;
    double pio_copy_ms;    // 无DMA：搬运耗时
    double pio_compute_ms; // 无DMA：计算耗时（搬运完成后才开始）
    double pio_total_ms;   // 无DMA：总耗时 = 搬运 + 计算（串行）
    double dma_total_ms;   // 有DMA：总耗时（传输与计算重叠）
    double dma_compute_ms; // 有DMA：CPU 并行计算耗时
    double dma_wait_ms;    // 有DMA：CPU 等待传输完成（模拟中断响应）
    double saved_ms;       // 节省时间
    double saved_pct;      // 节省百分比
};

DmaSimResult run_dma_sim(size_t bytes);
