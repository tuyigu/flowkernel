#pragma once

#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace RobotDataFlow {

struct HandlerConfig {
    std::string path;
    std::string priority;
    std::string description;
};

struct ChannelConfig {
    int queue_size = 512;
    std::string wakeup_mode = "eventfd";
};

struct BackgroundChannelConfig {
    std::string strategy = "latest_only";
    int max_age_ms = 200;
};

struct LivelinessConfig {
    std::string key = "robot/**";
    std::string estop_publish_path = "robot/*/cmd/estop";
};

struct TUIConfig {
    bool enabled = true;
    int refresh_rate_ms = 100;
    bool show_banner = false;
    std::string color_theme = "dark";
};

struct ServerConfig {
    std::string name = "FlowKernel";
    std::string version = "2.3";
    std::string log_level = "info";
};

struct ZenohConfig {
    std::string config_path = "";
};

struct ChannelsConfig {
    ChannelConfig critical;
    ChannelConfig normal;
    BackgroundChannelConfig background;
};

class Config {
public:
    Config() = default;
    
    bool load(const std::string& config_path);
    void use_defaults();
    
    const std::string& get_config_path() const { return config_path_; }

    const ServerConfig& server() const { return server_; }
    const ZenohConfig& zenoh() const { return zenoh_; }
    const ChannelsConfig& channels() const { return channels_; }
    const LivelinessConfig& liveliness() const { return liveliness_; }
    const TUIConfig& tui() const { return tui_; }
    const std::vector<HandlerConfig>& handlers() const { return handlers_; }

private:
    std::string config_path_;
    ServerConfig server_;
    ZenohConfig zenoh_;
    ChannelsConfig channels_;
    LivelinessConfig liveliness_;
    TUIConfig tui_;
    std::vector<HandlerConfig> handlers_;

    void parse_server(const YAML::Node& node);
    void parse_zenoh(const YAML::Node& node);
    void parse_channels(const YAML::Node& node);
    void parse_liveliness(const YAML::Node& node);
    void parse_tui(const YAML::Node& node);
    void parse_handlers(const YAML::Node& node);
};

} // namespace RobotDataFlow