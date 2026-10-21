# singlemic 单麦 BLE 麦克风固件

本目录包含 ESP32-S3 单路 I2S 麦克风采集固件。程序从 ICS-43434 读取音频，将 24 kHz 原始采样处理并降采样为 12 kHz、单声道、16 位 PCM，再通过 BLE GATT Notification 发送给电脑端接收程序。

## 目录结构

```text
singlemic/
├── singlemic.ino          Arduino 入口，只负责 setup()/loop() 转发
├── cfg.h                  I2S、按键、音频格式、BLE UUID/队列/连接参数
├── app_controller.h/.cpp  应用协调层，编排录音状态和各模块调用
├── I2SSampler.h/.cpp      I2S 驱动初始化、释放和阻塞采样读取
├── audio_processor.h/.cpp PCM 缩放、DC 阻断、抗混叠滤波和降采样
├── packet_protocol.h/.cpp BLE 包格式、MTU 换算和小端序编码
├── ble_transport.h/.cpp   NimBLE GATT、连接回调、发送队列和通知任务
├── button.h/.cpp          GPIO 按键输入和软件去抖
└── README.md              本目录说明
```

Arduino IDE 编译时应打开 `singlemic.ino` 所在的 `singlemic` 草图目录。其余 `.cpp` 文件由 Arduino 草图构建系统一并编译；不要只把 `.ino` 单文件复制到另一个目录，否则会缺少对应模块实现。

## 模块职责

- `singlemic.ino` 是最薄的入口层：启动串口、调用应用 `begin()`，并在 Arduino 主循环中调用应用 `loop()`。
- `cfg.h` 集中保存板级引脚、采样率、BLE UUID、MTU、队列深度和连接参数。需要更换板子、引脚或音频设置时优先检查这里。
- `app_controller` 拥有各模块对象，是录音工作流的协调者。它处理开始/结束录音、每秒统计日志以及 I2S → DSP → BLE 的数据通路。
- `I2SSampler` 只处理 I2S 硬件和原始 32 位采样，不依赖 BLE 协议。
- `AudioProcessor` 对采样做缩放、DC 阻断和 FIR 抗混叠滤波，然后按配置倍率降采样；滤波器历史跨包保存，在新录音段开始时重置。
- `AudioProtocol` 生成传输包并适配 MTU。多字节字段固定使用小端序，避免受 MCU 内存字节序影响。
- `BleTransport` 管理 GATT 服务和通知特征。应用将控制包与音频包写入同一个 FIFO；单独的 FreeRTOS 发送任务按顺序发送并等待通知状态，避免并行堆积多个通知。
- `DebouncedButton` 使用上拉输入、低电平有效，通过稳定时间滤除机械抖动。

## 数据流和录音时序

```text
ICS-43434
    │ I2S：24 kHz、32 位槽
    ▼
I2SSampler ── 原始采样 ──► AudioProcessor
                               │ 12 kHz / mono / PCM16
                               ▼
                         AudioProtocol
                               │ START + DATA... + END
                               ▼
                         BLE FIFO 队列
                               ▼
                  NimBLE Notification 发送任务
                               ▼
                        电脑端接收程序
```

开始录音前，应用要求 BLE 已连接且接收端已订阅通知，然后重置 DSP 状态并将 `START` 放入队列。音频 `DATA` 包随后进入同一 FIFO；停止录音时把 `END` 排到已有音频之后。断连会清空旧连接的队列并结束当前流状态。

## BLE 包格式

固定头部为 14 字节，负载为有符号 16 位小端 PCM：

| 偏移 | 长度 | 含义 |
| ---: | ---: | --- |
| 0 | 2 | 同步字节 `A5 5A` |
| 2 | 1 | 类型：`1=DATA`、`2=START`、`3=END` |
| 3 | 1 | 声道数 |
| 4 | 2 | 采样率（Hz，小端序） |
| 6 | 2 | DATA 帧数；控制包为 0 |
| 8 | 4 | 包序号（小端序） |
| 12 | 2 | PCM 负载字节数；控制包为 0 |
| 14 | 可变 | DATA 包的 PCM16 采样 |

ATT MTU 247 时，单次通知最多承载 244 字节；扣除 14 字节头部后，可装 115 个单声道 PCM16 帧。连接时会按实际协商 MTU 重新计算帧数，因此中心端 MTU 较小时也会缩小每包数据量。

## 参数和诊断

常用修改入口：

- 引脚、I2S 端口和输入时钟：`cfg.h` 的 `I2SConfig`。
- 按键 GPIO 和去抖时间：`cfg.h` 的 `IOConfig`。
- 输出采样率、通道、位深、滤波和降采样参数：`cfg.h` 的 `AudioConfig`。
- UUID、最大包帧数、连接间隔、监督超时、包队列深度、通知超时/重试：`cfg.h` 的 `BLEConfig`。

应用每秒输出一次 BLE 状态，包括协商 MTU、每包帧数、实际连接间隔、队列当前/最高水位、成功/丢弃/队列满、重试、通知失败和超时计数。若 `queue` 长时间上升或 `queue_full` 增加，表示发送端持续落后于采集端；增大队列只能吸收短时抖动，长期瓶颈需要降低音频数据率或改善接收端/链路吞吐。

通知最多尝试 3 次，失败或等待结果超时后会延迟重试相同包。重发可能产生重复序号；接收端应丢弃已处理序号的重复包。通知是无应用层确认的数据通道，不能仅凭 `notifyOk` 判断电脑端已经将音频写入文件。

## 依赖和注意事项

- Arduino-ESP32（使用本工程所需的 I2S 驱动接口）。
- NimBLE-Arduino 1.4.x 或 2.x。`ble_transport.cpp` 会根据库头文件自动选择对应的服务器回调和通知返回值接口。
- FreeRTOS（随 ESP32 Arduino Core 提供），用于包队列和独立发送任务。
- BLE UUID 与包字段必须与电脑端 `ics_test/ble_ics_receiver.py` 保持一致。
- 修改 `AudioConfig::SAMPLE_RATE`、通道数或包格式时，也要确认接收端按数据包头部解析并正确保存 WAV。
