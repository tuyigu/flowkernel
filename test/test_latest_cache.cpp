#include <gtest/gtest.h>
#include "robot_dataflow/latest_cache.hpp"
#include <thread>
#include <vector>

using namespace RobotDataFlow;

class LatestCacheTest : public ::testing::Test {
protected:
    LatestSampleCache cache;
};

TEST_F(LatestCacheTest, PreAllocateAndGetSlot) {
    cache.pre_allocate(100);
    
    auto* slot = cache.get_slot_fast(100);
    ASSERT_NE(slot, nullptr);
    
    auto* unknown_slot = cache.get_slot_fast(999);
    ASSERT_EQ(unknown_slot, nullptr);
}

TEST_F(LatestCacheTest, LazyGetSlot) {
    auto* slot = cache.get_slot(200);
    ASSERT_NE(slot, nullptr);
    
    // 再次获取应该返回同一个槽位
    auto* slot2 = cache.get_slot(200);
    ASSERT_EQ(slot, slot2);
}

TEST_F(LatestCacheTest, PutAndTake) {
    auto* slot = cache.get_slot(300);
    ASSERT_NE(slot, nullptr);
    
    uint8_t data[] = {1, 2, 3, 4, 5};
    slot->put(300, data, sizeof(data));
    
    auto result = slot->take();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->first, 300);
    ASSERT_EQ(result->second.size(), sizeof(data));
    ASSERT_EQ(result->second[0], 1);
    ASSERT_EQ(result->second[4], 5);
}

TEST_F(LatestCacheTest, OverwriteData) {
    auto* slot = cache.get_slot(400);
    
    uint8_t data1[] = {10, 20, 30};
    slot->put(400, data1, sizeof(data1));
    
    uint8_t data2[] = {40, 50, 60};
    slot->put(400, data2, sizeof(data2));  // 覆盖
    
    ASSERT_EQ(slot->drop_count.load(), 1);  // 一次丢帧
    
    auto result = slot->take();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->second[0], 40);  // 应该是最新数据
    ASSERT_EQ(result->second[2], 60);
}

TEST_F(LatestCacheTest, EmptySlotTake) {
    auto* slot = cache.get_slot(500);
    
    auto result = slot->take();
    ASSERT_FALSE(result.has_value());
}

TEST_F(LatestCacheTest, DropCountAccumulation) {
    auto* slot = cache.get_slot(600);
    
    for (int i = 0; i < 10; ++i) {
        uint8_t data[] = {static_cast<uint8_t>(i)};
        slot->put(600, data, 1);
    }
    
    ASSERT_EQ(slot->drop_count.load(), 9);  // 9 次丢帧
    
    auto result = slot->take();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->second[0], 9);  // 最新数据
}

TEST_F(LatestCacheTest, DrainAllSlots) {
    cache.pre_allocate(100);
    cache.pre_allocate(200);
    cache.pre_allocate(300);
    
    auto* slot1 = cache.get_slot_fast(100);
    auto* slot2 = cache.get_slot_fast(200);
    auto* slot3 = cache.get_slot_fast(300);
    
    uint8_t data1[] = {1};
    uint8_t data2[] = {2};
    uint8_t data3[] = {3};
    
    slot1->put(100, data1, 1);
    slot2->put(200, data2, 1);
    slot3->put(300, data3, 1);
    
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> collected;
    
    cache.drain([&](uint64_t hash, std::vector<uint8_t>&& payload) {
        collected.emplace_back(hash, std::move(payload));
    });
    
    ASSERT_EQ(collected.size(), 3);
    
    // 验证所有数据都被收集
    std::set<uint64_t> hashes;
    for (const auto& item : collected) {
        hashes.insert(item.first);
    }
    ASSERT_TRUE(hashes.count(100));
    ASSERT_TRUE(hashes.count(200));
    ASSERT_TRUE(hashes.count(300));
}

TEST_F(LatestCacheTest, DrainEmptySlots) {
    cache.pre_allocate(700);
    cache.pre_allocate(800);
    
    std::vector<uint64_t> collected;
    
    cache.drain([&](uint64_t hash, std::vector<uint8_t>&&) {
        collected.push_back(hash);
    });
    
    ASSERT_TRUE(collected.empty());  // 空槽位不处理
}

TEST_F(LatestCacheTest, ThreadSafety) {
    constexpr uint64_t hash = 999;
    cache.pre_allocate(hash);
    
    auto* slot = cache.get_slot_fast(hash);
    ASSERT_NE(slot, nullptr);
    
    constexpr int num_writers = 4;
    constexpr int writes_per_writer = 1000;
    
    std::vector<std::thread> writers;
    
    for (int i = 0; i < num_writers; ++i) {
        writers.emplace_back([slot, hash, i]() {
            for (int j = 0; j < writes_per_writer; ++j) {
                uint8_t data[] = {static_cast<uint8_t>(i), static_cast<uint8_t>(j)};
                slot->put(hash, data, 2);
            }
        });
    }
    
    for (auto& t : writers) {
        t.join();
    }
    
    // 总写入次数应该是 num_writers * writes_per_writer
    // 但只有最后一次会被保留
    auto result = slot->take();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->second.size(), 2);
    
    // drop_count 应该是总写入 - 1
    uint64_t expected_drops = num_writers * writes_per_writer - 1;
    ASSERT_EQ(slot->drop_count.load(), expected_drops);
}

