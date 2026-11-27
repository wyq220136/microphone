PEQ 软件模拟与调参

1. 已生成的试听
打开 results/listen.html。7 组录音均有两个分支：
  原始 raw → PEQ
  原始 raw → 既有低切＋高频增强 → PEQ
第二分支沿用此前 strong_both：70 Hz 二阶＋50 Hz 四阶 Butterworth 高通，
1.5 kHz +3 dB、3 kHz +6 dB 高架，S=1。这里没有新增低通抑制高频。
两个分支各有未加 PEQ 对照和 -3/-4/-6 dB PEQ，默认中心 300 Hz、带宽 300 Hz。
所有版本从原始 raw 重新计算，没有对旧试听重复施加滤波。
raw 已包含录制时原有 DSP，本模拟不再次添加去直流。

同一编号的全部版本按整段 LUFS 匹配，只施加恒定增益；不做 AGC、压缩或动态限幅。
共同目标 <= -18 LUFS；如峰值需要，整组一起降低目标。输出 16 kHz 单声道 PCM24。
固件中的固定 -12 dB 预衰减未重复添加，其恒定增益会被等响匹配抵消。

2. 浏览器调参
双击 peq_tuner.html，在文件选择框选择 res_1126/raw 中的一份 WAV。
选择“只追加 PEQ”或“现有滤波链＋PEQ”，设置频率、带宽、削减量，点击生成。
可试听、下载两份 PCM24 WAV，并下载参数 JSON。音频不上传。
自定义页面用整段 RMS 对齐；固定试听使用 LUFS 对齐，两者音量可能不同。
页面为两份音频共同预留 -1.2 dB 采样峰值余量；固定试听另外检查过采样真峰值。
本地页面用 Web Audio、16 kHz，系数形状与 Python 使用的 RBJ peaking EQ 一致。

3. 参数含义
center_hz：中心频率，中心处达到 gain_db。
bandwidth_hz：半增益频点之间的频差，不是硬截止的覆盖区间。
例如 -4 dB 时，两边 -2 dB 频点之间定义带宽。
默认中心 300 Hz / 带宽 300 Hz，半增益频点约 185–485 Hz，Q≈0.997。
Q 越大越窄。Python 也支持用 q 替代 bandwidth_hz，但不能同时指定两者。
gain_db：负数削减，0 为不作 PEQ，正数提升；建议先比较 -3、-4、-6 dB。
所有滤波均为因果 IIR，sosfilt 单向处理，不使用零相位前后向滤波。
公式参考：https://www.w3.org/TR/audio-eq-cookbook/

4. 修改配置后重新生成
环境：Python >=3.10，NumPy、SciPy、Matplotlib，以及 PATH 中的 ffmpeg。
在项目根目录运行：
python py_rec/res_1126_spectrum_20261127/peq_preview/simulate_peq.py

指定频率、带宽、削减量，并写到新的输出目录：
python py_rec/res_1126_spectrum_20261127/peq_preview/simulate_peq.py --center-hz 280 --bandwidth-hz 260 --gain-db -4 --output-dir py_rec/res_1126_spectrum_20261127/peq_preview/results_custom

或使用 Q：
python py_rec/res_1126_spectrum_20261127/peq_preview/simulate_peq.py --center-hz 300 --q 0.8 --gain-db -3 --output-dir py_rec/res_1126_spectrum_20261127/peq_preview/results_q08

浏览器下载的 config_custom.json 请放到本目录，再传 --config 路径。
配置中的相对输入、GT 和输出目录均相对于该配置文件所在目录。
同一输出目录再次运行会更新生成结果；如需保留上一版，请指定新的 --output-dir。

测试：
python -m unittest discover -s py_rec/res_1126_spectrum_20261127/peq_preview -p test_peq.py -v

5. 频谱与判断边界
results/figures 中包含响应、7 组中位数/IQR、各组频谱，PNG/PDF/SVG。
默认 PEQ 的原始功率谱与显示数据见 spectra.csv；各增益理论响应见 filter_response.csv。
频谱来自恒定音量调整前的信号；同一编号的所有 raw 派生版本共用原始 raw 的
500–2000 Hz 能量参照，保留实际 PEQ 削减形状。GT 单独用自身参照归一化。
Welch：Hann 2048 点、步长 512 点；显示采用相同 1/12 倍频程平滑。
统计以 7 份录音等权计算中位数与 25%–75% 四分位区间；不作显著性检验。
GT 是同一人另一次朗读，不能把谱差当作腔体传递函数。
PEQ 可缓解低中频过重，不能移动谐波、恢复丢失信息或保证消除腔体拖尾。
判断重点是等响试听的闷厚感、清晰度和自然度，不能只看峰是否变尖。
本轮不修改 singlemic、接收器、原始录音或此前的试听结果。
