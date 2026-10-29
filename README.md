# HT-XG170GC-T SDK 与取帧修复

适用设备：**HT-XG170GC-T**（SDK 产品型号 `MV-XG170GC`）。包含厂商 **MVSDK 2.1.0.49 Linux x86_64** 分发文件，以及不依赖 ROS 的 C++17 采集接口。

**修复需要同时覆盖取帧代码和电脑接收配置，单改 FIFO 不够。** 厂商动态库和头文件保持原样，修复放在新增调用代码中；新项目应使用 `xg170gc_capture`，厂商旧示例未自动获得这些修复。

## 1. 改了什么

图像依次经过以下环节，各层缓存互不替代：

```text
相机 → 网卡 RX ring → Linux UDP 接收缓存 → SDK 帧缓存 → FIFO 取帧 → 应用队列 → 算法/写盘
```

| 环节 | 本版本的处理 | 谁来设置 |
|---|---|---|
| 网卡接收描述符 RX ring | 使用网卡支持的最大值；本机从 512 调到 8192 | 使用者在采集电脑执行配置脚本 |
| Linux 接收缓存上限 `rmem_max` | 至少 128 MiB，允许程序申请较大的 UDP 缓存 | 同上，需管理员权限 |
| SDK 数据 socket 的实际接收缓存 | 请求 64 MiB；本机 Linux 读回 128 MiB，含加倍记账；启动检查是否生效 | 采集代码自动设置 |
| SDK 图像帧缓存 | `CameraInit` 前设置 `NumBuffers=64` | 采集代码自动设置 |
| SDK 取帧策略 | 显式使用 `OLDEST`，按 FIFO 读取，避免跳过积压旧帧 | 采集代码已修改 |
| 应用层缓存 | 独立采集线程、64 帧消息池、32 帧消费队列 | 采集代码已配置 |

**允许申请大缓存，不等于 socket 实际用了大缓存。** 本版本同时检查主机限制及实际 socket 缓冲。RX ring 的单位是描述符数量，SDK 缓存的单位是帧，不能与 MiB 混为一谈。

另外已移植：

- 缓存和消费端就绪后才 `CameraPlay`；原图只复制一次，立即释放 SDK 缓存，算法和写盘不占用 SDK 缓存。
- 原图保留 8-bit Bayer/Mono，不在取帧线程逐帧转彩色或压缩。
- 保存设备帧号、设备/主机时间和异常计数；识别 65535→1 正常回绕。
- 队列满或帧池耗尽时明确停止并报错，避免静默丢帧。

MTU 推荐 9000，须整条采集链路支持。本机原来已是 9000；它与 SDK 数据包长度不是同一个参数，代码保留 SDK 包长自动协商。

## 2. 源码位置

| 路径 | 内容 |
|---|---|
| `vendor/mvsdk/` | 厂商头文件、运行库、文档、原始示例和配置工具 |
| `include/xg170gc/camera.hpp` | 对外接口和参数 |
| `src/camera.cpp` | 初始化、FIFO 取帧、立即释放、应用缓存和帧号检查 |
| `src/transport_diagnostics.hpp` | 当前进程数据 socket 缓冲设置、主机 UDP 计数及回绕识别 |
| `apps/capture.cpp` | 独立原图录制入口，停止后排空队列 |
| `scripts/configure_transport.py` | 采集电脑的网络检查、配置及恢复 |
| `vendor_manifest.json` | 厂商文件 SHA-256 及排除文件清单 |

厂商文件保留原字节，排除了本地编译中间文件和运行产生的数据。不包含 ROS 封装、动捕/球轨迹业务代码、本地测试、日志或现场标定结果。

## 3. 编译

需要 Linux x86_64、CMake 3.10+、支持 C++17 的编译器（已验证 GCC 9.4）、pthread。核心采集不依赖 ROS、Qt 或 OpenCV。

```bash
git clone https://github.com/7bread/XG170GC-SDK.git
cd XG170GC-SDK
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/xg170gc-capture --help
```