TEST_F(LatestCacheTest, MultipleSlotsIndependent) {
    cache.pre_allocate(1000);
    cache.pre_allocate(2000);
    
    auto* slot1 = cache.get_slot_fast(1000);
    auto* slot2 = cache.get_slot_fast(2000);
    
    uint8_t data1[] = {10};
    uint8_t data2[] = {20};
    
    slot1->put(1000, data1, 1);
    slot2->put(2000, data2, 1);
    
    auto result1 = slot1->take();
    auto result2 = slot2->take();
    
    ASSERT_TRUE(result1.has_value());
    ASSERT_TRUE(result2.has_value());
    ASSERT_EQ(result1->second[0], 10);
    ASSERT_EQ(result2->second[0], 20);
}

// 并发测试：pre_allocate和get_slot_fast并发执行
TEST_F(LatestCacheTest, ConcurrentPreAllocateAndGetSlotFast) {
    constexpr int num_pre_allocate_threads = 2;
    constexpr int num_get_slot_fast_threads = 4;
    constexpr int operations_per_thread = 100;
    
    std::atomic<bool> stop_flag{false};
    std::atomic<int> pre_allocate_count{0};
    std::atomic<int> get_slot_fast_count{0};
    std::atomic<int> null_slot_count{0};
    
    // 线程1：持续调用pre_allocate添加新槽位
    std::vector<std::thread> pre_allocate_threads;
    for (int t = 0; t < num_pre_allocate_threads; ++t) {
        pre_allocate_threads.emplace_back([&, t]() {
            for (int i = 0; i < operations_per_thread; ++i) {
                uint64_t hash = 10000 + t * operations_per_thread + i;
                cache.pre_allocate(hash);
                pre_allocate_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    
    // 线程2：持续调用get_slot_fast读取槽位
    std::vector<std::thread> get_slot_fast_threads;
    for (int t = 0; t < num_get_slot_fast_threads; ++t) {
        get_slot_fast_threads.emplace_back([&]() {
            while (!stop_flag.load(std::memory_order_acquire)) {
                // 随机查询一个hash
                uint64_t hash = 10000 + (get_slot_fast_count.load() % (num_pre_allocate_threads * operations_per_thread));
                auto* slot = cache.get_slot_fast(hash);
                get_slot_fast_count.fetch_add(1, std::memory_order_relaxed);
                if (slot == nullptr) {
                    null_slot_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    
    // 等待pre_allocate线程完成
    for (auto& t : pre_allocate_threads) {
        t.join();
    }
    
    // 停止get_slot_fast线程
    stop_flag.store(true, std::memory_order_release);
    for (auto& t : get_slot_fast_threads) {
        t.join();
    }
    
    // 验证：所有pre_allocate操作都应该成功
    ASSERT_EQ(pre_allocate_count.load(), num_pre_allocate_threads * operations_per_thread);
    
    // 验证：get_slot_fast操作应该完成（不会崩溃或死锁）
    ASSERT_GT(get_slot_fast_count.load(), 0);
    
    // 验证：最终所有槽位都应该存在
    for (int t = 0; t < num_pre_allocate_threads; ++t) {
        for (int i = 0; i < operations_per_thread; ++i) {
            uint64_t hash = 10000 + t * operations_per_thread + i;
            auto* slot = cache.get_slot_fast(hash);
            ASSERT_NE(slot, nullptr) << "Slot " << hash << " should exist";
        }
    }
}

// 并发测试：drain和pre_allocate并发执行
TEST_F(LatestCacheTest, ConcurrentDrainAndPreAllocate) {
    constexpr int num_pre_allocate_threads = 2;
    constexpr int num_drain_threads = 2;
    constexpr int operations_per_thread = 50;
    
    std::atomic<bool> stop_flag{false};
    std::atomic<int> pre_allocate_count{0};
    std::atomic<int> drain_count{0};
    
    // 线程1：持续调用pre_allocate添加新槽位
    std::vector<std::thread> pre_allocate_threads;
    for (int t = 0; t < num_pre_allocate_threads; ++t) {
        pre_allocate_threads.emplace_back([&, t]() {
            for (int i = 0; i < operations_per_thread; ++i) {
                uint64_t hash = 20000 + t * operations_per_thread + i;
                cache.pre_allocate(hash);
                pre_allocate_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    
    // 线程2：持续调用drain消费数据
    std::vector<std::thread> drain_threads;
    for (int t = 0; t < num_drain_threads; ++t) {
        drain_threads.emplace_back([&]() {
            // 至少执行一次drain操作
            do {
                cache.drain([](uint64_t, std::vector<uint8_t>&&) {
                    // 空回调，只消费数据
                });
                drain_count.fetch_add(1, std::memory_order_relaxed);
            } while (!stop_flag.load(std::memory_order_acquire));
        });
    }
    
    // 等待pre_allocate线程完成
    for (auto& t : pre_allocate_threads) {
        t.join();
    }
    
    // 停止drain线程
    stop_flag.store(true, std::memory_order_release);
    for (auto& t : drain_threads) {
        t.join();
    }
    
    // 验证：所有pre_allocate操作都应该成功
    ASSERT_EQ(pre_allocate_count.load(), num_pre_allocate_threads * operations_per_thread);
    
    // 验证：drain操作应该完成（不会崩溃或死锁）
    ASSERT_GT(drain_count.load(), 0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}