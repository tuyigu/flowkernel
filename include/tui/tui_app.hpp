#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>

#include "reactor.hpp"
#include "config/config.hpp"

namespace RobotDataFlow {

class TUIApp {
public:
    TUIApp(DataFlowReactor& reactor, const Config& config);
    ~TUIApp();

    void run();
    void stop();

private:
    DataFlowReactor& reactor_;
    const Config& config_;
    
    ftxui::ScreenInteractive screen_;
    std::atomic<bool> running_{false};
    std::thread refresh_thread_;
    
    // 状态数据（v2.3: 加互斥锁防止数据竞争）
    mutable std::mutex data_mutex_;
    ChannelStats critical_stats_;
    ChannelStats normal_stats_;
    ChannelStats background_stats_;
    std::vector<SessionInfo> sessions_;
    std::vector<HandlerStatus> handlers_;
    double uptime_seconds_ = 0.0;
    
    // UI 组件
    ftxui::Component main_container_;
    
    // 内部方法
    void refresh_data();
    void setup_ui();
    
    ftxui::Element render_header();
    ftxui::Element render_channels();
    ftxui::Element render_sessions();
    ftxui::Element render_handlers();
    ftxui::Element render_footer();
    
    std::string format_duration(double seconds) const;
    std::string format_latency(double us) const;
    std::string format_number(uint64_t num) const;
};

} // namespace RobotDataFlow