# Zenoh C API 对齐手册
# Robot DataFlow Core — FlowKernel 与 Zenoh 1.8.0

> 本文档记录项目中全部 Zenoh C API 调用，  
> 包含函数签名、所有权语义和上下文解释，  
> 供后续项目开发快速上手参考。

---

## 背景：Zenoh 1.8.0 的所有权语义

Zenoh C 1.8.0 采用了严格的线性类型系统，所有资源均有明确的归属状态：

| 类型后缀 | 含义 | 可执行操作 |
|---|---|---|
| `z_owned_xxx_t` | **拥有**资源，需负责释放 | 可借用、可移动、必须 drop |
| `z_loaned_xxx_t` | **借用**资源（不拥有生命周期）| 可读写，不可 drop |
| `z_moved_xxx_t*` | **移动**资源（所有权转移）| 一次性消耗，用后失效 |

**关键宏：**
- `z_loan(x)` → 从 Owned 获取不可变借用
- `z_loan_mut(x)` / `z_xxx_loan_mut(&x)` → 可变借用
- `z_move(x)` → 移动（所有权转移给被调用方）
- `z_xxx_drop(z_move(x))` → 显式释放资源

> [!IMPORTANT]
> **与 Zenoh 0.x 的最大区别**：0.x 版直接返回 Owned 对象（如 `session = z_open(...)`）；  
> 1.8.0 版改为输出参数模式（如 `z_open(&session, ...)`），并明确区分 Owned/Loaned/Moved。

---

## 第一组：Session 生命周期管理

### `z_config_default`

```c
z_result_t z_config_default(struct z_owned_config_t *this_);
```

**用途**：初始化默认配置对象（对应 Zenoh Router 的默认连接策略，监听 localhost:7447）。  
**返回值**：`Z_OK (0)` 成功，负数失败。  
**注意**：1.8.0 前版本为 `z_config_default()` 直接返回对象，现在改为输出指针。

**项目用法**：
```cpp
z_owned_config_t config;
if (z_config_default(&config) != Z_OK) {
    throw std::runtime_error("Failed to initialize Zenoh config");
}
```

---

### `z_open`

```c
z_result_t z_open(struct z_owned_session_t *this_,
                   struct z_moved_config_t *config,
                   const struct z_open_options_t *_options);
```

**用途**：打开 Zenoh Session（与 Router 建立连接，或 Peer-to-Peer 发现）。  
**所有权**：`config` 被 **移动（消耗）**，调用后不应再使用 `config`。  
**选项**：`_options` 传 `nullptr` 使用默认设置。

**项目用法**：
```cpp
if (z_open(&session_, z_move(config), nullptr) != Z_OK) {
    throw std::runtime_error("Failed to open Zenoh session");
}
```

---

### `z_session_loan` / `z_session_loan_mut`

```c
// 不可变借用（用于声明订阅器、查询等只读操作）
const struct z_loaned_session_t* z_session_loan(const z_owned_session_t *s);

// 可变借用（用于 z_close 等需要修改 Session 状态的操作）
struct z_loaned_session_t* z_session_loan_mut(z_owned_session_t *s);
```

**用途**：将 `z_owned_session_t` 转换为可传给其他函数的借用引用。  
**规律**：凡接受 `z_loaned_session_t*` 的函数都不会释放 Session，调用方依然拥有资源。

**项目用法**：
```cpp
z_declare_publisher(z_session_loan(&session_), &pub, ...);  // 声明发布者
z_close(z_session_loan_mut(&session_), nullptr);             // 关闭时需可变借用
```

---

### `z_close`

```c
z_result_t z_close(struct z_loaned_session_t *session,
                   struct z_close_options_t *options);
```

**用途**：优雅关闭 Session，flush 所有待发送的数据，注销订阅器。  
**注意**：`z_close` 后必须调用 `z_session_drop` 才能真正释放内存。两步缺一不可。

---

### `z_session_drop`

```c
void z_session_drop(struct z_moved_session_t *this_);
```

**用途**：释放 Session 占用的内存（必须在 `z_close` 之后调用）。  
**所有权**：消耗 Owned 对象，调用后 session 句柄失效。

**项目析构流程**：
```cpp
z_close(z_session_loan_mut(&session_), nullptr);  // 1. 优雅关闭
z_session_drop(z_move(session_));                  // 2. 释放内存
```

---

## 第二组：Key Expression（话题路径）

