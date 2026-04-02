#include "tui/tui_app.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <iomanip>
#include <sstream>
#include <algorithm>

using namespace ftxui;

namespace RobotDataFlow {

TUIApp::TUIApp(DataFlowReactor& reactor, const Config& config)
    : reactor_(reactor)
    , config_(config)
    , screen_(ScreenInteractive::Fullscreen())
{
    setup_ui();
}

TUIApp::~TUIApp() {
    stop();
}

void TUIApp::run() {
    running_.store(true, std::memory_order_release);
    
    // 启动数据刷新线程
    refresh_thread_ = std::thread([this]() {
        while (running_.load(std::memory_order_acquire)) {
            refresh_data();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.tui().refresh_rate_ms));
        }
    });
    
    screen_.Loop(main_container_);
}

void TUIApp::stop() {
    running_.store(false, std::memory_order_release);
    screen_.Exit();
    
    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }
}

void TUIApp::refresh_data() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    critical_stats_ = reactor_.get_critical_stats();
    normal_stats_ = reactor_.get_normal_stats();
    background_stats_ = reactor_.get_background_stats();
    sessions_ = reactor_.get_sessions();
    handlers_ = reactor_.get_handler_statuses();
    uptime_seconds_ = reactor_.get_uptime_seconds();
    
    screen_.PostEvent(Event::Custom);
}

void TUIApp::setup_ui() {
    auto header_renderer = Renderer([this]() {
        return render_header();
    });
    
    auto channels_renderer = Renderer([this]() {
        return render_channels();
    });
    
    auto sessions_renderer = Renderer([this]() {
        return render_sessions();
    });
    
    auto handlers_renderer = Renderer([this]() {
        return render_handlers();
    });
    
    auto footer_renderer = Renderer([this]() {
        return render_footer();
    });
    
    // 键盘事件处理
    auto keyboard_handler = CatchEvent([&](Event event) {
        if (event == Event::Character('q') || event == Event::Character('Q')) {
            stop();
            return true;
        }
        if (event == Event::Character('r') || event == Event::Character('R')) {
            reactor_.reset_stats();
            return true;
        }
        return false;
    });
    
    main_container_ = Container::Vertical({
        header_renderer,
        channels_renderer,
        sessions_renderer,
        handlers_renderer,
        footer_renderer,
    }) | keyboard_handler;
}

Element TUIApp::render_header() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    std::string uptime = format_duration(uptime_seconds_);
    
    Elements elements;
    elements.push_back(text(" " + config_.server().name + " v" + config_.server().version) | bold | color(Color::Cyan));
    elements.push_back(text("  │  Uptime: " + uptime) | color(Color::GrayLight));
    elements.push_back(text("  │  Q:Quit  R:Reset") | color(Color::GrayLight));
    
    return hbox(std::move(elements)) | border;
}

Element TUIApp::render_channels() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    auto channel_row = [this](const std::string& name, const ChannelStats& stats, Color channel_color) {
        Elements elements;
        elements.push_back(text(" ● ") | color(channel_color));
        elements.push_back(text(name) | bold | color(channel_color));
        elements.push_back(text("  │  ") | color(Color::GrayDark));
        elements.push_back(text("Processed: " + format_number(stats.processed)) | color(Color::White));
        elements.push_back(text("  │  ") | color(Color::GrayDark));
        elements.push_back(text("Dropped: " + format_number(stats.dropped)) | color(Color::White));
        elements.push_back(text("  │  ") | color(Color::GrayDark));
        elements.push_back(text("Avg: " + format_latency(stats.avg_latency_us)) | color(Color::White));
        elements.push_back(text("  │  ") | color(Color::GrayDark));
        elements.push_back(text("P95: " + format_latency(stats.p95_latency_us)) | color(Color::White));
        return hbox(std::move(elements));
    };
    
    Elements elements;
    elements.push_back(text(" ▸ CHANNELS") | bold | color(Color::White));
    elements.push_back(separator());
    elements.push_back(channel_row("CRITICAL  ", critical_stats_, Color::Red));
    elements.push_back(channel_row("NORMAL    ", normal_stats_, Color::Yellow));
    elements.push_back(channel_row("BACKGROUND", background_stats_, Color::Green));
    
    return vbox(std::move(elements)) | border;
}

