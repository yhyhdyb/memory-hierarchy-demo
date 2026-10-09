// ============================================================================
// main.cpp
// 演示：自定义容器 allocator（内存池分配器）对性能的优化
//
// 基准场景：
//   1. list_insert    —— std::list 大量插入+遍历+删除（每节点 1 次小分配）
//   2. map_insert     —— std::map 大量插入+查找+删除（每节点 1 次小分配）
//   3. vector_churn   —— std::vector 反复扩容/缩容（批量分配）
//   4. churn          —— 高频创建/销毁节点（内存池复用 vs malloc/free 往返）
//
// 输出：
//   - 控制台：每轮耗时、加速比
//   - perf_log.txt：结构化日志（含环境信息、每轮结果、分配统计、结论）
//
// 注：本机 MinGW GCC 9.3 编译器在 double 浮点字面量/运算上存在 ICE
//     （内部编译器错误，与代码无关），故本文件全部使用整数运算：
//     - 计时使用 clock() 返回的整数 tick（转换为微秒，仍保留毫秒精度）
//     - 加速比用整数百分比表示（speedup% = std_t * 100 / pool_t）
//     分配器代码（pool_allocator.h）本身不受此限制影响。
// ============================================================================

#include "pool_allocator.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <list>
#include <map>
#include <string>
#include <vector>

using namespace std;

// 基准配置
static const int NODE_NUM = 200000;        // 节点数量
static const int LOOKUP_NUM = 50000;       // 查找次数
static const int ROUNDS = 3;               // 轮次
static const int CHURN_ROUNDS = 40;        // 高频增删轮数
static const int CHURN_NODES = 5000;       // 每轮增删节点数

// ----------------------------------------------------------------------------
// 计时工具：clock() 返回整数 tick，统一换算为微秒（整数）
// ----------------------------------------------------------------------------
static std::uint64_t now_us() {
    return static_cast<std::uint64_t>(clock()) * 1000000ull / CLOCKS_PER_SEC;
}

// 微秒 -> 毫秒字符串（整数，保留 2 位小数）
static string us_to_ms(std::uint64_t us) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%llu.%02llu",
             (unsigned long long)(us / 1000ull),
             (unsigned long long)((us % 1000ull) / 10ull));
    return string(buf);
}

// 字节 -> MB 字符串（整数，保留 2 位小数）
static string bytes_to_mb(std::uint64_t b) {
    // MB = b / 1048576，保留 2 位小数
    std::uint64_t mb = b / (1024ull * 1024ull);
    std::uint64_t frac = (b % (1024ull * 1024ull)) * 100ull / (1024ull * 1024ull);
    char buf[64];
    snprintf(buf, sizeof(buf), "%llu.%02llu", (unsigned long long)mb,
             (unsigned long long)frac);
    return string(buf);
}

// 加速比百分比（整数）：speedup_percent = std_us * 100 / pool_us
static std::uint64_t speedup_percent(std::uint64_t std_us, std::uint64_t pool_us) {
    if (pool_us == 0) return 0;
    return std_us * 100ull / pool_us;
}

// ----------------------------------------------------------------------------
// 简单的 LCG 伪随机数生成器（替代 mt19937，避免 <random> 的 ICE）
// ----------------------------------------------------------------------------
static std::uint32_t lcg_state = 42;
static std::uint32_t lcg_next() {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}
static int lcg_rand(int n) {
    return static_cast<int>(lcg_next() % static_cast<std::uint32_t>(n));
}

// ----------------------------------------------------------------------------
// 基准场景 1：std::list 大量插入 + 遍历 + 删除
// ----------------------------------------------------------------------------
template <typename Alloc>
static std::uint64_t bench_list_us(int n) {
    list<int, Alloc> lst;
    std::uint64_t t0 = now_us();
    for (int i = 0; i < n; ++i) lst.push_back(i);
    volatile long sum = 0;
    for (int v : lst) sum += v;
    for (int i = 0; i < n; ++i) lst.pop_front();
    (void)sum;
    return now_us() - t0;
}

