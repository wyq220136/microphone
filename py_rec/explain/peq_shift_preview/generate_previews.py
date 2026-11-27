"""Move only the PEQ centre; preserve original audio and firmware.

Reuses the established offline RBJ PEQ and five-section baseline. Every input
is processed from raw once; no PEQ is stacked on already processed audio.
"""
from pathlib import Path
import concurrent.futures
import csv
import html
import json
import sys
import wave

import numpy as np
from scipy import signal

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
HELPERS = ROOT / 'py_rec/res_1126_spectrum_20261127/peq_preview'
sys.path.insert(0, str(HELPERS))
from simulate_peq import baseline_sos, measure, peq_sos, read_wav, sha, write_csv, write_pcm24


def main():
    config = json.loads((HERE / 'config.json').read_text(encoding='utf-8-sig'))
    fs = config['sample_rate_hz']
    baseline_config = json.loads((HELPERS / 'config.json').read_text(encoding='utf-8-sig'))['baseline']
    chain = baseline_sos(fs, baseline_config)
    peqs, parameters = {}, []
    response_rows = []
    freq = np.geomspace(20, fs/2*.999, 1600)
    for center in config['centers_hz']:
        sos, p = peq_sos(fs, center, config['gain_db'], bandwidth_hz=config['bandwidth_hz'])
        assert np.abs(np.roots(sos[0, 3:])).max() < 1
        _, h = signal.sosfreqz(sos, worN=[187.5, 200, 218.75, center], fs=fs)
        p['gain_at_187p5_hz_db'], p['gain_at_200_hz_db'], p['gain_at_218p75_hz_db'] = (20*np.log10(abs(h[:3]))).tolist()
        assert abs(20*np.log10(abs(h[-1])) - config['gain_db']) < 1e-8
        assert abs(p['bandwidth_hz'] - config['bandwidth_hz']) < 1e-8
        parameters.append(p)
        peqs[f'peq{center:g}Hz'] = sos
        _, h = signal.sosfreqz(sos, worN=freq, fs=fs)
        response_rows.extend({'center_hz': center, 'frequency_hz': f, 'peq_gain_db': db}
                             for f, db in zip(freq, 20*np.log10(abs(h))))
    protected_paths = list((ROOT / 'singlemic').glob('*'))
    protected_paths += list((ROOT / 'py_rec/explain/raw').glob('*.wav'))
    protected_paths += list((ROOT / 'py_rec/explain/processed').glob('*.wav'))
    protected_paths += list((ROOT / 'res_1126/raw').glob('*.wav'))
    protected_paths += list((ROOT / 'res_1126/gt').glob('*.m4a'))
    protected = {str(p): sha(p) for p in protected_paths if p.is_file()}
    metrics, cards, band_rows = [], [], []
    max_spread = 0
    sources = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=config['workers']) as pool:
        for number in range(1, 8):
            src = ROOT / f'py_rec/explain/raw/{number}_raw.wav'
            x = read_wav(src, fs)
            base = signal.sosfilt(chain, x)
            versions = {'no_peq': base} if config['include_no_peq_control'] else {}
            versions.update({name: signal.sosfilt(sos, base) for name, sos in peqs.items()})
            levels = dict(zip(versions, pool.map(lambda y: measure(y, fs), versions.values())))
            target = min([config['target_lufs']] + [lu + config['true_peak_ceiling_dbtp'] - tp
                                                   for lu, tp in levels.values()])
            folder = HERE / f'pair_{number:02d}'
            folder.mkdir(exist_ok=True)
            destinations = {}
            for kind, y in versions.items():
                dest = folder / f'{number}_{kind}_matched.wav'
                write_pcm24(dest, y*10**((target-levels[kind][0])/20), fs)
                destinations[kind] = dest
            measured = dict(zip(destinations, pool.map(lambda p: measure(read_wav(p, fs), fs), destinations.values())))
            spread = max(v[0] for v in measured.values()) - min(v[0] for v in measured.values())
            assert spread <= .12
            max_spread = max(max_spread, spread)
            f, baseline_psd = signal.welch(base, fs=fs, window='hann', nperseg=2048, noverlap=1536)
            for kind, y in versions.items():
                dest = destinations[kind]
                exported = read_wav(dest, fs)
                with wave.open(str(dest), 'rb') as stream:
                    assert stream.getnframes() == len(x) and stream.getsampwidth() == 3
                assert len(exported) == len(x) and measured[kind][1] <= config['true_peak_ceiling_dbtp'] + .11
                assert not np.any((exported == -1) | (exported == 1-2**-23))
                metrics.append({'pair': number, 'variant': kind, 'file': dest.relative_to(HERE).as_posix(),
                                'center_hz': 0 if kind == 'no_peq' else parameters[list(peqs).index(kind)]['center_hz'],
                                'gain_db': 0 if kind == 'no_peq' else config['gain_db'],
                                'bandwidth_hz': 0 if kind == 'no_peq' else config['bandwidth_hz'],
                                'input_lufs': levels[kind][0], 'input_true_peak_dbtp': levels[kind][1],
                                'constant_gain_db': target-levels[kind][0], 'target_lufs': target,
                                'export_lufs': measured[kind][0], 'export_true_peak_dbtp': measured[kind][1],
                                'samples': len(x), 'sha256': sha(dest)})
                _, psd = signal.welch(y, fs=fs, window='hann', nperseg=2048, noverlap=1536)
                for low, high in [(150,250),(200,500),(350,650)]:
                    mask = (f >= low) & (f < high)
                    band_rows.append({'pair': number, 'variant': kind, 'low_hz': low, 'high_hz': high,
                                      'change_vs_no_peq_db_before_level_matching': float(10*np.log10(psd[mask].sum()/baseline_psd[mask].sum()))})
            # Validate the old 300 Hz response against the existing processed file.
            old_file = ROOT / f'py_rec/explain/processed/{number}_processed.wav'
            old = read_wav(old_file, fs)
            calculated = versions['peq300Hz']
            scale = np.dot(calculated, old) / np.dot(calculated, calculated)
            error_db = float(20*np.log10(np.linalg.norm(calculated*scale-old)/np.linalg.norm(old)))
            assert error_db < -90
            sources.append({'pair': number, 'source': src.relative_to(ROOT).as_posix(), 'source_sha256': sha(src),
                            'original_300hz_control_error_db_after_constant_gain': error_db})
            players = []
            order = ['peq300Hz'] + [k for k in peqs if k != 'peq300Hz'] + (['no_peq'] if 'no_peq' in versions else [])
            for kind in order:
                label = '不加 PEQ（高通＋高频增强）' if kind == 'no_peq' else (
                    '原方案：300 Hz' if kind == 'peq300Hz' else f'中心上移：{kind[3:-2]} Hz')
                rel = destinations[kind].relative_to(HERE).as_posix()
                players.append(f'<div class="clip"><strong>{html.escape(label)}</strong><audio controls preload="none" src="{rel}"></audio><a href="{rel}">打开 WAV</a></div>')
            cards.append(f'<section><h2>第 {number} 段</h2><div class="grid">'+''.join(players)+'</div></section>')
            print(f'Pair {number}: {len(versions)} exports, target {target:.2f} LUFS, spread {spread:.2f} LU', flush=True)
    write_csv(HERE / 'parameters.csv', parameters)
    write_csv(HERE / 'frequency_response.csv', response_rows)
    write_csv(HERE / 'preview_metrics.csv', metrics)
    write_csv(HERE / 'band_changes.csv', band_rows)
    for filename, digest in protected.items():
        assert sha(Path(filename)) == digest
    manifest = {'sample_rate_hz': fs, 'recordings': 7, 'exports': len(metrics), 'config': config,
                'baseline': baseline_config, 'effective_parameters': parameters,
                'max_group_lufs_spread': max_spread,
                'max_true_peak_dbtp': max(r['export_true_peak_dbtp'] for r in metrics),
                'sources': sources, 'protected_sha256': protected,
                'processing': 'Causal sosfilt, same baseline chain, only PEQ centre changes. Each file starts from raw once.',
                'level_matching': 'One LUFS target per numbered group, constant gain only; no limiter, compressor, AGC or denoiser',
                'source_integrity': 'All original/raw/current processed audio and singlemic top-level files unchanged',
                'boundary': 'Higher centre reduces first-peak attenuation, but does not fully bypass that band or preserve every harmonic'}
    (HERE / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf8')
    rows = ''.join(f'<tr><td>{p["center_hz"]:g} Hz</td><td>{p["half_gain_low_hz"]:.0f}–{p["half_gain_high_hz"]:.0f} Hz</td><td>{p["gain_at_200_hz_db"]:.2f} dB</td></tr>' for p in parameters)
    page = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>PEQ 中心上移试听</title><style>
    body{max-width:1200px;margin:30px auto;padding:0 20px;font:15px/1.75 system-ui,Microsoft YaHei,sans-serif;background:#f5f8fa;color:#253640}h1{font-size:27px}h2{font-size:20px}section{padding:20px;margin:20px 0;background:white;border:1px solid #dce4e9;border-radius:10px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:18px}.clip strong{display:block;min-height:3em}audio{width:100%;display:block;margin:8px 0}a{color:#087894}table{border-collapse:collapse;background:white;width:100%;max-width:650px}td,th{padding:8px 14px;border:1px solid #dce4e9;text-align:left}.note{padding:14px;background:#e8f2f7;border-radius:8px}</style>'''
    page += '<h1>PEQ 中心上移：300 / 350 / 400 / 450 Hz</h1><p class="note">高通与高频增强保持一致；PEQ 带宽固定 300 Hz，中心削减固定 −6 dB，只改变中心频率。每段 5 个版本已作 LUFS 等响匹配。</p>'
    page += '<p>建议先比较原方案 300 Hz 与 400 Hz，再听 450 Hz：关注第一主峰附近声音的厚度是否保留、闷感是否仍得到控制。中心上移仍会影响其他谐波，不等于完全避开所有主峰。</p>'
    page += '<table><tr><th>中心频率</th><th>−3 dB 边界（半增益带宽）</th><th>200 Hz 处实际削减</th></tr>'+rows+'</table>'
    page += '<p>切换同一段正在播放的不同版本时，会延续播放位置。所有音频均为 16 kHz、单声道 PCM24。未改变固件和旧处理文件。</p>'+''.join(cards)
    page += '''<script>document.addEventListener('play',e=>{if(e.target.tagName!=='AUDIO')return;const a=e.target;const section=a.closest('section');let time=null;document.querySelectorAll('audio').forEach(b=>{if(b!==a&&!b.paused){if(b.closest('section')===section)time=b.currentTime;b.pause();}});if(time!==null){const seek=()=>{if(time<a.duration)a.currentTime=time;};if(a.readyState>=1)seek();else a.addEventListener('loadedmetadata',seek,{once:true});}},true);</script></html>'''
    (HERE / 'listen.html').write_text(page, encoding='utf8')
    note = '''PEQ 中心上移试听
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
'''
    (HERE / 'README.txt').write_text(note, encoding='utf8')
    print('Completed:', HERE, flush=True)


if __name__ == '__main__':
    main()