Element TUIApp::render_sessions() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    Elements rows;
    
    if (sessions_.empty()) {
        rows.push_back(text("  No active sessions") | color(Color::GrayLight));
    } else {
        for (const auto& session : sessions_) {
            Color status_color = session.online ? Color::Green : Color::Red;
            std::string status_text = session.online ? "● ONLINE " : "○ OFFLINE";
            
            std::string last_seen;
            if (session.last_seen_ms < 1000) {
                last_seen = std::to_string(session.last_seen_ms) + "ms ago";
            } else {
                last_seen = format_duration(session.last_seen_ms / 1000.0) + " ago";
            }
            
            Elements row_elements;
            row_elements.push_back(text(" ") | color(Color::White));
            row_elements.push_back(text(status_text) | bold | color(status_color));
            row_elements.push_back(text("  │  ") | color(Color::GrayDark));
            row_elements.push_back(text(session.robot_id) | color(Color::White));
            row_elements.push_back(text("  │  ") | color(Color::GrayDark));
            row_elements.push_back(text(last_seen) | color(Color::GrayLight));
            rows.push_back(hbox(std::move(row_elements)));
        }
    }
    
    Elements elements;
    elements.push_back(text(" ▸ SESSIONS") | bold | color(Color::White));
    elements.push_back(separator());
    elements.push_back(vbox(std::move(rows)));
    
    return vbox(std::move(elements)) | border;
}

Element TUIApp::render_handlers() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    Elements rows;
    
    for (const auto& handler : handlers_) {
        Color priority_color;
        if (handler.priority == "CRITICAL") {
            priority_color = Color::Red;
        } else if (handler.priority == "NORMAL") {
            priority_color = Color::Yellow;
        } else {
            priority_color = Color::Green;
        }
        
        Elements row_elements;
        row_elements.push_back(text(" ● ") | color(Color::Green));
        row_elements.push_back(text(handler.path) | color(Color::White));
        row_elements.push_back(text("  │  ") | color(Color::GrayDark));
        row_elements.push_back(text(handler.priority) | bold | color(priority_color));
        row_elements.push_back(text("  │  ") | color(Color::GrayDark));
        row_elements.push_back(text(handler.active ? "● Active" : "○ Inactive") | color(handler.active ? Color::Green : Color::GrayLight));
        rows.push_back(hbox(std::move(row_elements)));
    }
    
    Elements elements;
    elements.push_back(text(" ▸ HANDLERS") | bold | color(Color::White));
    elements.push_back(separator());
    elements.push_back(vbox(std::move(rows)));
    
    return vbox(std::move(elements)) | border;
}

Element TUIApp::render_footer() {
    return hbox({
        text(" [Q]uit") | color(Color::Cyan),
        text("  [R]eset stats") | color(Color::Cyan),
        text("  [L]ogs") | color(Color::Cyan),
        text("  [H]elp") | color(Color::Cyan),
    }) | border;
}

std::string TUIApp::format_duration(double seconds) const {
    int hours = static_cast<int>(seconds) / 3600;
    int minutes = (static_cast<int>(seconds) % 3600) / 60;
    int secs = static_cast<int>(seconds) % 60;
    
    std::ostringstream oss;
    if (hours > 0) {
        oss << hours << "h";
    }
    if (minutes > 0 || hours > 0) {
        oss << minutes << "m";
    }
    oss << secs << "s";
    return oss.str();
}

std::string TUIApp::format_latency(double us) const {
    std::ostringstream oss;
    if (us >= 1000000.0) {
        oss << std::fixed << std::setprecision(2) << (us / 1000000.0) << "s";
    } else if (us >= 1000.0) {
        oss << std::fixed << std::setprecision(2) << (us / 1000.0) << "ms";
    } else {
        oss << std::fixed << std::setprecision(0) << us << "µs";
    }
    return oss.str();
}

std::string TUIApp::format_number(uint64_t num) const {
    if (num >= 1000000) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << (num / 1000000.0) << "M";
        return oss.str();
    } else if (num >= 1000) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (num / 1000.0) << "K";
        return oss.str();
    }
    return std::to_string(num);
}

} // namespace RobotDataFlow