// ----------------------------------------------------------------------------
// 基准场景 2：std::map 大量插入 + 查找 + 删除
// ----------------------------------------------------------------------------
template <typename Alloc>
static std::uint64_t bench_map_us(int n, int lookups) {
    map<int, int, less<int>, Alloc> mp;
    std::uint64_t t0 = now_us();
    for (int i = 0; i < n; ++i) mp.emplace(i, i * 2);
    volatile long found = 0;
    for (int i = 0; i < lookups; ++i) {
        int key = lcg_rand(n);
        auto it = mp.find(key);
        if (it != mp.end()) found += it->second;
    }
    for (int i = 0; i < n; ++i) mp.erase(mp.begin());
    (void)found;
    return now_us() - t0;
}

// ----------------------------------------------------------------------------
// 基准场景 3：std::vector 反复扩容/缩容（批量分配）
// ----------------------------------------------------------------------------
template <typename Alloc>
static std::uint64_t bench_vector_us(int n) {
    std::uint64_t t0 = now_us();
    for (int round = 0; round < 10; ++round) {
        vector<int, Alloc> vec;
        for (int i = 0; i < n; ++i) vec.push_back(i);
        for (int i = n - 1; i >= 0; --i) vec.pop_back();
    }
    return now_us() - t0;
}

// ----------------------------------------------------------------------------
// 基准场景 4：高频创建/销毁节点（内存池复用 vs malloc/free 往返）
// ----------------------------------------------------------------------------
struct Node {
    int a;
    int b;
    long payload[4];
};

template <typename Alloc>
static std::uint64_t bench_churn_us(int rounds, int per_round) {
    Alloc alloc;  // 共享同一分配器（PoolAllocator 内部共享同一池）
    std::uint64_t t0 = now_us();
    for (int r = 0; r < rounds; ++r) {
        vector<Node*> nodes;  // 指针数组用默认分配器，Node 本体用被测分配器
        nodes.reserve(per_round);
        for (int i = 0; i < per_round; ++i) {
            Node* p = alloc.allocate(1);
            new (p) Node{i, i + 1, {i, i, i, i}};
            nodes.push_back(p);
        }
        for (Node* p : nodes) {
            p->~Node();
            alloc.deallocate(p, 1);
        }
    }
    return now_us() - t0;
}

// ----------------------------------------------------------------------------
// 通用对比运行：返回 std/pool 耗时与分配统计
// ----------------------------------------------------------------------------
struct CaseResult {
    std::uint64_t t_std, t_pool;
    std::uint64_t std_calls, std_bytes;      // std 的分配调用次数与字节
    std::uint64_t pool_calls, pool_bytes;    // pool 的分配调用次数与字节
    std::uint64_t pool_real_malloc;          // pool 真正调用 malloc 的次数
    std::uint64_t pool_grow;                 // pool 扩容次数
};

template <typename StdAlloc, typename PoolAlloc, typename Fn>
static CaseResult run_pair(Fn fn) {
    CaseResult r;
    stats() = AllocationStats{};
    std::uint64_t t0 = now_us();
    fn.template run<StdAlloc>();
    r.t_std = now_us() - t0;
    r.std_calls = stats().std_alloc_calls;
    r.std_bytes = stats().std_alloc_bytes;

    stats() = AllocationStats{};
    t0 = now_us();
    fn.template run<PoolAlloc>();
    r.t_pool = now_us() - t0;
    r.pool_calls = stats().pool_alloc_calls;
    r.pool_bytes = stats().pool_alloc_bytes;
    r.pool_real_malloc = stats().pool_real_malloc;
    r.pool_grow = stats().pool_grow_calls;
    return r;
}

