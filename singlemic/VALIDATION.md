# GTCRN 集成验证记录

日期：2026-11-19。代码、模型导出与主机回归已完成；未执行本次固件的烧录或板端运行。

## 交付内容

- 生产模型：`models/gtcrn_stream.espdl`，429168 字节。
- 模型 SHA256：`ce6ac02f6f8e21c18894394d13e9fdcc31e0b053f48bf3a6763e8f23015a1b4c`。
- 原始权重 SHA256：`0951ed2588c657f558de25ab05b2e5ceeef86710499d5d0703f96242b24a7bae`。
- 契约：`models/gtcrn_stream.json`；独立板端参考向量：`models/golden_stream.json`。
- 可复现导出：`D:/HomeGuard/simulator/gtcrn/export/export_streaming.py`，依赖锁文件位于同目录。
- 固件：`build/singlemic_gtcrn.bin`，启动信息包含 `singlemic-gtcrn-stream-v1`。
- 本次生产固件大小：1681744 字节；SHA256：`a1cbb1f076b7bb78cc715a3ee36647c69675a0f1195d6e1c5a645e593f2ecb13`。
- 最终构建配置：`SINGLEMIC_GTCRN_SELF_TEST=OFF`，应用分区余量约 60%。

应用、模型和契约在构建与上电时校验 SHA256，模型以独立分区加入完整烧录命令。
当前工程要求 8 MB Flash 与可用 PSRAM；需核对实物配置。

## 已执行

| 检查 | 结果 |
| --- | --- |
| ESP-IDF 6.1 / Arduino 3.3.12 / 本地 ESP-DL 3.3.15 生产构建 | 通过 |
| 可选四帧连续参考向量自检构建 | 通过编译，未在板上执行 |
| 主机回归 | 39 项通过，包括 38 项音频/契约/流水线/分包测试及 1 项真实模型连续测试 |
| 非流式核心与 FP32 流式核心最大绝对误差 | `5.960464477539063e-08` |
| 导出 ONNX 与 FP32 流式核心最大绝对误差 | `1.430511474609375e-06` |
| 长历史连续验证 | 48 帧，FP32 与 ONNX 连续输出符合容差，输入缓存未被原地修改，会话重置可复现 |
| 量化校准 | 128 帧，取自 13 个分散片段，包含非零历史状态 |
| 量化主机参考 | 130 帧，相对 FP32 频谱 SNR `18.3670 dB`，最大绝对频谱误差 `0.619405` |
| 首个低能量片段的量化参考 | 相对 FP32 频谱 SNR `17.7316 dB` |

量化参考的 SNR 衡量与 FP32 输出的接近程度，不是降噪质量分数，不能据此宣称音质验收通过。
量化仍有可见误差，需结合板端参考向量与真实录音听测评估。复数模长运算保留 FP32，网络主体为 w16a16。
训练时的整段能量归一化/响度匹配在固件中改为因果平滑估计，属于明确的在线处理差异。

音频测试覆盖任意尾长、静音、脉冲、正负满幅、归一化撤销、峰值保护和 PCM24 到 PCM32 接收转换。
响度补偿单独覆盖 0–24 dB 上下限和语音停止后对模型残余噪声的静音保护。
生产流水线配合带时序的主机队列/线程，覆盖正常、缺失模型、预检失败、慢推理、断连重启、池耗尽、
立即停止及旧会话迟到错误；检查输出采样位置连续及原音降级后下一次重试。
分包测试验证 256 点到 76 点的跨块拼接、降级时未提交尾包恢复原音、尾包只发送一次以及会话重置。
主机测试使用替代 FFT、采集、推理和 FreeRTOS 后端，不代表实际硬件驱动或调度已通过。

验证命令：

```powershell
cd D:\HomeGuard\simulator
.\.venv-gtcrn\Scripts\python.exe -m pytest D:\HomeGuard\microphone\singlemic\tests gtcrn\export\test_streaming_model.py -q
```

主机测试日志：`D:/HomeGuard/simulator/output/singlemic_final_tests.log`。
生产构建日志：`D:/HomeGuard/simulator/output/singlemic_build.log`；
可选自检构建日志：`D:/HomeGuard/simulator/output/singlemic_selftest_build.log`。
导出数值记录：`D:/HomeGuard/simulator/gtcrn/export/streaming/validation.json`。

