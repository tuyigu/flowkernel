#!/usr/bin/env python3
"""
FlowKernel 全链路时延与弱网压力测试脚本 v3.0

配合 TC 弱网模拟，全面观察 CRITICAL, NORMAL, BACKGROUND 三通道端到端时延表象。
"""

import sys
import time
import argparse
import threading
import statistics
import os

import zenoh
import flatbuffers
from RobotDataFlow.fbs import (
    RobotMessage, Telemetry, EStop, MessageType, MessagePayload, MessageType as MT
)

# =============================================================================
# FlatBuffers 精准时间戳消息构建
# =============================================================================

def build_telemetry_msg(seq: int) -> bytes:
    """构造含系统高精度时间戳的遥测消息"""
    builder = flatbuffers.Builder(256)
    Telemetry.Start(builder)
    Telemetry.AddTimestamp(builder, time.time_ns()) # Python侧精确发包时间
    Telemetry.AddBatteryVoltage(builder, 12.5)
    Telemetry.AddCpuUsage(builder, 0.3)
    telemetry = Telemetry.End(builder)

    RobotMessage.Start(builder)
    RobotMessage.AddType(builder, MT.MessageType.TELEMETRY)
    RobotMessage.AddSeq(builder, seq)
    RobotMessage.AddPayloadType(builder, MessagePayload.MessagePayload.Telemetry)
    RobotMessage.AddPayload(builder, telemetry)
    msg = RobotMessage.End(builder)
    builder.Finish(msg)
    return bytes(builder.Output())

def build_estop_msg(seq: int) -> bytes:
    """构造含时间戳的急停消息"""
    builder = flatbuffers.Builder(128)
    
    reason = builder.CreateString("manual_trigger")
    EStop.Start(builder)
    EStop.AddTimestamp(builder, time.time_ns()) # Python侧精确发包时间
    EStop.AddReason(builder, reason)
    estop = EStop.End(builder)
    
    RobotMessage.Start(builder)
    RobotMessage.AddType(builder, MT.MessageType.ESTOP)
    RobotMessage.AddSeq(builder, seq)
    RobotMessage.AddPayloadType(builder, MessagePayload.MessagePayload.EStop)
    RobotMessage.AddPayload(builder, estop)
    msg = RobotMessage.End(builder)
    builder.Finish(msg)
    return bytes(builder.Output())

def build_fake_costmap(seq: int, size_kb=8) -> bytes:
    """构造大块背景数据，内部借用 Telemetry 携带时间戳以便内核打印延迟"""
    builder = flatbuffers.Builder(size_kb * 1024 + 256)
    
    Telemetry.Start(builder)
    Telemetry.AddTimestamp(builder, time.time_ns()) # Python侧精确发包时间
    telemetry = Telemetry.End(builder)
    
    RobotMessage.Start(builder)
    RobotMessage.AddType(builder, MT.MessageType.TELEMETRY)
    RobotMessage.AddSeq(builder, seq)
    RobotMessage.AddPayloadType(builder, MessagePayload.MessagePayload.Telemetry)
    RobotMessage.AddPayload(builder, telemetry)
    msg = RobotMessage.End(builder)
    builder.Finish(msg)
    
    # 填充无用尾部数据模拟大包
    final_bytes = bytearray(builder.Output())
    if len(final_bytes) < size_kb * 1024:
        final_bytes += os.urandom(size_kb * 1024 - len(final_bytes))
    return bytes(final_bytes)

# =============================================================================
# 测试核心
# =============================================================================

class E2ELatencyTester:
    def __init__(self):
        print("🚀 [INIT] 连接 Zenoh 路由器...")
        self.session = zenoh.open(zenoh.Config())
        self.stop_event = threading.Event()

    def close(self):
        self.session.close()

    def test_weak_net_all_channels(self, duration=15):
        """
        全通道混合测试：
        - BACKGROUND (Costmap): 100Hz 狂发大包
        - NORMAL (Telemetry): 20Hz 发送常规重载
        - CRITICAL (E-Stop): 1Hz 突发急停
        结合 tc qdisc，C++日志将直观显示这三者的时延隔离。
        """
        print(f"\n{'='*70}")
        print(f"🔥 [TEST] 弱网全通道深度混合测试 (总时长: {duration}秒)")
        print(f"   [目标] 观察在大量积压下，不同层级的 E2E 时延是否产生隔离")
        print(f"{'='*70}")
        print("\n请紧盯 C++ 端的打印日志！\n")

        # 启动 BACKGROUND 100Hz 线程
        def run_background():
            pub = self.session.declare_publisher("robot/uav0/costmap", priority=zenoh.Priority.BACKGROUND)
            seq = 0
            while not self.stop_event.is_set():
                pub.put(build_fake_costmap(seq, size_kb=16)) # 16KB * 100Hz = 1.6MB/s
                seq += 1
                time.sleep(0.01)
            pub.undeclare()

        # 启动 NORMAL 20Hz 线程
        def run_normal():
            pub = self.session.declare_publisher("robot/uav0/telemetry", priority=zenoh.Priority.DATA)
            seq = 0
            while not self.stop_event.is_set():
                pub.put(build_telemetry_msg(seq))
                seq += 1
                time.sleep(0.05)
            pub.undeclare()

        bg_th = threading.Thread(target=run_background, daemon=True)
        nm_th = threading.Thread(target=run_normal, daemon=True)
        bg_th.start()
        nm_th.start()
        
        # 主线程进行 CRITICAL 推送
        pub_critical = self.session.declare_publisher("robot/uav0/cmd/estop", priority=zenoh.Priority.INTERACTIVE_HIGH)
        
        start_time = time.time()
        seq = 0
        while time.time() - start_time < duration:
            print(f"  → 🔴 发送 E-Stop (CRITICAL 插队), seq={seq}")
            pub_critical.put(build_estop_msg(seq))
            seq += 1
            time.sleep(1.0)  # 每秒发一次
            
        self.stop_event.set()
        bg_th.join()
        nm_th.join()
        pub_critical.undeclare()
        
        print(f"\n✅ 测试结束。请在 C++ 控制台分析 E2E 延迟数据。\n")

    def print_tc_guide(self):
        print("\n" + "="*70)
        print("💡 弱网环境 (tc) 快速配置指南：")
        print("="*70)
        print("开启弱网 (100ms延迟, 20ms抖动, 10%丢包)：")
        print("    sudo tc qdisc add dev lo root netem delay 100ms 20ms loss 10%")
        print("关闭弱网：")
        print("    sudo tc qdisc del dev lo root")
        print("="*70 + "\n")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["weak_net_test", "guide"], default="weak_net_test")
    args = parser.parse_args()

    tester = E2ELatencyTester()
    try:
        if args.mode == "weak_net_test":
            tester.print_tc_guide()
            tester.test_weak_net_all_channels(duration=10)
        elif args.mode == "guide":
            tester.print_tc_guide()
    except KeyboardInterrupt:
        print("\n[INFO] 停止测试")
    finally:
        tester.close()
