#include "reactor.hpp"
#include "robot_state_generated.h"

#include <spdlog/spdlog.h>
#include <flatbuffers/flatbuffers.h>

#include <unistd.h>
#include <poll.h>
#include <chrono>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <numeric>

namespace RobotDataFlow {

// CallbackContext 已移至 include/reactor.hpp（v2.3 扩展 session 追踪）

// =============================================================================
// Zenoh 数据接收回调（热路径）
// =============================================================================

[[gnu::hot]]
static void zenoh_data_callback(struct z_loaned_sample_t* sample, void* context) {
    auto* ctx = static_cast<CallbackContext*>(context);

    const z_loaned_bytes_t* raw = z_sample_payload(sample);
    const size_t len = z_bytes_len(raw);
    if (len == 0) [[unlikely]] return;

    std::vector<uint8_t> buf(len);
    z_bytes_reader_t reader = z_bytes_get_reader(raw);
    z_bytes_reader_read(&reader, buf.data(), len);

    // FlatBuffers 协议盾牌
    {
        flatbuffers::Verifier v(buf.data(), buf.size());
        if (!fbs::VerifyRobotMessageBuffer(v)) [[unlikely]] return;
    }

    // v2.3: Session 追踪 — 从 keyexpr 提取 robot_id 并更新最后可见时间
    {
        const z_loaned_keyexpr_t* kexpr = z_sample_keyexpr(sample);
        z_view_string_t key_str;
        z_keyexpr_as_view_string(kexpr, &key_str);
        auto key_len = z_string_len(z_loan(key_str));
        if (key_len > 0) {
            auto key_data = z_string_data(z_loan(key_str));
            auto key_sv = std::string_view(key_data, key_len);
            // key 格式: "robot/<id>/..." → 提取 "robot/<id>"
            auto second_slash = key_sv.find('/', 6);
            if (second_slash != std::string_view::npos) {
                auto robot_id = std::string(key_sv.substr(0, second_slash));
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::lock_guard<std::mutex> lock(*ctx->session_mtx);
                (*ctx->session_last_seen)[robot_id] = now_ms;
            }
        }
    }

    if (ctx->priority != TopicPriority::BACKGROUND) {
        // CRITICAL + NORMAL → MPSC 队列（无丢帧语义）
        // v2.3: 加入 enqueue_time 用于端到端延迟计算
        CriticalSample cs{ctx->handler_hash, std::move(buf), std::chrono::steady_clock::now()};
        const bool pushed = ctx->critical_queue->push(std::move(cs));

        if (!pushed) [[unlikely]] {
            spdlog::warn("[{}] MPSC FULL — frame dropped! total={}",
                ctx->priority == TopicPriority::CRITICAL ? "CRITICAL" : "NORMAL",
                ctx->critical_queue->dropped_count());
        } else if (ctx->priority == TopicPriority::CRITICAL) {
            // 仅 CRITICAL 触发 eventfd 主动唤醒（NORMAL 允许等 50ms 超时）
            const uint64_t v = 1;
            ::write(ctx->wakeup_fd, &v, sizeof(v));
        }
    } else {
        // BACKGROUND → Latest-only 缓存（热路径无锁）
        auto* slot = ctx->background_cache->get_slot_fast(ctx->handler_hash);
        if (slot) [[likely]] {
            slot->put(ctx->handler_hash, buf.data(), buf.size());
        }
    }
}

// =============================================================================
// Liveliness 断线保护回调
// v2.2 修复：用正规 FlatBuffers 构建 EStop，不再使用硬编码字节
// =============================================================================

static void zenoh_liveliness_callback(struct z_loaned_sample_t* sample, void* context) {
    if (z_sample_kind(sample) != Z_SAMPLE_KIND_DELETE) {
        spdlog::info("Robot liveliness restored.");
        return;
    }
    spdlog::warn("Robot liveliness LOST — publishing EStop (FlatBuffers)!");

    auto* pub = static_cast<z_owned_publisher_t*>(context);

    // 构建规范的 FlatBuffers EStop 消息（含实时时间戳）
    flatbuffers::FlatBufferBuilder fbb(256);
    const auto reason = fbb.CreateString("liveliness_lost");
    const auto now_ns = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto estop = fbs::CreateEStop(fbb, now_ns, reason);
    const auto msg   = fbs::CreateRobotMessage(
        fbb,
        fbs::MessageType_ESTOP,
        /*seq=*/0,
        fbs::MessagePayload_EStop,
        estop.Union());
    fbb.Finish(msg);

    z_owned_bytes_t estop_bytes;
    z_bytes_copy_from_buf(&estop_bytes, fbb.GetBufferPointer(), fbb.GetSize());
    z_publisher_put(z_publisher_loan(pub), z_move(estop_bytes), nullptr);
}

// =============================================================================
// DataFlowReactor 实现
// =============================================================================

DataFlowReactor::DataFlowReactor(const std::string& config_path,
                                 const std::string& estop_publish_path,
                                 const std::string& liveliness_key)
    : estop_publish_path_(estop_publish_path)
    , liveliness_key_(liveliness_key)
    , start_time_(std::chrono::steady_clock::now())
{
    // 1. Zenoh 会话（v2.3: 支持自定义配置文件路径）
    z_owned_config_t config;
    if (!config_path.empty()) {
        // 设置环境变量后调用默认初始化（Zenoh C API 会读取该路径）
        setenv("ZENOH_CONFIG", config_path.c_str(), 1);
    }
    if (z_config_default(&config) != Z_OK)
        throw std::runtime_error("Failed to initialize Zenoh config");
    if (z_open(&session_, z_move(config), nullptr) != Z_OK)
        throw std::runtime_error("Failed to open Zenoh session");

    // 2. E-Stop Publisher（INTERACTIVE_HIGH 优先级）
    {
        z_owned_keyexpr_t ke;
        z_publisher_options_t pub_opts;
        z_publisher_options_default(&pub_opts);
        pub_opts.priority = Z_PRIORITY_INTERACTIVE_HIGH;

        if (z_keyexpr_from_str(&ke, estop_publish_path_.c_str()) != Z_OK)
            throw std::runtime_error("Invalid E-Stop keyexpr: " + estop_publish_path_);
        if (z_declare_publisher(z_session_loan(&session_), &estop_publisher_,
                                z_keyexpr_loan(&ke), &pub_opts) != Z_OK) {
            z_keyexpr_drop(z_move(ke));
            throw std::runtime_error("Failed to declare E-Stop publisher");
        }
        z_keyexpr_drop(z_move(ke));
    }

    // 3. io_uring
    setup_io_uring();

    // 4. eventfd（非阻塞，进程退出后内核自动关闭）
    wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0)
        throw std::runtime_error(std::string("eventfd failed: ") + strerror(errno));
}

