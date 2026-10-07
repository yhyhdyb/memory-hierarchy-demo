// ============================================================================
// main.cpp —— 内存 · 磁盘 · 多级访存 · DMA 实测演示（入口）
//
// 用法:
//   memhier             运行全部三个实验
//   memhier 1|2|3       只运行指定实验
//   memhier --disk-mb=N 磁盘测试文件大小(MB)，默认 512
//   memhier --mem-mb=N  内存/传输测试大小(MB)，默认 512
// ============================================================================
#include "bench_common.hpp"
#include "cache_hierarchy.hpp"
#include "ram_vs_disk.hpp"
#include "dma_sim.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

static void banner() {
    std::printf("\n");
    std::printf("  ============================================================\n");
    std::printf("   内存 · 磁盘 · 多级访存 · DMA —— 实测演示\n");
    std::printf("  ============================================================\n");
    std::printf("  实验1  多级访存：L1/L2/L3 缓存与主存的访问延迟阶梯\n");
    std::printf("  实验2  内存 vs 磁盘：带宽与随机访问延迟对比\n");
    std::printf("  实验3  DMA：CPU 亲自搬运 vs 硬件搬运并行\n");
}

static void print_sep(const char* title) {
    std::printf("\n──────────────────────────────────────────────────────────────\n");
    std::printf("  %s\n", title);
    std::printf("──────────────────────────────────────────────────────────────\n");
}

static void usage() {
    std::printf("用法: memhier [1|2|3] [--disk-mb=N] [--mem-mb=N] [--all]\n");
    std::printf("  无参数        = 依次运行全部实验\n");
    std::printf("  1 / 2 / 3     = 只运行指定实验\n");
    std::printf("  --disk-mb=N   = 磁盘测试文件大小(MB)，默认 512\n");
    std::printf("  --mem-mb=N    = 内存/传输测试大小(MB)，默认 512\n");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);   // 让中文输出在 Windows 终端正常显示
#endif

    bool run_cache = false, run_memdisk = false, run_dma = false, run_all = true;
    size_t disk_mb = 512, mem_mb = 512;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "1")                 { run_cache = true; run_all = false; }
        else if (a == "2")                 { run_memdisk = true; run_all = false; }
        else if (a == "3")                 { run_dma = true; run_all = false; }
        else if (a == "--all")             { run_all = true; }
        else if (a.rfind("--disk-mb=", 0) == 0) disk_mb = (size_t)std::atoll(a.c_str() + 10);
        else if (a.rfind("--mem-mb=", 0) == 0)  mem_mb  = (size_t)std::atoll(a.c_str() + 9);
        else if (a == "-h" || a == "--help") { usage(); return 0; }
    }
    if (run_all) run_cache = run_memdisk = run_dma = true;

    // 按物理内存裁剪默认大小，避免内存不足
    uint64_t phys = total_physical_memory_bytes();
    if (phys) {
        size_t phys_mb = (size_t)(phys >> 20);
        mem_mb = std::min(mem_mb, std::max<size_t>(phys_mb / 4, 64));
    }

    banner();
    print_sep("系统信息");
    print_cache_info();
    if (phys) std::printf("  物理内存: %s\n", fmt_bytes((size_t)phys).c_str());

    if (run_cache) {
        print_sep("实验1 多级访存：缓存层级延迟扫描（随机访问，指针追逐法）");
        size_t max_kb = 262144;                    // 默认扫到 256MB
        if (phys && (size_t)(phys >> 20) < 2048) max_kb = 65536;  // 小内存机器降档
        auto samples = run_cache_hierarchy_scan(max_kb * 1024);
        std::printf("\n  数组大小      单次访问延迟    预期所在层级\n");
        std::printf("  ────────────────────────────────────────────────\n");
        for (const auto& s : samples)
            std::printf("  %-12s  %8.2f ns    %s\n",
                        fmt_bytes(s.size_bytes).c_str(), s.ns_per_access, s.region_hint.c_str());
        std::printf("\n  解读：数组超过某一级缓存容量后，访问延迟会明显跳升，\n");
        std::printf("        这条“阶梯”就是 L1 → L2 → L3 → 主存的延迟轮廓。\n");
    }

    if (run_memdisk) {
        print_sep("实验2 内存 vs 磁盘");
        size_t mem_bytes  = mem_mb * 1024ull * 1024;
        size_t disk_bytes = disk_mb * 1024ull * 1024;
        auto r = run_mem_vs_disk(mem_bytes, disk_bytes);

        std::printf("\n  ── 内存 (%s) ──────────────────────────────────\n", fmt_bytes(mem_bytes).c_str());
        std::printf("    顺序写带宽       %9.2f GB/s\n", r.ram_write_gbps);
        std::printf("    顺序读带宽       %9.2f GB/s\n", r.ram_read_gbps);
        std::printf("    拷贝带宽(读+写)  %9.2f GB/s\n", r.ram_copy_gbps);
        std::printf("    随机访问延迟     %9.2f ns\n",   r.ram_random_ns);

        std::printf("\n  ── 磁盘文件 (%s, 用完即删) ───────────────────\n", fmt_bytes(disk_bytes).c_str());
        std::printf("    顺序写带宽       %9.2f GB/s\n", r.disk_write_gbps);
        std::printf("    顺序读带宽       %9.2f GB/s  (无缓冲, 绕开页缓存)\n", r.disk_read_gbps);
        std::printf("    随机读延迟       %9.2f µs   (无缓冲随机读)\n", r.disk_random_us);

        double ratio = r.disk_random_us > 0 ? (r.disk_random_us * 1000.0) / r.ram_random_ns : 0;
        std::printf("\n  数量级对比：内存随机访问比磁盘随机读快约 %.0f 倍\n", ratio);
        std::printf("  （真实机械硬盘随机延迟 5~15ms，NVMe SSD 20~100µs）\n");
    }

    if (run_dma) {
        print_sep("实验3 DMA：谁在搬运数据？");
        size_t dma_bytes = mem_mb * 1024ull * 1024;
        auto r = run_dma_sim(dma_bytes);

        std::printf("  传输数据量: %s\n", fmt_bytes(r.bytes).c_str());
        std::printf("\n  [无 DMA（PIO 模式）—— CPU 亲自搬运]\n");
        std::printf("    阶段1 搬运数据 ..... %8.2f ms\n", r.pio_copy_ms);
        std::printf("    阶段2 执行计算 ..... %8.2f ms\n", r.pio_compute_ms);
        std::printf("    总耗时 ............. %8.2f ms   (CPU 全程忙碌)\n", r.pio_total_ms);
        std::printf("\n  [有 DMA —— CPU 发起传输后立即返回]\n");
        std::printf("    CPU 发起传输 ....... %8.2f ms\n",
                    std::max(0.0, r.dma_total_ms - r.dma_compute_ms - r.dma_wait_ms));
        std::printf("    CPU 并行执行计算 ... %8.2f ms\n", r.dma_compute_ms);
        std::printf("    等待传输完成(中断) . %8.2f ms\n", r.dma_wait_ms);
        std::printf("    总耗时 ............. %8.2f ms   (CPU 几乎零等待)\n", r.dma_total_ms);
        std::printf("\n  对比：DMA 模式节省 %8.2f ms（%.1f%%）\n", r.saved_ms, r.saved_pct);
        std::printf("  说明：真实 DMA 由硬件控制器（磁盘/网卡/显卡）直接读写内存，\n");
        std::printf("        CPU 只配置描述符并响应完成中断；本实验用后台线程模拟该机制。\n");
    }

    std::printf("\n全部实验完成。\n");
    return 0;
}
