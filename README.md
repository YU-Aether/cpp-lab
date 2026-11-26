# PlantPulse：C++ 车载中间件学习项目

从零手写一条车载以太网通信链路，覆盖车载中间件方向最核心的七个主题：

> 并发基础 → 传输层 → 协议与序列化 → SOME/IP → 服务发现（SOME/IP-SD）→ 发布订阅 → 功能安全（E2E）

除 Qt6（上位机 UI）与 GoogleTest 外，不依赖任何网络库、序列化库或通信中间件——socket、epoll、SOME/IP、SD、E2E 编解码全部手写，共 **128 个单元测试**。

## 整体架构

```
上位机 plantpulse_app（Qt6 Widgets，需 WSLg / X11）
├── MainWindow                 日志、状态栏、目标地址同步
├── FrameSource                自动数据源（QTimer 周期产 Request）
├── EchoClient                 SOME/IP 客户端：超时重发 + E2E 收发保护
├── SdDiscoverer               服务发现：多播 300ms → 单播 300ms → 手动地址兜底
└── SdSubscriber               事件订阅：Subscribe → Ack → UDP 收 EVENT-N

        │  ① SD 多播 224.244.224.245:30490   OfferService（每 1s）
        │  ② SD 单播                          发现请求 / 应答
        │  ③ TCP 9555                         SOME/IP Request ⇄ Response（E2E 保护）
        │  ④ UDP 随机端口                     Subscribe/Ack + Notification（EVENT-N）
        ▼
服务端 plantpulse_server（纯 POSIX，无 Qt 依赖）
├── SomeipServer               epoll LT + 非阻塞 IO，一连接一 Decoder，字节透明回显
└── SdResponder                多播 Offer / 单播应答 / 订阅登记 / 周期事件推送
```

## 目录结构

```
cpp-lab/
├── CMakeLists.txt              # 18 个测试目标 + plantpulse_ui 静态库 + 2 个可执行程序
├── src/                        # 全部头文件优先（header-only 为主）
│   ├── ring_buffer.h           # 环形缓冲区
│   ├── spsc_queue.h            # 无锁 SPSC 队列（原子 + 内存序）
│   ├── blocking_spsc_queue.h   # 阻塞 SPSC 队列（mutex + condition_variable）
│   ├── blocking_echo_server.h  # 阻塞式回显服务（一连接一线程）
│   ├── epoll_echo_server.h     # epoll LT 回显服务（非阻塞 + EPOLLOUT）
│   ├── frame_codec.h           # 自定义帧编解码（三态解码）
│   ├── main_window.h/.cpp      # Qt 主窗口
│   ├── frame_source.h/.cpp     # Qt 自动数据源
│   ├── echo_client.h/.cpp      # Qt 客户端（重发 + E2E）
│   ├── request_tracker.h       # 请求超时重发（时间由调用方注入，可测）
│   ├── someip_codec.h          # SOME/IP 编解码（三态 + 协议错误）
│   ├── someip_server.h         # SOME/IP 服务端
│   ├── sd_codec.h              # SOME/IP-SD entry/option 编解码
│   ├── sd_discovery.h          # SD 服务发现 + 订阅 + 事件推送
│   ├── e2e_protection.h        # E2E 保护（CRC-32 + Counter + DataID）
│   ├── main.cpp                # 上位机入口（plantpulse_app）
│   └── plantpulse_server.cpp   # 服务端入口（plantpulse_server）
├── tests/                      # 18 个 GTest 测试文件，共 128 个用例
├── scripts/                    # 一键构建 / 测试脚本
└── .github/workflows/ci.yml    # GitHub Actions：push 自动构建 + ctest
```

## 模块地图

| 阶段 | 模块 | 文件 | 核心知识点 |
|------|------|------|-----------|
| 并发基础 | 环形缓冲 | `ring_buffer.h` | 首尾指针、满/空判别 |
| | 无锁队列 | `spsc_queue.h` | 原子操作、acquire/release 内存序、缓存行填充 |
| | 阻塞队列 | `blocking_spsc_queue.h` | mutex + condition_variable、虚假唤醒 |
| 传输层 | 阻塞回显 | `blocking_echo_server.h` | 一连接一线程、粘包/半包 |
| | epoll 回显 | `epoll_echo_server.h` | epoll LT、非阻塞 fd、EPOLLOUT 按需订阅、优雅关闭 |
| 协议与序列化 | 帧编解码 | `frame_codec.h` | 三态解码（不足/完整/错误）、边界用例 |
| | SOME/IP 编解码 | `someip_codec.h` | 16 字节大端头、Length 从 Request ID 起算（最小 8）、ProtocolError 携带原始头 |
| | SOME/IP 服务端 | `someip_server.h` | Request→Response/Error、版本错回包 protocol_version 重置、超限直接断连 |
| 服务发现 | SD 编解码 | `sd_codec.h` | entry 16 字节布局、`num_options` 高低 4 位、Type 1/Type 2 差异 |
| | 服务发现 | `sd_discovery.h` | 固定 Service ID 0xFFFF + Method 0x8100、多播组 224.244.224.245:30490、真实网卡地址通告 |
| 发布订阅 | 事件订阅 | `sd_discovery.h` | SubscribeEventgroup + IPv4 Endpoint Option、订阅去重、Ack、周期 Notification |
| 可靠性 | 超时重发 | `request_tracker.h`、`echo_client.h/.cpp` | RequestTracker 纯逻辑 + 假时钟可测、QTimer 100ms 扫描、pending≥3 流控跳帧 |
| 功能安全 | E2E 保护 | `e2e_protection.h` | 简化版 Profile 4：10 字节头 = Counter(2) + DataID(4) + CRC(4)，CRC 覆盖 Counter+DataID+数据 |

