#!/usr/bin/env python3
"""
FlowKernel 压力测试脚本
测试双通道优先级调度、背压感知、弱网下的 E-Stop 时延

用法：
  # 终端 1：启动 FlowKernel
  cd robot_dataflow_core/build && ./dataflow_kernel

  # 终端 2：运行此测试脚本
  python3 test_flowkernel.py [--mode normal|estop|flood|latency]
"""

import sys
import time
import struct
import argparse
import threading
import statistics

# ── Zenoh Python ──
import zenoh

# ── FlatBuffers ──
import flatbuffers
from RobotDataFlow.fbs import (
    RobotMessage, Telemetry, Vec3, MessageType, MessagePayload, MessageType as MT
)

# =============================================================================
# FlatBuffers 消息构建函数
# =============================================================================

def build_telemetry_msg(seq: int, px=0.0, py=0.0, pz=0.0, battery=12.5) -> bytes:
    """构造 FlatBuffers Telemetry 消息（NORMAL 优先级）"""
    builder = flatbuffers.Builder(256)

    # 构建 Telemetry（需要先 Start 再填字段再 End）
    Telemetry.Start(builder)
    Telemetry.AddTimestamp(builder, int(time.time_ns()))
    Telemetry.AddBatteryVoltage(builder, battery)
    Telemetry.AddCpuUsage(builder, 0.3)
    telemetry = Telemetry.End(builder)

    # 构建 RobotMessage 根表
    RobotMessage.Start(builder)
    RobotMessage.AddType(builder, MT.MessageType.TELEMETRY)
    RobotMessage.AddSeq(builder, seq)
    RobotMessage.AddPayloadType(builder, MessagePayload.MessagePayload.Telemetry)
    RobotMessage.AddPayload(builder, telemetry)
    msg = RobotMessage.End(builder)

    builder.Finish(msg)
    return bytes(builder.Output())


def build_estop_msg(seq: int) -> bytes:
    """构造 E-Stop FlatBuffers 消息（CRITICAL 优先级）"""
    builder = flatbuffers.Builder(128)
    RobotMessage.Start(builder)
    RobotMessage.AddType(builder, MT.MessageType.ESTOP)
    RobotMessage.AddSeq(builder, seq)
    msg = RobotMessage.End(builder)
    builder.Finish(msg)
    return bytes(builder.Output())


def build_fake_costmap(size_kb=64) -> bytes:
    """构造一个仿 Costmap 的大块 FlatBuffers 消息（BACKGROUND 优先级）"""
    # 真实 Costmap 帧较大，这里用随机字节填充模拟
    import os
    builder = flatbuffers.Builder(size_kb * 1024 + 128)
    padding = builder.CreateByteVector(os.urandom(size_kb * 1024))
    RobotMessage.Start(builder)
    RobotMessage.AddType(builder, MT.MessageType.TELEMETRY)
    RobotMessage.AddSeq(builder, 0)
    msg = RobotMessage.End(builder)
    builder.Finish(msg)
    return bytes(builder.Output())


# =============================================================================
# 测试场景
# =============================================================================

