#ifndef POOL_ALLOCATOR_H
#define POOL_ALLOCATOR_H

// ============================================================================
// pool_allocator.h
// 演示：自定义容器 allocator（内存池分配器）对性能的优化
//
// 包含三个组件：
//   1. FreeListPool         —— 单线程内存池，以固定大小块(block)为单位预分配大块内存，
//                             通过 free-list 复用已释放块，allocate/deallocate 为 O(1)。
//   2. PoolAllocator<T>     —— C++11 标准分配器接口，内部持有 FreeListPool 共享指针，
//                             可直接用于 std::list / std::map / std::vector 等容器。
//   3. CountingAllocator<T> —— 包装任意分配器，统计 allocate/deallocate 调用次数与
//                             分配的字节总数，用于日志与文档展示。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

// ----------------------------------------------------------------------------
// 分配统计（全局单例），供日志与文档采集数据
// ----------------------------------------------------------------------------
struct AllocationStats {
    std::uint64_t std_alloc_calls = 0;   // std::allocator 触发的 allocate 次数
    std::uint64_t std_alloc_bytes = 0;   // std::allocator 分配的字节总数
    std::uint64_t pool_alloc_calls = 0;  // PoolAllocator 触发的 allocate 次数
    std::uint64_t pool_alloc_bytes = 0;  // PoolAllocator 分配的字节总数
    std::uint64_t pool_dealloc_calls = 0; // PoolAllocator 释放次数
    std::uint64_t pool_real_malloc = 0;  // PoolAllocator 真正调用 malloc 的次数（grow + 多块）
    std::uint64_t pool_grow_calls = 0;   // 内存池扩容（grow）次数
};

inline AllocationStats& stats() {
    static AllocationStats s;
    return s;
}

// ----------------------------------------------------------------------------
// 固定大小块的内存池（单线程，非线程安全）
// ----------------------------------------------------------------------------
class FreeListPool {
public:
    // chunk 大小 = 块大小对齐到 8 字节（64 位下 alignof(std::max_align_t) 即 16，
    // 这里取 8 的倍数即可满足所有 8 字节对齐需求；如需 max_align 对齐可改成 16）。
    FreeListPool(std::size_t block_size, std::size_t chunk_count,
                 std::size_t growth = 4096)
        : block_size_(align_up(block_size, 8)),
          chunk_count_(chunk_count),
          growth_(growth) {
        // 预分配 chunk_count_ 个块
        grow(chunk_count_);
    }

    ~FreeListPool() {
        for (char* chunk : chunks_) std::free(chunk);
    }

    FreeListPool(const FreeListPool&) = delete;
    FreeListPool& operator=(const FreeListPool&) = delete;

    void* allocate() {
        if (head_ == nullptr) grow(growth_);
        Block* b = head_;
        head_ = b->next;
        return static_cast<void*>(b);
    }

    void deallocate(void* p) {
        if (p == nullptr) return;
        Block* b = static_cast<Block*>(p);
        b->next = head_;
        head_ = b;
    }

    // 判断指针 p 是否属于本池（用于跨池释放的安全回退）。
    // 说明：chunk 由 malloc 分配，地址不保证递增，不能二分查找；
    //       这里线性扫描 chunks_。增长步长足够大时 chunks_ 数量很少，
    //       扫描开销可忽略（200000 块 / 4096 步长 = 约 49 个 chunk）。
    bool owns(const void* p) const {
        for (char* chunk : chunks_) {
            const char* begin = chunk;
            const char* end = chunk + chunk_count_ * block_size_;
            if (p >= begin && p < end) return true;
        }
        return false;
    }

    std::size_t block_size() const { return block_size_; }
    std::size_t chunk_count() const { return chunk_count_; }

private:
    struct Block {
        Block* next;
    };

    static std::size_t align_up(std::size_t n, std::size_t align) {
        return (n + align - 1) / align * align;
    }

