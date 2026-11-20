# singlemic + streaming GTCRN

本固件面向经典 ESP32，当前按 8 MB Flash、2 MB PSRAM 的本地配置构建，使用
ESP-IDF 6.1、Arduino ESP32 3.3.12 和本机修改过的 ESP-DL 3.3.15。
Arduino 的 setup/loop、按键、配对、Classic SPP 包头/CRC 和电脑接收器继续复用。
音频池和模型工作区依赖 PSRAM；烧录前应核对实物容量，不能仅凭 ESP32-WROOM-32E 名称判断。

## 编译与烧录

从本目录运行：

```powershell
.\tools\build.ps1
.\tools\build.ps1 -Action 'flash monitor' -Port COM9
```

`COM9` 必须替换为实际 USB-UART 端口，烧录前关闭占用该端口的旧 monitor。
首次完整烧录包含 bootloader、分区表、应用和 `model` 模型分区；模型变化后仍需
完整 `flash`。仅修改应用时可使用 `-Action 'app-flash monitor'`。
默认工程不提供 OTA 分区；本次分区表占用 8 MB Flash 中的前约 6 MB。

脚本使用本机 `D:\esp\v6.1\esp-idf` 和 `D:\Espressif`，可通过 `-IdfPath`、
`-ToolsPath` 覆盖；Arduino 路径可通过 CMake 的 `ARDUINO_COMPONENT_DIR` 覆盖。
构建使用两个并行任务，避免 Windows 上 GCC 的大量并发编译耗尽提交内存。
Arduino 仅编译 core APIs；未启动 Wi-Fi、NimBLE 或 BluetoothSerial。
Arduino 的公开依赖仍可能被构建，未引用的代码由链接器删除。

构建读取 `models/gtcrn_stream.json`，校验模型 SHA256、大小、采样率、输入输出形状
和缓存端口，再生成 C++ 契约。上电再次校验 Flash 分区中的实际模型 SHA256；
模型缺失、过旧或无法构建时只启用原音路径，不将其报告为降噪成功。

## 流式模型导出

在 `D:\HomeGuard\simulator` 使用独立 Python 3.11 环境：

```powershell
python -m venv .venv-gtcrn
.\.venv-gtcrn\Scripts\python.exe -m pip install -r gtcrn\export\requirements-streaming.lock.txt --extra-index-url https://download.pytorch.org/whl/cpu
.\.venv-gtcrn\Scripts\python.exe -m pip install -e .\esp-ppq --no-deps
$env:PYTHONHASHSEED='0'
.\.venv-gtcrn\Scripts\python.exe gtcrn\export\export_streaming.py
```

默认读取当前 `gtcrn/checkpoints/best_model.tar` 和 `output/calib_data.npy`。
导出前先比较非流式核心、逐帧 FP32 核心和连续 ONNX 输出；使用 128 个包含非零
历史状态的校准帧。输出复制至本工程的 `models`，不再手工同步旧的整块模型。
`--checkpoint`、`--calibration`、`--output`、`--firmware` 可指定对应路径。

网络主体为 `target=c / w16a16`。复数幅度中的平方、相加和平方根保留 FP32，
防止平方值跨动态范围量化抹掉安静频点。`--float-layernorm` 仅供诊断比较，
默认不开启。生产模型不携带测试激活，四帧连续参考向量另存 `golden_stream.json`。
`validation.json` 记录主机误差；硬件未测试时 `board_validation=not_run`。

14 个独立缓存端口保存六个卷积历史、六个 TRA 状态和两个 DPGRNN 状态。
输入缓存与输出缓存的形状和指数写入清单。固件先快照全部输出缓存，再更新所有
输入缓存，避免模型工作区复用导致互相覆盖；不在每帧重置模型。

## 音频格式与延迟

```text
I2S 16 kHz / 32-bit LEFT slot
  -> signed PCM24 in int32_t / DC blocker
  -> float32 STFT (512 / hop 256 / sqrt periodic Hann)
  -> streaming GTCRN / causal level compensation
  -> float32 ISTFT / overlap normalization / peak gain protection
  -> signed PCM24 in int32_t
  -> existing three-byte PCM24 + CRC / Classic SPP
  -> existing receiver / samples << 8 / PCM32 WAV
```

音频缓冲和发送接口始终使用 `int32_t`，没有将音频截成 PCM16。INT16 仅用于
网络量化张量；这些张量有各自量化指数，不能作为 16 位音频播放。
接收脚本仍为 `../py_rec/spp_audio_receiver.py`，协议不变。

采集任务独占新 I2S API，CPU1 优先级 3，每次 256 点。推理任务 CPU1 优先级 1；
Arduino 主循环控制会话、累计 76 点发包；SPP owner 保留 CPU0。
八块原音/增强双缓冲载荷在 PSRAM（16 KiB），元数据、原子变量、栈和 FFT 工作区
在内部 RAM。FFT 输入按 16 字节对齐。缓存和模型只在启动时分配；每帧不创建
TensorBase 状态视图、不加载模型，也不打印各层信息。