直接链接仓库内 SDK，无需运行厂商 `install.sh`，也不将库复制到系统目录。厂商工具和示例有各自的依赖，核心程序不要求安装这些依赖。

## 4. 配置采集电脑

相机与采集网卡须处于可通信的子网，连接应满足带宽要求。先停止相机程序，用 `ip -brief address` 确认**相机专用网卡**，将下列 `CAMERA_NIC` 替换为真实接口名：

```bash
ip -brief address
python3 scripts/configure_transport.py status --interface CAMERA_NIC
sudo python3 scripts/configure_transport.py apply --interface CAMERA_NIC --state /tmp/camera-network-before.json
```

脚本在**宿主机**执行，备份原值后设置 MTU 9000、RX ring 最大值和至少 128 MiB 的 `rmem_max`；拒绝修改承载默认路由的网卡，不改变 IP、路由或代理。RX ring 上限按实际网卡读取，不固定写 8192。链路恢复后再启动采集。

这是运行时配置，**重启电脑后可能需要重新应用**。再次应用时使用新的备份文件名；已有备份不会覆盖。需要长期保留时，应按电脑的网络管理方式配置持久设置。容器内的采集程序不能替代宿主机网卡配置。

恢复备份值（先停止采集）：

```bash
sudo python3 scripts/configure_transport.py restore --interface CAMERA_NIC --state /tmp/camera-network-before.json
```

## 5. 检查取帧与录制

确保没有其他程序占用相机，先做 30 秒纯取帧检查，不显示、不录像：

```bash
./build/xg170gc-capture --discard --seconds 30
```

再录制原图，输出目录必须不存在：

```bash
./build/xg170gc-capture --output ./capture_001 --seconds 12
```

默认原生全幅、RAW8、曝光 1000 μs、增益 144，可用 `--exposure-us`、`--gain` 调整。输出：

- `frames.raw`：连续的未压缩原图。
- `frames.csv`：逐帧尺寸、编码、设备帧号、时间、曝光及文件偏移。
- `summary.json`：取帧、交付、保存数量和异常计数。

检查 `frame_id_gaps`、`queue_overruns`、`pool_exhaustions` 和 `error`；正常录制还应核对 `captured`、`enqueued`、`delivered`、`saved_frames` 一致。`observed_sequence_complete` 仅代表本次收到数据区间的序列检查，不代表永久零丢帧。

退出码：`0` 序列检查通过；`1` 配置/采集/写盘错误；`3` 有缺口或无法完整核对；`130` 用户中断。主机 UDP 错误是全机累计计数，不能直接当作该相机的掉帧数。

1600×1100、约 662 fps 的 RAW8 像素量约 **1.17 GB/s**，12 秒约 **14 GB**；持续写盘速度不足不能靠增大缓存解决。先确认纯取帧完整，再增加项目算法或录像负载，比较各层计数定位问题。

## 6. 接入其他项目

```cmake
add_subdirectory(path/to/XG170GC-SDK)
target_link_libraries(your_program PRIVATE xg170gc_capture)
```

包含 `<xg170gc/camera.hpp>`，按 `Camera → start() → read() → stop()` 使用；完整录制时，停止后继续 `read(frame, 0)` 排空队列并检查 `statistics()`。完整调用和保存流程见 `apps/capture.cpp`。

及时释放已处理的 `FramePtr`。每个进程仅支持一个活动相机实例，不能与其他程序同时打开同一相机。时间戳并未自动与动捕等其他设备同步。

## 7. 验证结果与边界

2026-10-29 独立版本实机纯取帧：1600×1100 Bayer8、约 **662.14 fps**，30 秒得到 **19,832 帧**，帧号缺口、队列溢出、取帧失败和 SDK 重传均为 0。验证未包含本次独立程序的长时间真实写盘；详情见 [VALIDATION.md](VALIDATION.md)。

本仓库修复已定位原因，但电脑、网络、SDK 和项目负载都可能影响完整性，之后掉帧仍需按计数定位，不能预先排除驱动或传输层。厂商文件及第三方依赖沿用原有授权，本仓库未新增整体开源许可。
