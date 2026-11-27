PEQ 中心上移试听
每组 5 个版本：原 300 Hz、350 Hz、400 Hz、450 Hz、不加 PEQ 对照。
高通为 70 Hz 二阶＋50 Hz 四阶 Butterworth；高架为 1.5 kHz +3 dB、3 kHz +6 dB。
PEQ 带宽固定 300 Hz，中心增益固定 -6 dB。带宽按 -3 dB 两侧频点的频差定义。
只改变 PEQ 中心，避免把带宽或削减量变化混入比较。移中心可以减轻对第一低频主峰
附近的衰减，但不会完全消除 PEQ 的裙边，也不代表避开所有人声谐波。
同编号全组匹配至一个共同 LUFS 目标；必要时整组降低，真峰值不高于 -1 dBTP。
无动态压缩、AGC、限幅或降噪。频率响应和带宽计算见 parameters.csv / frequency_response.csv。
band_changes.csv 的频带变化在等响匹配前计算，反映滤波器对频谱的实际作用。
现有 raw、processed 和原始录音均未改动；singlemic 未改动。
如需重新生成，可在项目根目录运行：
python py_rec/explain/peq_shift_preview/generate_previews.py
