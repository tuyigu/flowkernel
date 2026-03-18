#pragma once

#include <zenoh.h>
#include <liburing.h>
#include <vector>
#include <memory>
#include <memory_resource> // C++17 pmr 内存池
#include <functional>
#include <atomic>
#include <string>
#include <unordered_map>
#include <expected>
#include <span>

#include "robot_dataflow/common.hpp"

namespace RobotDataFlow {

/**
 * @brief 话题优先级枚举，直接映射到 Zenoh 1.8 的 z_priority_t
 *
 * Zenoh 的物理层优先级保证了 CRITICAL 数据包在网络层面优先传输，
 * 即使 BACKGROUND 的高频地图帧正在发送，E-Stop 也能"插队"触达。
 */
enum class TopicPriority : uint8_t {
    CRITICAL    = Z_PRIORITY_INTERACTIVE_HIGH, // 值=2，用于急停/控制指令
    NORMAL      = Z_PRIORITY_DATA,             // 值=5，用于遥测
    BACKGROUND  = Z_PRIORITY_BACKGROUND,       // 值=7，用于高频 Costmap
};

/**
 * @brief Reactor 错误代码（用于 std::expected 零开销错误处理）
 */
enum class ReactorError {
    ZenohInitFailed,           // Zenoh 会话初始化失败
    IORingSetupFailed,         // io_uring 初始化失败
    HandlerRegistrationFailed, // 处理器注册失败
    BackpressureHigh,          // 背压过高
    LivelinessLost,            // 机器人断连
};

/**
 * @brief DataFlowReactor —— FlowKernel 核心 Reactor
 *
 * 双通道架构：
 *   CRITICAL  → SPSC 无锁队列      → 绝不丢帧，最低延迟
 *   BACKGROUND → Latest-only 缓存  → 只保最新帧，防积压雪崩
 *
 * 性能优化：
 *   - io_uring 替代 epoll，大幅减少系统调用
 *   - std::pmr 内存池消灭高频 malloc 堆碎片
 *   - constexpr FNV-1a 哈希路由，O(1) 分发
 *   - [[likely]]/[[unlikely]] 分支预测提示
 */
class DataFlowReactor {
public:
    /**
     * @param config_path Zenoh 配置文件路径（可选）
     * @param estop_publish_path 机器人断线时自动发布 E-Stop 的 Key Expression
     */
    explicit DataFlowReactor(const std::string& config_path = "",
                             const std::string& estop_publish_path = "robot/*/cmd/estop");
    ~DataFlowReactor();

    // 禁止拷贝（管理 Zenoh Session 和 io_uring 等不可拷贝资源）
    DataFlowReactor(const DataFlowReactor&) = delete;
    DataFlowReactor& operator=(const DataFlowReactor&) = delete;

    /**
     * @brief 启动事件循环（阻塞）
     */
    std::expected<void, ReactorError> run();

    /**
     * @brief 线程安全地停止事件循环
     */
    void stop() noexcept;

    /**
     * @brief 注册话题处理器
     *
     * @param path     Zenoh Key Expression（如 "robot/uav0/telemetry"）
     * @param priority 话题优先级（CRITICAL / NORMAL / BACKGROUND）
     * @param callback 回调函数，std::span 零拷贝视图
     */
    void register_handler(const std::string& path,
                          TopicPriority priority,
                          std::function<void(std::span<const uint8_t>)> callback);

private:
    void setup_io_uring();

    /**
     * @brief 核心分发函数（[[gnu::hot]] 标注为热路径，提示编译器激进优化）
     *
     * 每次调用顺序：
     *   1. 消费 SPSC 队列中的所有 CRITICAL 数据
     *   2. 消费 Latest-only 缓存中的 BACKGROUND 最新帧
     *   3. io_uring_wait_cqe_timeout 精确休眠 5ms
     */
    [[gnu::hot]] void handle_events();

    // ── Zenoh 资源 ──────────────────────────────────────────────────────────
    z_owned_session_t   session_;
    z_owned_publisher_t estop_publisher_; // 用于 Liveliness 断线时发布 E-Stop
    std::string         estop_publish_path_;

    // ── io_uring ─────────────────────────────────────────────────────────────
    struct io_uring ring_;

    // ── 运行控制 ─────────────────────────────────────────────────────────────
    std::atomic<bool> running_{false};

    // ── pmr 内存池（消灭高频 malloc 堆碎片）──────────────────────────────────
    // 1MB 栈上缓冲区，每个 handle_events 周期结束后 release()，O(1) 回收
    alignas(64) std::array<std::byte, 1024 * 1024> pool_buf_;
    std::pmr::monotonic_buffer_resource pool_{pool_buf_.data(), pool_buf_.size()};

    // ── 内部处理器结构 ────────────────────────────────────────────────────────
    struct Handler {
        std::string   path;      // ← 修复：保留原始路径字符串，用于动态订阅
        uint64_t      path_hash; // FNV-1a Hash，O(1) 分发路由
        TopicPriority priority;
        std::function<void(std::span<const uint8_t>)> callback;
    };

    // 缓存行对齐，防止处理器列表与原子变量发生伪共享
    alignas(hardware_destructive_interference_size) std::vector<Handler> handlers_;
};

} // namespace RobotDataFlow
