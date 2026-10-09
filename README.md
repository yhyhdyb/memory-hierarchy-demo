# C++ 自定义容器 Allocator（内存池分配器）性能优化演示

本项目演示如何通过**自定义容器 allocator**（内存池分配器 `PoolAllocator`）显著优化
`std::list`、`std::map` 等节点型容器的性能，并输出可复现的基准测试日志与文档。

核心结论：**对每节点一次小分配的容器（list/map），内存池将 20 万次 `malloc` 合并为
几十次大块分配，性能提升 1.2~9 倍；对"创建-销毁"高频场景提升最明显（8~9 倍）。**

---

## 1. 背景：默认分配器的问题

C++ 标准容器的默认分配器是 `std::allocator<T>`，它直接调用 `malloc`/`free`：

- **每次插入一个节点就是一次 `malloc`**。`std::list<int>` 插入 20 万次节点 → 20 万次
  `malloc` + 20 万次 `free`。
- `malloc` 需要维护空闲链表、查找合适大小的块、处理堆元数据，是通用算法，对小对象
  分配尤其昂贵（一次分配约几百 ns）。
- 反复分配/释放相同大小的节点会造成**堆碎片**，且每次分配的内存地址随机，**缓存
  命中率差**。

`std::vector` 等连续内存容器一次 `realloc` 分配大块，分配次数少，受此影响小。

## 2. 方案：内存池分配器（PoolAllocator）

内存池的思路：**一次性从系统申请大块内存（chunk），按固定块大小切分成空闲链表
（free-list），分配/释放只操作链表头指针，O(1) 完成**。

本项目实现位于 `pool_allocator.h`，包含三个组件：

### 2.1 FreeListPool —— 核心内存池

```
┌────────────────────────── chunk（一次 malloc 申请 4096 个块）──────────────────────────┐
│  ┌────┐ ┌────┐ ┌────┐ ... ┌────┐                                                    │
│  │块 0│ │块 1│ │块 2│     │块 n│   ← 每个块大小 = block_size（对齐到 8 字节）          │
│  └────┘ └────┘ └────┘     └────┘                                                    │
└───────────────────────────────────────────────────────────────────────────────────────┘
        ↑ 空闲块通过"块内指针"串成 free-list：head → 块5 → 块3 → 块9 → ...（乱序，无要求）

allocate: 取出 head 指向的块，head 后移（O(1)）
deallocate: 把块挂回 free-list 头部（O(1)）
池容量不足时 grow()：再 malloc 一个 chunk，把新块全部链入 free-list
```

- **块大小对齐到 8 字节**，满足大多数 POD 与指针类型对齐要求。
- **块内借用第一个字（sizeof(void*)）存 next 指针**，零额外内存开销。
- **chunks 持有所有 chunk 指针**，析构时统一 `free`，不会泄漏。
- 单线程使用（演示目的）；多线程需加锁或使用线程本地池。

### 2.2 PoolAllocator<T> —— 标准分配器接口

实现 C++11 `std::allocator_traits` 所需接口，可直接作为 `std::list`/`std::map` 等
容器的 `Allocator` 模板参数：

```cpp
list<int, PoolAllocator<int>> lst;                 // 节点内存走池
map<int, int, less<int>, PoolAllocator<pair<const int,int>>> mp;
```

关键设计：

- 每个不同的 `T` 对应**独立池**（块大小不同），通过 `std::shared_ptr<FreeListPool>`
  持有；容器的拷贝/rebind 共享同一池，不重复申请。
- `allocate(1)` 走池分配；`allocate(n>1)`（如 vector 扩容）回退 `malloc`。
- `deallocate` 时先校验块归属（`owns()`，线性扫描 chunk 区间），跨池或大块安全
  `free`，保证不会错还内存。

### 2.3 CountingAllocator<T> —— 统计用包装器

包装任意底层分配器并记录 `allocate` 调用次数与字节数，用于量化"减少的 malloc 次数"。

## 3. 基准测试设计（main.cpp）

| 场景 | 操作 | 考察点 |
|------|------|--------|
| `list_insert` | 20 万次 push_back + 遍历 + pop_front | 节点型容器，每节点 1 次小分配 |
| `map_insert`  | 20 万次 emplace + 5 万次查找 + 删除 | 红黑树节点，每节点 1 次小分配 |
| `vector_churn`| 10 轮 × 20 万次 push/pop | 批量分配，池无明显优势 |
| `churn`       | 40 轮 × 5000 节点创建/销毁 | 高频"创建-销毁"，池复用最大化 |

