#include <iostream>
#include <csignal>
#include "reactor.hpp"
#include "robot_dataflow/common.hpp"

using namespace RobotDataFlow;

// 信号处理：支持 Ctrl+C 优雅退出
static DataFlowReactor* g_reactor = nullptr;
void signal_handler(int) {
    if (g_reactor) g_reactor->stop();
}

int main() {
    std::cout << R"(
╔══════════════════════════════════════════════════════════╗
║   Robot DataFlow Core — FlowKernel v2.0                 ║
║   双通道优先级调度 | io_uring | Zenoh 1.8               ║
╚══════════════════════════════════════════════════════════╝
)" << std::endl;

    try {
        DataFlowReactor reactor;
        g_reactor = &reactor;
        std::signal(SIGINT, signal_handler);

        // ── 演示：编译期哈希 ──
        // 编译器在此计算并内联常量，运行时零开销
        constexpr uint64_t telemetry_hash = fnv1a_hash("robot/uav0/telemetry");
        constexpr uint64_t estop_hash     = fnv1a_hash("robot/*/cmd/estop");
        std::cout << "[INIT] 编译期哈希路由表：" << std::endl;
        std::cout << "         'robot/uav0/telemetry' → 0x" << std::hex << telemetry_hash << std::dec << std::endl;
        std::cout << "         'robot/*/cmd/estop'    → 0x" << std::hex << estop_hash     << std::dec << std::endl;
        std::cout << std::endl;

        // ── CRITICAL 通道：急停指令 ── 
        // 使用 TopicPriority::CRITICAL → 走 SPSC 无锁队列，绝不丢帧
        reactor.register_handler("robot/*/cmd/estop", TopicPriority::CRITICAL,
            [](std::span<const uint8_t> data) {
                std::cout << "[🔴 CRITICAL] 急停指令到达！大小: " << data.size() 
                          << " 字节 (优先级: INTERACTIVE_HIGH)" << std::endl;
            }
        );

        // ── NORMAL 通道：机器人遥测 ──
        reactor.register_handler("robot/uav0/telemetry", TopicPriority::NORMAL,
            [](std::span<const uint8_t> data) {
                std::cout << "[🟡 NORMAL] 遥测包: " << data.size() << " 字节" << std::endl;
            }
        );

        // ── BACKGROUND 通道：高频 Costmap ──
        // 使用 TopicPriority::BACKGROUND → 走 Latest-only 缓存，高频下自动去积压
        reactor.register_handler("robot/*/costmap", TopicPriority::BACKGROUND,
            [](std::span<const uint8_t> data) {
                std::cout << "[🟢 BACKGROUND] Costmap 最新帧: " << data.size() 
                          << " 字节 (积压帧已自动丢弃)" << std::endl;
            }
        );

        std::cout << "[INFO] Reactor 已运行... (按 Ctrl+C 停止)" << std::endl;
        std::cout << "[INFO] Liveliness 监测已激活，机器人断线将自动触发 E-Stop 保护" << std::endl;
        std::cout << std::endl;

        auto result = reactor.run();
        if (!result) {
            std::cerr << "[ERROR] Reactor 运行失败" << std::endl;
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n[INFO] FlowKernel 正常退出" << std::endl;
    return 0;
}
