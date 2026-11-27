"""Configurable causal PEQ previews. Original audio and firmware are read-only.

Figure contract: compare the added PEQ within each of two processing branches;
do not infer a cavity transfer function from separately spoken raw / GT takes.
Quantitative grid, Python, 183 mm wide, >=7.5 pt, PNG/PDF/SVG plus CSV.
All seven recordings are included; summary median/IQR uses recordings as units.
"""
from pathlib import Path
import argparse
import concurrent.futures
import csv
import hashlib
import html
import json
import re
import subprocess
import wave

import numpy as np
from scipy import signal
from scipy.io import wavfile

HERE = Path(__file__).resolve().parent
FORMULA_SOURCE = 'https://www.w3.org/TR/audio-eq-cookbook/'


def peq_sos(fs, center_hz, gain_db, bandwidth_hz=None, q=None):
    """RBJ peaking EQ; Hz bandwidth is between the gain_db/2 crossings.

    Specify exactly one of bandwidth_hz and Q. Bandwidth is not a brick-wall
    coverage interval, and its geometric centre is warped by the digital BLT.
    """
    values = [fs, center_hz, gain_db]
    if not all(np.isfinite(v) for v in values) or not 0 < center_hz < fs / 2:
        raise ValueError('Require finite values and 0 < center_hz < fs/2')
    if (bandwidth_hz is None) == (q is None):
        raise ValueError('Specify exactly one of bandwidth_hz and q')
    w = 2 * np.pi * center_hz / fs
    if bandwidth_hz is not None:
        if not np.isfinite(bandwidth_hz) or not 0 < bandwidth_hz < fs / 2:
            raise ValueError('Require 0 < bandwidth_hz < fs/2')
        alpha = np.tan(np.pi * bandwidth_hz / fs)
        q = np.sin(w) / (2 * alpha)
    else:
        if not np.isfinite(q) or q <= 0:
            raise ValueError('q must be finite and positive')
        alpha = np.sin(w) / (2 * q)
    a = 10 ** (gain_db / 40)
    b = np.array([1 + alpha * a, -2 * np.cos(w), 1 - alpha * a])
    d = np.array([1 + alpha / a, -2 * np.cos(w), 1 - alpha / a])
    sos = np.r_[b / d[0], d / d[0]].reshape(1, 6)
    # Half-gain frequencies: warped normalized frequencies u obey uH*uL=1.
    u_high = (np.sqrt(4 + 1 / q**2) + 1 / q) / 2
    t0 = np.tan(w / 2)
    low = fs / np.pi * np.arctan(t0 / u_high)
    high = fs / np.pi * np.arctan(t0 * u_high)
    return sos, {'center_hz': center_hz, 'gain_db': gain_db, 'q': float(q),
                 'bandwidth_hz': float(high - low), 'half_gain_low_hz': float(low),
                 'half_gain_high_hz': float(high),
                 'bandwidth_definition': 'between gain_db/2 crossings; nominal when gain_db=0'}


def high_shelf(fs, frequency_hz, gain_db):
    a = 10 ** (gain_db / 40)
    w = 2 * np.pi * frequency_hz / fs
    c = np.cos(w)
    v = np.sqrt(2 * a) * np.sin(w)  # shelf slope S=1
    b = a * np.array([(a + 1) + (a - 1)*c + v,
                      -2*((a - 1) + (a + 1)*c), (a + 1) + (a - 1)*c - v])
    d = np.array([(a + 1) - (a - 1)*c + v,
                  2*((a - 1) - (a + 1)*c), (a + 1) - (a - 1)*c - v])
    return np.r_[b / d[0], d / d[0]].reshape(1, 6)


def baseline_sos(fs, config):
    parts = []
    for p in config['highpasses']:
        parts.append(signal.butter(p['order'], p['frequency_hz'], btype='highpass', fs=fs, output='sos'))
    for p in config['high_shelves']:
        parts.append(high_shelf(fs, p['frequency_hz'], p['gain_db']))
    return np.vstack(parts) if parts else np.array([[1, 0, 0, 1, 0, 0.]])


