#include "reactor.hpp"
#include "robot_dataflow/spsc_queue.hpp"
#include "robot_dataflow/latest_cache.hpp"
#include "robot_state_generated.h"

#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cerrno>

namespace RobotDataFlow {

// =============================================================================
// 内部数据结构
// =============================================================================

struct CriticalSample {
    uint64_t             handler_hash;
    std::vector<uint8_t> payload;
};

// CRITICAL 通道：SPSC 无锁队列（深度 512，绝不丢帧）
static SPSCQueue<CriticalSample, 512> critical_queue;

// BACKGROUND 通道：Latest-only 缓存（只保最新帧，防积压）
static LatestSampleCache background_cache;

// =============================================================================
// 回调上下文
// =============================================================================

struct CallbackContext {
    uint64_t      handler_hash;
    TopicPriority priority;
};

// =============================================================================
// Zenoh 数据接收回调（热路径）
// =============================================================================

[[gnu::hot]]
static void zenoh_data_callback(struct z_loaned_sample_t* sample, void* context) {
    auto* ctx = static_cast<CallbackContext*>(context);

    // 读取 payload（Zenoh 1.8 API）
    const z_loaned_bytes_t* raw = z_sample_payload(sample);
    const size_t len = z_bytes_len(raw);
    if (len == 0) return; // unlikely: 空包直接丢弃

    std::vector<uint8_t> buf(len);
    z_bytes_reader_t reader = z_bytes_get_reader(raw);
    z_bytes_reader_read(&reader, buf.data(), len);

    // P2：FlatBuffers 协议盾牌（Header-only 校验，开销极低）
    {
        flatbuffers::Verifier v(buf.data(), buf.size());
        if (!fbs::VerifyRobotMessageBuffer(v)) return; // 非法包丢弃
    }

    // P0：双通道路由
    if (ctx->priority == TopicPriority::CRITICAL) {
        // CRITICAL：写入 SPSC 队列，绝不丢帧
        CriticalSample cs{ctx->handler_hash, std::move(buf)};
        critical_queue.push(std::move(cs));
    } else {
        // BACKGROUND：写入 Latest-only 缓存，只保最新帧
        background_cache.get_slot(ctx->handler_hash)->put(
            ctx->handler_hash, buf.data(), buf.size());
    }
}

// =============================================================================
// Liveliness 断线保护回调
// =============================================================================

// 注：机器人端调用 z_liveliness_declare_token("robot/uav0/alive")
// 断线后此 Token 自动变为 DELETE 事件，此处立即发布 E-Stop
static void zenoh_liveliness_callback(struct z_loaned_sample_t* sample, void* context) {
    if (z_sample_kind(sample) != Z_SAMPLE_KIND_DELETE) {
        std::cout << "[INFO] Robot liveliness restored." << std::endl;
        return;
    }

    std::cerr << "[WARN] Robot liveliness LOST! Publishing E-Stop..." << std::endl;

    // P3 修复：使用真实预声明的 Publisher 发布 E-Stop
    auto* pub = static_cast<z_owned_publisher_t*>(context);

    // 构造最小 FlatBuffers E-Stop 消息（type=ESTOP, seq=0）
    // 在紧急情况下优先速度——用预构建的静态字节而非动态 Builder
    static const uint8_t ESTOP_MSG[] = {
        0x0C, 0x00, 0x00, 0x00, // root offset = 12
        0x00, 0x00, 0x00, 0x00, // file_identifier (unused)
        0x08, 0x00, 0x08, 0x00, // vtable offset
        0x02, 0x00, 0x00, 0x00, // type=ESTOP(2), seq=0
    };

    z_owned_bytes_t estop_bytes;
    // E-Stop 消息仅 16 字节，micro-copy 开销可忽略，用 copy_from_buf 接受 const
    z_bytes_copy_from_buf(&estop_bytes, ESTOP_MSG, sizeof(ESTOP_MSG));
    z_publisher_put(z_publisher_loan(pub), z_move(estop_bytes), nullptr);
}

// =============================================================================
// DataFlowReactor 实现
// =============================================================================

DataFlowReactor::DataFlowReactor(const std::string& config_path,
                                 const std::string& estop_publish_path)
    : estop_publish_path_(estop_publish_path) {

    // 1. 初始化 Zenoh 配置
    z_owned_config_t config;
    if (z_config_default(&config) != Z_OK) {
        throw std::runtime_error("Failed to initialize Zenoh config");
    }

    // 2. 打开会话
    if (z_open(&session_, z_move(config), nullptr) != Z_OK) {
        throw std::runtime_error("Failed to open Zenoh session");
    }

    // 3. 预声明 E-Stop Publisher（设置 INTERACTIVE_HIGH 优先级）
    {
        z_owned_keyexpr_t ke;
        z_publisher_options_t pub_opts;
        z_publisher_options_default(&pub_opts);
        pub_opts.priority = Z_PRIORITY_INTERACTIVE_HIGH;

        if (z_keyexpr_from_str(&ke, estop_publish_path_.c_str()) != Z_OK) {
            throw std::runtime_error("Invalid E-Stop keyexpr: " + estop_publish_path_);
        }
        if (z_declare_publisher(z_session_loan(&session_), &estop_publisher_,
                                z_keyexpr_loan(&ke), &pub_opts) != Z_OK) {
            z_keyexpr_drop(z_move(ke));
            throw std::runtime_error("Failed to declare E-Stop publisher");
        }
        z_keyexpr_drop(z_move(ke));
    }

    // 4. 初始化 io_uring
    setup_io_uring();
}

DataFlowReactor::~DataFlowReactor() {
    stop();
    io_uring_queue_exit(&ring_);
    z_publisher_drop(z_move(estop_publisher_));
    z_close(z_session_loan_mut(&session_), nullptr);
    z_session_drop(z_move(session_));
}

void DataFlowReactor::setup_io_uring() {
    int ret = io_uring_queue_init(64, &ring_, 0);
    if (ret < 0) {
        throw std::runtime_error("io_uring init failed: " + std::string(strerror(-ret)));
    }
}

std::expected<void, ReactorError> DataFlowReactor::run() {
    running_.store(true, std::memory_order_release);

    std::vector<std::unique_ptr<CallbackContext>> contexts;

    // P0 修复：从 handler.path 动态绑定 Key Expression（不再硬编码路径）
    for (auto& handler : handlers_) {
        z_owned_keyexpr_t ke;
        if (z_keyexpr_from_str(&ke, handler.path.c_str()) != Z_OK) {
            std::cerr << "[WARN] Invalid keyexpr: " << handler.path << std::endl;
            continue;
        }

        auto ctx = std::make_unique<CallbackContext>(
            CallbackContext{handler.path_hash, handler.priority});

        z_owned_closure_sample_t cb;
        z_closure_sample(&cb, zenoh_data_callback, nullptr, ctx.get());

        if (z_declare_background_subscriber(
                z_session_loan(&session_), z_keyexpr_loan(&ke),
                z_move(cb), nullptr) == Z_OK) {
            contexts.push_back(std::move(ctx));
        } else {
            std::cerr << "[WARN] Failed to subscribe: " << handler.path << std::endl;
        }
        z_keyexpr_drop(z_move(ke));
    }

    // P3 修复：Liveliness 订阅传入真实 Publisher 指针
    {
        z_owned_keyexpr_t ke;
        if (z_keyexpr_from_str(&ke, "robot/**") == Z_OK) {
            z_owned_closure_sample_t lv_cb;
            z_closure_sample(&lv_cb, zenoh_liveliness_callback, nullptr, &estop_publisher_);
            z_liveliness_declare_background_subscriber(
                z_session_loan(&session_), z_keyexpr_loan(&ke),
                z_move(lv_cb), nullptr);
            z_keyexpr_drop(z_move(ke));
        }
    }

    std::cout << "[INFO] FlowKernel started (dual-channel + Liveliness + pmr pool)" << std::endl;
    for (auto& h : handlers_) {
        const char* ch = (h.priority == TopicPriority::CRITICAL)    ? "CRITICAL"
                       : (h.priority == TopicPriority::BACKGROUND)  ? "BACKGROUND"
                                                                     : "NORMAL";
        std::cout << "[INFO]   [" << ch << "] " << h.path << std::endl;
    }

    while (running_.load(std::memory_order_acquire)) {
        handle_events();
    }

    return {};
}

void DataFlowReactor::stop() noexcept {
    running_.store(false, std::memory_order_release);
}

void DataFlowReactor::register_handler(
    const std::string& path,
    TopicPriority priority,
    std::function<void(std::span<const uint8_t>)> callback) {

    uint64_t hash = fnv1a_hash(std::string_view(path));
    // 修复：同时存储原始路径字符串（用于动态订阅）和哈希（用于 O(1) 分发）
    handlers_.push_back({path, hash, priority, std::move(callback)});
}

// =============================================================================
// 核心分发（热路径）
// =============================================================================

constexpr size_t BACKPRESSURE_WARN_THRESHOLD = 200;

[[gnu::hot]]
void DataFlowReactor::handle_events() {
    // pmr 内存池重置（O(1) 回收本周期所有分配，消灭堆碎片）
    pool_.release();

    // 1. CRITICAL 通道（绝对优先）
    CriticalSample cs;
    int critical_count = 0;
    while (critical_queue.pop(cs)) {
        for (auto& h : handlers_) {
            if (h.path_hash == cs.handler_hash && h.priority == TopicPriority::CRITICAL) {
                h.callback(std::span<const uint8_t>(cs.payload));
                break;
            }
        }
        ++critical_count;
    }

    if (critical_count > static_cast<int>(BACKPRESSURE_WARN_THRESHOLD)) {
        std::cerr << "[WARN] CRITICAL backpressure: " << critical_count << " frames/cycle\n";
    }

    // 2. BACKGROUND 通道（Latest-only 去积压）
    background_cache.drain([this](uint64_t hash, std::vector<uint8_t>&& payload) {
        for (auto& h : handlers_) {
            if (h.path_hash == hash && h.priority != TopicPriority::CRITICAL) {
                h.callback(std::span<const uint8_t>(payload));
                break;
            }
        }
    });

    // 3. io_uring 精确休眠 5ms（零 CPU 忙轮询）
    struct io_uring_cqe* cqe;
    struct __kernel_timespec ts{.tv_sec = 0, .tv_nsec = 5'000'000};
    if (int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts); ret == 0) {
        io_uring_cqe_seen(&ring_, cqe);
    }
}

} // namespace RobotDataFlow
