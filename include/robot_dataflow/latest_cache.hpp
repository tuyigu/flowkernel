#pragma once

#include <atomic>
#include <cstdint>
#include <vector>
#include <memory>
#include <optional>
#include <mutex>
#include <unordered_map>

namespace RobotDataFlow {

/**
 * @brief Latest-only 样本槽：BACKGROUND 话题的核心数据结构
 *
 * 设计原理：
 * - BACKGROUND 话题（如 Costmap）以 ~200Hz 发布，地面站只需 10Hz 渲染
 * - Latest-only 策略：生产者总是覆盖写入最新帧，消费者只取最新的
 * - 相比 FIFO 队列，天然防止"延迟雪崩"积压
 *
 * 线程安全：
 * - put()：Zenoh RX callback 线程（多线程），需要 mutex 保护
 * - take()：Reactor 主线程（单线程），需要 mutex 保护
 * - drop_count：原子计数，无锁
 */
struct LatestSampleSlot {
    std::atomic_flag        lock_ = ATOMIC_FLAG_INIT;
    std::vector<uint8_t>    payload;
    bool                    has_data    = false;
    uint64_t                handler_hash = 0;
    std::atomic<uint64_t>   drop_count  {0}; // 被覆盖丢弃的旧帧数

    /**
     * @brief 生产者调用：写入最新帧（覆盖旧帧，记录丢帧）
     */
    void put(uint64_t hash, const uint8_t* data, size_t len) {
        while (lock_.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ volatile("yield" ::: "memory");
#endif
        }
        if (has_data) {
            // 新帧覆盖了尚未被消费者取走的旧帧 → 统计丢帧
            drop_count.fetch_add(1, std::memory_order_relaxed);
        }
        payload.assign(data, data + len);
        handler_hash = hash;
        has_data     = true;
        lock_.clear(std::memory_order_release);
    }

    /**
     * @brief 消费者调用：取出最新帧并清空槽位（如有）
     */
    std::optional<std::pair<uint64_t, std::vector<uint8_t>>> take() {
        while (lock_.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ volatile("yield" ::: "memory");
#endif
        }
        if (!has_data) {
            lock_.clear(std::memory_order_release);
            return std::nullopt;
        }
        has_data = false;
        auto result = std::make_pair(handler_hash, std::move(payload));
        lock_.clear(std::memory_order_release);
        return result;
    }
};

/**
 * @brief Latest-only 缓存管理器
 *
 * 关键设计：两阶段访问
 *
 *   [初始化阶段] register_handler() → pre_allocate(hash)
 *     在启动前预分配所有 BACKGROUND 话题的槽位，加 map_mtx_ 写锁。
 *
 *   [热路径阶段] zenoh_data_callback() → get_slot_fast(hash)
 *     map 在运行期只读，无需加锁，直接 unordered_map::find()。
 *
 * 这样把"热路径的 mutex 持锁"彻底消除，200Hz 回调路径完全无锁。
 */
class LatestSampleCache {
public:
    /**
     * @brief [初始化阶段] 预分配槽位（在 run() 前调用，持写锁）
     *
     * 必须在 get_slot_fast() 被调用前完成所有预分配。
     */
    void pre_allocate(uint64_t hash) {
        std::lock_guard<std::mutex> lock(map_mtx_);
        slots_.try_emplace(hash, std::make_unique<LatestSampleSlot>());
    }

    /**
     * @brief [热路径] 无锁获取槽位指针
     *
     * 前提：map 在运行期只读（pre_allocate 已完成）。
     * unordered_map::find() 读操作在 map 不变时是线程安全的。
     *
     * @return 槽位指针（已预分配则非空，否则 nullptr）
     */
    [[gnu::hot]]
    LatestSampleSlot* get_slot_fast(uint64_t hash) const noexcept {
        auto it = slots_.find(hash);
        return (it != slots_.end()) ? it->second.get() : nullptr;
    }

    /**
     * @brief [兼容旧接口] 惰性创建槽位（初始化阶段使用，持写锁）
     */
    LatestSampleSlot* get_slot(uint64_t hash) {
        std::lock_guard<std::mutex> lock(map_mtx_);
        auto [it, _] = slots_.try_emplace(hash, std::make_unique<LatestSampleSlot>());
        return it->second.get();
    }

    /**
     * @brief 遍历所有槽位，消费每个话题的最新帧
     */
    void drain(std::function<void(uint64_t, std::vector<uint8_t>&&)> consumer) {
        // map 在运行期只读，无需 map_mtx_；槽位内部有各自的 slot::mtx
        for (auto& [hash, slot] : slots_) {
            auto sample = slot->take();
            if (sample) {
                consumer(sample->first, std::move(sample->second));
            }
        }
    }

    /**
     * @brief 返回所有 BACKGROUND 话题的累计丢帧统计
     */
    void print_drop_stats() const {
        for (auto& [hash, slot] : slots_) {
            uint64_t d = slot->drop_count.load(std::memory_order_relaxed);
            if (d > 0) {
                // 用 hash 标识（调用方可以对照注册表反查路径）
                // 避免在此引入 path string 存储以降低依赖
                fprintf(stderr,
                    "[STAT] BACKGROUND slot 0x%016llx: dropped=%llu frames\n",
                    (unsigned long long)hash, (unsigned long long)d);
            }
        }
    }

private:
    mutable std::unordered_map<uint64_t, std::unique_ptr<LatestSampleSlot>> slots_;
    mutable std::mutex map_mtx_; // 仅在初始化阶段（pre_allocate/get_slot）使用
};

} // namespace RobotDataFlow