## 2026-11-23 SPP 丢包定位

检查当前 `build/compile_commands.json` 的 1686 条编译命令：没有 `-Og`。
应用、SPP/BTC 和 RFCOMM 相关源码使用 `-Os`；部分 ESP-DL/DSP 源码追加
`-O3` 或 `-O2`，因此不能将所有组件都描述为最终使用 `-Os`。

用户提供的板端统计为 `queue_full=774 dropped=774 high=62/64`，
`write_fail=0 event_full=0 disconnects=0 congestion=144`，最大提交延迟约 1.07 秒。
队列保留两格给控制包，音频达到 62 格即拒收；音频序号在入队前递增，
这些拒收会形成接收端序号缺口。当前直接丢包点是设备端发送队列满；
底层 SPP 吞吐下降的原因仍需在录音期间结合拥塞、调度及链路对比验证。
`GTCRN mode=RAW reason=2` 表示实时预检未通过后使用原音，不能据此归因于正在执行模型推理。

同段 PC 日志为 687 个收到的音频包、775 个缺包；旧接收器还会拒收不足 76 点的
合法尾包，并在 END 时多计一个缺包，与设备端的 774 个拒收相符。
已修复 Python 接收器以保存短尾包，缺包补静音继续按常规 76 点估计。
主机回归 12 项通过，覆盖短尾包、尾包前缺包、仅收到尾包和完整包序号缺口；
修复前其中 10 项失败。此修改不增加 SPP 吞吐，也不构成连续无丢包验收通过。

随后将本分支的 SPP 提交状态机与 `master` 的四包聚合逻辑对齐：最多 976 字节，
单包立即提交，START/END 不聚合，明确零字节失败重试，成功部分写入续发后缀。
保留 IDF 6 命名 API 和逐 DATA 包的提交延迟统计，重试与续发不重复采样。
ESP-IDF 6.1 固件构建通过，应用大小 `0x18fe50`（1637968 字节）。
新增 11 项 SPP 主机测试通过，覆盖聚合上限、单包、控制顺序、重试、部分写入、
拥塞、超时、断连重连、会话边界、短尾包及真实 Python 接收器对聚合字节流的解析；
原有固件和接收器测试另有 50 项通过。SDK 由主机替代后端模拟，不能代表真实空口吞吐。
本次提交逻辑调整尚未重新烧录，持续录音无丢包验收待执行。

## 待执行的板端验收

| 验收项 | 状态 |
| --- | --- |
| 实物 Flash/PSRAM 容量与启动内部 RAM ≥48 KiB | 待测 |
| ESP-DL 连续四帧参考向量误差 ≤2 LSB | 待测 |
| 真实麦克风 → GTCRN → SPP → PCM32 WAV 音质和采样连续性 | 待测 |
| 蓝牙启用时，32 帧预热 +128 帧预检，平均 ≤12.8 ms、P95 ≤16 ms | 待测 |
| GTCRN 模式连续 SPP 传输至少 30 分钟 | 待测 |
| 采集至 SPP 提交 P95 ≤200 ms，无持续积压、WDT 或内存增长 | 待测 |
| 真实按键快速启停、断连重连、模型缺失与慢推理故障注入 | 待测 |

上板步骤见 `INTEGRATION.md`。串口被现有 monitor 占用时，应先关闭该 monitor，再完整烧录。
观察 `GTCRN golden ... result=PASS`、`GTCRN preflight ... realtime=PASS`、
录音期间 `mode=ENHANCED` 和每秒内存/栈/延迟统计；另保存 PC 接收的 PCM32 WAV 检查实际声音。
提交延迟统计不包含空中传输和 PC 缓冲，DMA 时间戳约有一个 hop 的估计误差。

必须分别报告“模型链路正确运行”和“当前硬件持续实时通过”。当前两项均尚无本次固件的板端证据；
已有约 752 ms/10 帧的历史结果提示性能风险，但不能当作本次流式实现的实测结果。
`FAIL_RAW` 或运行中原音降级是保护结果，不构成 GTCRN 实时验收通过。
