# FlowKernel — Zenoh 生态的确定性调度引擎

> 部署在云端/边缘服务器的高性能消息调度内核
> 基于 **Zenoh 1.8 + io_uring + FlatBuffers** 构建，专为大规模智能体集群而生
>
> 当前版本：**v2.4**

---

## 核心问题

在大规模智能体集群（无人机、自动驾驶汽车、工业设备）场景下：

```
2000 无人机 × 200Hz Costmap = 400,000 帧/秒
                              ↓
                    海量数据流洪泛
                              ↓
                    控制指令被阻塞 40ms+
                              ↓
                    碰撞/事故
```

**传统方案的致命缺陷**：

| 方案 | 问题 |
|------|------|
| ROS2 DDS | 2000 节点 discovery 爆炸；QoS 优先级在拥塞时失效 |
| MQTT | 没有优先级隔离；TCP 队头阻塞 |
| gRPC/HTTP | 连接数爆炸；延迟不可控 |

---

## FlowKernel 的解决方案

### 确定性调度：指令永远插队

```
┌─────────────────────────────────────────────────────────┐
│                   数据洪泛 (400K帧/秒)                   │
│  ┌───────────────────────────────────────────────────┐  │
│  │  Costmap (BACKGROUND) → Latest-only，只保留最新帧 │  │
│  └───────────────────────────────────────────────────┘  │
│                         ↓                               │
│  ┌───────────────────────────────────────────────────┐  │
│  │  遥测 (NORMAL) → MPSC 队列，无丢帧                │  │
│  └───────────────────────────────────────────────────┘  │
│                         ↓                               │
│  ┌───────────────────────────────────────────────────┐  │
│  │  急停 (CRITICAL) → eventfd 即时唤醒，~200µs 穿透  │  │ ← 永远优先
│  └───────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

### 测试数据

| 数据流 | 流量环境 | E2E 时延 |
|--------|----------|----------|
| **CRITICAL 急停** | 常规 | **~200 µs** |
| **CRITICAL 急停** | 100Hz 洪泛 | **~300 µs**（稳如磐石）|
| NORMAL 遥测 | 100Hz 洪泛 | ~40,000 µs（排队）|
| BACKGROUND Costmap | 100Hz 洪泛 | Latest-only 覆盖 |

**关键指标**：即使背景数据堆积 40ms，急停指令仍然以 ~300µs 穿透。

---

## 应用场景

### 🚁 大规模无人机集群

```
2000 无人机 × 城市级 Costmap

FlowKernel 价值：
  • Latest-only → 400K帧/秒 降为 2K帧/秒（降200倍）
  • 优先级隔离 → 控制指令永远穿透
  • 背压控制 → 网络拥塞时自动降频
```

### 🚗 自动驾驶 V2X

```
车辆 ↔ 路侧单元 ↔ 云端

FlowKernel 价值：
  • 碰撞预警 (CRITICAL) → < 10ms 穿透
  • 视频流 (BACKGROUND) → 不阻塞安全消息
  • 断线保护 → Liveliness 自动触发安全停
```

### 📡 5G 基站边缘计算

```
10,000+ 并发连接 × 混合业务

FlowKernel 价值：
  • io_uring → 系统调用减少 70%+
  • MPSC → 多线程并发安全
  • QoS 整形 → 关键业务优先
```

### 🏙️ 智慧城市传感器网络

```
10,000 传感器 × 10Hz = 100,000 条/秒

FlowKernel 价值：
  • 紧急报警 (CRITICAL) → 即时穿透
  • 视频监控 (BACKGROUND) → Latest-only
  • 智能降频 → 根据状态动态调整
```

---

## 架构设计

```
发布者 (Robot/Vehicle/Sensor)
         │
         ├── [CRITICAL] 安全指令 ─────────────────┐
         │                                         │
         ├── [NORMAL] 遥测数据 ────────────────────▼
         │                              ┌─────────────────────────┐
         │                              │  MPSC 队列（无丢帧）    │
         │                              │  push_mtx_ 保护生产端   │
         │                              └────────────┬────────────┘
         │                          eventfd write ↑  │ pop()（无锁）
         │                     （CRITICAL 即时唤醒）  │
         └── [BACKGROUND] 高频数据                   │
                   │                                │
            ┌──────▼──────────────┐        ┌────────▼────────────┐
            │  Latest-only 缓存   │        │  Reactor 主线程     │
            │  只保最新帧，防雪崩 │──drain─│  io_uring 驱动      │
            └─────────────────────┘        └─────────────────────┘
                                                       │
                                           O(1) 路由分发
                                                       │
                                           ┌───────────▼───────────┐
                                           │  Zenoh Liveliness    │
                                           │  断线自动保护         │
                                           └───────────────────────┘
```

---

## 技术选型

| 技术 | 选择理由 |
|------|----------|
| **Zenoh 1.8** | 原生 UDP + QoS 优先级，协议头极小，5G 弱网优势明显 |
| **io_uring** | 异步 I/O，系统调用减少 70%+，延迟更确定 |
| **FlatBuffers** | 零拷贝解析，Schema 强约束，杜绝非法包 |
| **MPSC Queue** | 适配 Zenoh RX 2-线程并发模型，避免 UB |
| **eventfd** | CRITICAL 即时唤醒，消灭固定轮询窗口 |

---

## 快速开始

### 依赖安装（Fedora）

```bash
# 系统依赖
sudo dnf install -y liburing-devel flatbuffers-devel flatbuffers-compiler

