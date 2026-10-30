# FLAC 载体复用与 DTS 定位、时长修正

版本：2026.10.29。FLAC 插件通过只读可定位 IStream 将无损解压后的 DTS 载荷交给本插件已有 DTS Reader；本插件不链接 FLAC，也不新增工厂或 ABI。原版与重建版宿主最终获得正常 PCM24／float64 和完整声道掩码。

## 本项目修改

`src/dts_reader.cpp` 的 Seek 对已经达到 `floor(requested_ms × sample_rate / 1000)` 的位置保留请求毫秒。旧实现再次向下换算毫秒，44.1 kHz 下例如 1 ms →44 samples→0 ms，会导致宿主的精确定位包装器又丢弃 44 个采样。若实际停在 EOF、未达到请求样本，仍返回真实到达位置。

原始 DTS 的码率估计并不总是精确时长。恒定整帧关系成立时记录精确结果；否则继续按包累计音频样本，包含 DTS Core 的 14-bit 部分字和尾部填充。此前只有时长缺失或部分 HD 情形执行这个扫描。扫描只保留有限缓冲，但某些文件打开时需要读取全曲；作为 FLAC 内层流时也会触发相应的无损解压。

AC3 Decoder、DTS Decoder、声道混合、增益和 PCM 数值算法均未修改。

## 验证

配套 FLAC 的 63 组测试在 Win11、XP SP3、Win7 SP1 全部通过，包含 44.1／48 kHz、四种 DTS 装包、PCM 完整比较、精确定位、CUE、尾部填充、未知总样本、取消恢复和旧接口。旧 AC3/DTS 矩阵及 DTS Express 样本的 PCM／定位／取消回归也通过。

测试位于工作区私有 `rebuild/tests/flac_dts_20261029`，不进入 Actions 或源码附件。原版／重建版宿主运行结果及包摘要见工作区 `rebuild/docs/FLAC_DTS_IMPLEMENTATION_20261029.md`。

仍使用固定 FFmpeg 9.0.2、现代 MSVC x86、VC-LTL／YY-Thunks；XP／Win7 导入清单通过，无新增运行库依赖。使用 FLAC 内 DTS 功能时，将本版与 `ttp_flac 2026.10.29` 配套安装到同一 `AddIn` 目录。
