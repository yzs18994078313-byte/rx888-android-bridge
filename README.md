# RX888 MKII Android Bridge 1.0.0

已 root 的 ARM64 Android 手机通过 USB 3 接收 RX888 MKII，以本机 SpyServer 向 OnAir 提供 16-bit IQ。沿用此前 B210 Android Bridge 的前台服务、SpyServer 和有界 IQ 队列。

已在一加 8T / Android 16 / KernelSU root 上验证固件加载、USB 3 识别、真实 IQ、调频、增益控制及停流后再次接收。

用户确认测试通过后发布 1.0.0 正式版。下载安装包见 [GitHub Releases](https://github.com/yzs18994078313-byte/rx888-android-bridge/releases/latest)，版本变化见 [CHANGELOG.md](CHANGELOG.md)。现有 0.1.1 用户可以直接覆盖安装。

手机本机接收测试（VHF ADC 16 MS/s、137.9 MHz 初始频率）：

| IQ 采样率 | 连续接收时间 | USB 错误 | 原始/IQ 队列丢样 |
|---|---:|---:|---:|
| 1 MS/s | 30 秒 | 0 | 0 / 0 |
| 2 MS/s | 20 秒 | 0 | 0 / 0 |
| 4 MS/s | 20 秒 | 0 | 0 / 0 |

调频及增益切换后恢复接收。调频会主动停流并重建 DDC，测试中记录到了调频期间的样本间断，不保证调频无缝。

Android ARM64 离线 DDC 和队列测试已通过，包括 16/32/64 MHz ADC 的输出速率、相位连续性、频谱翻转和带外抑制。详细记录见 [0.1.1 实机记录](docs/android-test-0.1.1.txt) 和 [1.0.0 发布验证](docs/release-1.0.0.md)。HF 路径、其他手机及长时间运行仍待验证，没有单独的 OnAir 解调听感测量记录。

## 驱动适配

- 处理 FX3 从 `04b4:00f3` 变为 `04b4:00f1`、并从 USB 2 总线迁移到 USB 3 配对总线。
- 固件上传后主动重建 libusb 发现上下文，避免 Android 热插拔列表未刷新。
- 枚举期间临时为运行态设备应用 NO_LPM 兼容标志，随后恢复原内核参数；该次设备连接保留对应标志。
- 接收进程持有设备期间保持其 USB runtime PM 唤醒，退出时恢复原值。
- VHF ADC 改为 16 MS/s，IQ 上限 4 MS/s；USB 队列使用 128 × 16 KiB 的普通内存缓冲。实机中 DMA 映射版本曾出现退出码 137，因此当前版本未使用 DMA 映射。
- 提高 USB 事件线程调度优先级，补充传输错误信息，允许失败后的重新连接。

## 使用

1. 从 Release 下载并安装 `RX888-Bridge-1.0.0-arm64.apk`；可用同页 `SHA256SUMS.txt` 核对文件。
2. 使用 USB 3 OTG 连接 RX888，确保供电充足。必要时使用有独立供电的 USB 3 Hub。
3. 打开“RX888 MKII 本机桥接”，默认初始频率 137.9 MHz、增益 30、本机端口 5557。启动并授予 root。
4. 日志显示 `READY` 后，在 OnAir 选择 SpyServer，连接 `127.0.0.1:5557`。从 **1 MS/s** 开始。
5. 可在 OnAir 调频、调增益并选择 **0.25 / 0.5 / 1 / 2 / 4 MS/s**。停止时点击 App 或通知中的“停止”。

“内置测试信号”无需 RX888 和 root，只输出 35 kHz 的复数测试载波，用于核对服务、协议和显示。

首次实机测试建议先连接手机 ADB 安装和检查程序，再启用无线 ADB，将手机 USB 口用于 RX888。App 使用独立包名 `org.sddc.rx888bridge`，默认端口 5557，可与原 B210 App 并存。

## 带宽与采样率

MKII 标配 R828D；上游 SDDC 固件在初始化调谐器时将模拟带宽设为 **8 MHz**。模拟带宽、复数 IQ 输出采样率和原始实数 ADC 采样率是不同指标。

本版 IQ 上限为 **4 MS/s**，默认 **1 MS/s**，滤波保留过渡区。

VHF 模式 ADC 为 **16 MS/s、16-bit 实数**，USB 样本负载约 **32 MB/s**。IQ 输出率与 ADC 输入率分别设置；进一步降低 IQ 输出率不会改变 USB 原始输入率。HF 路径保留 64 MS/s ADC，尚未完成实机验证。

低于 32 MHz 使用 HF 直采口，32 MHz 至 1.75 GHz 使用 VHF 调谐器口。HF 靠近 DC 或 ADC Nyquist 边界时，可用频谱不对称；本版没有提供全 HF 宽带模式，也没有测试 HF 路径。

VHF 使用上游 4.57 MHz 中频约定并做频谱翻转。SpyServer 的增益设置控制 VHF RF 增益，映射到最近硬件档位；IF 暂固定 16.3 dB。HF 映射到 VGA 0–33 dB、衰减器 0 dB。Bias tee 默认关闭。

## 运行与诊断

USB 异步采集 → 128 块原始样本队列 → FFTW 数字下变频、滤波及降采样 → 有界 IQ 队列 → 本机 SpyServer。

USB 完成回调不会等待 DSP 或 TCP。原始队列满时丢弃输入并计数，DSP 检测到间断后清空滤波历史。IQ 队列满时继续采集并累计 IQ 丢样。调频会先停流，再配置调谐器、重建 DDC 并重启。

每 5 秒输出 `STAT:`，包括：

- `usb_errors`：libusb 错误事件；不等于设备内部丢样次数。
- `raw_dropped_samples`：DSP 来不及处理时，原始队列丢弃的实数样本数。
- `queue_dropped_samples`：IQ 输出队列丢弃的复数样本数。
- `usb_bytes`：主机实际接收字节数。
- `queue`、`tcp_waits`：输出队列占用与 TCP 背压。

原始 ADC 流没有逐样本序号，因此不能仅凭 `usb_errors=0` 证明硬件端绝无丢样。实机测试需同时检查速率、队列丢样、频谱和持续运行情况。

## 构建

Windows、Python 3、NDK r28c、CMake 3.31、Ninja、JDK 17、Android platform/build-tools 35。最低 Android 9，目标 API 35；实测平台为 Android 16。

源码仓库与 Release 源码包均包含 libusb、FFTW 和 RX888 固件源码。Android 构建直接打包原固件镜像，无需 Cypress SDK；单独构建固件需自行获取该 SDK，见 [固件构建说明](firmware-source/BUILD.md)。Android 工具链需自行安装；已有 B210 项目的工具目录也可复用。将工具目录的绝对路径设为 `RX888_TOOLS_DIR`：

```powershell
$env:RX888_TOOLS_DIR='已有工具目录的绝对路径'
./build_native.ps1
python build_app.py
```

默认使用 `$RX888_TOOLS_DIR/android-ndk-r28c`；可用 `ANDROID_NDK_HOME` 指定 NDK。脚本要求 `tools/py/cmake/data/bin/cmake.exe`、`tools/py/bin/ninja.exe`（或对应 wheel 目录）、`jdk-17.0.17+10`、`android-35/android.jar` 及包含 `aapt2.exe` 的 build-tools 子目录。

从零准备时，将工具整理为下面的目录结构。CMake 和 Ninja 可用 `python -m pip install --target "$env:RX888_TOOLS_DIR/py" "cmake>=3.31,<3.32" ninja` 安装；其余从 Android SDK/NDK 和 JDK 安装包提取。`android-15` 是本项目沿用的目录名，里面放置 **build-tools 35** 文件，不表示 Android 15 运行要求。

```text
工具目录/
  android-ndk-r28c/
  jdk-17.0.17+10/bin/java.exe
  android-35/android.jar
  android-15/aapt2.exe
  android-15/zipalign.exe
  android-15/core-lambda-stubs.jar
  android-15/lib/d8.jar
  android-15/lib/apksigner.jar
  py/cmake/data/bin/cmake.exe
  py/ninja/data/bin/ninja.exe
```

APK 生成到 `dist/`，可用 `RX888_DIST_DIR` 更改。源码包不包含签名私钥；首次构建会生成自己的开发签名，不能覆盖以其他密钥签名的 APK。

`build_native.ps1` 同时构建 `ddc_tests` 和 `queue_tests`，运行时需使用 ARM64 Android 环境。固定频率接收的实机测试记录及其限制已保留在 `docs/`。

`librx888_bridge.so` 是用 `.so` 文件名打包的 PIE 可执行程序，由前台服务通过 `su -c` 启动，并非 JNI 插件。FFTW 静态链接；libusb 和 libc++ 随 APK 提供。固件只加载到 FX3 RAM。

## 上游和许可证

本项目按 GPL-3.0-or-later 分发。第三方组件保留原许可证，详见 `third_party/README.md`。

- [B210 Android Bridge](https://github.com/yzs18994078313-byte/b210-android-bridge)：复用桥接与 App 基础。
- [ExtIO_sddc](https://github.com/ik1xpv/ExtIO_sddc/tree/331b35cd619bf4af20444d64dc35b13a474493e6)：RX888 协议、增益表和 FX3 固件。
- [libusb 1.0.27](https://github.com/libusb/libusb/tree/v1.0.27)。
- [FFTW 3.3.10](https://www.fftw.org/download.html)。