250 ms 能量 EMA 为输入归一化和输出响度匹配提供因果估计。训练配置的输入增益
限制、目标 RMS、输出 0–24 dB 补偿范围由模型清单生成；输出撤销输入增益。
静音不额外放大；超满量程峰值即时降低增益并以 100 ms 恢复，最后保护到 PCM24
满量程。该处理不会恢复麦克风/ADC 本身已经削波的波形。

开始使用零历史，结束补一个零 hop 并仅输出实际采样数。停止按键在完整 hop 边界
截断采集，因此响应至多多采约 16 ms；待模型尾部和剩余不足 76 点包发完后排 END。
断连废弃旧会话结果，重连必须重新按键开始。

## 性能门限与原音回退

蓝牙启用后预热 32 帧，测量 128 帧的完整 STFT/模型/ISTFT 耗时。
平均不超过 12800 us、P95 不超过 16000 us，且内部可用 RAM 不低于 48 KiB 才启用
模型。预检未通过时本次启动使用原音，并明确打印 `realtime=FAIL_RAW`。
预检同时为尚未创建的采集/推理栈、I2S DMA 和驱动元数据预留空间；预检失败会释放模型占用。

运行中积压达到四块、最老块超过 128 ms 或推理错误时，从下一个未发送采样位置
转为去直流后的原音。本段余下部分保持原音；运行时回退后下一段重新尝试模型。
已发增强音频无法撤回，因此切换之前是增强音频，之后是原音。缓存被推理持有时
不会复用，迟到结果不会重复发送。采集失败或八块池完全耗尽会结束该段并报告，
不会丢失一段后继续将后续采样悄悄拼接。
尚未凑满 76 点的尾包另保留原音，降级时先将该尾包恢复为原音；已提交传输队列的包维持原样。

`reason`：0=无，1=模型不可用，2=预检未通过，3=积压，4=超龄，5=推理失败，
6=池耗尽，7=采集失败。蓝牙拥塞继续使用原来的流控和丢包计数；回退原音不降低
蓝牙带宽，因此不会将传输拥塞伪装成模型问题。

每秒输出耗时、输入张量饱和计数、峰值保护次数、缓存和内存/栈余量。
`Audio capture-to-SPP-submit` 统计最早采样到成功提交 SPP 写入的延迟，P95 使用
4 ms 分桶；不包含空中传输、Windows 蓝牙栈和播放器缓存。采集时间以 DMA 读取
开始时间估计，可能包含约一个 hop 的时间误差，严谨端到端测量应使用外部回环。
没有关闭或取消订阅 IDLE 看门狗。

## 验证

```powershell
cd D:\HomeGuard\simulator
.\.venv-gtcrn\Scripts\python.exe -m pytest D:\HomeGuard\microphone\singlemic\tests -q
.\.venv-gtcrn\Scripts\python.exe -m pytest gtcrn\export\test_streaming_model.py -q
```

Windows 主机测试使用 MinGW g++：生产 DSP 配独立 FFT 后端验证重建、任意尾长、
静音/脉冲/正负满幅、归一化恢复和限幅；生产流水线配带时序的队列/线程与假采集、
假推理后端，验证正常、模型缺失、预检失败、慢推理降级、断连重启和池耗尽。
这些测试验证软件逻辑，不能替代真实 FreeRTOS 调度和蓝牙联调。

可选板端数值自检构建包含四帧连续量化参考向量；生产构建默认不携带这些向量：

```powershell
cd D:\HomeGuard\microphone\singlemic
.\tools\build.ps1 -Action '-DSINGLEMIC_GTCRN_SELF_TEST=ON build flash monitor' -Port COM9
# 确认 GTCRN golden: frames=4 max_lsb=... result=PASS 后，恢复生产版本。
.\tools\build.ps1 -Action '-DSINGLEMIC_GTCRN_SELF_TEST=OFF build flash monitor' -Port COM9
```

自检误差门限为 2 个量化 LSB，失败时进入原音。此处是板端验收步骤，尚未执行。
`AudioPipeline::stats()` 提供模式、原因、队列、耗时、饱和计数、内存及栈余量快照。

上板验收需分别记录：模型参考向量校验、真实音频效果、持续实时性能。
SPP 连续运行 30 分钟，检查 P95 提交延迟 <=200 ms、内部 RAM >=48 KiB、队列不持续
增长、无复位/看门狗/内存增长。`RAW` 模式的稳定运行不能算 GTCRN 实时通过。
参见 `VALIDATION.md` 的实际记录；未执行的项目保持待验收。
