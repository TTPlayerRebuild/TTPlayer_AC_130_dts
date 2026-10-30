# TTPlayer AC3／DTS 插件

2026.10.29：[FLAC 载体复用、44.1 kHz 精确定位和填充时长修正](docs/FLAC_CARRIER_SEEK_DURATION_20261029.md)。

最新补全与验收：[2026-10-24](docs/COMPLETION_20261024.md)。

DTS Express 真实样本与时长修正：[2026-10-24](docs/DTS_EXPRESS_VALIDATION_20261024.md)。

面向原版 5.7.9 和重建播放器的 x86 `ttp_ac3dts.dll`。现代 DTS 功能已统一到此 DLL，独立 `ttp_dts` 项目停止分发并在本地备份后移除。2026.10.23p1 补齐 E-AC3、DTS Core 快速准确定位和 WAVE 标签／封面写入，保留原四个工厂编号、GUID、导出和 AC3 功能。

## 接口与播放

| 工厂编号 | 功能 | 实现 |
| --- | --- | --- |
| 0 | AC3 Decoder | liba52；保持原浮点 PCM 路径 |
| 1 | DTS Decoder | FFmpeg DCA；旧压缩流接口输出 float64 DTS Core |
| 2 | AC3 Reader | 保留压缩读取、同步扫描、宿主标签服务及定位行为 |
| 3 | DTS Reader | FFmpeg 解封装和解码；输出 float64／PCM24、WAVEFORMATEXTENSIBLE 和声道掩码 |
| 4 | Dolby Digital Plus Reader | E-AC3，支持 .eac3／.ec3／.ac3，直接输出 PCM |

历史显示名称保留，包括两个 Reader 都叫 `DTS Reader`、编号 1 名称中的 `libdts`；实际 DTS 后端已统一为 FFmpeg。

DTS Reader 支持原始 DTS、DTS-WAV、16／14 位大小端表示以及较大 RIFF 前置块。增加 `.dtshd`、`.wav`、`.wave` 候选扩展名，并通过可选 Probe／SourceInfo 接口避免把普通 PCM WAV 误认成 DTS。`.dts`、`.dtswav` 保留。

原始 DTS 标签委托宿主 `CreateStdContent`；原版公共服务不支持写入封面，重建版服务支持。WAVE 使用独立的 RIFF／RF64 原生编辑器，支持 Unicode 标签、多封面、显式 Commit 和失败恢复。旧宿主在最后释放时保存；重建宿主可获得真实提交结果。

## 固定依赖

| 组件 | 版本 | SHA-256 |
| --- | --- | --- |
| liba52 / a52dec | 0.8.0 | `03c181ce9c3fe0d2f5130de18dab9bd8bc63c354071515aa56983c74a9cffcc9` |
| FFmpeg | 9.0.2 | `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e` |
| GNU make（构建工具） | 4.4.1 | `dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3` |

构建时下载固定版本并校验。FFmpeg 仅启用所需 DCA／E-AC3 解码、DTS／AC3／E-AC3／WAV／SPDIF 解封装、DCA／AC3 parser 和 swresample；静态链接，不要求外置 FFmpeg DLL。版权和许可证保存在 `licenses/`，不作为资源嵌入 DLL。旧 libdca 的许可证作为历史记录保留，它已不参与构建。

```powershell
./build.ps1 -Package -SourcePackage
```

需要 Windows、现代 MSVC Win32、CMake 3.24+、Python 3.12+ 和 Git for Windows Bash。VC-LTL／YY-Thunks 用于同一个 XP SP3／Win7／新系统 DLL，CPU 要求 SSE2；不要求用户另装 VC 运行库。构建依赖和导入审计见 `cmake/`。

Action 使用北京时间日期版本，运行 ZIP 仅包含 `AddIn/ttp_ac3dts.dll` 和 `SHA256SUMS.txt`。发布另附完整对应源码 ZIP，包含 a52dec、FFmpeg、GNU make 源码、适配层和构建脚本。解压源码包后可直接运行同一构建命令，自动选用包内源码和 `BUILD_VERSION`；VC-LTL／YY-Thunks 仍按固定哈希取得。

liba52 为 GPL-2.0-or-later，当前插件整体分发须遵循其条款；FFmpeg 精简配置为 LGPL-2.1-or-later。GNU make 仅作为构建工具。私有测试在相邻 `rebuild/tests`，不上传、不打入运行包、不在 Action 中运行。

## 验证及边界

本轮已在 Windows 11、XP SP3、Win7 SP1 验证实际宿主加载／短时播放与关闭，并检查独立解码、短读、容量边界、定位、声道和宿主标签生命周期。AC3 与整合前重建 DLL 逐字节一致；DTS 两个适配入口的核心 PCM 逐字节一致。

DTS 保留原核心主声道音量系数，LFE 不套用该系数。但 FFmpeg 与 libdca 的滤波实现不同，**不声明与原 DLL 逐样本一致**。DTS-HD 的完整解码走文件 Reader；旧 Decoder 保持预先协商的 Core 采样率和声道，不能在播放途中改成 HD 格式。真实 MA／HRA 的有效片段以及 Express stereo／5.1 已验证；损坏尾部、全部扩展选项和原版宿主的 5／7 声道输出限制仍单列，详见上方补全报告。

原版 EXE 自带 WAVE Reader 可能抢先接受 PCM 头的 DTS-WAV；此宿主限制不能靠新增扩展名彻底修复。在原版中使用 `.dtswav` 可进入插件，重建版已有按内容选择 DTS 的路径。独立 DTS Core 帧使用稀疏索引与预卷定位，不能证明状态独立的编码仍保留从头精确解码。

详见 [边界 3～5 补全与迁移](docs/BOUNDARIES_3_5_20261023.md)、[先前整合记录](docs/DTS_INTEGRATION_20261023.md) 和 [原实现恢复证据](docs/RECONSTRUCTION.md)。Windows 10 未独立实测；按用户要求使用 Windows 11 代验。