### `z_keyexpr_from_str`

```c
z_result_t z_keyexpr_from_str(struct z_owned_keyexpr_t *this_,
                               const char *expr);
```

**用途**：将字符串（如 `"robot/uav0/telemetry"`）解析并创建 Zenoh Key Expression。  
**Key Expression 语法**：
- `*` 匹配单段（`robot/*/telemetry` 匹配 `robot/uav0/telemetry`）
- `**` 匹配任意多段（`robot/**` 匹配所有机器人的所有话题）

**项目用法（动态路径绑定）**：
```cpp
// handler.path 来自 register_handler() 的注册，真正动态绑定
z_owned_keyexpr_t ke;
z_keyexpr_from_str(&ke, handler.path.c_str());
```

---

### `z_keyexpr_loan`

```c
const struct z_loaned_keyexpr_t* z_keyexpr_loan(const z_owned_keyexpr_t *ke);
```

**用途**：借用 Key Expression，传给需要 `z_loaned_keyexpr_t*` 的函数（绝大多数声明类 API）。

---

### `z_keyexpr_drop`

```c
void z_keyexpr_drop(struct z_moved_keyexpr_t *this_);
```

**用途**：释放 Key Expression 资源。  
**注意**：声明订阅器/发布者后，Zenoh 内部已持有 Key Expression 的引用，  
原始 Key Expression 可以立即 drop，**无需等到订阅器生命周期结束**。

```cpp
z_declare_background_subscriber(..., z_keyexpr_loan(&ke), ...);
z_keyexpr_drop(z_move(ke));  // 声明后立即释放，安全
```

---

## 第三组：订阅器（Subscriber）

### `z_closure_sample`

```c
void z_closure_sample(struct z_owned_closure_sample_t *this_,
                      void (*call)(struct z_loaned_sample_t *sample, void *context),
                      void (*drop)(void *context),
                      void *context);
```

**用途**：构造一个回调闭包，绑定函数指针和上下文指针。  
**参数解释**：
- `call`：数据到来时调用的函数（在 Zenoh 内部线程池中运行）
- `drop`：上下文释放时调用的析构函数（可传 `nullptr`）
- `context`：透传给 `call` 的用户数据指针

**回调签名（1.8.0 新变化）**：
```c
// 旧版（0.x）：z_sample_t* sample（Owned）
// 新版（1.x）：z_loaned_sample_t*（借用，生命周期仅限回调函数内）
void my_callback(struct z_loaned_sample_t* sample, void* context);
```

**项目用法**：
```cpp
z_owned_closure_sample_t cb;
z_closure_sample(&cb, zenoh_data_callback, nullptr, ctx.get());
```

---

### `z_declare_background_subscriber`

```c
z_result_t z_declare_background_subscriber(
    const struct z_loaned_session_t *session,
    const struct z_loaned_keyexpr_t *key_expr,
    struct z_moved_closure_sample_t *callback,
    const struct z_subscriber_options_t *options);
```

**用途**：声明一个"后台订阅器"——Session 关闭时自动注销，无需手动管理生命周期。  
**与 `z_declare_subscriber` 的区别**：
- `z_declare_subscriber`：返回 `z_owned_subscriber_t`，需要 `z_undeclare_subscriber` 手动注销
- `z_declare_background_subscriber`：Session 关闭时自动清理，代码更简洁，**推荐首选**

**所有权**：`callback` 被 **移动（消耗）**，调用后不应再使用。

---

## 第四组：发布者（Publisher）

### `z_declare_publisher`

```c
z_result_t z_declare_publisher(
    const struct z_loaned_session_t *session,
    struct z_owned_publisher_t *publisher,
    const struct z_loaned_keyexpr_t *key_expr,
    const struct z_publisher_options_t *options);
```

**用途**：在 Session 上声明一个发布者，绑定到指定 Key Expression。  
**优势**：预声明后可复用（不像 `z_put` 每次都要临时创建），适合高频发送场景。

---

### `z_publisher_options_default`

```c
void z_publisher_options_default(struct z_publisher_options_t *this_);
```

**用途**：初始化发布者选项为默认值，然后可以修改字段。

**项目用法（设置 CRITICAL 优先级）**：
```cpp
z_publisher_options_t opts;
z_publisher_options_default(&opts);
opts.priority = Z_PRIORITY_INTERACTIVE_HIGH;  // 物理层优先级

z_declare_publisher(z_session_loan(&session_), &pub, z_keyexpr_loan(&ke), &opts);
```

