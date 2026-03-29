#include <gtest/gtest.h>
#include "config/config.hpp"
#include <filesystem>
#include <fstream>

using namespace RobotDataFlow;

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_config_path_ = "/tmp/test_flowkernel_config.yaml";
    }
    
    void TearDown() override {
        std::filesystem::remove(test_config_path_);
    }
    
    void write_config(const std::string& content) {
        std::ofstream file(test_config_path_);
        file << content;
        file.close();
    }
    
    std::string test_config_path_;
};

TEST_F(ConfigTest, LoadDefaultConfig) {
    Config config;
    config.use_defaults();
    
    ASSERT_EQ(config.server().name, "FlowKernel");
    ASSERT_EQ(config.server().version, "2.3");
    ASSERT_EQ(config.server().log_level, "info");
    
    ASSERT_EQ(config.liveliness().key, "robot/**");
    ASSERT_EQ(config.liveliness().estop_publish_path, "robot/*/cmd/estop");
    
    ASSERT_TRUE(config.tui().enabled);
    ASSERT_EQ(config.tui().refresh_rate_ms, 100);
}

TEST_F(ConfigTest, LoadFromFile) {
    write_config(R"(
server:
  name: "TestKernel"
  version: "1.0"
  log_level: "debug"

liveliness:
  key: "drone/**"
  estop_publish_path: "drone/*/cmd/stop"

tui:
  enabled: false
  refresh_rate_ms: 200
)");
    
    Config config;
    ASSERT_TRUE(config.load(test_config_path_));
    
    ASSERT_EQ(config.server().name, "TestKernel");
    ASSERT_EQ(config.server().version, "1.0");
    ASSERT_EQ(config.server().log_level, "debug");
    
    ASSERT_EQ(config.liveliness().key, "drone/**");
    ASSERT_EQ(config.liveliness().estop_publish_path, "drone/*/cmd/stop");
    
    ASSERT_FALSE(config.tui().enabled);
    ASSERT_EQ(config.tui().refresh_rate_ms, 200);
}

TEST_F(ConfigTest, LoadNonExistentFile) {
    Config config;
    ASSERT_TRUE(config.load("/nonexistent/config.yaml"));  // 应该使用默认值
    
    ASSERT_EQ(config.server().name, "FlowKernel");  // 默认值
}

TEST_F(ConfigTest, ParseHandlers) {
    write_config(R"(
handlers:
  - path: "robot/uav0/telemetry"
    priority: "CRITICAL"
    description: "无人机遥测"
  - path: "robot/ugv0/costmap"
    priority: "BACKGROUND"
    description: "地面车地图"
)");
    
    Config config;
    ASSERT_TRUE(config.load(test_config_path_));
    
    ASSERT_EQ(config.handlers().size(), 2);
    
    ASSERT_EQ(config.handlers()[0].path, "robot/uav0/telemetry");
    ASSERT_EQ(config.handlers()[0].priority, "CRITICAL");
    ASSERT_EQ(config.handlers()[0].description, "无人机遥测");
    
    ASSERT_EQ(config.handlers()[1].path, "robot/ugv0/costmap");
    ASSERT_EQ(config.handlers()[1].priority, "BACKGROUND");
    ASSERT_EQ(config.handlers()[1].description, "地面车地图");
}

TEST_F(ConfigTest, ParseChannels) {
    write_config(R"(
channels:
  critical:
    queue_size: 1024
    wakeup_mode: "polling"
  normal:
    queue_size: 256
  background:
    strategy: "drop_oldest"
    max_age_ms: 500
)");
    
    Config config;
    ASSERT_TRUE(config.load(test_config_path_));
    
    ASSERT_EQ(config.channels().critical.queue_size, 1024);
    ASSERT_EQ(config.channels().critical.wakeup_mode, "polling");
    
    ASSERT_EQ(config.channels().normal.queue_size, 256);
    
    ASSERT_EQ(config.channels().background.strategy, "drop_oldest");
    ASSERT_EQ(config.channels().background.max_age_ms, 500);
}

TEST_F(ConfigTest, PartialConfig) {
    write_config(R"(
server:
  name: "PartialKernel"
)");
    
    Config config;
    ASSERT_TRUE(config.load(test_config_path_));
    
    ASSERT_EQ(config.server().name, "PartialKernel");
    ASSERT_EQ(config.server().version, "2.3");  // 保留默认值
    ASSERT_EQ(config.server().log_level, "info");  // 保留默认值
}

TEST_F(ConfigTest, EmptyHandlers) {
    write_config(R"(
handlers: []
)");
    
    Config config;
    ASSERT_TRUE(config.load(test_config_path_));
    
    ASSERT_TRUE(config.handlers().empty());
}

TEST_F(ConfigTest, NoHandlersKey) {
    write_config(R"(
server:
  name: "NoHandlers"
)");
    
    Config config;
    ASSERT_TRUE(config.load(test_config_path_));
    
    ASSERT_TRUE(config.handlers().empty());  // 无 handlers 配置
}

TEST_F(ConfigTest, GetConfigPath) {
    Config config;
    config.use_defaults();
    ASSERT_TRUE(config.get_config_path().empty());
    
    write_config(R"(
server:
  name: "WithPath"
)");
    
    Config config2;
    config2.load(test_config_path_);
    ASSERT_EQ(config2.get_config_path(), test_config_path_);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}