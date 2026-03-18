#pragma once

#include <atomic>
#include <cstdint>
#include <vector>
#include <memory>
#include <optional>
#include <mutex>

namespace RobotDataFlow {

/**
 * @brief Latest-only 样本槽：BACKGROUND 话题的核心数据结构
 *
 * 设计原理：
 * - BACKGROUND 话题（如 Costmap）以 ~200Hz 发布，但地面站只需 10Hz 渲染
 * - 传统 FIFO 队列在帧率不匹配时会无限积压，造成"延迟雪崩"
 * - Latest-only 策略：生产者总是覆盖写入最新帧，消费者读取时无论积压多少都只取最新的
 *
 * 实现选择：
 * - 使用 mutex + optional 保证线程安全
 * - 生产者（Zenoh 回调线程）写入时覆盖旧帧
 * - 消费者（Reactor 主线程）读取并清空槽位
 * - 相比 lock-free triple buffer，这里简化实现优先保证正确性
 */
struct LatestSampleSlot {
    std::mutex mtx;
    std::vector<uint8_t> payload;
    bool has_data = false;
    uint64_t handler_hash = 0;

    /**
     * @brief 生产者调用：写入最新帧（覆盖旧帧）
     */
    void put(uint64_t hash, const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lock(mtx);
        payload.assign(data, data + len);
        handler_hash = hash;
        has_data = true;
    }

    /**
     * @brief 消费者调用：取出最新帧（如有），并清空槽位
     * @return optional 包含 payload（如果有新帧），否则为空
     */
    std::optional<std::pair<uint64_t, std::vector<uint8_t>>> take() {
        std::lock_guard<std::mutex> lock(mtx);
        if (!has_data) return std::nullopt;
        has_data = false;
        return std::make_pair(handler_hash, std::move(payload));
    }
};

/**
 * @brief Latest-only 缓存管理器：为每个 BACKGROUND 话题维护一个独立槽位
 *
 * 用法：
 *   LatestSampleCache cache;
 *   cache.get_slot(hash)->put(hash, data, len); // 生产者
 *   auto sample = cache.get_slot(hash)->take(); // 消费者
 */
class LatestSampleCache {
public:
    /**
     * @brief 获取（或创建）指定话题 hash 对应的槽位
     */
    LatestSampleSlot* get_slot(uint64_t hash) {
        std::lock_guard<std::mutex> lock(map_mtx_);
        auto it = slots_.find(hash);
        if (it == slots_.end()) {
            auto [inserted_it, _] = slots_.emplace(hash, std::make_unique<LatestSampleSlot>());
            return inserted_it->second.get();
        }
        return it->second.get();
    }

    /**
     * @brief 遍历所有槽位，消费每个话题的最新帧
     * @param consumer 消费函数，接收 (handler_hash, payload)
     */
    void drain(std::function<void(uint64_t, std::vector<uint8_t>&&)> consumer) {
        std::lock_guard<std::mutex> lock(map_mtx_);
        for (auto& [hash, slot] : slots_) {
            auto sample = slot->take();
            if (sample) {
                consumer(sample->first, std::move(sample->second));
            }
        }
    }

private:
    // 使用 unordered_map 存储槽位，hash 作为 key，O(1) 查找
    std::unordered_map<uint64_t, std::unique_ptr<LatestSampleSlot>> slots_;
    std::mutex map_mtx_; // 保护 map 本身的并发修改
};

} // namespace RobotDataFlow