# Rust（构建 zenoh-c 需要）
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# 编译安装 zenoh-c 1.8
git clone https://github.com/eclipse-zenoh/zenoh-c
cd zenoh-c && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) && sudo make install
```

### 编译

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### 运行

```bash
# 使用默认配置启动（TUI 模式）
./dataflow_kernel

# 指定配置文件
./dataflow_kernel /path/to/config.yaml
```

---

## TUI 监控面板

FlowKernel 内置专业的 TUI 监控界面：

```
╭──────────────────────────────────────────────────────────────────────────────╮
│ FlowKernel v2.4  │  Uptime: 2h34m12s  │  Q:Quit  R:Reset                   │
╰──────────────────────────────────────────────────────────────────────────────╯
╭──────────────────────────────────────────────────────────────────────────────╮
│ ▸ CHANNELS                                                                   │
├──────────────────────────────────────────────────────────────────────────────┤
│ ● CRITICAL    │  Processed: 12,847  │  Dropped: 0  │  Avg: 247µs  │  P95: 312µs  │
│ ● NORMAL      │  Processed: 892,341 │  Dropped: 0  │  Avg: 1.2ms  │  P95: 2.1ms  │
│ ● BACKGROUND  │  Processed: 4.2M    │  Dropped: 12K│  Avg: 8.4ms  │  P95: latest │
╰──────────────────────────────────────────────────────────────────────────────╯
╭──────────────────────────────────────────────────────────────────────────────╮
│ ▸ SESSIONS                                                                   │
├──────────────────────────────────────────────────────────────────────────────┤
│ ● ONLINE   │  robot/uav0   │  23ms ago                                      │
│ ● ONLINE   │  robot/uav1   │  45ms ago                                      │
│ ○ OFFLINE  │  robot/ugv0   │  2m ago (EStop triggered)                      │
╰──────────────────────────────────────────────────────────────────────────────╯
```

### 快捷键

| 键 | 功能 |
|----|------|
| `Q` | 退出程序 |
| `R` | 重置统计数据 |
| `Ctrl+C` | 优雅退出 |

---

## 配置系统

所有参数通过 YAML 配置文件指定，无需修改代码：

```yaml
# config/default.yaml
server:
  name: "FlowKernel"
  version: "2.4"
  log_level: "info"

zenoh:
  config_path: ""  # Zenoh 配置文件路径（可选）

channels:
  critical:
    queue_size: 512
    wakeup_mode: "eventfd"
  normal:
    queue_size: 512
  background:
    strategy: "latest_only"
    max_age_ms: 200

liveliness:
  key: "robot/**"
  estop_publish_path: "robot/*/cmd/estop"

handlers:
  - path: "robot/*/cmd/estop"
    priority: "CRITICAL"
  - path: "robot/uav0/telemetry"
    priority: "NORMAL"
  - path: "robot/*/costmap"
    priority: "BACKGROUND"

tui:
  enabled: true
  refresh_rate_ms: 100
```

---

## 项目结构

```
flowkernel/
├── config/
│   └── default.yaml                   # 默认配置文件
├── include/
│   ├── reactor.hpp                    # Reactor 主类
│   ├── config/
│   │   └── config.hpp                 # 配置加载器
│   ├── tui/
│   │   └── tui_app.hpp                # TUI 应用
│   └── robot_dataflow/
│       ├── common.hpp                 # FNV-1a 哈希 + CacheAligned
│       ├── mpsc_queue.hpp             # MPSC 多生产者队列
│       └── latest_cache.hpp           # Latest-only 缓存
├── src/
│   ├── main.cpp                       # 入口（配置加载 + TUI）
│   ├── reactor.cpp                    # 核心实现
│   ├── config.cpp                     # 配置解析
│   └── tui/
│       └── tui_app.cpp                # TUI 实现
├── fbs/
│   └── robot_state.fbs                # FlatBuffers Schema
├── test_flowkernel.py                 # Python 压力测试
└── CMakeLists.txt
```

---

## Python 压力测试

```bash
pip install zenoh flatbuffers

# 全通道混合压测
python3 test_flowkernel.py --mode weak_net_test

# 弱网模拟（需 sudo）
sudo tc qdisc add dev lo root netem delay 100ms 20ms loss 10%
python3 test_flowkernel.py --mode weak_net_test
sudo tc qdisc del dev lo root
```

---

## 路线图

### v2.4（当前）
- [x] TUI 监控面板
- [x] YAML 配置系统
- [x] 统计接口（Avg/P95/P99）
- [x] 会话追踪

### v3.0（计划）
- [ ] 多机集群模式
- [ ] Prometheus metrics
- [ ] 动态 QoS 策略
- [ ] 背压下行指令
- [ ] Zenoh Queryable（Service 原语）

---

## 许可证

[MIT License](LICENSE)
```
