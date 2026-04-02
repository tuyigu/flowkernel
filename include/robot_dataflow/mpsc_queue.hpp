#pragma once

#include <atomic>
#include <array>
#include <mutex>
#include <cstdint>
#include <type_traits>
#include "common.hpp"

namespace RobotDataFlow {

/**
 * @brief 多生产者单消费者 (MPSC) 有界环形队列
 *
 * 背景：Zenoh 1.8 的底层运行时 (ZRuntime::RX) 默认分配 2 个 worker threads，
 * subscriber callback 在其上执行，无法保证单生产者语义。
 * 因此 CRITICAL 通道从 SPSC 升级为 MPSC。
 *
 * 设计选择：
 * - 生产者侧：轻量 mutex (push_mtx_) —— 竞争极少（仅 2 个 Zenoh RX 线程）
 * - 消费者侧：纯原子操作，无锁（单消费者 Reactor 线程）
 * - 满队列时：计数并丢弃（不阻塞，避免 Zenoh callback 线程被反压死锁）
 *
 * @tparam T          元素类型（需要支持移动语义）
 * @tparam Capacity   队列容量，必须是 2 的幂次
 */
template <typename T, size_t Capacity>
class MPSCQueue {
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity 必须是 2 的幂次");

    MPSCQueue() : head_(0), tail_(0), dropped_(0) {}

    ~MPSCQueue() {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_relaxed);
        while (h != t) {
            get_ptr(h)->~T();
            h = (h + 1) & (Capacity - 1);
        }
    }

    // 禁止拷贝
    MPSCQueue(const MPSCQueue&) = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;

    /**
     * @brief 线程安全的生产者入队（支持多生产者并发）
     *
     * 使用 push_mtx_ 保证多个 Zenoh RX 线程不会同时修改 tail_，
     * 从而避免 SPSC 的单生产者假设被违反（UB）。
     *
     * @return true 入队成功；false 队列已满（帧被计数丢弃）
     */
    bool push(T&& item) {
        while (push_lock_.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ volatile("yield" ::: "memory");
#endif
        }
        const size_t t    = tail_.load(std::memory_order_relaxed);
        const size_t next = (t + 1) & (Capacity - 1);

        if (next == head_.load(std::memory_order_acquire)) {
            // 队列已满：计数 + 丢弃，不阻塞 Zenoh callback 线程
            dropped_.fetch_add(1, std::memory_order_relaxed);
            push_lock_.clear(std::memory_order_release);
            return false;
        }

        new (get_ptr(t)) T(std::move(item));
        tail_.store(next, std::memory_order_release);
        push_lock_.clear(std::memory_order_release);
        return true;
    }

    /**
     * @brief 单消费者出队（仅允许一个线程调用）
     *
     * @param item 用于存放出队元素的引用
     * @return true 出队成功；false 队列为空
     */
    bool pop(T& item) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) {
            return false; // 队列为空
        }
        item = std::move(*get_ptr(h));
        get_ptr(h)->~T();
        head_.store((h + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    /**
     * @brief 返回累计被丢弃的帧数（生产者满队列时计数）
     */
    [[nodiscard]] uint64_t dropped_count() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    std::atomic_flag push_lock_ = ATOMIC_FLAG_INIT; // 仅保护生产者侧，用自旋锁替代重量级 mutex

    // 使用 aligned_storage 避免要求 T 有默认构造函数
    struct Storage {
        alignas(T) unsigned char data[sizeof(T)];
    };
    std::array<Storage, Capacity> buffer_;

    // head（消费者）和 tail（生产者）对齐到不同缓存行，防止伪共享
    alignas(hardware_destructive_interference_size) std::atomic<size_t> head_;
    alignas(hardware_destructive_interference_size) std::atomic<size_t> tail_;

    std::atomic<uint64_t> dropped_; // 满队时累计丢帧数

    // 辅助函数：获取缓冲区中元素的引用
    T* get_ptr(size_t index) { return reinterpret_cast<T*>(&buffer_[index]); }
    const T* get_ptr(size_t index) const { return reinterpret_cast<const T*>(&buffer_[index]); }
};

} // namespace RobotDataFlow
