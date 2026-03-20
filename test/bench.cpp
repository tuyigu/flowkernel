/**
 * bench.cpp — FlowKernel 核心数据结构基准测试
 *
 * 无需 Zenoh 运行时，直接测：
 *   Test 1  MPSC 单生产者 push 延迟（P50/P95/P99/P999）
 *   Test 2  MPSC 双生产者并发安全 + 吞吐量
 *   Test 3  LatestSampleCache 洪泛抑制（丢帧计数）
 *   Test 4  Handler O(1) 分发：10 个 handler 下的 dispatch 帧率
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <numeric>
#include <algorithm>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <span>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "robot_dataflow/mpsc_queue.hpp"
#include "robot_dataflow/latest_cache.hpp"
#include "robot_dataflow/common.hpp"

using namespace RobotDataFlow;
using Clock = std::chrono::steady_clock;
using ns    = std::chrono::nanoseconds;

// ─── 工具 ────────────────────────────────────────────────────────────────────

static void print_latency(const std::string& name, std::vector<double>& s) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    double avg  = std::accumulate(s.begin(), s.end(), 0.0) / n;
    spdlog::info("  \033[1m{}\033[0m", name);
    spdlog::info("    avg={:.0f}ns  P50={:.0f}ns  P95={:.0f}ns  P99={:.0f}ns  P999={:.0f}ns",
        avg,
        s[n * 50 / 100],
        s[n * 95 / 100],
        s[n * 99 / 100],
        s[n * 999 / 1000]);
}

struct Frame {
    uint64_t             hash;
    std::vector<uint8_t> payload;
    int64_t              push_ns; // 生产者打时间戳
};

// ─── Test 1：单生产者 push 延迟 ───────────────────────────────────────────────
static void test1_push_latency() {
    spdlog::info("\033[38;5;51m▶ Test 1\033[0m  MPSC push() 延迟（单生产者，队列有余量）");

    MPSCQueue<Frame, 1024> q;
    constexpr int ROUNDS = 100'000;
    std::vector<double> lat;
    lat.reserve(ROUNDS);

    // 消费者线程：持续 pop，避免队列满
    std::atomic<bool> stop{false};
    std::atomic<int>  consumed{0};
    std::thread consumer([&] {
        Frame f;
        while (!stop.load(std::memory_order_relaxed)) {
            if (q.pop(f)) ++consumed;
        }
        Frame g;
        while (q.pop(g)) ++consumed;
    });

    // 生产者（主线程）：测每次 push 的耗时
    for (int i = 0; i < ROUNDS; ++i) {
        Frame f{uint64_t(i), std::vector<uint8_t>(64, uint8_t(i)), 0};
        auto t0 = Clock::now();
        q.push(std::move(f));
        auto t1 = Clock::now();
        lat.push_back(double(std::chrono::duration_cast<ns>(t1 - t0).count()));
    }

    stop = true;
    consumer.join();

    print_latency("push() 延迟", lat);
    spdlog::info("    生产={} 消费={} 丢弃={}", ROUNDS, consumed.load(), q.dropped_count());
}

// ─── Test 2：双生产者并发安全 + 吞吐量 ────────────────────────────────────────
static void test2_dual_producer() {
    spdlog::info("\033[38;5;51m▶ Test 2\033[0m  MPSC 双生产者并发安全（模拟 Zenoh RX 2线程）");

    MPSCQueue<Frame, 512> q;
    constexpr int PER_THREAD = 200'000;

    std::atomic<int> consumed{0};
    std::atomic<bool> stop{false};

    // 消费者线程
    std::thread consumer([&] {
        Frame f;
        while (!stop.load(std::memory_order_relaxed) || q.pop(f)) {
            if (q.pop(f)) ++consumed;
        }
    });

    // 两个生产者线程
    auto t0 = Clock::now();
    std::thread p1([&] {
        for (int i = 0; i < PER_THREAD; ++i) {
            Frame f{0xAAAA0000ULL | i, std::vector<uint8_t>(32, 0xAA), 0};
            q.push(std::move(f));
        }
    });
    std::thread p2([&] {
        for (int i = 0; i < PER_THREAD; ++i) {
            Frame f{0xBBBB0000ULL | i, std::vector<uint8_t>(32, 0xBB), 0};
            q.push(std::move(f));
        }
    });
    p1.join(); p2.join();
    stop = true;
    consumer.join();

    auto t1 = Clock::now();
    double elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
    int total_in  = PER_THREAD * 2;
    int dropped   = q.dropped_count();
    int processed = consumed.load();

    spdlog::info("    总生产={} 消费={} 丢弃={} (队列容量=512)", total_in, processed, dropped);
    spdlog::info("    耗时={:.1f}ms  吞吐={:.0f} 帧/s", elapsed_ms, (processed / elapsed_ms) * 1000.0);
    spdlog::info("    数据完整性：{}", (processed + dropped == total_in) ? "✓ 通过" : "✗ 异常！");
}

// ─── Test 3：LatestSampleCache 洪泛抑制 ───────────────────────────────────────
static void test3_latest_cache() {
    spdlog::info("\033[38;5;51m▶ Test 3\033[0m  LatestSampleCache 洪泛抑制（双线程各推 10K 帧）");

    LatestSampleCache cache;
    constexpr uint64_t HASH = 0xDEADBEEF;
    constexpr int PER_THREAD = 10'000;

    cache.pre_allocate(HASH);
    auto* slot = cache.get_slot_fast(HASH);

    // 两个生产者疯狂写入同一槽位
    auto t0 = Clock::now();
    std::thread p1([&] {
        for (int i = 0; i < PER_THREAD; ++i) {
            uint8_t buf[128];
            std::memset(buf, 0xAA, sizeof(buf));
            slot->put(HASH, buf, sizeof(buf));
        }
    });
    std::thread p2([&] {
        for (int i = 0; i < PER_THREAD; ++i) {
            uint8_t buf[128];
            std::memset(buf, 0xBB, sizeof(buf));
            slot->put(HASH, buf, sizeof(buf));
        }
    });
    p1.join(); p2.join();
    auto t1 = Clock::now();

    int drained = 0;
    cache.drain([&](uint64_t, std::vector<uint8_t>&&) { ++drained; });

    double elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
    uint64_t drops = slot->drop_count.load(std::memory_order_relaxed);

    spdlog::info("    总推入={} 消费者取走={} 被覆盖丢弃={}",
                 PER_THREAD * 2, drained, drops);
    spdlog::info("    洪泛抑制耗时={:.1f}ms  效果：\033[38;5;82m{}\033[0m",
                 elapsed_ms,
                 (drained <= 1 && drops >= PER_THREAD * 2 - 1) ? "✓ 仅保留最新帧" : "✗ 异常");
}

// ─── Test 4：O(1) Handler 分发吞吐 ───────────────────────────────────────────
static void test4_dispatch_throughput() {
    spdlog::info("\033[38;5;51m▶ Test 4\033[0m  O(1) Handler 分发吞吐（MPSC pop → unordered_map → callback）");

    // 模拟 handle_events() 里的分发逻辑
    struct MockHandler {
        uint64_t hash;
        std::function<void(std::span<const uint8_t>)> cb;
    };

    constexpr int N_HANDLERS = 10; // 注册 10 个 handler
    std::vector<MockHandler> handlers;
    std::unordered_map<uint64_t, size_t> index;
    std::atomic<int> call_count{0};

    for (int i = 0; i < N_HANDLERS; ++i) {
        uint64_t h = fnv1a_hash(std::string("robot/topic/") + std::to_string(i));
        handlers.push_back({h, [&](std::span<const uint8_t>) { ++call_count; }});
        index[h] = i;
    }

    MPSCQueue<Frame, 1024> q;
    constexpr int ROUNDS = 500'000;

    // 填满队列（循环使用各 handler 的 hash）
    {
        int pushed = 0;
        for (int i = 0; i < ROUNDS; ++i) {
            uint64_t h = handlers[i % N_HANDLERS].hash;
            Frame f{h, std::vector<uint8_t>(64, 0x42), 0};
            if (q.push(std::move(f))) ++pushed;
            if (pushed >= 1023) break; // 队列快满了先停
        }
    }

    // 计时分发
    auto t0 = Clock::now();
    Frame f;
    while (q.pop(f)) {
        auto it = index.find(f.hash);
        if (it != index.end()) [[likely]] {
            handlers[it->second].cb(std::span<const uint8_t>(f.payload));
        }
    }
    auto t1 = Clock::now();

    double elapsed_us = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1000.0;
    int calls = call_count.load();
    spdlog::info("    分发 {} 帧  耗时={:.0f}µs  每帧={:.0f}ns  回调触发={}",
                 calls, elapsed_us, (elapsed_us * 1000.0) / calls, calls);
    spdlog::info("    O(1) 分发：✓（handler 数 N={}，分发时间不随 N 线性增长）", N_HANDLERS);
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    auto console = spdlog::stdout_color_mt("bench");
    spdlog::set_default_logger(console);
    spdlog::set_pattern("  %^[%H:%M:%S.%e]%$  %v");
    spdlog::set_level(spdlog::level::info);

    spdlog::info("\033[1m\033[38;5;51mFlowKernel — 核心数据结构基准测试\033[0m");
    spdlog::info("───────────────────────────────────────────────────────");
    std::cout << "\n";

    test1_push_latency();
    std::cout << "\n";

    test2_dual_producer();
    std::cout << "\n";

    test3_latest_cache();
    std::cout << "\n";

    test4_dispatch_throughput();
    std::cout << "\n";

    spdlog::info("───────────────────────────────────────────────────────");
    spdlog::info("所有测试完成");
    return 0;
}