DataFlowReactor::~DataFlowReactor() {
    stop();
    if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
    io_uring_queue_exit(&ring_);
    z_publisher_drop(z_move(estop_publisher_));
    z_close(z_session_loan_mut(&session_), nullptr);
    z_session_drop(z_move(session_));
}

void DataFlowReactor::setup_io_uring() {
    int ret = io_uring_queue_init(64, &ring_, 0);
    if (ret < 0)
        throw std::runtime_error("io_uring init failed: " + std::string(strerror(-ret)));
}

void DataFlowReactor::arm_poll_eventfd() {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) [[unlikely]] {
        spdlog::warn("io_uring SQ full! Cannot arm eventfd, fallback to 50ms timeout.");
        return; // SQ 满（极罕见），下次 handle_events 重试
    }
    io_uring_prep_poll_add(sqe, wakeup_fd_, POLLIN);
    sqe->user_data = 1; // tag=1 标识 eventfd 事件
    io_uring_submit(&ring_);
}

std::expected<void, ReactorError> DataFlowReactor::run() {
    running_.store(true, std::memory_order_release);

    contexts_.clear(); // Clear any previous instance runs

    // 1. 预分配 BACKGROUND 槽位（v2.2: 仅 BACKGROUND，不再错误包含 NORMAL）
    for (auto& handler : handlers_) {
        if (handler.priority == TopicPriority::BACKGROUND) {
            background_cache_.pre_allocate(handler.path_hash);
        }
    }

    // 2. 声明订阅者（含 wakeup_fd 和实例指针的上下文）
    for (auto& handler : handlers_) {
        z_owned_keyexpr_t ke;
        if (z_keyexpr_from_str(&ke, handler.path.c_str()) != Z_OK) {
            spdlog::warn("Invalid keyexpr: {}", handler.path);
            continue;
        }

        auto ctx = std::make_unique<CallbackContext>(CallbackContext{
            .handler_hash       = handler.path_hash,
            .priority           = handler.priority,
            .wakeup_fd          = wakeup_fd_,
            .critical_queue     = &critical_queue_,
            .background_cache   = &background_cache_,
            .session_last_seen  = &session_last_seen_,
            .session_mtx        = &session_mutex_,
            .handler_paths      = &handler_paths_,
        });

        z_owned_closure_sample_t cb;
        z_closure_sample(&cb, zenoh_data_callback, nullptr, ctx.get());

        if (z_declare_background_subscriber(
                z_session_loan(&session_), z_keyexpr_loan(&ke),
                z_move(cb), nullptr) == Z_OK) {
            contexts_.push_back(std::move(ctx));
        } else {
            spdlog::warn("Failed to subscribe: {}", handler.path);
        }
        z_keyexpr_drop(z_move(ke));
    }

    // 3. Liveliness 订阅
    {
        z_owned_keyexpr_t ke;
        if (z_keyexpr_from_str(&ke, liveliness_key_.c_str()) == Z_OK) {
            z_owned_closure_sample_t lv_cb;
            z_closure_sample(&lv_cb, zenoh_liveliness_callback, nullptr, &estop_publisher_);
            z_liveliness_declare_background_subscriber(
                z_session_loan(&session_), z_keyexpr_loan(&ke),
                z_move(lv_cb), nullptr);
            z_keyexpr_drop(z_move(ke));
        }
    }

    // 4. 首次 arm eventfd poll
    arm_poll_eventfd();

    spdlog::info("FlowKernel v2.2 — io_uring+eventfd | MPSC dual-priority | pmr pool");
    spdlog::info("Liveliness key: {}", liveliness_key_);
    for (auto& h : handlers_) {
        const char* ch = (h.priority == TopicPriority::CRITICAL)   ? "CRITICAL  "
                       : (h.priority == TopicPriority::BACKGROUND) ? "BACKGROUND"
                                                                   : "NORMAL    ";
        spdlog::info("  [{}] {}", ch, h.path);
    }

    while (running_.load(std::memory_order_acquire)) {
        handle_events();
    }
    return {};
}

