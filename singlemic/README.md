# singlemic 单麦克风固件

本目录面向物理模块 `ESP32-WROOM-32E-N8R2`。代码中的设备名称
`ESP32S3-ICS` 仅用于兼容既有电脑端配置，不代表目标芯片型号。

固件从 ICS-43434 以 16 kHz、32 位 I2S 槽采集。当前`DECIMATION_FACTOR=1`，
逐点转换并去直流，输出 16 kHz、单声道、PCM24，不经过降采样FIR。
倍率改回`2`时保留原五抽头低通和2:1降采样路径，输出采样率为I2S采样率的一半。
传输层可在编译时选择：

- `CLASSIC_SPP`：默认，使用经典蓝牙SPP发送到Windows蓝牙串口。
- `BLE_NOTIFY`：保留原NimBLE GATT Notification实现作为兼容模式。

每个固件镜像只初始化一种蓝牙Host路径，不会同时运行SPP和NimBLE。

## 目录结构

```text
singlemic/
├── singlemic.ino                 Arduino入口
├── cfg.h                         引脚、音频、协议和传输配置
├── app_controller.h/.cpp         录音工作流协调
├── I2SSampler.h/.cpp             I2S采集
├── audio_processor.h/.cpp        PCM24 DSP与降采样
├── packet_protocol.h/.cpp        公共包头、CRC与无损压缩
├── active_transport.h            编译时传输选择
├── ble_transport.h/.cpp          NimBLE兼容传输
├── spp_transport.h/.cpp          经典蓝牙SPP传输
├── transport_channel.h          音频FIFO与会话状态交换
├── transport_atomic.h           内部RAM原子变量检查及SDK兼容约束
├── transport_diagnostics.h      固定内存、无队列锁的SPP诊断日志
├── button.h/.cpp                 按键去抖
└── spp_audio_receiver.py         Windows SPP接收与WAV保存
```

Arduino构建系统会编译草图目录内所有 `.cpp`。BLE和SPP实现文件内部均按
`SINGLEMIC_TRANSPORT_MODE` 条件编译，因此未选中的传输不会引入对应Host库。

## 选择传输方式

`cfg.h` 默认配置：

```cpp
#define SINGLEMIC_TRANSPORT_MODE SINGLEMIC_TRANSPORT_CLASSIC_SPP
```

需要生成BLE兼容镜像时，将构建宏改为：

```cpp
#define SINGLEMIC_TRANSPORT_MODE SINGLEMIC_TRANSPORT_BLE_NOTIFY
```

也可以通过编译参数覆盖 `SINGLEMIC_TRANSPORT_MODE`，无需修改源码默认值。

SPP必须选择经典ESP32板型，例如Arduino IDE中的 `ESP32 Dev Module`。不要选择
ESP32-S3目标，因为ESP32-S3没有本方案所需的经典蓝牙SPP控制器支持。

## Arduino-ESP32 3.3.12 构建与丢包验证

当前按 3.3.12 构建验证。Arduino IDE 中关闭 Sketch/项目菜单的
`Optimize for Debugging`（为调试优化），使用发布优化 `-Os`，CPU 240 MHz，
Core Debug Level=None。不要把 IDE 版本与 Boards Manager 中的 ESP32 核心版本混淆。
当前工程使用 `ESP32-WROOM-DA Module`（`esp32da`）构建；板型应与实际模块匹配。

在项目根目录执行以下脚本，可以检查已安装的核心版本并强制使用 `-Os`，
不受 IDE 的调试优化开关影响。脚本只编译，不烧录；IDE 打开时可自动找到其 CLI，
也可传入 `-ArduinoCli` 指定完整路径。默认构建目录位于系统临时目录，
默认板型沿用本工程的 `esp32da`，可用 `-Fqbn` 指定经典 ESP32 Dev Module。

```powershell
.\singlemic\build-release.ps1
# 未自动找到 CLI 时：
.\singlemic\build-release.ps1 -ArduinoCli 'D:\term_files\arduino\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
```