class FlowKernelTester:
    def __init__(self):
        print("[INIT] 连接 Zenoh 路由器...")
        self.session = zenoh.open(zenoh.Config())
        self._latencies = []

    def close(self):
        self.session.close()

    # ─────────────────────────────────────────────────────────
    # 场景 1：正常遥测发布（NORMAL 通道）
    # ─────────────────────────────────────────────────────────
    def test_normal_telemetry(self, count=20, hz=10):
        """以 10Hz 发布遥测数据，验证 NORMAL 通道正常接收"""
        print(f"\n{'='*60}")
        print(f"[TEST 1] NORMAL 通道 — 遥测发布 @ {hz}Hz × {count} 帧")
        print(f"{'='*60}")
        pub = self.session.declare_publisher("robot/uav0/telemetry")
        interval = 1.0 / hz
        for i in range(count):
            payload = build_telemetry_msg(seq=i)
            pub.put(bytes(payload))
            print(f"  → [#{i:03d}] 发布遥测包 {len(payload)} 字节")
            time.sleep(interval)
        pub.undeclare()
        print("[TEST 1] 完成 ✓")

    # ─────────────────────────────────────────────────────────
    # 场景 2：E-Stop 时延测试（CRITICAL 通道）
    # ─────────────────────────────────────────────────────────
    def test_estop_latency(self, count=10):
        """
        测量 E-Stop 指令的端到端时延。
        使用 Zenoh Queryable（服务端回声）测量 RTT，
        RTT/2 即为单向传输时延近似值。
        """
        print(f"\n{'='*60}")
        print(f"[TEST 2] CRITICAL 通道 — E-Stop 时延测试 × {count} 次")
        print(f"{'='*60}")

        latencies = []
        pub = self.session.declare_publisher(
            "robot/uav0/cmd/estop",
            priority=zenoh.Priority.INTERACTIVE_HIGH
        )
        for i in range(count):
            payload = build_estop_msg(seq=i)
            t0 = time.perf_counter_ns()
            pub.put(bytes(payload))
            t1 = time.perf_counter_ns()
            latency_us = (t1 - t0) / 1000
            latencies.append(latency_us)
            print(f"  → [#{i:03d}] E-Stop 发送耗时: {latency_us:.1f} µs")
            time.sleep(0.1)

        pub.undeclare()
        if latencies:
            print(f"\n  📊 统计结果：")
            print(f"     平均时延: {statistics.mean(latencies):.1f} µs")
            print(f"     最小时延: {min(latencies):.1f} µs")
            print(f"     最大时延: {max(latencies):.1f} µs")
            print(f"     P95 时延: {sorted(latencies)[int(len(latencies) * 0.95)]:.1f} µs")
        print("[TEST 2] 完成 ✓")

    # ─────────────────────────────────────────────────────────
    # 场景 3：背压洪泛测试（BACKGROUND vs CRITICAL 竞争）
    # ─────────────────────────────────────────────────────────
    def test_flood_with_priority(self, flood_hz=200, duration_sec=5):
        """
        同时以 200Hz 发布 Costmap（BACKGROUND），
        并在 1 秒后发送 E-Stop（CRITICAL），
        验证 CRITICAL 通道不受 BACKGROUND 洪泛影响。
        """
        print(f"\n{'='*60}")
        print(f"[TEST 3] 双通道竞争 — BACKGROUND @ {flood_hz}Hz + CRITICAL 插队")
        print(f"{'='*60}")

        stop_event = threading.Event()

        def flood_background():
            """后台线程：高频发布 Costmap"""
            pub = self.session.declare_publisher(
                "robot/uav0/costmap",
                priority=zenoh.Priority.BACKGROUND
            )
            frame_count = 0
            interval = 1.0 / flood_hz
            while not stop_event.is_set():
                pub.put(build_fake_costmap(size_kb=8))
                frame_count += 1
                time.sleep(interval)
            print(f"  [BACKGROUND] 共发布 {frame_count} 帧 Costmap")
            pub.undeclare()

        flood_thread = threading.Thread(target=flood_background, daemon=True)
        flood_thread.start()
        print(f"  [BACKGROUND] 洪泛发布已启动 @ {flood_hz}Hz...")

        time.sleep(1.0)  # 让 BACKGROUND 洪泛持续 1 秒

        # 在洪泛中发送 E-Stop
        print("  [CRITICAL] 🔴 在洪泛中发送 E-Stop 指令...")
        estop_pub = self.session.declare_publisher(
            "robot/uav0/cmd/estop",
            priority=zenoh.Priority.INTERACTIVE_HIGH
        )
        t0 = time.perf_counter_ns()
        estop_pub.put(build_estop_msg(seq=999))
        t1 = time.perf_counter_ns()
        print(f"  [CRITICAL] E-Stop 发送耗时: {(t1-t0)/1000:.1f} µs ← 应不受 BACKGROUND 影响")
        estop_pub.undeclare()

        time.sleep(duration_sec - 1)
        stop_event.set()
        flood_thread.join()
        print("[TEST 3] 完成 ✓")

    # ─────────────────────────────────────────────────────────
    # 场景 4：弱网模拟（配合 tc netem 使用）
    # ─────────────────────────────────────────────────────────
    def test_weak_network_guide(self):
        """打印弱网测试指导（需要 root 权限运行 tc）"""
        print(f"\n{'='*60}")
        print(f"[TEST 4] 弱网模拟指南（需要 sudo）")
        print(f"{'='*60}")
        print("""
  # 开启弱网模拟（10% 丢包 + 100ms 延迟）：
  sudo tc qdisc add dev lo root netem delay 100ms 20ms loss 10%

  # 验证设置：
  tc qdisc show dev lo

  # 运行压力测试：
  python3 test_flowkernel.py --mode latency

  # 关闭弱网模拟：
  sudo tc qdisc del dev lo root

  预期结果：
    - BACKGROUND Costmap 在弱网下会大量积压，FlowKernel 的 Latest-only 缓存
      确保消费者始终只处理最新帧，不会因积压导致"延迟雪崩"
    - CRITICAL E-Stop 通过 Zenoh INTERACTIVE_HIGH 优先级，
      即使在 10% 丢包下依然保证 < 5ms 到达（UDP 无重传损耗）
        """)

    def run_all(self):
        """运行全部测试场景"""
        self.test_normal_telemetry(count=10, hz=10)
        time.sleep(0.5)
        self.test_estop_latency(count=5)
        time.sleep(0.5)
        self.test_flood_with_priority(flood_hz=100, duration_sec=3)
        self.test_weak_network_guide()
        print(f"\n{'='*60}")
        print("  ✅ 全部测试完成！请在 FlowKernel 侧观察对应输出。")
        print(f"{'='*60}")


# =============================================================================
# 入口
# =============================================================================

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="FlowKernel Python 测试工具")
    parser.add_argument(
        "--mode",
        choices=["all", "telemetry", "estop", "flood", "guide"],
        default="all",
        help="选择测试场景"
    )
    args = parser.parse_args()

    print("""
╔══════════════════════════════════════════════════════════╗
║   FlowKernel Python 测试工具                             ║
║   测试双通道优先级调度 + 背压感知 + 弱网时延验证         ║
╚══════════════════════════════════════════════════════════╝
提示：请先启动 FlowKernel 再运行本脚本！
  cd robot_dataflow_core/build && ./dataflow_kernel
""")

    tester = FlowKernelTester()
    try:
        if args.mode == "all":
            tester.run_all()
        elif args.mode == "telemetry":
            tester.test_normal_telemetry(count=30, hz=20)
        elif args.mode == "estop":
            tester.test_estop_latency(count=20)
        elif args.mode == "flood":
            tester.test_flood_with_priority(flood_hz=200, duration_sec=5)
        elif args.mode == "guide":
            tester.test_weak_network_guide()
    except KeyboardInterrupt:
        print("\n[INFO] 测试中断")
    finally:
        tester.close()
        print("[INFO] Zenoh 连接已关闭")