void DataFlowReactor::stop() noexcept {
    running_.store(false, std::memory_order_release);
    // 主动写 eventfd 唤醒等待中的 io_uring，让 run() 尽快退出
    if (wakeup_fd_ >= 0) {
        const uint64_t v = 1;
        ::write(wakeup_fd_, &v, sizeof(v));
    }
}

void DataFlowReactor::register_handler(
    const std::string& path,
    TopicPriority priority,
    std::function<void(std::span<const uint8_t>)> callback)
{
    uint64_t hash = fnv1a_hash(std::string_view(path));
    size_t   idx  = handlers_.size();
    handlers_.push_back({path, hash, priority, std::move(callback)});
    handler_index_.emplace(hash, idx);
    handler_paths_.push_back(path);  // v2.3: 保存 path 供回调中 session 追踪
}

void DataFlowReactor::print_stats() const {
    uint64_t d = critical_queue_.dropped_count();
    if (d > 0)
        spdlog::warn("[STAT] CRITICAL+NORMAL queue: total_dropped={} frames", d);
    background_cache_.print_drop_stats();
}

ChannelStats DataFlowReactor::get_critical_stats() const {
    ChannelStats stats;
    stats.processed = critical_processed_.load(std::memory_order_relaxed);
    stats.dropped = critical_dropped_.load(std::memory_order_relaxed);
    
    std::lock_guard<std::mutex> lock(latency_mutex_);
    if (!critical_latencies_.empty()) {
        std::vector<double> sorted(critical_latencies_.begin(), critical_latencies_.end());
        std::sort(sorted.begin(), sorted.end());
        
        stats.avg_latency_us = std::accumulate(sorted.begin(), sorted.end(), 0.0) / sorted.size();
        size_t p95_idx = static_cast<size_t>(sorted.size() * 0.95);
        size_t p99_idx = static_cast<size_t>(sorted.size() * 0.99);
        stats.p95_latency_us = sorted[std::min(p95_idx, sorted.size() - 1)];
        stats.p99_latency_us = sorted[std::min(p99_idx, sorted.size() - 1)];
    }
    return stats;
}

ChannelStats DataFlowReactor::get_normal_stats() const {
    ChannelStats stats;
    stats.processed = normal_processed_.load(std::memory_order_relaxed);
    stats.dropped = normal_dropped_.load(std::memory_order_relaxed);
    
    std::lock_guard<std::mutex> lock(latency_mutex_);
    if (!normal_latencies_.empty()) {
        std::vector<double> sorted(normal_latencies_.begin(), normal_latencies_.end());
        std::sort(sorted.begin(), sorted.end());
        
        stats.avg_latency_us = std::accumulate(sorted.begin(), sorted.end(), 0.0) / sorted.size();
        size_t p95_idx = static_cast<size_t>(sorted.size() * 0.95);
        size_t p99_idx = static_cast<size_t>(sorted.size() * 0.99);
        stats.p95_latency_us = sorted[std::min(p95_idx, sorted.size() - 1)];
        stats.p99_latency_us = sorted[std::min(p99_idx, sorted.size() - 1)];
    }
    return stats;
}

