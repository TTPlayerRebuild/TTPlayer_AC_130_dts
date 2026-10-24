# DTS Express 真实样本验证与时长修正

日期：2026-10-24。本记录补充 `COMPLETION_20261024.md`，该早期记录中“尚无真实样本”的结论不再适用于下面明确列出的样本。

## 修正

原始 DTS Express 文件可以具有非零但不准确的时长。FFmpeg 在没有容器时间戳时会按码率估算；旧实现仅对零时长启用包扫描，因此测试文件实际输出 45,056 帧／48 kHz（938.666… ms），却报告 949 ms。

现在本地、可定位的 raw DTS，如果是非普通 Core profile 且 `duration_estimation_method == AVFMT_DURATION_FROM_BITRATE`，也使用既有的包时长累加。修正后报告 938 ms（旧接口的整数毫秒取整），PCM 输出及其哈希保持不变。准确时长仍由音频包给出，不由容器填充字节计算。

扫描仍限制四百万包，不作用于不可定位或远程流；普通 Core 已有的固定帧大小时长计算不变。没有修改 WAVEFORMATEXTENSIBLE、精度、声道、原版宿主格式协商或输出方式。

## 真实样本与来源

1. [AndroidX Media 测试音频](https://github.com/androidx/media/blob/8c6678b657ede1e7883fc164ef73ed483c7796c3/libraries/test_data/src/test/assets/media/mka/sample_with_dts_express.mka)。固定提交 `8c6678b657ede1e7883fc164ef73ed483c7796c3`；MKA 45,646 字节，SHA-256 `a1ae2b5bbe048ce4bf8efc3bf6ee59c5a7af49962ff5c2116129566fb43b8ce8`。使用 stream copy 提取 DTS，不重新编码。同仓库 MP4、fragmented MP4、TS 也被 ffprobe 确认为 DTS Express；本插件验收使用提取的裸流，不将容器识别等同于容器直接播放支持。
2. [dcadec 官方仓库 issue #33 中上传者提供的样本](https://github.com/foo86/dcadec/issues/33#issuecomment-111174107)。ZIP 内的 `6-channels/5point1.LBR`，329,152 字节，SHA-256 `e053a1820f8418730fdfd82cafe6afcddbbf011b102bcdf009217da8e38b3632`。
3. 同一上传的 `2-channels/feel-it.LBR`，4,405,408 字节，SHA-256 `36bf7cb46747ab1245e1d8ee0db9b7827d635b09aadef7d2cadc1c67090cbadb`。完整文件用于整曲解码，前 64 个完整包另用 stream copy 提取，供定位及取消验证。

后两个有声样本与 ZIP 中的参考 WAV 通过 HTTP Range 单独提取，共传输 8,097,749 字节，不需要下载整个约 27 MB ZIP。旧 LAV 维护者样本链接和另一个 mono 样本链接已返回 404，未将其记作验证成功。

## 当前解码后端

固定 FFmpeg 9.0.2 构建的 `CONFIG_DCA_DECODER`、`CONFIG_DCA_PARSER` 均为 1。静态 `avcodec.lib` 实际包含 `ff_dca_lbr_parse`、`ff_dca_lbr_filter_frame`、init/flush/close；`dcadec.c` 根据 LBR extension mask 调用该分支。真实样本的 ffprobe profile 为 `DTS Express`，因此本次没有把普通 DTS Core 或 HD MA 的成功代替 LBR 验证。

## 本机结果

以下均为 Windows 11、Win32 Release DLL，输出 float64：

| 输入 | 采样率／声道 | PCM 帧数 | 正确整数时长 |
|---|---|---:|---:|
| AndroidX 提取裸流 | 48 kHz／5.1(side) | 45,056 | 938 ms |
| 完整 5point1.LBR | 48 kHz／5.1(side) | 282,624 | 5,888 ms |
| stereo 前 64 包 | 48 kHz／立体声 | 262,144 | 5,461 ms |
| 完整 feel-it.LBR | 48 kHz／立体声 | 8,810,496 | 183,552 ms |

- 三个较短输入各完成 8 个精确定位：0、1 ms、150 ms、中点、末尾前、末尾、超出末尾、再次回到 0。整个后续 PCM 与顺序解码切片逐字节一致。
- 真实 5.1、stereo 64 包、AndroidX 裸流重复 16 次组成的故障测试，各执行 8 组取消恢复。包括失败前先消耗 13 字节和恢复时再次取消；PCM 与无故障基线完全相同。重复片段只是为了可靠触发 I/O，不声称是另一个自然音频样本。
- 与本机 x64 FFmpeg 的 float64 参考比较，样本数相同。最大绝对差分别约 `4.26e-14`、`1.79e-7`、`5.96e-8`；不声称跨编译器浮点输出逐位相同。
- 原有 MA 96 kHz／5 声道、HRA 48 kHz／7 声道及 120 秒普通 Core 的精确时长／定位回归通过。
- XP／Win7 静态导入检查通过：5 个系统 DLL、155 个导入。最终 `2026.10.24p1` DLL 的 9 项顶层矩阵已在 Windows 11、XP SP3 和 Win7 SP1 全部通过；原版及重建版在三系统实际播放 Express 5.1 也通过。原版 Win7／Win11 的 5／7 声道输出限制另见主报告，不能推广此处的 6 声道结果。

## 私有验证材料与边界

工作区 `rebuild/tests/plugins_followup_20261024/dts-express` 保存下载来源、原始文件摘要、`results.json`、参考 PCM 差异、旧版错误复现及回归日志；`vmstage/run.cmd` 包含 9 个顶层用例，`vm-manifest.json` 是传送清单。测试不进入 Action、插件仓库或发行包。

本次确认 48 kHz stereo／5.1 的 Express 路径。mono、其他采样率、全部 LBR 编码选项、ECS/downmix 组合及原版播放器全部 GUI 路径仍未据此穷尽。此功能输出解码后的 PCM，不是 DTS 位流直通。
