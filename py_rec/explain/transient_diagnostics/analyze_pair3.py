"""Diagnose transients in pair 3 / 350 Hz without changing source audio.

Excerpt players preserve whole-recording -18 LUFS matching. They are not
independently normalized. Identical 5 ms fades apply only at excerpt edges.
"""
from pathlib import Path
import csv
import html
import json
import sys

import numpy as np
from scipy import signal

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
HELPERS = ROOT / 'py_rec/res_1126_spectrum_20261127/peq_preview'
sys.path.insert(0, str(HELPERS))
from simulate_peq import baseline_sos, measure, peq_sos, read_wav, sha, write_csv, write_pcm24


def main():
    fs = 16000
    raw_path = ROOT / 'py_rec/explain/raw/3_raw.wav'
    before_path = ROOT / 'py_rec/explain/peq_shift_preview/pair_03/3_no_peq_matched.wav'
    processed_path = ROOT / 'py_rec/explain/peq_shift_preview/pair_03/3_peq350Hz_matched.wav'
    protected_files = list((ROOT / 'singlemic').glob('*'))
    protected_files += [raw_path, before_path, processed_path]
    protected_files += list((ROOT / 'py_rec/explain/processed').glob('*.wav'))
    protected = {str(p): sha(p) for p in protected_files if p.is_file()}
    x = read_wav(raw_path, fs)
    before = read_wav(before_path, fs)
    y = read_wav(processed_path, fs)
    metrics = list(csv.DictReader((ROOT / 'py_rec/explain/peq_shift_preview/preview_metrics.csv').open(encoding='utf-8-sig')))
    row0 = next(r for r in metrics if r['pair'] == '3' and r['variant'] == 'no_peq')
    row1 = next(r for r in metrics if r['pair'] == '3' and r['variant'] == 'peq350Hz')
    makeup_difference = float(row1['constant_gain_db']) - float(row0['constant_gain_db'])
    raw_lufs, raw_tp = measure(x, fs)
    raw_gain = float(row1['target_lufs']) - raw_lufs
    raw_matched = x * 10**(raw_gain/20)
    versions = {'raw': raw_matched, 'no_peq': before, 'peq350': y}
    highpass = signal.butter(4, 2000, btype='highpass', fs=fs, output='sos')
    highs = {name: signal.sosfilt(highpass, data) for name, data in versions.items()}
    # Deterministic diagnostic candidates, not perceptually confirmed artefacts.
    events = [('start', .2485, .05, .52, '开头强瞬态'),
              ('speech_1', 5.8885, 5.60, 6.20, '语音内突发候选一'),
              ('speech_2', 13.5445, 13.25, 13.85, '语音内突发候选二'),
              ('end', 18.5605, 18.20, len(x)/fs, '结尾强瞬态'),
              ('gap', .893, .45, 1.15, '补零段结束边界')]
    rows, cards = [], []
    labels = {'raw': '原始 raw（整段等响）', 'no_peq': '高通＋高频增强，不加 PEQ', 'peq350': '高通＋高频增强＋350 Hz PEQ'}
    for name, sec, start, end, title in events:
        index = int(round(sec*fs))
        a, b = max(0,index-80), min(len(x),index+80)
        for kind, data in versions.items():
            h = highs[kind]
            # Highest 1 ms RMS in a common +/-5 ms neighborhood.
            energy = np.convolve(h[a:b]**2, np.ones(16)/16, 'valid')
            rows.append({'event': name, 'time_s': sec, 'version': kind,
                         'highband_definition': '4th-order 2 kHz highpass, 1 ms RMS, maximum within +/-5 ms',
                         'highband_rms_dbfs': float(10*np.log10(max(energy.max(),1e-30))),
                         'sample_peak_dbfs_within_10ms': float(20*np.log10(max(np.max(abs(data[a:b])),1e-30)))})
        players = []
        folder = HERE / 'excerpts'
        folder.mkdir(exist_ok=True)
        for kind, data in versions.items():
            clip = data[round(start*fs):round(end*fs)].copy()
            fade = min(round(.005*fs), len(clip)//2)
            ramp = np.linspace(0, 1, fade)
            clip[:fade] *= ramp
            clip[-fade:] *= ramp[::-1]
            path = folder / f'3_{name}_{kind}.wav'
            write_pcm24(path, clip, fs)
            rel = path.relative_to(HERE).as_posix()
            players.append(f'<div><strong>{labels[kind]}</strong><audio controls preload="none" src="{rel}"></audio><a href="{rel}">WAV</a></div>')
        cards.append(f'<section><h2>{html.escape(title)} · 原录音约 {sec:.3f} 秒</h2><p>片段范围：{start:.3f}–{end:.3f} 秒。三个版本使用整段等响增益，片段不另行归一化。</p><div class="clips">'+''.join(players)+'</div></section>')
    write_csv(HERE / 'event_comparison.csv', rows)
    config = json.loads((HELPERS / 'config.json').read_text(encoding='utf8'))
    chain = baseline_sos(fs, config['baseline'])
    peq, params = peq_sos(fs, 350, -6, bandwidth_hz=300)
    reference = signal.sosfilt(peq, signal.sosfilt(chain, x))
    gain = np.dot(reference,y)/np.dot(reference,reference)
    error = y-reference*gain
    impulse = signal.sosfilt(peq, np.r_[1.,np.zeros(fs)])
    tail_indices = np.flatnonzero(abs(impulse[1:]) > .001)
    # PCM24 exact rail counts, not merely near-full samples.
    rails = int(np.count_nonzero((x == -1) | (x == 1-2**-23)))
    z = x == 0
    transitions = np.diff(np.r_[False,z,False].astype(int))
    zero_runs = [{'start_s': float(a/fs), 'end_s': float(b/fs), 'samples': int(b-a)}
                 for a,b in zip(np.flatnonzero(transitions==1),np.flatnonzero(transitions==-1)) if b-a>=76]
    # Check the repeatability of strong end transients over all seven raw files.
    end_events = []
    for i in range(1,8):
        raw = read_wav(ROOT/f'py_rec/explain/raw/{i}_raw.wav',fs)
        high = signal.sosfilt(highpass,raw)
        start = max(0,len(raw)-fs//2)
        energy = np.convolve(high[start:]**2,np.ones(16)/16,'valid')
        index = start+int(np.argmax(energy))
        end_events.append({'pair':i,'time_s':index/fs,'milliseconds_before_file_end':(len(raw)-index)/16})
    report = {'focus':'pair 3, PEQ350Hz, gain -6 dB, bandwidth 300 Hz',
              'raw_integrated_lufs':raw_lufs,'raw_true_peak_dbtp':raw_tp,
              'raw_gain_for_full_track_matching_db':raw_gain,
              'export_350_true_peak_dbtp':float(row1['export_true_peak_dbtp']),
              'extra_makeup_vs_no_peq_db':makeup_difference,
              'generation_error_db':float(20*np.log10(np.linalg.norm(error)/np.linalg.norm(y))),
              'max_generation_error_in_pcm24_steps':float(np.max(abs(error))*2**23),
              'peq':params,'max_peq_pole_radius':float(np.max(abs(np.roots(peq[0,3:])))),
              'peq_impulse_last_sample_above_minus60db_ms':float((tail_indices[-1]+1)/16),
              'raw_exact_rail_samples':rails,'zero_runs':zero_runs,'end_events_across_7_raw_recordings':end_events,
              'findings':[
                  'Strong start/end high-frequency transients are present in the original raw.',
                  'The high shelves amplify their high-frequency content; constant LUFS matching adds a further 2.3 dB relative to the no-PEQ chain for this recording.',
                  'No new clipping or random output corruption is supported by sample-accurate reconstruction.',
                  'The PEQ is stable, with a short decaying impulse tail; sustained high-frequency ringing is not supported.',
                  'Similar end events 37-50 ms before file end suggest recording-operation/mechanical transients, but waveform alone cannot prove a button or electrical source.',
                  'Internal speech bursts may include legitimate plosives/fricatives; exact perceptual identification needs matching the heard timestamp.',
                  'The listening page hard-pauses/seeks during A/B switching, so switching-only clicks should be distinguished from file-contained events.'
              ],
              'excerpt_policy':'Same whole-track LUFS target; no independent excerpt normalization; identical 5 ms edge fades only',
              'source_policy':'No firmware or source audio modifications'}
    for filename,digest in protected.items():
        assert sha(Path(filename))==digest
    report['protected_sha256']=protected
    (HERE/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
    page='''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>第3段350Hz瞬态诊断</title><style>body{max-width:1100px;margin:30px auto;padding:0 20px;font:15px/1.7 system-ui,Microsoft YaHei,sans-serif;background:#f5f8fa;color:#253640}section{padding:20px;margin:20px 0;background:white;border:1px solid #dce4e9;border-radius:10px}.clips{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:20px}audio{width:100%;display:block;margin:10px 0}a{color:#087894}h1{font-size:26px}h2{font-size:19px}</style><h1>第 3 段 · 350 Hz PEQ：瞬态定位对比</h1><p>比较原始录音、仅高通＋高频增强、再加 350 Hz PEQ。原始录音已按整段 −18 LUFS 匹配；其余两份原本即为 −18 LUFS，片段不再单独拉响。</p><p>强瞬态候选不等于已经确认的人耳爆鸣；先听开头与结尾，再核对语音内部位置。片段两端统一作 5 ms 淡入淡出，避免截取边界自己产生点击声；中间事件未修复。</p>'''+''.join(cards)+'''<script>document.addEventListener('play',e=>{if(e.target.tagName==='AUDIO')document.querySelectorAll('audio').forEach(a=>{if(a!==e.target)a.pause()})},true)</script></html>'''
    (HERE/'listen.html').write_text(page,encoding='utf8')
    print(json.dumps({k:report[k] for k in ['extra_makeup_vs_no_peq_db','export_350_true_peak_dbtp','generation_error_db','max_generation_error_in_pcm24_steps','max_peq_pole_radius','peq_impulse_last_sample_above_minus60db_ms','raw_exact_rail_samples']},indent=2),flush=True)
    print('Created diagnostic report, 15 excerpts and listening page:',HERE,flush=True)


if __name__=='__main__':
    main()
