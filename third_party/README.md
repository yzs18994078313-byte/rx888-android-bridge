# 第三方组件

| 组件 | 版本/来源 | 用途与许可证 |
|---|---|---|
| B210 Android Bridge | [1.0.0](https://github.com/yzs18994078313-byte/b210-android-bridge) | App、SpyServer 与 IQ 队列基础；GPL-3.0-or-later |
| ExtIO_sddc | `331b35cd619bf4af20444d64dc35b13a474493e6` | Interface.h、协议与增益映射参考、标准 FX3 镜像；保留上游许可证 |
| libusb | 1.0.27 | USB 主机访问；LGPL-2.1-or-later |
| FFTW | 3.3.10 | 单精度 FFT；GPL-2.0-or-later |
| libc++ | NDK r28c | LLVM Apache 2.0 with LLVM Exceptions 等；见随附 NDK NOTICE |
| Cypress FX3 SDK | 上游固件使用 1.3.4 | 厂商单独许可；见 `Cypress-FX3-LICENSE.txt`，SDK 本体不随本仓库或源码包分发 |

FFTW 源码包 SHA-256：`56c932549852cddcfafdab3820b0200c7742675be92179e59e6215b340e26467`。

标准 `SDDC_FX3.img` SHA-256：`d5cefa943bc425dc0d46e32be666b9a2e966411c94cae7e1738bcb591febc6ce`。

FFTW 的 Android NEON 配置在顶层 CMake 中添加；`patch_fftw.py` 使内核将 NEON 识别为 SIMD，并在 AArch64 上跳过 ARM32 指令探测。源码包已应用补丁，脚本可重复运行。

libusb 源码未修改。FX3 镜像未修改。固件对应的 `SDDC_FX3` 源码保留于 `firmware-source/`，各文件的原始版权及许可声明保留。Cypress SDK 需由使用者从厂商获取，固件构建说明见 `firmware-source/BUILD.md`。固件在 FX3 上独立运行，Android 构建只打包既有镜像，不编译或链接 Cypress SDK。

源码包包含 Android 驱动和 libusb/FFTW 源码；不包含 Cypress SDK、Android/JDK/NDK 工具链、缓存、设备标识、签名私钥或 B210 镜像。
