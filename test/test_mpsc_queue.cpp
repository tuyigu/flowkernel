#include <gtest/gtest.h>
#include "robot_dataflow/mpsc_queue.hpp"
#include <thread>
#include <vector>
#include <atomic>

using namespace RobotDataFlow;

class MPSCQueueTest : public ::testing::Test {
protected:
    MPSCQueue<int, 64> queue;
};

TEST_F(MPSCQueueTest, PushPopBasic) {
    ASSERT_TRUE(queue.push(42));
    int val;
    ASSERT_TRUE(queue.pop(val));
    ASSERT_EQ(val, 42);
}

TEST_F(MPSCQueueTest, EmptyPop) {
    int val;
    ASSERT_FALSE(queue.pop(val));
}

TEST_F(MPSCQueueTest, FIFOOrder) {
    ASSERT_TRUE(queue.push(1));
    ASSERT_TRUE(queue.push(2));
    ASSERT_TRUE(queue.push(3));
    
    int val;
    ASSERT_TRUE(queue.pop(val));
    ASSERT_EQ(val, 1);
    ASSERT_TRUE(queue.pop(val));
    ASSERT_EQ(val, 2);
    ASSERT_TRUE(queue.pop(val));
    ASSERT_EQ(val, 3);
    ASSERT_FALSE(queue.pop(val));
}

TEST_F(MPSCQueueTest, FullQueue) {
    MPSCQueue<int, 4> small_queue;
    
    // 环形缓冲区容量为 4 时，实际只能存储 3 个元素
    for (int i = 0; i < 3; ++i) {
        int val = i;
        ASSERT_TRUE(small_queue.push(std::move(val)));
    }
    
    int val99 = 99;
    ASSERT_FALSE(small_queue.push(std::move(val99)));
    ASSERT_EQ(small_queue.dropped_count(), 1);
    
    // 多次满队列
    int val100 = 100;
    ASSERT_FALSE(small_queue.push(std::move(val100)));
    ASSERT_EQ(small_queue.dropped_count(), 2);
}

TEST_F(MPSCQueueTest, DroppedCount) {
    MPSCQueue<int, 2> tiny_queue;
    
    // 环形缓冲区容量为 2 时，实际只能存储 1 个元素
    ASSERT_TRUE(tiny_queue.push(1));
    ASSERT_EQ(tiny_queue.dropped_count(), 0);
    
    ASSERT_FALSE(tiny_queue.push(2));
    ASSERT_EQ(tiny_queue.dropped_count(), 1);
    
    ASSERT_FALSE(tiny_queue.push(3));
    ASSERT_EQ(tiny_queue.dropped_count(), 2);
}

TEST_F(MPSCQueueTest, WrapAround) {
    MPSCQueue<int, 4> small_queue;
    
    // 填满（环形缓冲区容量为 4 时，实际只能存储 3 个元素）
    for (int i = 0; i < 3; ++i) {
        small_queue.push(std::move(i));
    }
    
    // 弹出两个
    int val;
    small_queue.pop(val);
    small_queue.pop(val);
    
    // 再 push 两个（测试环形缓冲区）
    int v1 = 10;
    int v2 = 11;
    ASSERT_TRUE(small_queue.push(std::move(v1)));
    ASSERT_TRUE(small_queue.push(std::move(v2)));
    
    ASSERT_TRUE(small_queue.pop(val));
    ASSERT_EQ(val, 2);
    ASSERT_TRUE(small_queue.pop(val));
    ASSERT_EQ(val, 10);
    ASSERT_TRUE(small_queue.pop(val));
    ASSERT_EQ(val, 11);
}

TEST_F(MPSCQueueTest, MultiProducer) {
    MPSCQueue<int, 4096> big_queue;
    constexpr int num_producers = 4;
    constexpr int items_per_producer = 1000;
    
    std::vector<std::thread> producers;
    std::atomic<int> success_count{0};
    
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&big_queue, &success_count, i]() {
            for (int j = 0; j < items_per_producer; ++j) {
                if (big_queue.push(i * items_per_producer + j)) {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    
    for (auto& t : producers) {
        t.join();
    }
    
    int total_popped = 0;
    int val;
    while (big_queue.pop(val)) {
        ++total_popped;
    }
    
    ASSERT_EQ(total_popped, success_count.load());
    ASSERT_GT(total_popped, 0);
}

TEST_F(MPSCQueueTest, MultiProducerConsumer) {
    MPSCQueue<int, 4096> big_queue;
    constexpr int num_producers = 4;
    constexpr int items_per_producer = 500;
    
    std::vector<std::thread> producers;
    std::atomic<bool> producing{true};
    std::atomic<int> consumed_count{0};
    
    // 消费者线程
    std::thread consumer([&]() {
        int val;
        while (producing.load() || big_queue.pop(val)) {
            if (big_queue.pop(val)) {
                consumed_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });
    
    // 生产者线程
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&big_queue, i]() {
            for (int j = 0; j < items_per_producer; ++j) {
                big_queue.push(i * items_per_producer + j);
            }
        });
    }
    
    for (auto& t : producers) {
        t.join();
    }
    
    producing.store(false);
    consumer.join();
    
    ASSERT_EQ(consumed_count.load(), num_producers * items_per_producer);
}

TEST(MPSCQueueStructTest, MoveOnlyType) {
    struct MoveOnly {
        int value;
        MoveOnly(int v) : value(v) {}
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
        MoveOnly(MoveOnly&& other) noexcept : value(other.value) {}
        MoveOnly& operator=(MoveOnly&& other) noexcept {
            value = other.value;
            return *this;
        }
    };
    
    MPSCQueue<MoveOnly, 64> queue;
    
    ASSERT_TRUE(queue.push(MoveOnly(42)));
    
    MoveOnly val(0);
    ASSERT_TRUE(queue.pop(val));
    ASSERT_EQ(val.value, 42);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}