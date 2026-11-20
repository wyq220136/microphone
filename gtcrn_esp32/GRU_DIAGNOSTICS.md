# GRU v12 共享 FP32 查表修复与板端验收

## 已确认与未确认

- 补充板端日志确认，原 `/dpgrnn1/intra_rnn/rnn1/GRU` 的 2640 个输出已全部匹配；最先超容差的是 `/dpgrnn1/inter_rnn/rnn1/GRU`，输入与 GT 完全相同，但输出 30 个元素不同、最大差 51 LSB。
- 主机提取生产 C++ 算术代码后，完整复现 30 个不同元素及最大差 51 LSB，也复现 `[4,0,28,5]` 的旧 GT=21777、实际=21761。
- 首个原因是投影累加方式：INT16 乘积的 INT64 精确总和为 69140473，设备转 FP32 并缩放后得到 527.49994；旧 FP32 GEMM 得到 527.5，跨过舍入边界，含偏置后的门缓存从 534 变成 535，并被递推放大。v11 修正 C INT16 GRU 导出参考，按设备方式整数累加后转回 float32；不是手工修改 GT，也不是放宽容差。
- `Expected per-tensor output exponent` 是上一版诊断代码误用初始化参数查询 API 读取图输出元数据。现从已构建模型复制输出描述符，覆盖 Y 和 Y_h。
- 模型结构及实际被使用的参数数据、指数保持一致。重新生成测试向量；仍使用约 1 LSB 的严格阈值，没有引入 double 张量。INT64 是整数累加器，不是 FP64/double。
- v11 的补充板端日志确认原来的 51 LSB 差异已消除，但 inter_rnn/rnn1 仍有 5 个元素相差 1 LSB；整网依然失败。主机生产 C++ 实现复现这 5 个位置：1754、2018、2282、2546、2548。
- 首个残余差异位于 `[6,0,21,2]`。XW、HR 和此前状态相同，sigmoid 输入索引为 664；导出端门值位模式 `0x3f4909fb`，旧运行时为 `0x3f4909fc`。舍入前分别为 6253.5 和 6253.49951171875，输出变成 6254 和 6253。后续不同层的失败不能据此全部认定为独立故障，也不能仅凭单层回归断言整网已修好。
- v12 让经典 ESP32 的该 INT16 GRU 路径与导出器共享冻结的 FP32 sigmoid/tanh 表，采用相同的负索引规则和截断范围。表放在固件只读区，不在板端调用 expf/tanhf 重算。其他目标、INT8、浮点 GRU 和自定义表范围保持原有路径。
- 重新生成后的调试模型已对 14 个 GRU 的全部 22080 个输出做主机同输入检查，全部精确一致；`--require-exact` 会拒绝任何 1 LSB 差异。这仍不是完整模型的 ESP32 验收，尚需用户烧录后的日志。
- 固件已完成编译，镜像 1121040 字节；最终 ELF 中两张表的全部 14340 个 FP32 值与导出资产逐字节一致，并位于 `.flash.rodata`。验证记录在 `build/diagnostics/firmware-lut-gru-v12.log`。

本轮调试模型 SHA256：`298cc6ebe49fbc3a5ccb13f940a6ae0c873885ab3d780e3dd858421baf0cb8d8`，967072 字节。
正式模型 SHA256：`13d61c222e82ba38815722216fc8dedc5a23eba5bbff382eeedd5a8e8318fdbe`，446336 字节。
共享 FP32 表的原始数据 SHA256：`a591bf1fb184b4c8d7aef2a9c8b702a0f3ac9098a16e270ac73fe7b344a0a586`。
v11 调试模型保留在 `build/diagnostics/v11_before_shared_lut.espdl`。导出、编译和检查报告在 `build/diagnostics/*gru-v12*`。

## 用户上板步骤

在 ESP-IDF PowerShell 中执行（代理只编译，不访问 COM5）：

```powershell
cd D:\HomeGuard\microphone\gtcrn_esp32
idf.py -DGTCRN_GRU_DIAGNOSTICS=ON build
idf.py -p COM5 flash monitor
```

当前 build 已开启诊断；如果直接使用本次编译结果，运行最后一条即可。新模型已重新导出并同步至 `models/gtcrn.espdl`，用户无需再次导出或训练。必须使用 `flash` 同时更新固件和模型分区，不要仅执行 `app-flash`。