每轮分别计时（`clock()` CPU 时间）、分别统计分配次数，结果写入控制台和
`perf_log.txt`，重复 3 轮。

## 4. 真实基准结果

> 环境：Windows 11 / MinGW GCC 9.3 / `-O2`，`clock()` CPU 计时。
> 完整原始数据见 [perf_log.txt](perf_log.txt)。

### 4.1 耗时对比（3 轮中位数，毫秒）

| 场景 | std::allocator | PoolAllocator | 加速比 |
|------|---------------:|--------------:|-------:|
| list_insert  | 10.0 ms | 4.0 ms | **2.5x** |
| map_insert   | 31.0 ms | 25.5 ms | **1.2x** |
| vector_churn | 6.5 ms  | 6.0 ms  | 1.1x |
| churn        | 8.5 ms  | 1.0 ms  | **8.5x** |

### 4.2 真正的 malloc 调用次数（决定性能的指标）

| 场景 | std（malloc 次数） | pool（malloc 次数） | 说明 |
|------|-------------------:|--------------------:|------|
| list_insert  | 200,000 | **49** | 49 = 池扩容次数（grow） |
| map_insert   | 200,000 | **49** | 同上 |
| churn        | 200,000 | **2**  | 初始 4096 块 + 1 次扩容即覆盖 5000 节点峰值，此后全部复用 |
| vector_churn | 190     | 190    | 批量分配，池回退 malloc，无差别 |

### 4.3 解读

1. **list（2.5x）**：20 万次 malloc/free 变成 49 次大块 malloc + O(1) 链表指针操作。
   分配次数减少约 99.98%。
2. **map（1.2x）**：map 耗时中红黑树旋转/查找占大头，纯分配占比低，池收益被摊薄，
   但仍有 20%+ 提升。
3. **churn（8.5x）**：创建-销毁节奏下，池内块"还了再用"，几乎零系统调用，是池
   最大价值场景。
4. **vector（1.1x）**：vector 每次扩容 `allocate(n)` 走 malloc 回退路径，池无优势。
   **这说明池只对"大量同尺寸小分配"有效，不是万能加速器。**

## 5. 构建与运行

```bash
# 编译（需支持 C++11 的编译器）
g++ -std=c++11 -O2 -Wall -Wextra -o allocator_demo.exe main.cpp

# 运行：控制台输出 + 生成 perf_log.txt
./allocator_demo.exe
```

> 注意：本机 MinGW GCC 9.3 在 `double` 浮点字面量上有编译器内部错误（ICE），
> 故 main.cpp 全程使用整数运算（计时用整数微秒、格式化用整数除法），
> 不影响任何基准语义。若你的编译器正常，可将计时部分换回 `double`。

## 6. 适用边界（什么时候该用内存池）

**适合**：
- 同尺寸对象频繁创建/销毁（游戏实体、连接对象、消息节点）
- `std::list`/`std::map`/`std::unordered_map` 等节点型容器的大规模使用
- 需要分配确定性（固定块地址复用、无堆碎片）

**不适合**：
- 尺寸不一的请求（需多级池/slab）
- 跨线程共享容器（本实现非线程安全）
- 大块连续分配（vector 场景，池无法受益）

## 7. 文件清单

| 文件 | 说明 |
|------|------|
| `pool_allocator.h` | FreeListPool + PoolAllocator + CountingAllocator 完整实现 |
| `main.cpp` | 4 场景基准测试、控制台与日志输出 |
| `perf_log.txt` | 运行生成的原始基准日志 |
| `README.md` | 本文档 |











## 8. 学习FAQ
1.pool_allocator类为什么还需要声明friend class，什么意义
 
这是一个非常经典且重要的 C++ 模板元编程和标准库合规性问题。

### 核心结论

声明 `friend class PoolAllocator;` 的主要目的是：**允许不同实例化类型（如 `PoolAllocator<int>` 和 `PoolAllocator<double>`）之间互相访问彼此的私有成员（特别是 `pool_`），从而实现“多态分配器”的互操作性。**

