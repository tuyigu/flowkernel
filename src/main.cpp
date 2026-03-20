#include <iostream>
#include <csignal>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "reactor.hpp"
#include "robot_dataflow/common.hpp"
#include "robot_state_generated.h"
#include <chrono>

using namespace RobotDataFlow;

// ── ANSI 颜色 ────────────────────────────────────────────────────────────────
namespace C {
    constexpr auto R  = "\033[0m";            // reset
    constexpr auto B  = "\033[1m";            // bold
    constexpr auto D  = "\033[2m";            // dim
    constexpr auto W  = "\033[38;5;255m";     // bright white
    constexpr auto CY = "\033[38;5;51m";      // cyan
    constexpr auto YL = "\033[38;5;220m";     // yellow
    constexpr auto GR = "\033[38;5;245m";     // gray
    constexpr auto RE = "\033[38;5;196m";     // red
    constexpr auto GN = "\033[38;5;82m";      // green
}

// ── Banner ───────────────────────────────────────────────────────────────────
// 内框可见宽度 = 58 列（═ 数量）
// 每行用 row(带ANSI内容, 可见字符数) 精确填充尾部空格
static void print_banner() {
    constexpr int IW = 58; // inner width in visible columns

    // ═ 是 UTF-8 三字节 E2 95 90
    std::string bar;
    for (int i = 0; i < IW; ++i) bar += "\xe2\x95\x90";

    // row(): 生成一行 ║ content <padding> ║
    auto row = [&](const std::string& content, int vis) -> std::string {
        return std::string("  \xe2\x95\x91")  // ║
             + content
             + std::string(IW - vis, ' ')
             + "\xe2\x95\x91\n";              // ║
    };
    auto blank = [&]() -> std::string {
        return "  \xe2\x95\x91" + std::string(IW, ' ') + "\xe2\x95\x91\n";
    };

    // ── 各行内容 + 精确可见字符数 ────────────────────────────────────────────
    //
    // 标题："    Robot DataFlow Core ─── FlowKernel v2.1"
    //        4  +  19  +  5  +  10  +  5  = 43
    const std::string title =
        std::string("    ")
        + C::B + C::W  + "Robot DataFlow Core"
        + C::GR        + " \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 "  // " ─── "
        + C::CY + C::B + "FlowKernel"
        + C::YL        + " v2.1" + C::R;

    // 行1："    MPSC dual-channel  │  io_uring  │  Zenoh 1.8"
    //       4  +  18  +  4  +  8  +  5  +  9  = 48
    const std::string tech1 =
        std::string("    ")
        + C::D + C::W + "MPSC dual-channel "   // 18（补1空格，使│列对齐）
        + C::GR       + " \xe2\x94\x82  "       //  │  = 4 cols
        + C::D + C::W + "io_uring"              //  8
        + C::GR       + "  \xe2\x94\x82  "      //   │  = 5 cols
        + C::D + C::W + "Zenoh 1.8" + C::R;    //  9

    // 行2："    FlatBuffers shield │  pmr pool  │  spdlog"
    //       4  +  18  +  4  +  8  +  5  +  6  = 45
    const std::string tech2 =
        std::string("    ")
        + C::D + C::W + "FlatBuffers shield"   // 18
        + C::GR       + " \xe2\x94\x82  "       //  │  = 4 cols
        + C::D + C::W + "pmr pool"              //  8
        + C::GR       + "  \xe2\x94\x82  "      //   │  = 5 cols
        + C::D + C::W + "spdlog" + C::R;       //  6

    std::cout << "\n"
        << C::B << C::CY
        << "  \xe2\x95\x94" << bar << "\xe2\x95\x97\n"  // ╔═...═╗
        << blank()
        << row(title,  43)
        << blank()
        << row(tech1,  48)
        << row(tech2,  45)
        << blank()
        << "  \xe2\x95\x9a" << bar << "\xe2\x95\x9d\n"  // ╚═...═╝
        << C::R << "\n";
}

// ── Handler 汇总表 ────────────────────────────────────────────────────────────
static void print_handlers(
    const std::vector<std::pair<std::string, TopicPriority>>& hs)
{
    std::cout << C::B << C::W << "  Registered Handlers:\n" << C::R;
    for (auto& [path, prio] : hs) {
        const char *dot, *label, *col;
        switch (prio) {
            case TopicPriority::CRITICAL:
                dot = "\xe2\x97\x8f"; label = "CRITICAL  "; col = C::RE; break;
            case TopicPriority::NORMAL:
                dot = "\xe2\x97\x8f"; label = "NORMAL    "; col = C::YL; break;
            default:
                dot = "\xe2\x97\x8f"; label = "BACKGROUND"; col = C::GN; break;
        }
        std::cout
            << "    " << col << C::B << dot << C::R
            << "  " << col << C::B << label << C::R
            << "  " << C::GR << path << C::R << "\n";
    }
    std::cout << "\n";
}