启动时确认 `runtime revision: gru-shared-fp32-lut-v12`，以及 `GRU FP32 LUT SHA256` 与上面一致。仅保存一次启动的完整日志，不要拼接多次重启结果。监视器可先按 `Ctrl+T`、再按 `Ctrl+L` 开关文件日志，`Ctrl+]` 退出。若开启文件日志时已错过启动输出，再手动复位板子，保存这一次完整启动的日志。

## 日志含义

1. `GRU_TRACE`：独立 inter_rnn/rnn1 GRU 使用序列化 GT 输入，在正向分支、batch=21 按 `t=0..9` 记录 XW、HR（均含偏置、已饱和）、GATES 和更新后的 H。GATES 是 FP32 位模式的十六进制，不改变计算类型。跟踪整个递推而非只看输出坐标 t=6。
2. `GRU_ISOLATED ... changed=... max_lsb=...`：同一份 GT 输入直接进入板端真实 GRU 的输出比较；不经过上游算子。`GRU_ISOLATED_DIFF` 保留全部输出差异。
3. `GRU_DIAG INPUT ...` 与 `GRU_INPUT_DIFF`：完整模型执行到目标 GRU 时，记录真实输入相对内嵌 GT 的全部整数差异；没有替换真实输入，也没有重置完整模型状态。
4. `GRU_DIAG OUTPUT ...`：严格比较各 INT16 检查点，包括只有 1 LSB 的差异。诊断模式会收集后续数值失败，最终仍返回 `ESP_FAIL`，不会把失败变成通过。

请提供从启动版本号到最终测试结束的日志，包括所有 `GRU_TRACE`、`GRU_ISOLATED`、`GRU_DIAG INPUT` 和 `GRU_INPUT_DIFF` 行。恢复严格容差后，更早检查点也可能报错；诊断继续运行是预期行为。

## 主机复核

```powershell
cd D:\HomeGuard\simulator\gtcrn\export
$env:PYTHONPATH='D:\HomeGuard\simulator\esp-ppq'
conda run -n sam3 --no-capture-output python -m unittest test_gru_runtime.py
conda run -n sam3 --no-capture-output python generate_gru_lut.py --check
conda run -n sam3 --no-capture-output python diagnose_gru.py --node /dpgrnn1/inter_rnn/rnn1/GRU --trace-direction 0 --trace-batch 21 --device-log D:\path\device.log
conda run -n sam3 --no-capture-output python validate_gru_export.py gtcrn_debug.espdl --require-exact
```

需要 PATH 中可用的 MinGW `g++`。主机脚本直接提取当前生产 dotprod、舍入、LUT 和 GRU 递推代码构建探针，并检查日志完整性。整数计算和 FP32 门值按实际执行顺序分开比较；仅有 FP32 位差异不自动等于误差根因。复查 v11 旧日志时应指定旧模型文件、`--trace-batch 28 --legacy-runtime-lut`。

冻结表资产是 `esp-ppq/esp_ppq/executor/op/torch/gru_lut_fp32.npz`，对应固件头文件是 `esp-dl/esp-dl/dl/math/include/dl_gru_lut_fp32.hpp`。二者须一起保留。`generate_gru_lut.py --check` 验证内容一致；不加参数只从已冻结资产重建 C++ 头文件，不重新计算激活值。导出入口会先自动执行一致性检查。正常使用不要删除资产重跑 `--create`。

编译后还可在 ESP-IDF Python 环境运行 `verify_gru_lut_firmware.py`，核对最终 ELF 中的实际表数据，而非仅检查源码。该脚本需要 ESP-IDF 已有的 pyelftools，不要求安装 NumPy。

主机检查的是每个 GRU 在相同输入下的输出，不包含所有上游板端误差的传播。若仍有超容差节点，继续依据最早差异定位，不直接提高阈值。日志中 `changed` 包含 1 LSB 差异，并不全代表超容差失败。

诊断完成后可用 `idf.py -DGTCRN_GRU_DIAGNOSTICS=OFF build` 关闭额外日志。默认构建选项为 OFF；当前诊断构建显式为 ON。
