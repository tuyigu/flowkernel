#pragma once

#include <cstdint>
#include <string_view>
#include <new>

// 处理不同编译器下的硬件破坏性干扰大小（Cache Line Size）
// 现代 CPU 的缓存行通常为 64 字节
#ifdef __cpp_lib_hardware_interference_size
    using std::hardware_destructive_interference_size;
#else
    // 如果标准库未定义，默认使用 64 字节
    constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

namespace RobotDataFlow {

/**
 * @brief 编译期 FNV-1a 哈希算法，用于 Zenoh Key Expressions。
 * 
 * 传统的字符串匹配在运行时开销较大，通过在编译期将字符串转换为 uint64_t 哈希值，
 * 可以将路由分发优化为 O(1) 的整数比较，大幅提升高频小包的处理性能。
 * 
 * @param str 需要哈希的字符串视图
 * @return uint64_t 计算得到的哈希值
 */
constexpr uint64_t fnv1a_hash(std::string_view str) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (char c : str) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/**
 * @brief 缓存行对齐辅助结构，用于防止“伪共享”(False Sharing)。
 * 
 * 伪共享发生在多个线程频繁修改位于同一个缓存行内的不同变量时，
 * 这会导致 CPU 频繁同步缓存，严重拖慢并发性能。
 */
template <typename T>
struct CacheAligned {
    alignas(hardware_destructive_interference_size) T value;
};

} // namespace RobotDataFlow
