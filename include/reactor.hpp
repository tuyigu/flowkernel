#pragma once

#include <zenoh.h>
#include <liburing.h>
#include <sys/eventfd.h>

#include <vector>
#include <memory>
#include <memory_resource>
#include <functional>
#include <atomic>
#include <string>
#include <unordered_map>
#include <expected>
#include <span>
#include <array>

#include "robot_dataflow/common.hpp"
#include "robot_dataflow/mpsc_queue.hpp"
#include "robot_dataflow/latest_cache.hpp"

namespace RobotDataFlow {

// ── MPSC 队列元素（CRITICAL + NORMAL 通道共用）────────────────────────────────
// v2.2: 从 reactor.cpp 移至头文件，供实例成员声明使用
struct CriticalSample {
    uint64_t             handler_hash;
    std::vector<uint8_t> payload;
};

/**
 * @brief 话题优先级枚举，直接映射 Zenoh 1.8 的 z_priority_t
 *
 * 调度策略（v2.2 明确）：
 *   CRITICAL / NORMAL  → MPSC 队列（无丢帧，有序传递）
 *   BACKGROUND         → Latest-only 缓存（只保最新帧，防积压雪崩）
 *
 * v2.2 修复：NORMAL 原错误地走了 Latest-only，导致遥测帧被静默丢弃。
 */
enum class TopicPriority : uint8_t {
    CRITICAL   = Z_PRIORITY_INTERACTIVE_HIGH, // 值=2，急停/控制指令
    NORMAL     = Z_PRIORITY_DATA,             // 值=5，遥测（不应丢帧）
    BACKGROUND = Z_PRIORITY_BACKGROUND,       // 值=7，高频 Costmap
};

/**
 * @brief Reactor 错误代码（用于 std::expected 零开销错误处理）
 */
enum class ReactorError {
    ZenohInitFailed,
    IORingSetupFailed,
    HandlerRegistrationFailed,
    BackpressureHigh,
    LivelinessLost,
};

/**
 * @brief DataFlowReactor —— FlowKernel 核心 Reactor（v2.2）
 *
 * 改进摘要（相对 v2.1）：
 *
 *   1. io_uring + eventfd 反应式唤醒
 *      CRITICAL 数据到达 → Zenoh callback 写 eventfd → io_uring 立即唤醒
 *      消灭原先固定 5ms 等待窗口，CRITICAL 调度延迟降至 ~1µs。
 *
 *   2. NORMAL 优先级路由修复
 *      NORMAL（遥测）现在走 MPSC 队列，不再误入 Latest-only 导致丢帧。
 *
 *   3. 多实例安全
 *      critical_queue_ / background_cache_ 作为实例成员，不再是 static 全局。
 *      多个 DataFlowReactor 实例可安全并存（多机场景）。
 *
 *   4. Liveliness E-Stop 使用正规 FlatBuffers（不再硬编码字节）
 */
class DataFlowReactor {
public:
    /**
     * @param config_path        Zenoh 配置文件路径（可选）
     * @param estop_publish_path 机器人断线时发布 E-Stop 的 Key Expression
     * @param liveliness_key     监听 Liveliness 的 Key Expression
     */
    explicit DataFlowReactor(
        const std::string& config_path       = "",
        const std::string& estop_publish_path = "robot/*/cmd/estop",
        const std::string& liveliness_key    = "robot/**");

    ~DataFlowReactor();

    DataFlowReactor(const DataFlowReactor&)            = delete;
    DataFlowReactor& operator=(const DataFlowReactor&) = delete;

    std::expected<void, ReactorError> run();

    /// 线程安全地停止事件循环，并通过 eventfd 主动唤醒 io_uring
    void stop() noexcept;

    /**
     * @brief 注册话题处理器
     * @param path     Zenoh Key Expression
     * @param priority CRITICAL/NORMAL → MPSC；BACKGROUND → Latest-only
     * @param callback 回调，std::span 零拷贝视图
     */
    void register_handler(const std::string& path,
                          TopicPriority priority,
                          std::function<void(std::span<const uint8_t>)> callback);

    void print_stats() const;

private:
    void setup_io_uring();

    /**
     * @brief 向 io_uring SQ 提交一次 POLLIN 监听（wakeup_fd_ 上）
     *
     * io_uring POLL_ADD 是一次性的：每次 CQE 被消费后需重新调用。
     */
    void arm_poll_eventfd();

    /**
     * @brief 核心分发（热路径）
     *
     * 执行顺序：
     *   1. 清空 MPSC 队列（CRITICAL + NORMAL）
     *   2. 消费 Latest-only 缓存（BACKGROUND）
     *   3. io_uring_wait_cqe_timeout（等 eventfd 或 50ms 超时）
     */
    [[gnu::hot]] void handle_events();

    // ── Zenoh 资源 ────────────────────────────────────────────────────────────
    z_owned_session_t   session_;
    z_owned_publisher_t estop_publisher_;
    std::string         estop_publish_path_;
    std::string         liveliness_key_;

    // ── io_uring + eventfd 反应式唤醒 ─────────────────────────────────────────
    // wakeup_fd_：(EFD_NONBLOCK) eventfd，CRITICAL push 后由 Zenoh callback 写入
    // io_uring POLL_ADD 监听此 fd，立即唤醒 Reactor 主线程
    struct io_uring ring_;
    int             wakeup_fd_{-1};

    // ── 运行控制 ──────────────────────────────────────────────────────────────
    std::atomic<bool> running_{false};

    // ── pmr 内存池（消灭高频 malloc 堆碎片）──────────────────────────────────
    alignas(64) std::array<std::byte, 1024 * 1024> pool_buf_;
    std::pmr::monotonic_buffer_resource pool_{pool_buf_.data(), pool_buf_.size()};

    // ── 内部处理器结构 ────────────────────────────────────────────────────────
    struct Handler {
        std::string   path;
        uint64_t      path_hash;
        TopicPriority priority;
        std::function<void(std::span<const uint8_t>)> callback;
    };

    alignas(hardware_destructive_interference_size) std::vector<Handler> handlers_;
    std::unordered_map<uint64_t, size_t> handler_index_;

    // ── 双通道数据结构（实例成员，多 Reactor 实例安全）──────────────────────
    // v2.2 修复：原为 static 文件级全局，移入实例，消除多机场景队列混用 bug。
    MPSCQueue<CriticalSample, 512> critical_queue_;   // CRITICAL + NORMAL
    LatestSampleCache              background_cache_;  // BACKGROUND only
};

} // namespace RobotDataFlow