SPP 将已积压的最多 4 个 DATA 包合并为一次最多 976 字节的 SDK 写入，
队列只有 1 包时立即发送，不额外等待凑批；START/END 分别单独发送。
原 76 帧包头、CRC、16 kHz PCM24 及 Python 解码规则保持兼容。
明确零字节失败保留原批次，退让至少 5 ms 后重试；成功的部分写入只续发未接受的后缀。
此处理依据 IDF 的 `PORT_WriteDataCO` 返回已接受字节数的行为，参见
[IDF 5.5.5 RFCOMM 实现](https://github.com/espressif/esp-idf/blob/v5.5.5/components/bt/host/bluedroid/stack/rfcomm/port_api.c)。

重新烧录后，启动日志应出现：

```text
SPP TX: batch_max_packets=4 batch_max_bytes=976 retry_ms=5
```

接收仍使用 `python .\py_rec\spp_audio_receiver.py --address 8C:8C:29:1A:03:62`；
旧板须填写其自身 MAC。保持完整录音期间观察速率，当前原始 PCM 约 210.5 个 DATA 包/秒，
协议字节约 51.4 kB/s（不包含蓝牙空口开销），空闲/开始/结束附近的统计窗口可以更低。
连续录音及重复启停时要求 Python 最终 `lost packets=0`、`crc_errors=0`、
`dropped_bytes=0`，ESP32 的 `queue_full/dropped/event_full` 不增长，且队列不持续堆积。
`partial`、`write_fail` 可以因重试增长，是否真正缺包以最终序号完整性为准。
这些构建和流控修改不能保证任意无线环境下零丢包；持续吞吐低于音频生成速度时，
有限 FIFO 最终仍会溢出，应保留同期两端日志继续定位。

主机回归测试：在有 g++ 和 Python 的环境中运行
`python tests/test_spp_transport.py`，覆盖批量发送、控制顺序、零字节重试、
成功部分写入、拥塞、超时不重复发送、断连重连及原 Python 解析器兼容性。

与 `ics_test/ble_ics_receiver.py` 的兼容关系：当前singlemic包末尾增加了2字节CRC，
因此旧BLE接收器不能直接解析新包。SPP模式使用已同步CRC规则的
`spp_audio_receiver.py`；BLE模式启用前也需要让对应BLE接收器按相同规则校验并去除CRC。

## 数据流

```text
ICS-43434
  -> I2S 16 kHz / 32-bit slot
  -> DC blocker (factor=1: one-to-one; factor=2: anti-alias FIR + 2:1 decimation)
  -> 16 kHz / mono / PCM24
  -> START + DATA... + END packet FIFO
  -> SPP byte stream or BLE Notification
  -> PC receiver -> 32-bit PCM WAV
```

按键只有在电脑端已连接后才能开始录音。START、DATA和END进入同一个FIFO，
保证顺序。断连会停止当前录音、清空旧会话队列并增加会话epoch；重连后必须
重新按键开始新的录音段。

SPP直接使用ESP-IDF回调接口，不使用BluetoothSerial。SPP与BLE均采用
“回调零等待入事件FIFO、发送任务独占状态机、主循环读取完整快照”的结构。
传输层不再使用应用级 `portMUX` 自旋锁；回调不打印、不延时、不清队列，
也不调用可能等待蓝牙栈队列的API。FreeRTOS与蓝牙栈内部仍有自己的短临界区，
因此这里的无锁特指本项目不再让回调/主循环竞争无限等待的跨核自旋锁。
同一个发送任务处理连接变化、配对/广播、拥塞和发送完成。最多只有一个未完成写入，
收到WRITE_EVT且非拥塞后才继续。64项音频FIFO保留两个位置给控制包，
START/END未完成时不能开始下一段；录音切换不清空待发队列。
立即提交失败或明确零字节失败时，保留当前 DATA 批次或控制包，按配置退让后重试。
SPP的拥塞前 `len=-1` 和成功但 `len=0` 均按明确零字节处理，不再误判成
永久控制故障。成功且有明确字节数的部分写入只续发后缀；失败且交付不明的结果
进入fault，缺失完成事件保持等待；
BLE明确失败最多按配置次数重试，通知超时则因交付情况不明进入fault。
这些故障都只暂停当前音频会话，不会主动断链或
重发可能已交付的数据。fault在电脑实际断开重连后清除。
事件FIFO溢出表示状态证据丢失，当前会话停止提交；断开并重新连接后恢复，
不能在证据丢失的会话中强行继续发送。音频FIFO不在连接切换时并发reset，
发送任务按epoch有界丢弃旧包，避免reset与主循环入队互相竞争。

## 按键

- `Key1 / GPIO12`：低电平有效。连续按住至少3秒后开启2分钟配对窗口，
  此后可以松开按键，窗口不会提前结束。短按不触发；一直按住也不会反复续期。
  窗口到期后需先松开，再长按3秒才能重新开启；窗口内重新长按会重新计时。
  时间从去抖后的稳定按下状态起算，配置为 `IOConfig::PAIRING_HOLD_MS`
  和 `IOConfig::PAIRING_WINDOW_MS`，主循环计时，不使用阻塞延时。
  SPP上电默认不可发现，只在窗口内且尚未连接时允许Windows搜索发现。
  已配对电脑无需再按Key1，Python打开传出蓝牙COM口即可连接，包括设备重启后。
  窗口到期只关闭可发现性，不断开连接，也不停止录音。
  主连接建立后关闭额外连接入口，断开后重新允许已配对客户端连接。
  使用SSP无固定PIN配对，不清除已有配对信息。
  Windows删除设备后，重新长按Key1进入窗口，再执行添加设备和配对。
  不可发现不会清除Windows已有列表或搜索缓存，也不是拒绝已知地址连接的访问控制。
  BLE兼容模式使用相同按键计时开启/停止广播；窗口外重连保证仅适用于SPP。
- `Key2 / GPIO13`：按下沿切换录音开始/停止。

## 公共包格式

BLE和SPP使用完全相同的包。SPP是无边界字节流，电脑端根据同步字节和负载
长度恢复包边界。

| 偏移 | 长度 | 含义 |
| ---: | ---: | --- |
| 0 | 2 | 同步字节 `A5 5A` |
| 2 | 1 | `1=raw PCM24`、`2=START`、`3=END`、`0x83=全字节Huffman`；`0x81/0x82`保留给旧格式 |
| 3 | 1 | 声道数，当前固定为1 |
| 4 | 2 | 采样率，小端序 |
| 6 | 2 | DATA帧数；控制包为0 |
| 8 | 4 | DATA序号，小端序 |
| 12 | 2 | 负载字节数，小端序 |
| 14 | 可变 | PCM24或无损压缩负载 |
| 14+负载长度 | 2 | CRC-16/CCITT-FALSE，小端序，覆盖包头和负载 |

每包最多76帧，最大长度244字节（14字节包头、最多228字节负载、2字节CRC）。

当前`cfg.h`设置`SINGLEMIC_ENABLE_HUFFMAN=0`，所有DATA包直接发送原始PCM24，
类型为`0x01`，每个样本3字节小端；76帧对应228字节负载和244字节完整包。
Huffman码表、工作缓冲区和编码过程均不编译进该模式。启动会打印
`Audio encoding: RAW_PCM24 (Huffman disabled)`，用于确认当前固件模式。
I2S读取、PCM24转换、去直流、采样率、声道、CRC和队列保持原流程；这里的“原始”
指未经压缩的PCM24，并非直接发送32位I2S槽。现有Python接收器可直接接收，无需修改。
16kHz时音频负载约48000B/s，包含包头及CRC约51368B/s；实测需同时检查丢包和队列统计。
如需恢复压缩，将该宏改为`1`重新编译。

启用时，发送端使用标准静态规范Huffman（canonical Huffman）：按样本顺序，将每个PCM24样本的
低、中、高三个字节全部作为0～255的符号编码，不再保留低16位原文，也不再发送block-delta。
沿用固定频率表 `1 + 65535 // (1 + min(symbol, 255-symbol))²`；合并相同频率时按节点编号排序，
规范码按码长、符号值排序。编码位按MSB优先排列，最后一字节补零，由帧数确定解码字节数。
不逐包发送码表。仅在Huffman负载短于原始PCM24时使用它，否则回退raw，保证不扩大包长。
PCM24逐样本无损保留；静态码表不保证对所有声音都能压缩，去掉block-delta后部分信号的传输量会增加。
新格式使用`0x83`，必须配合本目录更新后的Python接收器；旧接收器不支持新格式。
新接收器仍支持带CRC的旧`0x81`高字节Huffman及`0x82`block-delta；不支持无CRC的更早固件。

## 临时固定PCM24隔离测试

当前`cfg.h`默认使用`SINGLEMIC_FIXED_PCM_TEST=0`，启用真实I2S采样和麦克风DSP。
使用板载另一组麦克风：SCK/BCLK=GPIO5、WS=GPIO18、DATA=GPIO19，选择右声道，
显式禁用未使用的MCLK输出。当前16kHz采集，倍率1逐点处理，输出16kHz单声道PCM24。
启动日志应包含`BCLK=5 WS=18 SD=19, channel=RIGHT MCLK=disabled`。

需要隔离测试时，将宏改为`1`重新编译：不初始化或读取I2S，不启动I2S DMA/中断/时钟，
也不执行麦克风DSP。仅该测试模式会打印
`Audio source=FIXED_PCM_TEST: I2S/DMA and microphone DSP disabled`。

配对、连接和Python接收步骤不变。按Key2发送START，然后循环读取`fixed_pcm_source.h`中的
32个有符号PCM24固定值；再次按Key2发送END。每次新START都从数组首元素开始，
未开始或停止后不生成DATA；断连后不自动开始下一段。固定值包括满量程，WAV不是实际麦克风声音。
编码遵循`SINGLEMIC_ENABLE_HUFFMAN`（当前直接raw PCM24），仍经过CRC、原FIFO和SPP流控，
不直接调用底层发送。
按`AudioConfig::SAMPLE_RATE`、76帧/包限速（当前16kHz约210.5个DATA包/秒），小数微秒累计避免舍入漂移；
主循环延迟时不无限补发，因此实际速率可更低。包丢失时Python仍按序号补静音。

先在该模式持续传输，观察是否复位以及`queue_full/write_fail/write_timeout`，
保留本次ELF哈希和复位日志。再将宏改为`0`重新编译，恢复原I2S/DSP路径作对照。
固定输入也复位说明复位不需要I2S采集参与，但还包含编码、队列、系统及硬件等可能原因，
不能直接判定SPP SDK有错。只有真实输入复位则重点查采集/DSP及其与蓝牙的资源、时序交互；
两种模式的内存布局、发送内容和中断负载不同，单次稳定不能证明I2S驱动有错。
本测试不修改任务栈、看门狗、采样格式或蓝牙回调。

## Windows SPP接收

1. 初次配对或在Windows删除设备后，连续按住Key1至少3秒，看到配对窗口日志后
   即可松开。在随后2分钟内，从Windows“添加设备”搜索ESP32S3-ICS并完成配对。
   上电默认不可发现，未触发长按不会响应新的设备搜索；Windows可能保留历史缓存。
   已经配对过则跳过这一步，直接运行Python，无需按Key1。
2. 安装依赖：python -m pip install pyserial。
3. 脚本按默认MAC 88:F1:55:B6:9D:DE自动寻找Windows传出蓝牙COM口。
   这个COM是无线连接，无需ESP32通过USB连接电脑。

```powershell
python .\spp_audio_receiver.py --output spp_capture.wav
python .\spp_audio_receiver.py --port COM14 --stats-interval 1
python .\spp_audio_receiver.py --address 24:6F:28:12:34:56
```

端口打开不等于已经收到设备音频，等待START/DATA确认数据路径。
找不到COM、打开失败、读取异常都会记录错误并重试；只有一个读取线程打开端口。
空读、静音、END和停止录音不会关闭连接。重连间隔由 --reconnect-delay 设置。

专用线程持续读取COM，最多缓冲256块，每块4096字节；解析、解码、打印、写盘
在消费线程执行。WAV每块更新头部，每秒刷新文件缓冲，内存不随录音时长增长。
断连或Ctrl+C先处理已接收队列再完成WAV。队列溢出明确报告丢失字节并重置同步，
不会把丢失前后的半包直接拼接。序号缺口补零，包含START后的首包及END前的尾包。
大于60秒的异常序号跳跃拒绝补零并报告，防止损坏包头造成无界写盘。

WAV采样率来自包头，单声道32-bit PCM。Python将有符号PCM24左移8位，
放入PCM32的高24位、低8位补零，保持相对满量程幅度，避免仅符号扩展造成约48.16dB衰减。
全部24位有效数据保留；合法PCM24范围映射后仍在有符号32位范围内，不新增削波或16位量化。
raw PCM24和各兼容压缩格式在解码后共用此转换；蓝牙包和固件无需为此更改。
此修正只作用于重新运行接收器后保存的录音，不自动修改已有WAV；全零输入仍为全零。
输出为 spp_capture.wav、spp_capture_002.wav 等；重新运行继续已有编号，
不覆盖旧录音，断线前后不会静默拼接。

## 诊断

### 本轮软件边界保护与定位

- 当前麦克风引脚：BCLK=GPIO5、WS=GPIO18、SD=GPIO19，接收右声道，禁用MCLK输出；
  Key1=GPIO12、Key2=GPIO13。代码调整仅在 `singlemic` 内。
- I2S 读取接口从数组推导真实容量，驱动调用前拒绝超容量请求及字节数乘法溢出；
  应用层先校验传输帧数不超过 76，再计算输入采样数。原有返回 chunk 长度检查仍保留，
  但它只能发现驱动返回值异常，不能撤销驱动已经发生的越界写入。
- 启动打印完整 `Firmware ELF SHA256`，用于与对应 ELF 严格匹配；不同构建不能混用地址解析。
- 使用 ESP-IDF `heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true)`，分别检查 I2S 初始化前后、
  传输初始化调用返回后、每次请求开始录音前的内部堆。`Memory check begin/end` 标明检查阶段。
  检查只在主任务低频执行，不进入 ISR、蓝牙回调或逐包发送路径，不扫描外部 PSRAM。
  初始化检查失败会停止后续初始化；录音前检查失败会拒绝本次 START，不主动断开蓝牙。
- `after_transport_begin` 仅表示初始化函数返回的时间点，不代表异步蓝牙初始化全部完成。
  `OK` 也只覆盖当时内部堆的结构和保护值，不能排除之后的越界、全局数组损坏或取指异常。
  本轮不关闭看门狗，不更改采样率、PCM24、CRC、压缩协议或 SPP 写入流控。

参考乐鑫 [Heap Memory Debugging（IDF 4.4）](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/api-reference/system/heap_debug.html)
及 [Heap Memory Allocation（IDF 4.4）](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/api-reference/system/mem_alloc.html)。
这些检查用于缩小首次内存破坏的时间窗口，不代表已证明或修复硬件端复位根因。

应用主循环和 BLE/SPP 发送任务分别注册任务看门狗。任务循环、发送事件批处理、
I2S 分段读取和首次 Huffman 建表在有进展的边界检查时间，距离上次让出 CPU
至少 2 ms（向下换算为 tick，最少 1 tick）时执行 `vTaskDelay(1)`，
返回后仅为已注册的当前任务调用 `esp_task_wdt_reset()`。一个检查点不会打断
其前面仍在阻塞的 SDK 调用，也不会为其他任务或 IDLE 任务代喂狗。
主循环在初始化成功后注册；发送任务在进入循环前注册。SDK 默认看门狗超时
和 IDLE 监视保持原配置，任务注册失败会明确报错。回调和中断不调用这些检查点。
这用于减少应用循环饿死低优先级任务的可能性，不能修复非法指令、内存破坏，
也不能强制 SDK 内部的 `hciT` 任务让出 CPU；需要重新烧录后实测确认效果。

固件每秒输出连接/录音状态、队列水位、堆和栈余量，以及：

- write_submit：IDF接受的提交数；write_evt：匹配在途写入的完成事件数。
- sent：完整批次完成后计入的协议包数；write_submit/write_evt 按 SDK 写入次数计数，
  一次写入可包含多个 DATA 包，不等于电脑已经写入WAV。
- batch_high：单次写入包含协议包数的历史最高值/配置上限；partial：成功部分写入次数。
- wait_ms：当前写入等待时长；last_write_ms/last_cong_ms：最近事件的开机时间。
- write_status：最后匹配的WRITE_EVT状态；write_timeout：完成事件延迟次数。
- stale：非当前连接、旧会话或无在途写入的完成事件。
- control_fail：控制包失败次数。最终完整性以电脑端序号缺口为准。
- event_full：蓝牙回调事件FIFO溢出。非零表示当前会话状态证据不完整，
  发送状态机进入fault并等待真实断开/重连恢复，不会由固件主动断开。
- close_ms/close_status/port_status/close_async：最后一次主连接关闭信息，
  单独保存，避免故障历史冻结后遗漏后续关闭原因。
- diag_fault：诊断缓冲自检故障位，正常为0；1=边界保护值损坏，2=历史索引异常，
  4=输出索引或待发送长度异常，8=日志格式化失败。非零时停止诊断trace通路，
  不主动断开蓝牙、不重启、不停止录音；故障位保留到重启。

首次异常冻结最近32条事件，在主循环UART可用时逐行输出，不阻塞蓝牙任务。
诊断日志使用对象内固定32条输出缓冲，蓝牙任务为唯一生产者，主循环为唯一消费者；
通过32位原子索引及acquire/release交接，不分配堆队列、不调用FreeRTOS队列锁。
满时保留待输出历史并立即返回；UART空间不足时延后输出，短写只重发未发送的后缀。
启动日志中的 `trace=static_spsc` 用于确认已刷入这一版本。
音频包、状态快照和回调事件仍使用FreeRTOS队列；本次消除的是已定位的诊断队列
`xQueuePeek()`锁断言路径，不能据此宣称所有内存破坏来源均已排除。

Arduino-ESP32 3.3.12的ESP32构建启用了PSRAM原子操作兼容路径，
`ATOMIC_INT_LOCK_FREE`为1，不能再要求其恒等于2。
保留`std::atomic<uint32_t>`及原有acquire/release语义；初始化时检查配对标记、
诊断索引/故障位及回调溢出计数均位于内部RAM且按4字节对齐。
该SDK会将内部RAM上的32位原子操作分派给S32C1I实现，避免落入外部RAM的锁回退路径。
不覆盖SDK编译参数，也不把并发共享变量改成普通或仅volatile变量。

Arduino-ESP32 3.3.12还会在`setup()`之前释放未声明使用的蓝牙内存。
SPP直接使用IDF API，因此显式包含核心提供的`esp32-hal-alloc-bt-classic-mem.h`，
在静态初始化阶段声明使用经典蓝牙；仅调用`btStarted()`不足以保留内存。
启动日志应显示`SPP startup: classic_memory=reserved`，随后出现`SPP server ready`。
这表示初始化阶段正常；仍需长按Key1至少3秒开启2分钟可发现窗口。
IDF 5及以上使用`esp_spp_enhanced_init()`显式关闭L2CAP ERTM，
选择旧版IDF 4.4使用的Basic模式；IDF 4.x保留原初始化入口。
这是针对升级后连接失败的兼容性调整，连接恢复效果需烧录后实测。
启动日志应包含`SPP initialization requested: L2CAP Basic, ERTM disabled`；
该行只表示初始化请求已提交，不表示电脑已经连接。
后续已收到CRC正确的START和音频包，证实SPP能够传输；
尚不能将连接恢复单独归因于ERTM设置，也未证实长时间运行稳定。

连接排查用的原始字节预览、协议确认提示、COM映射及额外ACL统计已移除。
保留接收速率、CRC错误、录音状态和异常信息。
Key2在未连接时也会被处理，并打印`Recording start refused: transport is not connected`。

若只剩`TG1WDT_SYS_RESET`且没有Backtrace，固件用16字节RTC未初始化区保存
主任务与SPP任务的最后执行阶段。正常运行时不为此打印、不分配内存、不写Flash，
每个任务只更新自己的一项32位记录，不在回调或中断中更新。
发生panic或看门狗重启后，启动时仅输出一次`Previous run: main=... spp=...`；
首次运行、掉电或记录无效时可能显示`no valid retained checkpoints`。
`marks`是24位循环计数，不是音频包数或时间。记录是最后执行位置，不能单独证明
该位置就是故障源，系统可能在任意位置被中断或因其他核的问题复位。
阶段覆盖START堆检查、I2S、DSP、Huffman建表/编码、CRC、入队、
SPP写调用/返回、状态发布及任务等待。该机制用于定位无日志复位，不改变看门狗配置。

SPP trace包含开机毫秒、epoch、handle、event、status、len、detail。
event沿用IDF编号：27=CLOSE、31=CONG、33=WRITE、34=OPEN；
100=提交返回值、101=写等待超时、102=可连接/可发现设置、103=含糊写入、
104=额外客户端、105=认证结果。
CLOSE的detail为 (port_status << 1) | async；async为false表示本地发起关闭，
true不能单独证明Windows应用主动关闭。WRITE/CONG的detail为cong。
本模块不调用主动断连、蓝牙停机或重启接口。

Python默认每秒显示COM原始B/s、协议B/s、载荷B/s、包率，以及
rx_queue、high、dropped_bytes、max_read_gap、crc_errors；可用 --stats-interval 调整。
速率按实际经过时间计算，包含空闲时间，COM字节数不包含空口协议开销。
异常带本地时区时间、类型、errno/winerror，便于和固件事件对照。

BLE保留原有MTU、连接间隔、Notify、重试、超时及epoch日志。

2026-10-30验证：Arduino-ESP32 2.0.14、ESP32 Dev Module下SPP/BLE完整编译；
基于实际源码的公共通道、SPP和BLE状态机主机测试通过，回调禁用操作审计通过，
Python接收器语法检查通过。临时测试与构建文件不随主流程保留。
编译、静态审计和主机模拟不代表无线硬件实测通过。

2026-10-31兼容性验证：Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5、
沿用当前`esp32:esp32:esp32da`板型配置，SPP/BLE均完整编译及链接通过。
修复旧版“所有32位原子操作始终无锁”的编译断言；构建产物位于临时目录，未烧录。

诊断队列锁断言修复：SPP/BLE均在Arduino-ESP32 2.0.14、ESP32 Dev Module下编译；
主机测试覆盖最近32条历史、满缓冲、UART空间不足/短写、32位索引回绕、故障注入，
以及10万条日志的双线程顺序传递。测试文件仅放在临时目录，不加入固件主流程。
仍需板上长时间录音确认；若 `diag_fault` 非零或出现其他回溯，须继续追查内存破坏源。

验收目标：目标采样率连续2小时无复位/意外断连/序号缺口且队列不持续增长；
100次录音开始/停止保持连接；20次电脑主动断开重连均生成独立、可打开的WAV。
同时记录启动原因、空闲/最低堆、任务栈、写入/拥塞/事件队列统计。
这些是验收标准，不代表已取得硬件测试结果。