**Zenoh 1.8.0 优先级枚举值**：

| 枚举值 | 整数值 | 用途 |
|---|---|---|
| `Z_PRIORITY_REAL_TIME` | 1 | 实时控制（最高优先级）|
| `Z_PRIORITY_INTERACTIVE_HIGH` | 2 | E-Stop、关键指令 ← 本项目 CRITICAL |
| `Z_PRIORITY_INTERACTIVE_LOW` | 3 | 一般控制指令 |
| `Z_PRIORITY_DATA_HIGH` | 4 | 重要数据 |
| `Z_PRIORITY_DATA` | 5 | 普通数据 ← 本项目 NORMAL（默认）|
| `Z_PRIORITY_DATA_LOW` | 6 | 低优先级数据 |
| `Z_PRIORITY_BACKGROUND` | 7 | 后台数据 ← 本项目 BACKGROUND（Costmap）|

---

### `z_publisher_loan`

```c
const struct z_loaned_publisher_t* z_publisher_loan(const z_owned_publisher_t *pub);
```

**用途**：借用发布者句柄，用于 `z_publisher_put` 等操作。

---

### `z_publisher_put`

```c
z_result_t z_publisher_put(
    const struct z_loaned_publisher_t *this_,
    struct z_moved_bytes_t *payload,
    struct z_publisher_put_options_t *options);
```

**用途**：通过预声明的发布者发送数据。  
**所有权**：`payload` 被 **移动（消耗）**。  
**性能优势**：相比 `z_put`（临时发送），`z_publisher_put` 使用预注册的路由信息，不需要每次路由查找。

---

### `z_publisher_drop`

```c
void z_publisher_drop(struct z_moved_publisher_t *this_);
```

**用途**：注销发布者并释放资源。

---

## 第五组：数据读取（Sample / Bytes）

### `z_sample_payload`

```c
const struct z_loaned_bytes_t* z_sample_payload(const struct z_loaned_sample_t *this_);
```

**用途**：从接收到的 Sample 中获取 Payload 的借用引用。  
**生命周期**：返回的 `z_loaned_bytes_t*` 生命周期与 Sample 一致（仅限回调函数内）。

---

### `z_sample_kind`

```c
enum z_sample_kind_t z_sample_kind(const struct z_loaned_sample_t *this_);
```

**用途**：获取 Sample 的类型。

| 枚举值 | 含义 |
|---|---|
| `Z_SAMPLE_KIND_PUT` | 数据发布事件（正常数据）|
| `Z_SAMPLE_KIND_DELETE` | 数据删除事件（Liveliness 断线使用）|

**项目用法（Liveliness 断线检测）**：
```cpp
if (z_sample_kind(sample) == Z_SAMPLE_KIND_DELETE) {
    // 机器人断线！触发 E-Stop 保护
}
```

---

### `z_bytes_len`

```c
size_t z_bytes_len(const struct z_loaned_bytes_t *this_);
```

**用途**：获取 Payload 字节总长度（用于分配接收缓冲区）。

---

### `z_bytes_get_reader`

```c
struct z_bytes_reader_t z_bytes_get_reader(const struct z_loaned_bytes_t *data);
```

**用途**：创建一个顺序读取器，用于从 Payload 中逐步读取字节。  
**设计理念**：Zenoh 1.8.0 的 Payload 可能由多个内存分片（slice）组成，`reader` 提供统一的线性接口。

---

### `z_bytes_reader_read`

```c
size_t z_bytes_reader_read(struct z_bytes_reader_t *this_,
                           uint8_t *dst,
                           size_t len);
```

**用途**：从 Reader 中读取最多 `len` 字节到 `dst` 缓冲区。  
**返回值**：实际读取的字节数。

**项目中完整的 Payload 读取流程**：
```cpp
const z_loaned_bytes_t* raw = z_sample_payload(sample);  // 1. 获取 payload 借用
size_t len = z_bytes_len(raw);                            // 2. 获取长度
std::vector<uint8_t> buf(len);                            // 3. 分配缓冲区
z_bytes_reader_t reader = z_bytes_get_reader(raw);         // 4. 创建读取器
z_bytes_reader_read(&reader, buf.data(), len);             // 5. 读取数据
```

---

### `z_bytes_copy_from_buf`

