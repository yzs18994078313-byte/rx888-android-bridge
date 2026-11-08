# FX3 固件源码说明

本目录中的 `SDDC_FX3/`、`Interface.h`、`HWSDRtable.h` 和上游 README 来自 ExtIO_sddc 提交 `331b35cd619bf4af20444d64dc35b13a474493e6`，保留原文件版权和许可证。固件中的调谐器代码另有 GPL 声明，不能用顶层 MIT 许可替代各文件原有条款。

Android APK 使用 `third_party/SDDC_FX3.img` 的原始镜像，SHA-256 为 `d5cefa943bc425dc0d46e32be666b9a2e966411c94cae7e1738bcb591febc6ce`。构建 Android 桥接程序无需重新构建固件。

本仓库及 Release 源码包不分发 Cypress SDK。若需单独构建固件，请按本目录上游 [README 的固件构建章节](README.md#build-instructions-for-firmware)，从厂商获取 EZ-USB FX3 SDK，并遵守其许可；许可文本也保留于 `../third_party/Cypress-FX3-LICENSE.txt`。

上游原工程使用 SDK 1.3.4。安装其配套 ARM GCC 后，按实际位置设置环境变量，再执行：

```sh
export FX3FWROOT=/path/to/installed/fx3-sdk
export ARMGCC_INSTALL_PATH=/path/to/arm-gcc
export ARMGCC_VERSION=4.8.1
cd SDDC_FX3
make
```

`ARMGCC_VERSION` 应与实际安装版本一致。未验证不同 SDK/编译器生成的固件与随附原镜像逐字节一致。