## 关键常量约定

| 常量 | 值 | 用途 |
|------|-----|------|
| kServiceId | 0x1234 | 回显服务 |
| kMethodId | 0x0001 | Request/Response 方法 |
| kEventMethodId | 0x8001 | 事件 Notification 方法 |
| kEventgroupId | 0x0010 | 事件组（订阅单位） |
| kClientId | 0x0001 | 客户端 ID |
| kE2eDataId | 0x1234ABCD | E2E DataID |
| SD 多播组 | 224.244.224.245:30490 | OfferService 周期通告 |
| SOME/IP TCP 端口 | 9555 | Request/Response 通道 |

## 构建与运行

依赖（WSL2 / Ubuntu，CMake ≥ 3.28）：

```bash
sudo apt install cmake g++ libgtest-dev qt6-base-dev libgl1-mesa-dev
```

一键构建 / 测试：

```bash
bash scripts/build.sh             # 常规构建 → build/
bash scripts/build.sh --tsan      # ThreadSanitizer 构建 → build-tsan/
bash scripts/test.sh              # 跑全部 128 个用例（自动 QT_QPA_PLATFORM=offscreen）
bash scripts/test.sh build-tsan   # 跑 TSan 版用例
```

等价的手动命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

运行（两个终端）：

```bash
./build/plantpulse_server         # 服务端：TCP 9555 + SD 多播通告（无 Qt 依赖）
./build/plantpulse_app            # 上位机：需要 WSLg 或 X11
```

上位机逻辑：先多播发现 300ms → 单播发现 300ms → 都失败回退到输入框手动地址；发现成功后输入框自动同步为真实地址，连接成功自动订阅事件组 0x0010，事件以 `[事件] EVENT-N` 滚动显示。

## 测试（128 用例）

| 测试目标 | 用例数 | 覆盖内容 |
|---------|-------|---------|
| ring_buffer_test | 12 | 环形缓冲读写、满/空、回绕 |
| spsc_queue_test | 8 | 无锁队列入队出队、容量边界 |
| blocking_spsc_queue_test | 9 | 阻塞语义、超时、唤醒 |
| blocking_echo_server_test | 5 | 粘包、半包、并发连接 |
| epoll_echo_server_test | 5 | 事件驱动回显、背靠背报文、关闭时机 |
| frame_codec_test | 10 | 帧三态解码、非法长度 |
| someip_test | 12 | SOME/IP 头、Length 语义、协议错误 |
| someip_server_test | 9 | 正常回显、未知方法、版本错、超限断连 |
| sd_codec_test | 12 | SD entry/option 编解码、Type 1/2、未知选项 |
| sd_discovery_test | 4 | 单播发现请求-应答 |
| sd_multicast_test | 1 | 多播组收发 |
| sd_eventgroup_test | 4 | 订阅 entry 编解码 |
| sd_subscription_test | 3 | 订阅-Ack-事件推送全链路 |
| request_tracker_test | 7 | 超时判定、确认去重、重发次数 |
| e2e_protection_test | 7 | CRC 校验、篡改检测、最小长度 |
| main_window_test | 9 | Qt 界面状态同步、日志显示 |
| echo_client_test | 5 | 客户端收发、超时信号、E2E 失败信号 |
| frame_source_test | 6 | 自动数据源启停、频率 |
| **合计** | **128** | |

CI 中 Qt 用例以 offscreen 平台运行，无需显示服务。

## 手工联调

1. `./build/plantpulse_server` 启动服务端，确认 `ss -tlnp | grep 9555` 有 TCP 监听、`ss -ulnp | grep 30490` 有 SD 多播。
2. 启动上位机，目标填任意错误端口（如 1234）验证：多播/单播发现成功后输入框自动同步为 `真实IP:9555`；发现失败回退输入框，连接失败时状态栏显示 `连接 host:port 失败：...`。
3. 连接成功后观察：不手动发送也有 `[事件] EVENT-N` 持续滚动（订阅生效）；手动发送的请求随会话号递增返回。
4. E2E 验证：用一个篡改回包最后一字节的假服务端（Python 改 `b[-1] ^= 0x01`）替换真服务端，上位机应显示 `E2E 校验失败，会话 N 的回包已丢弃`，且不影响后续会话。

## ThreadSanitizer 回归

```bash
bash scripts/build.sh --tsan
bash scripts/test.sh build-tsan
```

预期 128/128。WSL2 下若 Qt 内部符号误报，可参照常规做法对 Qt 库符号做抑制（`TSAN_OPTIONS=suppressions=...`），项目自身代码不允许出现真实数据竞争。

## CI

`.github/workflows/ci.yml`：push / PR 自动执行 安装依赖 → CMake 配置 → 并行编译 → `ctest --output-on-failure`（Qt 用例 offscreen）。若 Runner 禁止组播，可临时用 `-E sd_multicast_test` 排除多播用例。

## 已知简化与取舍

- **E2E CRC**：使用标准 CRC-32（IEEE 反射多项式 0xEDB88320，校验向量 0xCBF43926）而非 AUTOSAR CRC32P4（0x1F4ACFB13），头布局为简化版 Profile 4；如需对齐规范，仅需替换多项式与初始值。
- **SD 范围**：实现 Find/Offer/Subscribe/Ack 四类 entry 与 IPv4 Endpoint Option，未实现 TTL 续订与 Reboot 语义。
- **服务端回显语义**：服务端对 payload 字节透明回显，E2E 的保护与校验全部在客户端两侧，便于独立验证。
- 项目定位为学习线，接口设计优先可测性（如 RequestTracker 时间由调用方注入），不追求生产级完备性。
