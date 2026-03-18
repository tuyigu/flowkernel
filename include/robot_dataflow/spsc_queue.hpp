#pragma once

#include <atomic>
#include <vector>
#include <optional>
#include "common.hpp"

namespace RobotDataFlow {

/**
 * @brief 单生产者单消费者 (SPSC) 无锁队列。
 * 
 * 专门针对机器人遥测数据等高频场景优化。通过缓存行对齐 (Cache Line Alignment) 
 * 彻底消除生产者和消费者指针之间的伪共享。
 * 
 * @tparam T 数据类型
 * @tparam Capacity 队列容量，必须是 2 的幂次，以便进行位掩码偏移计算
 */
template <typename T, size_t Capacity>
class SPSCQueue {
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "容量必须是 2 的幂次");

    SPSCQueue() : head(0), tail(0) {
        buffer.resize(Capacity);
    }

    /**
     * @brief 向队列压入数据。
     * @param item 要入队的数据
     * @return true 成功, false 队列已满
     */
    bool push(const T& item) {
        const size_t t = tail.load(std::memory_order_relaxed);
        const size_t h = head.load(std::memory_order_acquire);
        
        if (((t + 1) & (Capacity - 1)) == h) {
            return false; // 队列已满
        }

        buffer[t] = item;
        tail.store((t + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    /**
     * @brief 从队列弹出数据。
     * @param item 用于存储弹出数据的引用
     * @return true 成功, false 队列为空
     */
    bool pop(T& item) {
        const size_t h = head.load(std::memory_order_relaxed);
        const size_t t = tail.load(std::memory_order_acquire);

        if (h == t) {
            return false; // 队列为空
        }

        item = buffer[h];
        head.store((h + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

private:
    std::vector<T> buffer;
    
    // 将头指针和尾指针对齐到不同的缓存行，防止生产者和消费者核心之间的缓存一致性抖动
    alignas(hardware_destructive_interference_size) std::atomic<size_t> head;
    alignas(hardware_destructive_interference_size) std::atomic<size_t> tail;
};

} // namespace RobotDataFlow
