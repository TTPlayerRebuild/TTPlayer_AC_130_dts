# TTPlayer AC3／DTS 插件重建

恢复原版四工厂的 x86 `ttp_ac3dts.dll`，供原版 5.7.9 与重建版共用：AC3 Decoder、DTS Decoder、AC3 Reader、DTS Reader。原两 Reader 的显示名均为 `DTS Reader`，保留此 ABI 可见行为。

Reader 读取压缩流，宿主选择独立 Decoder；Decoder 输出 float64 PCM，恢复 1–6 声道模式与原版通道顺序。支持 AC3／A52、DTS／DTSWAV、DTS 16-bit BE／LE 和 14-bit packed 表示。标签仍委托宿主 `CreateStdContent`；没有该宿主服务时可播放，标签写入返回不支持。

## 固定依赖

| 组件 | 版本 | SHA-256 |
| --- | --- | --- |
| liba52 / a52dec | 0.8.0 | `03c181ce9c3fe0d2f5130de18dab9bd8bc63c354071515aa56983c74a9cffcc9` |
| libdca | 0.0.7 | `3a0b13815f582c661d2388ffcabc2f1ea82f471783c400f765f2ec6c81065f6a` |

构建时下载，来源及补丁见 `cmake/codec.cmake`；GPL-2.0-or-later、VC-LTL、YY-Thunks 许可证保存在 `licenses/`。

```powershell
./build.ps1 -Package -SourcePackage
```

现代 MSVC、Win32、VC-LTL／YY-Thunks；XP SP3／Win7／新系统共用，要求 SSE2。独立 Action 采用北京时间日期版本，包中仅 DLL 和 SHA256SUMS；私有测试不上传、不在 Action 运行。

## 已验证与差异

恢复了原 DTS 的 **49152 QMF 归一化**，保留 LFE 的 8388608。不能直接使用新版 libdca 默认 `sqrt(2)*32768`，否则主声道会比原版更大声。

双声道、5.1、两档音量、短读和小缓冲均已与原 DLL 对照；修正后 DTS 幅度拟合约为 1，残差约 `1.2e-10`。AC3 有小的逐样本差异，不能称为原二进制 PCM 完全相同。详情、地址和限制见 `docs/RECONSTRUCTION.md`。

## 发布对应源码

发布 Action 同时提供 `ttp_ac3dts-版本-source.zip`，包含适配源码、完整的 a52dec 0.8.0 和 libdca 0.0.7 上游发行源码、许可证与构建脚本。两份 ZIP 的 SHA-256 都写入发行清单。上游源码仍不提交 Git，运行 ZIP 的内容保持最小。

解压源码包后执行上述构建命令；CMake 自动选用包内 `third_party/a52/source`、`third_party/dca/source`，通过 `BUILD_VERSION` 保持对应资源版本。需要 Windows、CMake 3.24+、现代 MSVC Win32 和 Python；固定的 VC-LTL／YY-Thunks 构建依赖仍按 `cmake/legacy_windows.cmake` 下载并校验。内核修改集中在 `cmake/codec.cmake` 与 `src/decoder.cpp`，原始发行源码不被就地修改。

包含这两个 GPL-2.0-or-later 核心的插件整体分发遵循 GPL 条款；`licenses/` 中保留完整许可证。源码包不含测试音频、私有测试和反编译证据。

## 可选格式识别接口

2026-10-20：Reader 工厂增加 Probe，Reader 增加 SourceInfo；旧导出和接口槽位保持不变。声明与辅助实现位于 `include/ttpcomm/audio_format_probe.h` 和 `audio_format_probe_impl.h`，不增加运行时 DLL 依赖。协议原件在相邻 `ttpcomm/sdk/AUDIO_FORMAT_PROBE.md`；新建插件指南在相邻 `rebuild/docs/AUDIO_PLUGIN_DEVELOPMENT_GUIDE.md`。原版播放器继续使用旧接口；错误后缀候选选择和更名提示由支持新协议的重建播放器执行。

[a52dec-0.8.0.tar.gz](https://github.com/user-attachments/files/33714374/a52dec-0.8.0.tar.gz)