def measure(x, fs):
    result = subprocess.run(['ffmpeg', '-hide_banner', '-nostats', '-f', 'f32le',
                             '-ar', str(fs), '-ac', '1', '-i', 'pipe:0', '-af',
                             'loudnorm=I=-18:TP=-1:LRA=11:print_format=json', '-f', 'null', '-'],
                            input=np.asarray(x, dtype='<f4').tobytes(), capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr.decode('utf8', errors='replace')[-1500:])
    data = json.loads(re.findall(r'\{[\s\S]*?\}', result.stderr.decode('utf8', errors='replace'))[-1])
    values = float(data['input_i']), float(data['input_tp'])
    if not all(np.isfinite(v) for v in values):
        raise ValueError('Input must contain measurable, non-silent audio')
    return values


def write_pcm24(path, x, fs):
    q = np.rint(np.asarray(x) * 2**23).astype(np.int64)
    if q.min() < -2**23 or q.max() >= 2**23:
        raise ValueError('PCM24 overflow: output is not silently clipped')
    packed = np.column_stack([q & 255, (q >> 8) & 255, (q >> 16) & 255]).astype(np.uint8)
    with wave.open(str(path), 'wb') as stream:
        stream.setparams((1, 3, fs, 0, 'NONE', 'not compressed'))
        stream.writeframes(packed.tobytes())