// ----------------------------------------------------------------------------
// 输出一轮结果（控制台 + 日志）
// ----------------------------------------------------------------------------
static void print_case(const char* name, const CaseResult& r, ofstream& log) {
    std::uint64_t sp = speedup_percent(r.t_std, r.t_pool);
    printf("  [%s]\n", name);
    printf("    std::allocator : %s ms\n", us_to_ms(r.t_std).c_str());
    printf("    PoolAllocator  : %s ms\n", us_to_ms(r.t_pool).c_str());
    printf("    加速比: %llu%% (std/pool)\n", (unsigned long long)sp);
    printf("    分配调用次数: std=%llu  pool=%llu (池内复用)\n",
           (unsigned long long)r.std_calls, (unsigned long long)r.pool_calls);
    printf("    真正 malloc  : std=%llu  pool=%llu (grow %llu 次)\n",
           (unsigned long long)r.std_calls, (unsigned long long)r.pool_real_malloc,
           (unsigned long long)r.pool_grow);
    printf("    分配字节     : std=%llu (%s MB)  pool=%llu (%s MB)\n",
           (unsigned long long)r.std_bytes, bytes_to_mb(r.std_bytes).c_str(),
           (unsigned long long)r.pool_bytes, bytes_to_mb(r.pool_bytes).c_str());
    printf("\n");

    log << "  " << name << ": std=" << us_to_ms(r.t_std) << "ms  pool="
        << us_to_ms(r.t_pool) << "ms  speedup=" << sp << "%\n";
    log << "    alloc_calls: std=" << r.std_calls << " pool=" << r.pool_calls
        << " (pool 内复用)\n";
    log << "    real_malloc: std=" << r.std_calls << " pool=" << r.pool_real_malloc
        << " (grow " << r.pool_grow << ")\n";
    log << "    bytes: std=" << r.std_bytes << " pool=" << r.pool_bytes << "\n";
}

// ----------------------------------------------------------------------------
// 每个场景的适配器（让 run_pair 能统一调用）
// ----------------------------------------------------------------------------
struct FnList {
    int n;
    template <typename A> void run() { bench_list_us<A>(n); }
};
struct FnMap {
    int n, lookups;
    template <typename A> void run() { bench_map_us<A>(n, lookups); }
};
struct FnVector {
    int n;
    template <typename A> void run() { bench_vector_us<A>(n); }
};
struct FnChurn {
    int rounds, per_round;
    template <typename A> void run() { bench_churn_us<A>(rounds, per_round); }
};

// PoolAllocator 的实例化需要对应元素类型
using PairConstInt = std::pair<const int, int>;

// ----------------------------------------------------------------------------
// 入口
// ----------------------------------------------------------------------------
int main() {
    printf("=== C++ 自定义容器 Allocator 性能优化演示 ===\n");
    printf("编译时间: %s %s\n", __DATE__, __TIME__);
    printf("配置: NODE_NUM=%d, LOOKUP_NUM=%d, ROUNDS=%d, CHURN_ROUNDS=%d\n\n",
           NODE_NUM, LOOKUP_NUM, ROUNDS, CHURN_ROUNDS);

    ofstream log("perf_log.txt", ios::out | ios::trunc);
    if (!log) {
        fprintf(stderr, "无法打开日志文件 perf_log.txt\n");
        return 1;
    }

    log << "=== C++ 自定义容器 Allocator 性能优化演示 ===\n";
    log << "生成时间: " << __DATE__ << " " << __TIME__ << "\n";
    log << "配置: NODE_NUM=" << NODE_NUM << ", LOOKUP_NUM=" << LOOKUP_NUM
        << ", ROUNDS=" << ROUNDS << ", CHURN_ROUNDS=" << CHURN_ROUNDS << "\n";
    log << "计时方式: clock() CPU 时间，单位毫秒（整数微秒换算）\n\n";

    for (int round = 1; round <= ROUNDS; ++round) {
        printf("========== Round %d ==========\n", round);
        log << "--- Round " << round << " ---\n";

        // list
        CaseResult r1 = run_pair<CountingAllocator<int>, PoolAllocator<int>>(FnList{NODE_NUM});
        print_case("list_insert", r1, log);

        // map
        CaseResult r2 = run_pair<CountingAllocator<PairConstInt>, PoolAllocator<PairConstInt>>(
            FnMap{NODE_NUM, LOOKUP_NUM});
        print_case("map_insert", r2, log);

        // vector
        CaseResult r3 = run_pair<CountingAllocator<int>, PoolAllocator<int>>(
            FnVector{NODE_NUM});
        print_case("vector_churn", r3, log);

        // churn
        CaseResult r4 = run_pair<CountingAllocator<Node>, PoolAllocator<Node>>(
            FnChurn{CHURN_ROUNDS, CHURN_NODES});
        print_case("churn", r4, log);
    }

    log.close();
    printf("日志已写入: perf_log.txt\n");
    return 0;
}
