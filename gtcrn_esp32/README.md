# GTCRN ESP32 模型验证固件

该 ESP-IDF 工程用于在 `ESP32-WROOM-32E-N8R2` 上验证
`models/gtcrn.espdl`。目前只验证模型加载、测试向量和推理性能，尚未合并
`singlemic` 的 I2S 和蓝牙链路。

模型会写入独立的 `model` Flash 分区。上电后，程序通过 ESP-DL 加载模型，
运行 ESP-PPQ 导出的测试向量，并输出内存占用和各算子耗时。

## 前置条件

- ESP-IDF 5.3 或更新版本。
- 本机存在 `D:/HomeGuard/esp-dl/esp-dl`，当前工程通过
  `main/idf_component.yml` 使用这份 ESP-DL 3.3.15 源码。
- ESP32 通过 USB-UART 连接电脑。蓝牙 SPP 虚拟 COM 口不能用于烧录。

## 编译和烧录

打开 ESP-IDF PowerShell，执行：

```powershell
cd D:\HomeGuard\microphone\gtcrn_esp32
idf.py set-target esp32
idf.py build
idf.py -p COM_PORT flash monitor
```

将 `COM_PORT` 替换成 USB-UART 端口，例如 `COM6`。

第一次执行 `flash` 会同时写入 bootloader、分区表、应用程序和模型分区。
之后如果只修改应用代码，可以不重复写入模型：

```powershell
idf.py -p COM_PORT app-flash monitor
```

## 预期串口输出

串口日志应包含：

1. `model` 分区地址、分区大小和编译时模型大小。
2. 模型加载前后的内部 RAM 和 PSRAM 余量。
3. `GTCRN model test passed`。
4. ESP-DL 模型内存占用和各算子推理耗时。

模型构建失败、内存分配失败或测试结果不一致时，程序只报告错误而不主动重启，
以便保留前面的 ESP-DL 诊断日志。

## 烧录前备份

烧录本工程会替换当前 `singlemic` 应用。需要保留现有固件时，第一次烧录前先备份
完整 8 MB Flash：

```powershell
python -m esptool --chip esp32 -p COM_PORT read_flash 0x0 0x800000 singlemic_backup_8MB.bin
```