def read_wav(path, fs):
    rate, data = wavfile.read(path)
    if rate != fs or data.ndim != 1:
        raise ValueError(f'{path.name}: require {fs} Hz mono WAV')
    if data.dtype == np.int32:
        return data.astype(float) / 2**31
    if data.dtype == np.int16:
        return data.astype(float) / 2**15
    if np.issubdtype(data.dtype, np.floating):
        return data.astype(float)
    raise ValueError(f'Unsupported PCM container: {data.dtype}')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_csv(path, rows):
    with path.open('w', newline='', encoding='utf-8-sig') as stream:
        writer = csv.DictWriter(stream, list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def spectrum(x, fs):
    return signal.welch(x, fs=fs, window='hann', nperseg=2048, noverlap=1536, detrend='constant')


def make_figures(out, fs, spectra, responses, params):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter, NullFormatter
    plt.rcParams.update({'font.family': 'sans-serif', 'font.sans-serif': ['Microsoft YaHei', 'Arial', 'DejaVu Sans'],
                         'font.size': 8, 'axes.titlesize': 9, 'axes.labelsize': 8,
                         'xtick.labelsize': 7.5, 'ytick.labelsize': 7.5, 'legend.fontsize': 7.5,
                         'axes.spines.top': False, 'axes.spines.right': False,
                         'legend.frameon': False, 'pdf.fonttype': 42, 'svg.fonttype': 'none',
                         'axes.unicode_minus': False})
    folder = out / 'figures'
    folder.mkdir(exist_ok=True)
    colors = {'raw': '#777777', 'raw_peq': '#0077A2', 'chain': '#777777',
              'chain_peq': '#8B4BA7', 'gt': '#171717'}
    labels = {'raw': 'Raw', 'raw_peq': 'Raw + PEQ', 'chain': '低切 + 高频增强',
              'chain_peq': '低切 + 高频增强 + PEQ', 'gt': 'GT（另一次朗读）'}
    def save(fig, name):
        fig.savefig(folder / f'{name}.png', dpi=300)
        fig.savefig(folder / f'{name}.pdf')
        fig.savefig(folder / f'{name}.svg')
        plt.close(fig)
    def format_ax(ax, lo, hi):
        ax.set_xscale('log')
        ax.set_xlim(lo, hi)
        ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f'{x/1000:g}k' if x >= 1000 else f'{x:g}'))
        ax.xaxis.set_minor_formatter(NullFormatter())
        ax.grid(alpha=.18, which='major')
        ax.set_xlabel('频率（Hz）')
    freq = np.geomspace(20, fs/2*.999, 1600)
    fig, axes = plt.subplots(1, 2, figsize=(7.2047244094, 3.2677165354), layout='constrained')
    for name, sos in responses.items():
        _, h = signal.sosfreqz(sos, worN=freq, fs=fs)
        db = 20*np.log10(np.maximum(np.abs(h), 1e-20))
        axes[0 if name.startswith('PEQ') else 1].plot(freq, db, label=name, linewidth=1.4)
    for i, ax in enumerate(axes):
        format_ax(ax, 40 if i == 0 else 20, 2000 if i == 0 else 8000)
        ax.set_ylabel('滤波增益（dB）')
        ax.set_title(('a  PEQ：带宽与削减量', 'b  现有滤波链叠加 PEQ')[i])
        ax.legend(loc='lower right' if i == 0 else 'lower right')
    axes[0].axvline(params['half_gain_low_hz'], color='#aaaaaa', linestyle=':', linewidth=.8)
    axes[0].axvline(params['half_gain_high_hz'], color='#aaaaaa', linestyle=':', linewidth=.8)
    fig.suptitle(f"中心 {params['center_hz']:g} Hz · 半增益带宽 {params['bandwidth_hz']:.0f} Hz", fontsize=10)
    save(fig, '01_filter_response')

    # Common normalization within each raw pair: preserve the actual PEQ dip.
    # GT has its own raw 500-2000 Hz reference because it is a separate device/take.
    normalized = {}
    source_rows = []
    for pair, items in spectra.items():
        f = items['frequency_hz']
        df = f[1]-f[0]
        mask = (f >= 500) & (f < 2000)
        ref = np.sum(items['raw'][mask])*df
        gt_ref = np.sum(items['gt'][mask])*df
        assert ref > 0 and gt_ref > 0 and np.all(np.diff(f) > 0)
        normalized[pair] = {}
        for kind in ['raw', 'raw_peq', 'chain', 'chain_peq', 'gt']:
            power = items[kind] / (gt_ref if kind == 'gt' else ref)
            # Same 1/12-octave display smoothing for all curves. Full PSD is saved.
            smooth = np.array([np.mean(power[(f >= x/2**(1/24)) & (f <= x*2**(1/24))])
                               if x else power[0] for x in f])
            normalized[pair][kind] = 10*np.log10(np.maximum(smooth, 1e-30))
            for j in range(len(f)):
                source_rows.append({'pair': pair, 'version': kind, 'frequency_hz': f[j],
                                    'psd': items[kind][j], 'normalized_psd': power[j],
                                    'display_db': normalized[pair][kind][j]})
    write_csv(out / 'spectra.csv', source_rows)

    def plot_comparison(data, name, title, spread=False):
        fig, axes = plt.subplots(2, 2, figsize=(7.2047244094, 5.4330708661), layout='constrained')
        for col, kinds in enumerate([['raw', 'raw_peq', 'gt'], ['chain', 'chain_peq', 'gt']]):
            for row in range(2):
                ax = axes[row, col]
                for kind in kinds:
                    y = data[kind]
                    if spread:
                        low, mid, high = np.percentile(y, [25, 50, 75], axis=0)
                        ax.fill_between(f, low, high, color=colors[kind], alpha=.10, linewidth=0)
                    else:
                        mid = y
                    ax.plot(f, mid, color=colors[kind], label=labels[kind],
                            linestyle='--' if kind in ['raw', 'chain'] else '-', linewidth=1.3)
                format_ax(ax, 20 if row == 0 else 100, 8000 if row == 0 else 1000)
                ax.set_ylabel('相对功率谱密度（dB/Hz）')
                ax.set_title(('a', 'b', 'c', 'd')[row*2+col] + '  ' +
                             ('原音分支' if col == 0 else '现有滤波链分支') + ('：全频段' if row == 0 else '：低中频'))
                ax.legend(loc='lower left')
        fig.suptitle(title, fontsize=10)
        save(fig, name)
    summary = {k: np.stack([v[k] for v in normalized.values()]) for k in normalized[1]}
    plot_comparison(summary, '02_spectral_summary', '7 组录音：中位数与四分位区间 · 默认 PEQ', True)
    for pair, data in normalized.items():
        plot_comparison(data, f'pair_{pair:02d}_spectra', f'第 {pair} 组：加入默认 PEQ 前后')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=HERE / 'config.json')
    parser.add_argument('--center-hz', type=float)
    width = parser.add_mutually_exclusive_group()
    width.add_argument('--bandwidth-hz', type=float)
    width.add_argument('--q', type=float)
    parser.add_argument('--gain-db', type=float)
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    config_path = args.config.resolve()
    config = json.loads(config_path.read_text(encoding='utf-8-sig'))
    config['peq'] = dict(config['peq'])
    for key, value in [('center_hz', args.center_hz), ('gain_db', args.gain_db)]:
        if value is not None:
            config['peq'][key] = value
    if args.q is not None:
        config['peq'].pop('bandwidth_hz', None)
        config['peq']['q'] = args.q
    elif args.bandwidth_hz is not None:
        config['peq'].pop('q', None)
        config['peq']['bandwidth_hz'] = args.bandwidth_hz
    fs = config['sample_rate_hz']
    eq, params = peq_sos(fs, **config['peq'])
    chain = baseline_sos(fs, config['baseline'])
    gains = list(dict.fromkeys([config['peq']['gain_db']] + config['comparison_gains_db']))
    if not all(-24 <= g <= 12 and np.isfinite(g) for g in gains):
        raise ValueError('Preview gain must be between -24 and +12 dB')
    if not np.isfinite(config['target_lufs']) or not np.isfinite(config['true_peak_ceiling_dbtp']):
        raise ValueError('Loudness and peak targets must be finite')
    source_dir = (config_path.parent / config['input_dir']).resolve()
    gt_dir = (config_path.parent / config['gt_dir']).resolve()
    out = args.output_dir.resolve() if args.output_dir else (config_path.parent / config['output_dir']).resolve()
    if out == source_dir or source_dir.is_relative_to(out):
        raise ValueError('Output must not be the input directory or one of its ancestors')
    out.mkdir(parents=True, exist_ok=True)
    files = sorted(source_dir.glob('*.wav'))
    if not files:
        raise ValueError('No input WAVs found')
    firmware = HERE.parents[2] / 'singlemic'
    protected = list(firmware.glob('*')) + list(source_dir.glob('*.wav')) + list(gt_dir.glob('*.m4a'))
    protected = {str(p): sha(p) for p in protected if p.is_file()}
    responses = {f'PEQ {g:g} dB': peq_sos(fs, **{**config['peq'], 'gain_db': g})[0] for g in gains}
    responses.update({'原有滤波链': chain, '滤波链 + 默认 PEQ': np.vstack([chain, eq])})
    assert all(np.abs(np.roots(row[3:])).max() < 1 for s in responses.values() for row in s)
    freq = np.geomspace(20, fs/2*.999, 1600)
    response_rows = []
    for name, sos in responses.items():
        _, h = signal.sosfreqz(sos, worN=freq, fs=fs)
        response_rows.extend({'version': name, 'frequency_hz': f, 'gain_db': g}
                             for f, g in zip(freq, 20*np.log10(np.maximum(abs(h), 1e-30))))
    write_csv(out / 'filter_response.csv', response_rows)
    metrics, cards, spectra, band_rows = [], [], {}, []
    max_spread = 0
    default_gain = config['peq']['gain_db']
    labels = {'raw': '原音对照', 'chain': '低切 + 高频增强（对照）'}
    with concurrent.futures.ThreadPoolExecutor(max_workers=config.get('workers', 4)) as pool:
        for pair, path in enumerate(files, 1):
            x = read_wav(path, fs)
            base = signal.sosfilt(chain, x)
            versions = {'raw': x, 'chain': base}
            for branch, data in [('raw', x), ('chain', base)]:
                for gain in gains:
                    key = f'{branch}_peq_{gain:g}dB'.replace('-', 'minus').replace('.', 'p')
                    versions[key] = signal.sosfilt(peq_sos(fs, **{**config['peq'], 'gain_db': gain})[0], data)
                    labels[key] = ('原音' if branch == 'raw' else '低切 + 高频增强') + f' + PEQ {gain:g} dB'
            measured = dict(zip(versions, pool.map(lambda v: measure(v, fs), versions.values())))
            # Common target for every version in this pair, no compressor or limiter.
            target = min([config['target_lufs']] + [lu + config['true_peak_ceiling_dbtp'] - tp
                                                   for lu, tp in measured.values()])
            pair_dir = out / f'pair_{pair:02d}'
            pair_dir.mkdir(exist_ok=True)
            pair_paths = {}
            for key, y in versions.items():
                lu, tp = measured[key]
                gain = target - lu
                dest = pair_dir / f'{key}_matched.wav'
                write_pcm24(dest, y*10**(gain/20), fs)
                pair_paths[key] = dest
            exported = dict(zip(pair_paths, pool.map(lambda p: measure(read_wav(p, fs), fs), pair_paths.values())))
            spread = max(v[0] for v in exported.values()) - min(v[0] for v in exported.values())
            assert spread <= .12
            max_spread = max(max_spread, spread)
            for key, dest in pair_paths.items():
                lu, tp = exported[key]
                y = read_wav(dest, fs)
                assert tp <= config['true_peak_ceiling_dbtp'] + .11 and len(y) == len(x)
                metrics.append({'pair': pair, 'version': key, 'file': dest.relative_to(out).as_posix(),
                                'target_lufs': target, 'input_lufs': measured[key][0],
                                'constant_gain_db': target-measured[key][0], 'export_lufs': lu,
                                'export_true_peak_dbtp': tp, 'samples': len(y), 'sha256': sha(dest)})
            dk = f'_peq_{default_gain:g}dB'.replace('-', 'minus').replace('.', 'p')
            data = {'raw': x, 'raw_peq': versions['raw'+dk], 'chain': base,
                    'chain_peq': versions['chain'+dk]}
            gt_path = gt_dir / f'{pair}.m4a'
            gt_data = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(gt_path),
                                               '-f', 'f32le', '-ac', '1', '-ar', str(fs), 'pipe:1'])
            data['gt'] = np.frombuffer(gt_data, dtype='<f4').astype(float)
            spectra[pair] = {}
            for key, y in data.items():
                f, power = spectrum(y, fs)
                spectra[pair]['frequency_hz'] = f
                spectra[pair][key] = power
            for branch in ['raw', 'chain']:
                p0, p1 = spectra[pair][branch], spectra[pair][branch+'_peq']
                for lo, hi in [(100,200),(200,500),(500,1000),(1000,2000),(2000,4000),(4000,8000)]:
                    mask = (f >= lo) & (f < hi)
                    band_rows.append({'pair': pair, 'branch': branch, 'band_low_hz': lo, 'band_high_hz': hi,
                                      'peq_energy_change_db': float(10*np.log10(np.sum(p1[mask])/np.sum(p0[mask])))})
            def player(key):
                rel = pair_paths[key].relative_to(out).as_posix()
                return f'<div class="clip"><strong>{html.escape(labels[key])}</strong><audio controls preload="none" src="{rel}"></audio><a href="{rel}">WAV</a></div>'
            main_keys = ['raw', 'raw'+dk, 'chain', 'chain'+dk]
            extras = [k for k in versions if k not in main_keys]
            cards.append(f'<section><h2>第 {pair} 组</h2><div class="grid">'+''.join(player(k) for k in main_keys)+
                         '</div><details><summary>其他削减幅度</summary><div class="grid">'+''.join(player(k) for k in extras)+
                         f'</div></details><details><summary>频谱图</summary><img src="figures/pair_{pair:02d}_spectra.png" alt="第 {pair} 组频谱"></details></section>')
            print(f'Pair {pair}: {len(versions)} WAVs; {target:.2f} LUFS; spread {spread:.2f} LU', flush=True)
    make_figures(out, fs, spectra, responses, params)
    write_csv(out / 'preview_metrics.csv', metrics)
    write_csv(out / 'band_changes.csv', band_rows)
    config['effective_peq'] = params
    (out / 'effective_config.json').write_text(json.dumps(config, ensure_ascii=False, indent=2), encoding='utf8')
    (out / 'coefficients.json').write_text(json.dumps({k: v.tolist() for k, v in responses.items()}, ensure_ascii=False, indent=2), encoding='utf8')
    for name, digest in protected.items():
        assert sha(Path(name)) == digest, f'Protected file changed: {name}'
    manifest = {'recording_count': len(files), 'audio_exports': len(metrics), 'max_group_lufs_spread': max_spread,
                'max_true_peak_dbtp': max(r['export_true_peak_dbtp'] for r in metrics),
                'peq': params, 'causal_sosfilt': True, 'all_poles_inside_unit_circle': True,
                'loudness': 'one shared target per raw pair, constant gain only, no AGC/limiter/compression',
                'source_integrity': 'raw, GT and all singlemic top-level files unchanged',
                'protected_hashes': protected, 'formula_source': FORMULA_SOURCE,
                'branch_2': 'existing low-cut + high-frequency BOOST, not a low-pass',
                'spectrum': 'Welch Hann 2048, hop512; 1/12 octave display smoothing; raw variants share original raw 500-2000 Hz reference; GT normalized independently',
                'statistics': 'all seven recordings, equal-weight median and 25th-75th percentiles; no significance tests',
                'boundary': 'different speech takes, not measured cavity transfer function; source clipping and zero-fill not restored'}
    (out / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf8')
    page = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>PEQ 试听对比</title><style>
    body{max-width:1120px;margin:30px auto;padding:0 20px;font:15px/1.7 system-ui,Microsoft YaHei,sans-serif;color:#24313d;background:#f6f8fa}section{padding:20px;margin:20px 0;background:white;border:1px solid #dbe2e8;border-radius:10px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:18px}.clip strong,audio{display:block}audio{width:100%;margin:9px 0}a{color:#087899}img{max-width:100%}summary{cursor:pointer;margin:12px 0}.note{padding:15px;background:#e9f3f8;border-radius:8px}</style>'''
    page += f'<h1>PEQ：原音与现有滤波链的两组对比</h1><p class="note">默认：中心 {params["center_hz"]:g} Hz，带宽 {params["bandwidth_hz"]:.0f} Hz，削减 {abs(default_gain):g} dB。半增益频点约 {params["half_gain_low_hz"]:.0f}–{params["half_gain_high_hz"]:.0f} Hz；Q={params["q"]:.3f}。</p>'
    page += '<p>同组全部音频已作 LUFS 等响匹配，只调整恒定增益。第 2 分支使用既有低切＋高频增强。重点听闷厚感是否减少、声音是否变薄；峰位变化不能据此认定已修复。</p><p><a href="../peq_tuner.html">打开可调参数试听工具</a> · <a href="figures/01_filter_response.png">滤波响应</a> · <a href="figures/02_spectral_summary.png">7 组频谱汇总</a></p>'
    page += ''.join(cards) + '<script>document.addEventListener("play",e=>{if(e.target.tagName==="AUDIO")document.querySelectorAll("audio").forEach(a=>{if(a!==e.target)a.pause()})},true)</script></html>'
    (out / 'listen.html').write_text(page, encoding='utf8')
    print('Completed:', out, flush=True)


if __name__ == '__main__':
    main()
