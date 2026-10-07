#include "dma_sim.hpp"
#include "bench_common.hpp"

#include <algorithm>
#include <cstring>
#include <future>
#include <vector>

// CPU 密集型工作：xorshift 依赖链。只占用 ALU/寄存器、几乎不占内存带宽，
// 因此能与“传输”真正并行，而不像 memcpy 那样互相挤占内存带宽。
static uint64_t cpu_work(size_t iters) {
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < iters; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x += 0xA5A5A5A5A5A5A5A5ull ^ i;
    }
    return x;
}

// 校准：使 cpu_work 的耗时 ≈ 一次 memcpy 传输的耗时，保证对比公平
static size_t calibrate_compute(double target_ms) {
    auto t0 = Clock::now();
    volatile uint64_t s = cpu_work(100'000'000);
    (void)s;
    auto t1 = Clock::now();
    double per_iter_ms = ms_between(t0, t1) / 100'000'000.0;
    size_t iters = (size_t)(target_ms / per_iter_ms * 1.05);
    return std::max<size_t>(iters, 1000);
}

DmaSimResult run_dma_sim(size_t bytes) {
    DmaSimResult r{};
    r.bytes = bytes;

    std::vector<char> src(bytes, 0x3C), dst(bytes, 0x00);

    // 基准：一次传输的耗时
    auto t0 = Clock::now();
    std::memcpy(dst.data(), src.data(), bytes);
    auto t1 = Clock::now();
    double copy_ms = ms_between(t0, t1);
    size_t iters = calibrate_compute(copy_ms);

    // ---- 无 DMA（PIO 模式）：CPU 亲自搬运，搬运期间无法干别的 ----
    auto p0 = Clock::now();
    auto c0 = Clock::now();
    std::memcpy(dst.data(), src.data(), bytes);   // CPU 逐块搬运（占用 CPU）
    auto c1 = Clock::now();
    volatile uint64_t w1 = cpu_work(iters);       // 之后 CPU 才能开始计算
    (void)w1;
    auto p1 = Clock::now();
    r.pio_copy_ms    = ms_between(c0, c1);
    r.pio_compute_ms = ms_between(c1, p1);
    r.pio_total_ms   = ms_between(p0, p1);

    // ---- 有 DMA：CPU 发起传输后立即返回，DMA 引擎后台搬运 ----
    auto d0 = Clock::now();
    std::future<void> engine = std::async(std::launch::async, [&] {
        std::memcpy(dst.data(), src.data(), bytes);   // “DMA 引擎”在后台搬运
    });
    auto w0 = Clock::now();
    volatile uint64_t w2 = cpu_work(iters);           // CPU 发起后并行干自己的活
    (void)w2;
    auto w1_ = Clock::now();
    engine.wait();                                    // 模拟“传输完成中断”通知 CPU
    auto d1 = Clock::now();
    r.dma_compute_ms = ms_between(w0, w1_);
    r.dma_total_ms   = ms_between(d0, d1);
    r.dma_wait_ms    = std::max(0.0, r.dma_total_ms - r.dma_compute_ms);

    r.saved_ms  = r.pio_total_ms - r.dma_total_ms;
    r.saved_pct = r.pio_total_ms > 0 ? r.saved_ms / r.pio_total_ms * 100.0 : 0;
    return r;
}