ChannelStats DataFlowReactor::get_background_stats() const {
    ChannelStats stats;
    stats.processed = background_processed_.load(std::memory_order_relaxed);
    stats.dropped = background_dropped_.load(std::memory_order_relaxed);
    
    std::lock_guard<std::mutex> lock(latency_mutex_);
    if (!background_latencies_.empty()) {
        std::vector<double> sorted(background_latencies_.begin(), background_latencies_.end());
        std::sort(sorted.begin(), sorted.end());
        
        stats.avg_latency_us = std::accumulate(sorted.begin(), sorted.end(), 0.0) / sorted.size();
        size_t p95_idx = static_cast<size_t>(sorted.size() * 0.95);
        size_t p99_idx = static_cast<size_t>(sorted.size() * 0.99);
        stats.p95_latency_us = sorted[std::min(p95_idx, sorted.size() - 1)];
        stats.p99_latency_us = sorted[std::min(p99_idx, sorted.size() - 1)];
    }
    return stats;
}

std::vector<SessionInfo> DataFlowReactor::get_sessions() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    std::vector<SessionInfo> sessions;
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    for (const auto& [robot_id, last_seen] : session_last_seen_) {
        SessionInfo info;
        info.robot_id = robot_id;
        info.last_seen_ms = now - last_seen;
        info.online = (info.last_seen_ms < 5000);  // 5秒内有数据视为在线
        sessions.push_back(std::move(info));
    }
    return sessions;
}

std::vector<HandlerStatus> DataFlowReactor::get_handler_statuses() const {
    std::vector<HandlerStatus> statuses;
    for (const auto& handler : handlers_) {
        HandlerStatus status;
        status.path = handler.path;
        status.active = true;
        
        switch (handler.priority) {
            case TopicPriority::CRITICAL:
                status.priority = "CRITICAL";
                status.total_processed = critical_processed_.load(std::memory_order_relaxed);
                break;
            case TopicPriority::NORMAL:
                status.priority = "NORMAL";
                status.total_processed = normal_processed_.load(std::memory_order_relaxed);
                break;
            case TopicPriority::BACKGROUND:
                status.priority = "BACKGROUND";
                status.total_processed = background_processed_.load(std::memory_order_relaxed);
                break;
        }
        statuses.push_back(std::move(status));
    }
    return statuses;
}

void DataFlowReactor::reset_stats() {
    critical_processed_.store(0, std::memory_order_relaxed);
    critical_dropped_.store(0, std::memory_order_relaxed);
    normal_processed_.store(0, std::memory_order_relaxed);
    normal_dropped_.store(0, std::memory_order_relaxed);
    background_processed_.store(0, std::memory_order_relaxed);
    background_dropped_.store(0, std::memory_order_relaxed);
    
    // v2.3: 重置 dropped/latency baseline，防止 reset 后计数跳跃
    last_queue_dropped_ = critical_queue_.dropped_count();
    last_bg_dropped_ = background_cache_.get_total_dropped();
    critical_count_last_ = 0;
    normal_count_last_ = 0;
    
    std::lock_guard<std::mutex> lock(latency_mutex_);
    critical_latencies_.clear();
    normal_latencies_.clear();
    background_latencies_.clear();
}

double DataFlowReactor::get_uptime_seconds() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start_time_).count();
}

void DataFlowReactor::record_latency(TopicPriority priority, double latency_us) {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    
    switch (priority) {
        case TopicPriority::CRITICAL:
            critical_latencies_.push_back(latency_us);
            if (critical_latencies_.size() > LATENCY_WINDOW_SIZE) {
                critical_latencies_.pop_front();
            }
            break;
        case TopicPriority::NORMAL:
            normal_latencies_.push_back(latency_us);
            if (normal_latencies_.size() > LATENCY_WINDOW_SIZE) {
                normal_latencies_.pop_front();
            }
            break;
        case TopicPriority::BACKGROUND:
            background_latencies_.push_back(latency_us);
            if (background_latencies_.size() > LATENCY_WINDOW_SIZE) {
                background_latencies_.pop_front();
            }
            break;
    }
}

// =============================================================================
// 核心分发（热路径）
// =============================================================================

constexpr size_t BACKPRESSURE_WARN_THRESHOLD = 200;

