#include <iostream>
#include <csignal>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "reactor.hpp"
#include "config/config.hpp"
#include "tui/tui_app.hpp"
#include "robot_dataflow/common.hpp"
#include "robot_state_generated.h"

using namespace RobotDataFlow;

static DataFlowReactor* g_reactor = nullptr;

void signal_handler(int) {
    if (g_reactor) g_reactor->stop();
}

TopicPriority parse_priority(const std::string& priority_str) {
    if (priority_str == "CRITICAL") return TopicPriority::CRITICAL;
    if (priority_str == "NORMAL") return TopicPriority::NORMAL;
    return TopicPriority::BACKGROUND;
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    std::string config_path = "config/default.yaml";
    if (argc > 1) {
        config_path = argv[1];
    }

    // 加载配置
    Config config;
    if (!config.load(config_path)) {
        std::cerr << "Failed to load config from: " << config_path << std::endl;
        return 1;
    }

    // 设置日志
    auto console = spdlog::stdout_color_mt("flowkernel");
    spdlog::set_default_logger(console);
    
    if (config.tui().enabled) {
        // TUI 模式：静默日志
        spdlog::set_level(spdlog::level::warn);
    } else {
        // 传统模式：详细日志
        spdlog::set_pattern("  %^[%H:%M:%S.%e]%$  %^[%l]%$  %v");
        spdlog::set_level(spdlog::level::info);
    }

    try {
        // 创建 Reactor
        DataFlowReactor reactor(
            config.zenoh().config_path,
            config.liveliness().estop_publish_path,
            config.liveliness().key
        );
        g_reactor = &reactor;
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        // 根据配置注册 handlers
        for (const auto& handler_config : config.handlers()) {
            TopicPriority priority = parse_priority(handler_config.priority);
            
            reactor.register_handler(handler_config.path, priority,
                [priority](std::span<const uint8_t> data) {
                    auto msg = fbs::GetRobotMessage(data.data());
                    
                    if (priority == TopicPriority::CRITICAL && 
                        msg->payload_type() == fbs::MessagePayload_EStop) {
                        auto estop = static_cast<const fbs::EStop*>(msg->payload());
                        if (estop) {
                            auto now = std::chrono::system_clock::now().time_since_epoch().count();
                            double latency = (now - estop->timestamp()) / 1000.0;
                            spdlog::warn("[CRITICAL] E-Stop arrived! E2E Latency: {:.1f} µs", latency);
                        }
                    } else if (priority == TopicPriority::NORMAL && 
                               msg->payload_type() == fbs::MessagePayload_Telemetry) {
                        auto tel = static_cast<const fbs::Telemetry*>(msg->payload());
                        if (tel) {
                            auto now = std::chrono::system_clock::now().time_since_epoch().count();
                            double latency = (now - tel->timestamp()) / 1000.0;
                            spdlog::info("[NORMAL] Telemetry E2E Latency: {:.1f} µs", latency);
                        }
                    } else if (priority == TopicPriority::BACKGROUND && 
                               msg->payload_type() == fbs::MessagePayload_Telemetry) {
                        auto tel = static_cast<const fbs::Telemetry*>(msg->payload());
                        if (tel) {
                            auto now = std::chrono::system_clock::now().time_since_epoch().count();
                            double latency = (now - tel->timestamp()) / 1000.0;
                            spdlog::debug("[BACKGROUND] Costmap E2E Latency: {:.1f} µs", latency);
                        }
                    }
                });
        }

        // 运行模式
        if (config.tui().enabled) {
            // TUI 模式
            TUIApp tui(reactor, config);
            
            // 在后台线程运行 Reactor
            std::thread reactor_thread([&reactor]() {
                auto result = reactor.run();
                if (!result) {
                    spdlog::error("Reactor exited with error");
                }
            });
            
            // 运行 TUI（主线程）
            tui.run();
            
            // 停止 Reactor
            reactor.stop();
            if (reactor_thread.joinable()) {
                reactor_thread.join();
            }
        } else {
            // 传统模式（无 TUI）
            spdlog::info("FlowKernel v{} starting...", config.server().version);
            spdlog::info("Liveliness key: {}", config.liveliness().key);
            
            for (const auto& h : config.handlers()) {
                spdlog::info("  [{}] {}", h.priority, h.path);
            }
            
            spdlog::info("Press Ctrl+C to stop\n");
            
            auto result = reactor.run();
            if (!result) {
                spdlog::error("Reactor exited with error");
                return 1;
            }
        }

    } catch (const std::exception& e) {
        spdlog::critical("Fatal: {}", e.what());
        return 1;
    }

    spdlog::info("FlowKernel exited cleanly");
    return 0;
}