如果没有这个 `friend` 声明，代码中的**转换构造函数**和**相等性比较运算符**将无法编译通过。

---

### 详细解释

#### 1. C++ 模板实例化的隔离性
在 C++ 中，`PoolAllocator<int>` 和 `PoolAllocator<double>` 是两个**完全独立**的类。
- `PoolAllocator<int>` 的私有成员 `pool_` 对 `PoolAllocator<double>` 来说是**不可见**的。
- 即使它们源自同一个模板，编译器也视它们为不同的类型。

#### 2. 为什么需要访问私有成员？

请看代码中的两个关键部分：

##### A. 转换构造函数 (Rebind Allocator)
```cpp
template <typename U>
PoolAllocator(const PoolAllocator<U>& other) noexcept
    : pool_(other.pool_) {} // <--- 这里访问了 other 的私有成员 pool_
```
- **场景**：当 `std::vector<int>` 需要分配内存时，它使用 `PoolAllocator<int>`。如果内部逻辑需要分配 `double` 类型的辅助结构（虽然少见，但标准库允许），或者更常见的情况是，STL 容器在内部可能需要将分配器“rebind”到另一种类型。
- **问题**：`PoolAllocator<int>` 的构造函数试图读取 `PoolAllocator<double>` 的 `pool_` 成员。由于 `pool_` 是 `private` 的，且 `PoolAllocator<double>` 不是 `PoolAllocator<int>` 的友元，这将导致**编译错误**。
- **解决**：通过 `friend class PoolAllocator;`，`PoolAllocator<int>` 告诉编译器：“所有 `PoolAllocator` 的实例化版本都是我的朋友，可以访问我的私有成员。” 因此，`PoolAllocator<double>` 可以访问 `PoolAllocator<int>` 的 `pool_`。

##### B. 相等性比较运算符
```cpp
template <typename U>
bool operator==(const PoolAllocator<U>& other) const noexcept {
    return pool_.get() == other.pool_.get(); // <--- 这里访问了 other 的私有成员 pool_
}
```
- **场景**：STL 容器（如 `std::vector`）在移动或复制时，需要检查两个分配器是否“相等”。如果相等，它们可以共享内存；如果不相等，则必须深拷贝数据。
- **问题**：`PoolAllocator<int>` 的 `operator==` 需要读取 `PoolAllocator<double>` 的 `pool_` 指针进行比较。同样，由于私有性限制，这会失败。
- **解决**：`friend` 声明允许这种跨类型的私有成员访问。

#### 3. 为什么不能把 `pool_` 设为 `public`？

虽然将 `pool_` 设为 `public` 也能解决编译问题，但这违反了**封装原则**：
- **安全性**：外部代码可能意外修改 `pool_`，导致内存池状态不一致。
- **接口清晰性**：分配器的用户不应该直接操作内部池指针，而应通过 `allocate`/`deallocate` 接口。
- **标准合规性**：C++ 标准库的分配器模型要求分配器是可复制、可比较的，但内部实现细节应隐藏。`friend` 是一种“受控的”访问方式，只允许同类模板的其他实例化版本访问，而不暴露给任意外部代码。

#### 4. 为什么需要 `friend` 而不是其他方法？

- **不能只用 `public`**：如上所述，破坏封装。
- **不能用 `protected`**：`protected` 只允许派生类访问，而 `PoolAllocator<int>` 和 `PoolAllocator<double>` 不是继承关系，是平行关系。
- **不能用静态成员**：`pool_` 是每个分配器实例的状态（尽管在这个设计中它们共享同一个 `shared_ptr`，但每个分配器对象仍持有自己的指针副本），不能是静态的。

---

### 总结

| 特性 | 说明 |
|------|------|
| **目的** | 实现 C++ 标准库要求的“分配器可重绑定”（rebindable）和“可比较”（comparable）特性。 |
| **关键机制** | 允许 `PoolAllocator<T>` 访问 `PoolAllocator<U>` 的私有成员 `pool_`。 |
| **应用场景** | 1. 转换构造函数：从一个类型的分配器构造另一个类型的分配器。<br>2. 相等性比较：判断两个不同类型的分配器是否指向同一个内存池。 |
| **设计原则** | 在保持封装性的前提下，提供模板实例化之间的互操作性。 |