[[gnu::hot]]
void DataFlowReactor::handle_events() {
    pool_.release();

    // v2.3: 同步 MPSC 队列的 dropped 计数到 reactor 统计（实例变量，非 static）
    uint64_t current_dropped = critical_queue_.dropped_count();
    uint64_t new_dropped = current_dropped - last_queue_dropped_;
    last_queue_dropped_ = current_dropped;
    if (new_dropped > 0) {
        uint64_t total = critical_count_last_ + normal_count_last_;
        if (total > 0) {
            uint64_t c_drop = new_dropped * critical_count_last_ / total;
            critical_dropped_.fetch_add(c_drop, std::memory_order_relaxed);
            normal_dropped_.fetch_add(new_dropped - c_drop, std::memory_order_relaxed);
        } else {
            critical_dropped_.fetch_add(new_dropped, std::memory_order_relaxed);
        }
    }

    // 1. CRITICAL + NORMAL 通道（MPSC，绝对优先）
    CriticalSample cs;
    uint64_t critical_count = 0;
    uint64_t normal_count = 0;
    while (critical_queue_.pop(cs)) {
        auto it = handler_index_.find(cs.handler_hash);
        if (it != handler_index_.end()) [[likely]] {
            auto& handler = handlers_[it->second];
            
            // v2.3: 计算从入队到回调完成的延迟
            auto now = std::chrono::steady_clock::now();
            double latency_us = std::chrono::duration<double, std::micro>(now - cs.enqueue_time).count();
            record_latency(handler.priority, latency_us);
            
            handler.callback(std::span<const uint8_t>(cs.payload));
            
            if (handler.priority == TopicPriority::CRITICAL) {
                ++critical_count;
            } else {
                ++normal_count;
            }
        }
    }
    critical_processed_.fetch_add(critical_count, std::memory_order_relaxed);
    normal_processed_.fetch_add(normal_count, std::memory_order_relaxed);
    critical_count_last_ = critical_count;
    normal_count_last_ = normal_count;
    
    uint64_t total_processed = critical_count + normal_count;
    if (total_processed > BACKPRESSURE_WARN_THRESHOLD) [[unlikely]]
        spdlog::warn("MPSC backpressure: {} frames/cycle", total_processed);

    // 2. BACKGROUND 通道（Latest-only，带延迟和 dropped 统计）
    uint64_t background_count = 0;
    background_cache_.drain([this, &background_count](uint64_t hash, std::vector<uint8_t>&& payload, std::chrono::steady_clock::time_point put_time) {
        auto it = handler_index_.find(hash);
        if (it != handler_index_.end()) [[likely]] {
            // v2.3: BACKGROUND 端到端延迟（put_time → drain 处理完成）
            auto now = std::chrono::steady_clock::now();
            double latency_us = std::chrono::duration<double, std::micro>(now - put_time).count();
            record_latency(TopicPriority::BACKGROUND, latency_us);

            handlers_[it->second].callback(std::span<const uint8_t>(payload));
            ++background_count;
        }
    });
    background_processed_.fetch_add(background_count, std::memory_order_relaxed);

    // v2.3: BACKGROUND dropped 增量同步（实例变量，非 static）
    uint64_t bg_current_dropped = background_cache_.get_total_dropped();
    uint64_t bg_new_dropped = bg_current_dropped - last_bg_dropped_;
    last_bg_dropped_ = bg_current_dropped;
    background_dropped_.fetch_add(bg_new_dropped, std::memory_order_relaxed);

    // 3. io_uring 等待：eventfd 唤醒（CRITICAL 到达）或 50ms 超时（BACKGROUND 轮询）
    //
    //  - CRITICAL 到达 → callback 写 wakeup_fd_ → CQE 立即就绪 → Reactor 唤醒
    //  - 无事件       → 50ms 后超时 → 轮询一次 BACKGROUND Latest-only 缓存
    //  - stop() 调用  → 也写 wakeup_fd_ → 立即唤醒 → running_ 为 false → 退出
    struct io_uring_cqe* cqe = nullptr;
    struct __kernel_timespec ts{.tv_sec = 0, .tv_nsec = 50'000'000}; // 50ms
    if (int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts); ret == 0) {
        if (cqe->user_data == 1) {
            // 消费 eventfd 计数器（必须读出，否则 POLLIN 不复位）
            uint64_t val;
            ::read(wakeup_fd_, &val, sizeof(val));
        }
        io_uring_cqe_seen(&ring_, cqe);
        arm_poll_eventfd(); // POLL_ADD 一次性，必须重新提交
    }
    // ret == -ETIME：超时，静默返回，下一轮循环处理积累的 BACKGROUND 帧
}

} // namespace RobotDataFlow