```c
z_result_t z_bytes_copy_from_buf(struct z_owned_bytes_t *this_,
                                  const uint8_t *data,
                                  size_t len);
```

**用途**：从 `const` 缓冲区拷贝数据，创建 `z_owned_bytes_t`（用于发送）。  
**适用场景**：数据量小（如 E-Stop 消息 ~16 字节），拷贝开销可忽略。

---

## 第六组：Liveliness（存活监测）

### `z_liveliness_declare_background_subscriber`

```c
z_result_t z_liveliness_declare_background_subscriber(
    const struct z_loaned_session_t *session,
    const struct z_loaned_keyexpr_t *key_expr,
    struct z_moved_closure_sample_t *callback,
    const struct z_liveliness_subscriber_options_t *options);
```

**用途**：订阅指定路径下的 Liveliness Token 变化。  
**工作原理**：
- 机器人端：调用 `z_liveliness_declare_token("robot/uav0/alive")` 声明存活
- Session 断开时：Zenoh 路由器自动广播 `Z_SAMPLE_KIND_DELETE` 事件
- FlowKernel 收到 DELETE 事件 → 立刻触发 E-Stop 发布

**要与 `z_liveliness_declare_token`（板端）配对使用**：
```c
// 板端（Python/Rust/C++ ROS 2 节点）
z_liveliness_declare_token(&token, z_keyexpr_loan(&ke), nullptr);
// Token 存活 = Session 连接正常
// Session 断开 → Token 自动消失 → 触发 DELETE 事件
```

---

## 第七组：所有权通用宏

### `z_move`

```c
// 对于 C++，z_move 是重载函数模板，接受引用，返回 z_moved_xxx_t* 指针
template<typename T>
auto* z_move(T& owned);
```

**用途**：将 `z_owned_xxx_t` 转换为 `z_moved_xxx_t*`（所有权转移语义）。  
**重要**：调用 `z_move` 后，原变量**失效**，不应再访问。  
**等价于 C++ 的 `std::move`**，但针对 Zenoh 的 C 类型系统设计。

---

## API 调用全景图

```
构造阶段（DataFlowReactor::DataFlowReactor）
  z_config_default ──► z_open ──► z_declare_publisher（E-Stop Publisher）
                                         │
                                   z_publisher_options_default
                                   (设置 Z_PRIORITY_INTERACTIVE_HIGH)

运行阶段（DataFlowReactor::run）
  z_keyexpr_from_str ──► z_closure_sample ──► z_declare_background_subscriber
  z_keyexpr_loan ──────────────────────────────────────────────────────────────┘
  z_keyexpr_drop（声明后即可释放）

  z_liveliness_declare_background_subscriber（断线保护）

数据接收（zenoh_data_callback，Zenoh内部线程）
  z_sample_payload ──► z_bytes_len ──► z_bytes_get_reader ──► z_bytes_reader_read
  z_sample_kind（仅 Liveliness 回调使用）

数据发送（zenoh_liveliness_callback，断线时）
  z_bytes_copy_from_buf ──► z_publisher_put（通过 z_publisher_loan 借用发布者）

析构阶段（DataFlowReactor::~DataFlowReactor）
  z_publisher_drop ──► z_close ──► z_session_drop
```

---

## 与 0.x 版本的 API 对比速查

| 功能 | Zenoh 0.x | Zenoh 1.8.0 (本项目) |
|---|---|---|
| 默认配置 | `z_config_default()` (返回值) | `z_config_default(&config)` (输出指针) |
| 打开会话 | `session = z_open(z_move(config))` | `z_open(&session, z_move(config), nullptr)` |
| 关闭会话 | `z_close(z_move(session))` | `z_close(z_loan_mut(session)) + z_session_drop` |
| 借用会话 | `z_loan(session)` | `z_session_loan(&session)` |
| 回调参数 | `z_sample_t*` | `z_loaned_sample_t*` |
| 检查有效性 | `z_check(obj)` | 直接比较 `!= Z_OK` |
| 声明订阅 | `z_declare_subscriber(z_loan(s), ...)` | `z_declare_background_subscriber(z_session_loan(&s), ...)` |
| 发布数据 | `z_publisher_put(z_loan(pub), ...)` | `z_publisher_put(z_publisher_loan(&pub), ...)` |

---

*文档生成时间：2026-03-18 | Zenoh C 版本：1.8.x | 项目：Robot DataFlow Core FlowKernel*