// ── signal ────────────────────────────────────────────────────────────────────
static DataFlowReactor* g_reactor = nullptr;
void signal_handler(int) {
    if (g_reactor) g_reactor->stop();
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    auto console = spdlog::stdout_color_mt("flowkernel");
    spdlog::set_default_logger(console);
    spdlog::set_pattern("  %^[%H:%M:%S.%e]%$  %^[%l]%$  %v");
    spdlog::set_level(spdlog::level::info);

    print_banner();

    try {
        DataFlowReactor reactor;
        g_reactor = &reactor;
        std::signal(SIGINT, signal_handler);

        // 编译期哈希（debug 模式可见）
        constexpr uint64_t telemetry_hash = fnv1a_hash("robot/uav0/telemetry");
        constexpr uint64_t estop_hash     = fnv1a_hash("robot/*/cmd/estop");
        spdlog::debug("hash 'robot/uav0/telemetry' = {:#018x}", telemetry_hash);
        spdlog::debug("hash 'robot/*/cmd/estop'    = {:#018x}", estop_hash);

        // CRITICAL
        reactor.register_handler("robot/*/cmd/estop", TopicPriority::CRITICAL,
            [](std::span<const uint8_t> data) {
                auto msg = fbs::GetRobotMessage(data.data());
                if (msg->payload_type() == fbs::MessagePayload_EStop) {
                    auto estop = static_cast<const fbs::EStop*>(msg->payload());
                    if (estop) {
                        auto now = std::chrono::system_clock::now().time_since_epoch().count();
                        double latency = (now - estop->timestamp()) / 1000.0;
                        spdlog::warn("{}\xe2\x97\x8f{} [CRITICAL]   E-Stop arrived! E2E Latency: {:>8.1f} µs ({} bytes)",
                                     C::RE, C::R, latency, data.size());
                    }
                }
            });

        // NORMAL
        reactor.register_handler("robot/uav0/telemetry", TopicPriority::NORMAL,
            [](std::span<const uint8_t> data) {
                auto msg = fbs::GetRobotMessage(data.data());
                if (msg->payload_type() == fbs::MessagePayload_Telemetry) {
                    auto tel = static_cast<const fbs::Telemetry*>(msg->payload());
                    if (tel) {
                        auto now = std::chrono::system_clock::now().time_since_epoch().count();
                        double latency = (now - tel->timestamp()) / 1000.0;
                        spdlog::info("{}\xe2\x97\x8f{} [NORMAL]     Telemetry       E2E Latency: {:>8.1f} µs ({} bytes)",
                                     C::YL, C::R, latency, data.size());
                    }
                }
            });

        // BACKGROUND
        reactor.register_handler("robot/*/costmap", TopicPriority::BACKGROUND,
            [](std::span<const uint8_t> data) {
                auto msg = fbs::GetRobotMessage(data.data());
                if (msg->payload_type() == fbs::MessagePayload_Telemetry) {
                    auto tel = static_cast<const fbs::Telemetry*>(msg->payload());
                    if (tel) {
                        auto now = std::chrono::system_clock::now().time_since_epoch().count();
                        double latency = (now - tel->timestamp()) / 1000.0;
                        spdlog::info("{}\xe2\x97\x8f{} [BACKGROUND] Costmap (L-O)   E2E Latency: {:>8.1f} µs ({} bytes)",
                                     C::GN, C::R, latency, data.size());
                    }
                }
            });

        print_handlers({
            {"robot/*/cmd/estop",    TopicPriority::CRITICAL},
            {"robot/uav0/telemetry", TopicPriority::NORMAL},
            {"robot/*/costmap",      TopicPriority::BACKGROUND},
        });

        spdlog::info("Liveliness watchdog armed  \xe2\x86\x92  key: robot/**");
        spdlog::info("Press {}Ctrl+C{} to stop\n", C::B, C::R);

        auto result = reactor.run();
        if (!result) {
            spdlog::error("Reactor exited with error");
            return 1;
        }

    } catch (const std::exception& e) {
        spdlog::critical("Fatal: {}", e.what());
        return 1;
    }

    std::cout << "\n";
    spdlog::info("FlowKernel exited cleanly");
    return 0;
}
