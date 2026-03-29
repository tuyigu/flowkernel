#include "config/config.hpp"
#include <spdlog/spdlog.h>
#include <filesystem>

namespace RobotDataFlow {

bool Config::load(const std::string& config_path) {
    try {
        if (!std::filesystem::exists(config_path)) {
            spdlog::warn("Config file not found: {}, using defaults", config_path);
            use_defaults();
            return true;
        }

        YAML::Node root = YAML::LoadFile(config_path);
        config_path_ = config_path;

        if (root["server"]) parse_server(root["server"]);
        if (root["zenoh"]) parse_zenoh(root["zenoh"]);
        if (root["channels"]) parse_channels(root["channels"]);
        if (root["liveliness"]) parse_liveliness(root["liveliness"]);
        if (root["tui"]) parse_tui(root["tui"]);
        if (root["handlers"]) parse_handlers(root["handlers"]);

        spdlog::info("Config loaded from: {}", config_path);
        return true;
    } catch (const YAML::Exception& e) {
        spdlog::error("Failed to parse config: {}", e.what());
        use_defaults();
        return false;
    }
}

void Config::use_defaults() {
    spdlog::info("Using default configuration");
    config_path_ = "";
}

void Config::parse_server(const YAML::Node& node) {
    if (node["name"]) server_.name = node["name"].as<std::string>();
    if (node["version"]) server_.version = node["version"].as<std::string>();
    if (node["log_level"]) server_.log_level = node["log_level"].as<std::string>();
}

void Config::parse_zenoh(const YAML::Node& node) {
    if (node["config_path"]) zenoh_.config_path = node["config_path"].as<std::string>();
}

void Config::parse_channels(const YAML::Node& node) {
    if (node["critical"]) {
        auto crit = node["critical"];
        if (crit["queue_size"]) channels_.critical.queue_size = crit["queue_size"].as<int>();
        if (crit["wakeup_mode"]) channels_.critical.wakeup_mode = crit["wakeup_mode"].as<std::string>();
    }
    if (node["normal"]) {
        auto norm = node["normal"];
        if (norm["queue_size"]) channels_.normal.queue_size = norm["queue_size"].as<int>();
    }
    if (node["background"]) {
        auto bg = node["background"];
        if (bg["strategy"]) channels_.background.strategy = bg["strategy"].as<std::string>();
        if (bg["max_age_ms"]) channels_.background.max_age_ms = bg["max_age_ms"].as<int>();
    }
}

void Config::parse_liveliness(const YAML::Node& node) {
    if (node["key"]) liveliness_.key = node["key"].as<std::string>();
    if (node["estop_publish_path"]) liveliness_.estop_publish_path = node["estop_publish_path"].as<std::string>();
}

void Config::parse_tui(const YAML::Node& node) {
    if (node["enabled"]) tui_.enabled = node["enabled"].as<bool>();
    if (node["refresh_rate_ms"]) tui_.refresh_rate_ms = node["refresh_rate_ms"].as<int>();
    if (node["show_banner"]) tui_.show_banner = node["show_banner"].as<bool>();
    if (node["color_theme"]) tui_.color_theme = node["color_theme"].as<std::string>();
}

void Config::parse_handlers(const YAML::Node& node) {
    handlers_.clear();
    for (const auto& handler : node) {
        HandlerConfig hc;
        if (handler["path"]) hc.path = handler["path"].as<std::string>();
        if (handler["priority"]) hc.priority = handler["priority"].as<std::string>();
        if (handler["description"]) hc.description = handler["description"].as<std::string>();
        handlers_.push_back(std::move(hc));
    }
}

} // namespace RobotDataFlow