    void grow(std::size_t n) {
        const std::size_t chunk_bytes = n * block_size_;
        char* chunk = static_cast<char*>(std::malloc(chunk_bytes));
        if (chunk == nullptr) throw std::bad_alloc();
        chunks_.push_back(chunk);
        ++stats().pool_grow_calls;
        ++stats().pool_real_malloc;
        for (std::size_t i = 0; i < n; ++i) {
            Block* b = reinterpret_cast<Block*>(chunk + i * block_size_);
            b->next = head_;
            head_ = b;
        }
    }

    std::size_t block_size_;
    std::size_t chunk_count_;
    std::size_t growth_;
    Block* head_ = nullptr;                 // free-list 头指针
    std::vector<char*> chunks_;             // 持有所有大块内存以便统一释放
};

// ----------------------------------------------------------------------------
// C++11 标准分配器：基于 FreeListPool 的 PoolAllocator<T>
// ----------------------------------------------------------------------------
template <typename T>
class PoolAllocator {
public:
    using value_type = T;

    explicit PoolAllocator(std::shared_ptr<FreeListPool> pool = nullptr)
        : pool_(pool ? std::move(pool)
                     : std::make_shared<FreeListPool>(sizeof(T), 4096)) {}

    // 所有拷贝/转换构造共享同一个池
    template <typename U>
    PoolAllocator(const PoolAllocator<U>& other) noexcept
        : pool_(other.pool_) {}

    PoolAllocator(const PoolAllocator&) noexcept = default;
    PoolAllocator& operator=(const PoolAllocator&) noexcept = default;
    ~PoolAllocator() = default;

    T* allocate(std::size_t n) {
        // 统计
        ++stats().pool_alloc_calls;
        stats().pool_alloc_bytes += n * sizeof(T);

        if (n == 0) return nullptr;
        if (n == 1) {
            // 单块：直接走内存池
            void* p = pool_->allocate();
            return static_cast<T*>(p);
        }
        // 多块（如 std::vector 连续内存）：回退到 malloc
        void* p = std::malloc(n * sizeof(T));
        if (p == nullptr) throw std::bad_alloc();
        ++stats().pool_real_malloc;
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (p == nullptr) return;
        ++stats().pool_dealloc_calls;
        if (n == 1 && pool_->owns(p)) {
            // 单块且属于本池：归还池中 free-list
            pool_->deallocate(static_cast<void*>(p));
        } else {
            // 多块或跨池指针：回退到 free（安全兜底）
            std::free(p);
        }
    }

    // 所有 PoolAllocator 实例共享同一池（池指针相同），因此视为相等
    template <typename U>
    bool operator==(const PoolAllocator<U>& other) const noexcept {
        return pool_.get() == other.pool_.get();
    }

    template <typename U>
    bool operator!=(const PoolAllocator<U>& other) const noexcept {
        return !(*this == other);
    }

private:
    std::shared_ptr<FreeListPool> pool_;

    // 允许 PoolAllocator<U> 访问本类私有成员
    template <typename U>
    friend class PoolAllocator;
};

// ----------------------------------------------------------------------------
// CountingAllocator<T>：包装任意分配器，统计 allocate/deallocate 调用
// 注意：容器会通过 rebind 用节点类型实例化本类（如 _List_node<int>），
//       因此 Base 必须同步 rebind 到对应类型。
// ----------------------------------------------------------------------------
template <typename T, typename Base = std::allocator<T>>
class CountingAllocator {
public:
    using value_type = T;
    using base_type = Base;

    template <typename U>
    struct rebind {
        using other = CountingAllocator<
            U, typename std::allocator_traits<Base>::template rebind_alloc<U>>;
    };

    CountingAllocator() = default;
    template <typename U, typename B2>
    CountingAllocator(const CountingAllocator<U, B2>&) noexcept {}

    T* allocate(std::size_t n) {
        ++stats().std_alloc_calls;
        stats().std_alloc_bytes += n * sizeof(T);
        return Base().allocate(n);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        Base().deallocate(p, n);
    }

    template <typename U, typename B2>
    bool operator==(const CountingAllocator<U, B2>&) const noexcept { return true; }
    template <typename U, typename B2>
    bool operator!=(const CountingAllocator<U, B2>&) const noexcept { return false; }
};

#endif // POOL_ALLOCATOR_H