**简而言之：** 没有 `friend class PoolAllocator;`，你的分配器将无法与 STL 容器（如 `std::vector`, `std::list` 等）正确配合使用，因为 STL 会尝试在不同类型之间转换和比较分配器，而这些操作需要访问私有成员。

---

2.chunk是什么，free list是什么

### 核心结论

- **Chunk**：内存池一次性从系统申请的一大块连续内存，是"池"与操作系统之间的基本单位。
  一次 `malloc` 换来几千个可用块，把系统调用次数从 20 万降到几十。
- **Free list（空闲链表）**：把所有"当前空闲的块"串起来的单向链表，用来记住哪些块可以
  重复利用。分配/释放只操作链表头，均为 O(1)，**且不额外占用任何内存**（空闲块的头部
  直接存放下一个空闲块的指针）。

---

### 详细解释

#### 1. Chunk：批发 vs 零售

在 `pool_allocator.h` 中，chunk 就是 `grow()` 里那次 `malloc` 的结果（pool_allocator.h:104-116）：

```cpp
void grow(std::size_t n) {
    const std::size_t chunk_bytes = n * block_size_;
    char* chunk = static_cast<char*>(std::malloc(chunk_bytes));  // 一次 malloc 一大块
    chunks_.push_back(chunk);                                     // 记下指针，析构时统一释放
    // 把这 n 个块全部挂到 free-list 上
    for (std::size_t i = 0; i < n; ++i) {
        Block* b = reinterpret_cast<Block*>(chunk + i * block_size_);
        b->next = head_;
        head_ = b;
    }
}
```

- 以 list 场景为例：块大小 24 字节，`chunk_count = 4096`，一个 chunk = `24 × 4096 ≈ 96 KB`。
- 20 万次节点插入只需要 **49 次** chunk 申请（`perf_log.txt` 中的 `grow 49`）；
  每次插入的节点不再调 `malloc`，而是从已有的 chunk 里切一块用。
- 析构时（pool_allocator.h:58-60）遍历 `chunks_` 把所有大块统一 `free`，不会逐块释放。

```
chunk（一次 malloc 申请的连续内存）
┌──────────────────────────────────────────────┐
│ [块0] [块1] [块2] ... [块4095]                │  ← 每个块大小 = block_size（对齐到 8 字节）
└──────────────────────────────────────────────┘
```

#### 2. Free list：记住哪些块是空闲的

实现极其简单——**零额外内存开销**，每个空闲块的头部直接存放下一个空闲块的指针
（pool_allocator.h:96-98, 65-77）：

```cpp
struct Block {          // 空闲块的结构：块内第一个字存指针
    Block* next;
};

Block* head_ = nullptr; // 链表头，指向第一个空闲块

void* allocate() {      // 分配：从链表头部取一块，O(1)
    if (head_ == nullptr) grow(growth_);  // 没空闲块了，去申请新 chunk
    Block* b = head_;
    head_ = b->next;
    return b;
}

void deallocate(void* p) {  // 释放：把块放回链表头部，O(1)
    Block* b = (Block*)p;
    b->next = head_;
    head_ = b;
}
```

以 4 块为例的链表状态变化：

```
初始（全空闲，free-list 串起所有块）:
head_ → [块0] → [块1] → [块2] → [块3] → nullptr
        块0存的是块1的地址，块1存块2的地址……

分配 2 块后（从头取）:
head_ → [块2] → [块3] → nullptr
已用: 块0, 块1

释放块1后（挂回头部）:
head_ → [块1] → [块2] → [块3] → nullptr
```

#### 3. 两者配合的本质

| 组件 | 解决的问题 | 效果 |
|------|-----------|------|
| **Chunk** | 减少系统调用 | 一次 `malloc` 换来几千个可用块，malloc 次数从 20 万降到 49 |
| **Free list** | 复用已释放块 | 释放的块立刻回到链表头部，下次分配优先复用 |

churn 场景是两者配合的极致体现：40 轮 × 5000 节点创建/销毁，只有最初申请 chunk 时
触发 2 次系统调用（`grow 2`），其余 20 万次分配全部复用 free-list 上的块。

**类比**：Chunk 是仓库批发的一箱货，free list 是货架上的"可再售清单"。卖出一个货
（allocate）从清单头部拿，退回来的货（deallocate）记回清单头部——都是 O(1) 的账本
操作，不需要每次联系厂商（操作系统）。
