# include "algorithm.h"
#include <cmath>
#include <vector>
#include <algorithm>

namespace IcsDSP {

namespace {
struct C { float r, i; C(float a=0, float b=0):r(a),i(b){} C operator+(C v)const{return C(r+v.r,i+v.i);} C operator-(C v)const{return C(r-v.r,i-v.i);} C operator*(C v)const{return C(r*v.r-i*v.i,r*v.i+i*v.r);} C operator*(float v)const{return C(r*v,i*v);} C& operator+=(C v){r+=v.r;i+=v.i;return *this;} C& operator*=(float v){r*=v;i*=v;return *this;} C conj()const{return C(r,-i);} };

void fft(std::vector<C> &x, bool inverse)
{
    const size_t n=x.size();
    for(size_t j=1,i=0;j<n;++j){size_t bit=n>>1;for(;i&bit;bit>>=1)i^=bit;i^=bit;if(j<i)std::swap(x[j],x[i]);}
    for(size_t len=2;len<=n;len<<=1){const float ang=(inverse?1.0f:-1.0f)*2.0f*PI/len;const C step(cosf(ang),sinf(ang));for(size_t p=0;p<n;p+=len){C w(1,0);for(size_t j=0;j<len/2;++j){C u=x[p+j],v=w*x[p+j+len/2];x[p+j]=u+v;x[p+j+len/2]=u-v;w=w*step;}}}
    if(inverse){const float s=1.0f/n;for(size_t i=0;i<n;++i)x[i]*=s;}
}
}

MVDRConfig::MVDRConfig() : frame_size(256), hop_size(128), sample_rate(16000.0f),
    microphone_spacing_m(0.05f), sound_speed_mps(343.0f), target_angle_deg(0.0f),
    covariance_alpha(0.90f), diagonal_loading(1.0e-3f) {}

bool mvdr_process(const float *input1, const float *input2, float *output,
                  size_t sample_count, const MVDRConfig &c)
{
    if(!input1||!input2||!output||!sample_count||c.frame_size<2||
       (c.frame_size&(c.frame_size-1))||!c.hop_size||c.hop_size>c.frame_size||
       c.sample_rate<=0||c.sound_speed_mps<=0||c.microphone_spacing_m<0||
       c.covariance_alpha<0||c.covariance_alpha>=1||c.diagonal_loading<0) return false;
    const size_t N=c.frame_size, K=N/2+1;
    const size_t F=sample_count<=N?1:1+(sample_count-N+c.hop_size-1)/c.hop_size;
    std::vector<float> win(N), norm(sample_count,0); std::vector<C> x(N),z(N),y(N),r00(K),r01(K),r11(K);
    for(size_t n=0;n<N;++n) win[n]=0.5f*(1-cosf(2*PI*n/(N-1)));
    std::fill(output,output+sample_count,0);
    const float tau=c.microphone_spacing_m*sinf(c.target_angle_deg*PI/180.0f)/c.sound_speed_mps;
    for(size_t f=0;f<F;++f){const size_t off=f*c.hop_size;
        for(size_t n=0;n<N;++n){size_t p=off+n;float w=win[n];x[n]=C((p<sample_count?input1[p]:0)*w);z[n]=C((p<sample_count?input2[p]:0)*w);} fft(x,false);fft(z,false);
        for(size_t k=0;k<K;++k){C p00=x[k]*x[k].conj(),p01=x[k]*z[k].conj(),p11=z[k]*z[k].conj();r00[k]=r00[k]*c.covariance_alpha+p00*(1-c.covariance_alpha);r01[k]=r01[k]*c.covariance_alpha+p01*(1-c.covariance_alpha);r11[k]=r11[k]*c.covariance_alpha+p11*(1-c.covariance_alpha);
            float ph=-2*PI*(c.sample_rate*k/N)*tau;C a0(1,0),a1(cosf(ph),sinf(ph));float load=c.diagonal_loading*std::max(r00[k].r+r11[k].r,1e-12f);float q00=r00[k].r+load,q11=r11[k].r+load;C q01=r01[k];float det=std::max(q00*q11-q01.r*q01.r-q01.i*q01.i,1e-12f);C u0=(a0*q11-q01*a1)*(1/det),u1=((q01.conj()*a0)*-1+a1*q00)*(1/det);C dc=a0.conj()*u0+a1.conj()*u1;float d=std::max(dc.r,1e-12f);C w0=u0*(1/d),w1=u1*(1/d);y[k]=w0.conj()*x[k]+w1.conj()*z[k];if(k&&k+1<N)y[N-k]=y[k].conj();}
        fft(y,true);for(size_t n=0;n<N;++n){size_t p=off+n;if(p<sample_count){output[p]+=y[n].r*win[n];norm[p]+=win[n]*win[n];}}
    }
    for(size_t n=0;n<sample_count;++n){if(norm[n]>1e-8f)output[n]/=norm[n];}
    return true;
}

WPEConfig::WPEConfig() : frame_size(256), hop_size(128), sample_rate(16000.0f),
    prediction_delay_frames(3), filter_taps(10), iterations(2),
    covariance_alpha(0.95f), regularization(1.0e-6f), power_floor(1.0e-8f) {}

namespace {

bool valid_wpe_config(const WPEConfig &c)
{
    return c.frame_size >= 2 &&
           (c.frame_size & (c.frame_size - 1)) == 0 &&
           c.hop_size > 0 && c.hop_size <= c.frame_size &&
           c.sample_rate > 0.0f && std::isfinite(c.sample_rate) &&
           c.prediction_delay_frames > 0 && c.filter_taps > 0 &&
           c.iterations > 0 && c.covariance_alpha >= 0.0f &&
           c.covariance_alpha < 1.0f && std::isfinite(c.covariance_alpha) &&
           c.regularization >= 0.0f &&
           std::isfinite(c.regularization) && c.power_floor > 0.0f &&
           std::isfinite(c.power_floor);
}

// Solve A*x=b in place using pivoted Gaussian elimination.  WPE filters are
// small (normally 5-20 taps), so this keeps the implementation dependency-free.
bool solve_linear_system(std::vector<C> a, std::vector<C> b,
                         std::vector<C> &x)
{
    const size_t n = b.size();
    if (a.size() != n * n) return false;
    const float epsilon = 1.0e-12f;
    for (size_t col = 0; col < n; ++col) {
        size_t pivot = col;
        float best = a[col * n + col].r * a[col * n + col].r +
                     a[col * n + col].i * a[col * n + col].i;
        for (size_t row = col + 1; row < n; ++row) {
            const C &v = a[row * n + col];
            const float magnitude = v.r * v.r + v.i * v.i;
            if (magnitude > best) {
                best = magnitude;
                pivot = row;
            }
        }
        if (best <= epsilon * epsilon) return false;
        if (pivot != col) {
            for (size_t j = col; j < n; ++j)
                std::swap(a[col * n + j], a[pivot * n + j]);
            std::swap(b[col], b[pivot]);
        }
        const C diagonal = a[col * n + col];
        const float denom = diagonal.r * diagonal.r + diagonal.i * diagonal.i;
        for (size_t j = col; j < n; ++j)
            a[col * n + j] = a[col * n + j] * diagonal.conj() * (1.0f / denom);
        b[col] = b[col] * diagonal.conj() * (1.0f / denom);
        for (size_t row = col + 1; row < n; ++row) {
            const C factor = a[row * n + col];
            for (size_t j = col; j < n; ++j)
                a[row * n + j] = a[row * n + j] - factor * a[col * n + j];
            b[row] = b[row] - factor * b[col];
        }
    }
    x.assign(n, C());
    for (size_t row = n; row-- > 0;) {
        C value = b[row];
        for (size_t j = row + 1; j < n; ++j)
            value = value - a[row * n + j] * x[j];
        x[row] = value;
    }
    return true;
}

} // namespace

bool wpe_process(const float *input, float *output, size_t sample_count,
                 const WPEConfig &c)
{
    if (!input || !output || sample_count == 0 || !valid_wpe_config(c))
        return false;

    const size_t nfft = c.frame_size;
    const size_t bins = nfft / 2 + 1;
    const size_t frames = sample_count <= nfft
        ? 1 : 1 + (sample_count - nfft + c.hop_size - 1) / c.hop_size;
    const size_t history = c.prediction_delay_frames + c.filter_taps;

    std::vector<float> window(nfft);
    // Periodic Hann avoids a zero-valued first sample in overlap-add output.
    for (size_t n = 0; n < nfft; ++n)
        window[n] = 0.5f * (1.0f - cosf(2.0f * PI * (n + 0.5f) / nfft));

    std::vector<C> spectra(frames * bins);
    std::vector<C> frame(nfft);
    for (size_t t = 0; t < frames; ++t) {
        const size_t offset = t * c.hop_size;
        for (size_t n = 0; n < nfft; ++n) {
            const size_t p = offset + n;
            frame[n] = C(p < sample_count ? input[p] * window[n] : 0.0f);
        }
        fft(frame, false);
        for (size_t k = 0; k < bins; ++k)
            spectra[t * bins + k] = frame[k];
    }

    std::vector<float> power(frames * bins, c.power_floor);
    std::vector<C> residual(frames * bins);
    for (size_t iteration = 0; iteration < c.iterations; ++iteration) {
        for (size_t t = 0; t < frames; ++t) {
            for (size_t k = 0; k < bins; ++k) {
                const C value = spectra[t * bins + k];
                const float energy = value.r * value.r + value.i * value.i;
                power[t * bins + k] = std::max(energy, c.power_floor);
            }
        }

        for (size_t k = 0; k < bins; ++k) {
            const size_t taps = c.filter_taps;
            std::vector<C> covariance(taps * taps, C());
            std::vector<C> cross(taps, C());
            for (size_t sample = history; sample < frames; ++sample) {
                const float weight = 1.0f / power[sample * bins + k];
                const C observed = spectra[sample * bins + k];
                for (size_t i = 0; i < taps; ++i) {
                    const C xi = spectra[(sample - c.prediction_delay_frames - i) * bins + k];
                    cross[i] += xi * observed.conj() * weight;
                    for (size_t j = 0; j < taps; ++j) {
                        const C xj = spectra[(sample - c.prediction_delay_frames - j) * bins + k];
                        covariance[i * taps + j] += xi * xj.conj() * weight;
                    }
                }
            }
            float diagonal_scale = 0.0f;
            for (size_t i = 0; i < taps; ++i)
                diagonal_scale += covariance[i * taps + i].r;
            diagonal_scale = std::max(diagonal_scale / static_cast<float>(taps),
                                      c.power_floor);
            for (size_t i = 0; i < taps; ++i)
                covariance[i * taps + i].r += c.regularization * diagonal_scale;

            std::vector<C> filter;
            solve_linear_system(covariance, cross, filter);
            for (size_t t = 0; t < frames; ++t) {
                const C target = spectra[t * bins + k];
                if (t < history || filter.empty()) {
                    residual[t * bins + k] = target;
                    continue;
                }
                C prediction;
                for (size_t i = 0; i < taps; ++i) {
                    const C past = spectra[(t - c.prediction_delay_frames - i) * bins + k];
                    prediction += filter[i].conj() * past;
                }
                residual[t * bins + k] = target - prediction;
            }
        }
        spectra.swap(residual);
    }

    std::fill(output, output + sample_count, 0.0f);
    std::vector<float> normalization(sample_count, 0.0f);
    for (size_t t = 0; t < frames; ++t) {
        for (size_t k = 0; k < bins; ++k)
            frame[k] = spectra[t * bins + k];
        for (size_t k = 1; k + 1 < bins; ++k)
            frame[nfft - k] = spectra[t * bins + k].conj();
        fft(frame, true);
        const size_t offset = t * c.hop_size;
        for (size_t n = 0; n < nfft; ++n) {
            const size_t p = offset + n;
            if (p < sample_count) {
                output[p] += frame[n].r * window[n];
                normalization[p] += window[n] * window[n];
            }
        }
    }
    for (size_t n = 0; n < sample_count; ++n)
        if (normalization[n] > 1.0e-8f) output[n] /= normalization[n];
    return true;
}

namespace {

static DSPComplex make_complex(float r = 0.0f, float i = 0.0f)
{
    DSPComplex value = {r, i};
    return value;
}

static DSPComplex complex_add(DSPComplex a, DSPComplex b)
{
    return make_complex(a.r + b.r, a.i + b.i);
}

static DSPComplex complex_sub(DSPComplex a, DSPComplex b)
{
    return make_complex(a.r - b.r, a.i - b.i);
}

static DSPComplex complex_mul(DSPComplex a, DSPComplex b)
{
    return make_complex(a.r * b.r - a.i * b.i,
                        a.r * b.i + a.i * b.r);
}

static DSPComplex complex_scale(DSPComplex a, float scale)
{
    return make_complex(a.r * scale, a.i * scale);
}

static DSPComplex complex_conj(DSPComplex a)
{
    return make_complex(a.r, -a.i);
}

static float complex_power(DSPComplex a)
{
    return a.r * a.r + a.i * a.i;
}

static void fft_fixed(DSPComplex *x, size_t n, bool inverse)
{
    for (size_t j = 1, i = 0; j < n; ++j) {
        size_t bit = n >> 1;
        for (; i & bit; bit >>= 1) i ^= bit;
        i ^= bit;
        if (j < i) std::swap(x[j], x[i]);
    }
    for (size_t length = 2; length <= n; length <<= 1) {
        const float angle = (inverse ? 1.0f : -1.0f) *
                            2.0f * PI / static_cast<float>(length);
        const DSPComplex step = make_complex(cosf(angle), sinf(angle));
        for (size_t offset = 0; offset < n; offset += length) {
            DSPComplex twiddle = make_complex(1.0f, 0.0f);
            for (size_t j = 0; j < length / 2; ++j) {
                const DSPComplex even = x[offset + j];
                const DSPComplex odd = complex_mul(twiddle,
                                                   x[offset + j + length / 2]);
                x[offset + j] = complex_add(even, odd);
                x[offset + j + length / 2] = complex_sub(even, odd);
                twiddle = complex_mul(twiddle, step);
            }
        }
    }
    if (inverse) {
        const float scale = 1.0f / static_cast<float>(n);
        for (size_t i = 0; i < n; ++i) x[i] = complex_scale(x[i], scale);
    }
}

static bool solve_small_system(const DSPComplex input_a[4][4],
                               const DSPComplex input_b[4],
                               DSPComplex output_x[4], size_t n)
{
    DSPComplex a[4][4] = {};
    DSPComplex b[4] = {};
    for (size_t row = 0; row < n; ++row) {
        b[row] = input_b[row];
        for (size_t col = 0; col < n; ++col)
            a[row][col] = input_a[row][col];
    }
    for (size_t col = 0; col < n; ++col) {
        size_t pivot = col;
        float best = complex_power(a[col][col]);
        for (size_t row = col + 1; row < n; ++row) {
            const float candidate = complex_power(a[row][col]);
            if (candidate > best) {
                best = candidate;
                pivot = row;
            }
        }
        if (best <= 1.0e-16f) return false;
        if (pivot != col) {
            for (size_t j = col; j < n; ++j)
                std::swap(a[col][j], a[pivot][j]);
            std::swap(b[col], b[pivot]);
        }
        const DSPComplex diagonal = a[col][col];
        const float denominator = complex_power(diagonal);
        for (size_t j = col; j < n; ++j)
            a[col][j] = complex_scale(complex_mul(a[col][j],
                                                   complex_conj(diagonal)),
                                      1.0f / denominator);
        b[col] = complex_scale(complex_mul(b[col], complex_conj(diagonal)),
                               1.0f / denominator);
        for (size_t row = col + 1; row < n; ++row) {
            const DSPComplex factor = a[row][col];
            for (size_t j = col; j < n; ++j)
                a[row][j] = complex_sub(a[row][j],
                                         complex_mul(factor, a[col][j]));
            b[row] = complex_sub(b[row], complex_mul(factor, b[col]));
        }
    }
    for (size_t row = n; row-- > 0;) {
        DSPComplex value = b[row];
        for (size_t col = row + 1; col < n; ++col)
            value = complex_sub(value, complex_mul(a[row][col],
                                                   output_x[col]));
        output_x[row] = value;
    }
    return true;
}

static bool valid_stream_frame(size_t frame_size, size_t hop_size)
{
    return frame_size >= 2 && frame_size <= 256 &&
           (frame_size & (frame_size - 1)) == 0 && hop_size > 0 &&
           hop_size <= frame_size;
}

} // namespace

AudioFilterConfig::AudioFilterConfig() : raw_shift(13), dc_block_shift(8) {}

AudioFilter::AudioFilter(const AudioFilterConfig &config) : config_(config),
    previous_input_(0.0f), previous_output_(0.0f), history_{0.0f},
    history_position_(0) {}

void AudioFilter::reset()
{
    previous_input_ = 0.0f;
    previous_output_ = 0.0f;
    history_position_ = 0;
    for (size_t i = 0; i < 5; ++i) history_[i] = 0.0f;
}

float AudioFilter::process(int32_t raw_sample)
{
    int32_t scaled_integer = raw_sample;
    if (config_.raw_shift > 0 && config_.raw_shift < 31)
        scaled_integer >>= config_.raw_shift;
    const float scaled = static_cast<float>(scaled_integer);
    const float feedback = previous_output_ -
        previous_output_ / static_cast<float>(1u << config_.dc_block_shift);
    float filtered = scaled - previous_input_ + feedback;
    previous_input_ = scaled;
    if (filtered > 32767.0f) filtered = 32767.0f;
    if (filtered < -32768.0f) filtered = -32768.0f;
    previous_output_ = filtered;

    history_[history_position_] = filtered;
    history_position_ = (history_position_ + 1) % 5;
    const size_t p = history_position_;
    const float sum = history_[p] + 4.0f * history_[(p + 1) % 5] +
                      6.0f * history_[(p + 2) % 5] +
                      4.0f * history_[(p + 3) % 5] + history_[(p + 4) % 5];
    return sum / 16.0f;
}

StreamingWPE::StreamingWPE() : initialized_(false), frame_size_(0), bins_(0),
    hop_size_(0), delay_frames_(0), filter_taps_(0), covariance_alpha_(0.0f),
    regularization_(0.0f), power_floor_(0.0f), frame_fill_(0),
    history_count_(0), history_position_(0), pending_head_(0),
    pending_count_(0) {}

bool StreamingWPE::init(const WPEConfig &config)
{
    const size_t required_history = config.prediction_delay_frames +
                                    config.filter_taps;
    if (!valid_stream_frame(config.frame_size, config.hop_size) ||
        config.frame_size / 2 + 1 > 129 || config.filter_taps == 0 ||
        config.filter_taps > 4 || config.prediction_delay_frames == 0 ||
        required_history > 8 || config.sample_rate <= 0.0f ||
        config.covariance_alpha < 0.0f || config.covariance_alpha >= 1.0f ||
        config.regularization < 0.0f || config.power_floor <= 0.0f)
        return false;
    frame_size_ = config.frame_size;
    bins_ = config.frame_size / 2 + 1;
    hop_size_ = config.hop_size;
    delay_frames_ = config.prediction_delay_frames;
    filter_taps_ = config.filter_taps;
    covariance_alpha_ = config.covariance_alpha;
    regularization_ = config.regularization;
    power_floor_ = config.power_floor;
    for (size_t n = 0; n < frame_size_; ++n)
        window_[n] = 0.5f * (1.0f - cosf(2.0f * PI * (n + 0.5f) /
                                         static_cast<float>(frame_size_)));
    initialized_ = true;
    reset();
    return true;
}

void StreamingWPE::reset()
{
    frame_fill_ = 0;
    history_count_ = 0;
    history_position_ = 0;
    pending_head_ = 0;
    pending_count_ = 0;
    for (size_t i = 0; i < 256; ++i) {
        frame_[i] = 0.0f;
        ola_[i] = 0.0f;
        ola_norm_[i] = 0.0f;
    }
    for (size_t i = 0; i < 512; ++i) pending_[i] = 0.0f;
    for (size_t h = 0; h < 8; ++h)
        for (size_t k = 0; k < 129; ++k) history_[h][k] = make_complex();
    for (size_t k = 0; k < 129; ++k) {
        for (size_t i = 0; i < 4; ++i) {
            cross_[k][i] = make_complex();
            filter_[k][i] = make_complex();
            for (size_t j = 0; j < 4; ++j)
                covariance_[k][i][j] = make_complex();
        }
    }
}

bool StreamingWPE::process_frame()
{
    for (size_t n = 0; n < frame_size_; ++n)
        fft_[n] = make_complex(frame_[n] * window_[n]);
    fft_fixed(fft_, frame_size_, false);
    const size_t current_slot = history_position_;
    for (size_t k = 0; k < bins_; ++k)
        history_[current_slot][k] = fft_[k];

    const size_t required_history = delay_frames_ + filter_taps_;
    const bool can_predict = history_count_ >= required_history;
    for (size_t k = 0; k < bins_; ++k) {
        const DSPComplex current = history_[current_slot][k];
        if (can_predict) {
            const float weight = 1.0f / std::max(complex_power(current),
                                                  power_floor_);
            const float alpha = covariance_alpha_;
            const float update = 1.0f - alpha;
            for (size_t i = 0; i < filter_taps_; ++i) {
                const size_t slot_i = (history_position_ + 8 -
                    delay_frames_ - i) % 8;
                const DSPComplex xi = history_[slot_i][k];
                cross_[k][i] = complex_add(complex_scale(cross_[k][i], alpha),
                    complex_scale(complex_mul(xi, complex_conj(current)),
                                  update * weight));
                for (size_t j = 0; j < filter_taps_; ++j) {
                    const size_t slot_j = (history_position_ + 8 -
                        delay_frames_ - j) % 8;
                    const DSPComplex xj = history_[slot_j][k];
                    covariance_[k][i][j] = complex_add(
                        complex_scale(covariance_[k][i][j], alpha),
                        complex_scale(complex_mul(xi, complex_conj(xj)),
                                      update * weight));
                }
            }
            DSPComplex system[4][4] = {};
            DSPComplex rhs[4] = {};
            const float diagonal = std::max(power_floor_, regularization_);
            for (size_t i = 0; i < filter_taps_; ++i) {
                rhs[i] = cross_[k][i];
                for (size_t j = 0; j < filter_taps_; ++j)
                    system[i][j] = covariance_[k][i][j];
                system[i][i].r += diagonal;
            }
            if (solve_small_system(system, rhs, filter_[k], filter_taps_)) {
                DSPComplex prediction = make_complex();
                for (size_t i = 0; i < filter_taps_; ++i) {
                    const size_t slot = (history_position_ + 8 -
                        delay_frames_ - i) % 8;
                    prediction = complex_add(prediction,
                        complex_mul(complex_conj(filter_[k][i]),
                                    history_[slot][k]));
                }
                fft_[k] = complex_sub(current, prediction);
            }
        }
    }
    history_position_ = (history_position_ + 1) % 8;
    if (history_count_ < 8) ++history_count_;

    for (size_t k = 1; k + 1 < bins_; ++k)
        fft_[frame_size_ - k] = complex_conj(fft_[k]);
    fft_fixed(fft_, frame_size_, true);
    for (size_t n = 0; n < frame_size_; ++n) {
        ola_[n] += fft_[n].r * window_[n];
        ola_norm_[n] += window_[n] * window_[n];
    }
    if (pending_count_ + hop_size_ > 512) return false;
    for (size_t n = 0; n < hop_size_; ++n) {
        const float norm = ola_norm_[n];
        const size_t slot = (pending_head_ + pending_count_) % 512;
        pending_[slot] = norm > 1.0e-8f ? ola_[n] / norm : 0.0f;
        ++pending_count_;
    }
    for (size_t n = 0; n + hop_size_ < frame_size_; ++n) {
        ola_[n] = ola_[n + hop_size_];
        ola_norm_[n] = ola_norm_[n + hop_size_];
    }
    for (size_t n = frame_size_ - hop_size_; n < frame_size_; ++n) {
        ola_[n] = 0.0f;
        ola_norm_[n] = 0.0f;
    }
    return true;
}

bool StreamingWPE::process(const float *input, float *output, size_t sample_count)
{
    if (!initialized_ || !input || !output) return false;
    for (size_t i = 0; i < sample_count; ++i) {
        frame_[frame_fill_++] = input[i];
        if (frame_fill_ == frame_size_) {
            if (!process_frame()) return false;
            for (size_t n = 0; n + hop_size_ < frame_size_; ++n)
                frame_[n] = frame_[n + hop_size_];
            frame_fill_ = frame_size_ - hop_size_;
        }
        if (pending_count_ > 0) {
            output[i] = pending_[pending_head_];
            pending_head_ = (pending_head_ + 1) % 512;
            --pending_count_;
        } else {
            output[i] = 0.0f;
        }
    }
    return true;
}

StreamingMVDR::StreamingMVDR() : initialized_(false), frame_size_(0), bins_(0),
    hop_size_(0), sample_rate_(0.0f), spacing_m_(0.0f),
    sound_speed_mps_(0.0f), target_angle_deg_(0.0f), covariance_alpha_(0.0f),
    diagonal_loading_(0.0f), frame_fill_(0), pending_head_(0),
    pending_count_(0) {}

bool StreamingMVDR::init(const MVDRConfig &config)
{
    if (!valid_stream_frame(config.frame_size, config.hop_size) ||
        config.frame_size / 2 + 1 > 129 || config.sample_rate <= 0.0f ||
        config.microphone_spacing_m < 0.0f || config.sound_speed_mps <= 0.0f ||
        config.covariance_alpha < 0.0f || config.covariance_alpha >= 1.0f ||
        config.diagonal_loading < 0.0f)
        return false;
    frame_size_ = config.frame_size;
    bins_ = config.frame_size / 2 + 1;
    hop_size_ = config.hop_size;
    sample_rate_ = config.sample_rate;
    spacing_m_ = config.microphone_spacing_m;
    sound_speed_mps_ = config.sound_speed_mps;
    target_angle_deg_ = config.target_angle_deg;
    covariance_alpha_ = config.covariance_alpha;
    diagonal_loading_ = config.diagonal_loading;
    for (size_t n = 0; n < frame_size_; ++n)
        window_[n] = 0.5f * (1.0f - cosf(2.0f * PI * (n + 0.5f) /
                                         static_cast<float>(frame_size_)));
    initialized_ = true;
    reset();
    return true;
}

void StreamingMVDR::reset()
{
    frame_fill_ = 0;
    pending_head_ = 0;
    pending_count_ = 0;
    for (size_t i = 0; i < 256; ++i) {
        frame1_[i] = 0.0f;
        frame2_[i] = 0.0f;
        ola_[i] = 0.0f;
        ola_norm_[i] = 0.0f;
    }
    for (size_t i = 0; i < 512; ++i) pending_[i] = 0.0f;
    for (size_t k = 0; k < 129; ++k) {
        covariance00_[k] = make_complex();
        covariance01_[k] = make_complex();
        covariance11_[k] = make_complex();
    }
}

bool StreamingMVDR::process_frame()
{
    for (size_t n = 0; n < frame_size_; ++n) {
        fft1_[n] = make_complex(frame1_[n] * window_[n]);
        fft2_[n] = make_complex(frame2_[n] * window_[n]);
    }
    fft_fixed(fft1_, frame_size_, false);
    fft_fixed(fft2_, frame_size_, false);
    const float tau_scale = spacing_m_ / sound_speed_mps_;
    for (size_t k = 0; k < bins_; ++k) {
        const DSPComplex x = fft1_[k];
        const DSPComplex z = fft2_[k];
        const float alpha = covariance_alpha_;
        const float update = 1.0f - alpha;
        covariance00_[k] = complex_add(complex_scale(covariance00_[k], alpha),
            complex_scale(complex_mul(x, complex_conj(x)), update));
        covariance01_[k] = complex_add(complex_scale(covariance01_[k], alpha),
            complex_scale(complex_mul(x, complex_conj(z)), update));
        covariance11_[k] = complex_add(complex_scale(covariance11_[k], alpha),
            complex_scale(complex_mul(z, complex_conj(z)), update));

        const float frequency = sample_rate_ * static_cast<float>(k) /
                                static_cast<float>(frame_size_);
        const float phase = -2.0f * PI * frequency * tau_scale *
                            sinf(target_angle_deg_ * PI / 180.0f);
        const DSPComplex a0 = make_complex(1.0f, 0.0f);
        const DSPComplex a1 = make_complex(cosf(phase), sinf(phase));
        const float scale = std::max(covariance00_[k].r + covariance11_[k].r,
                                     1.0e-12f);
        const float load = diagonal_loading_ * scale;
        const float q00 = covariance00_[k].r + load;
        const float q11 = covariance11_[k].r + load;
        const DSPComplex q01 = covariance01_[k];
        const float determinant = std::max(q00 * q11 - complex_power(q01),
                                           1.0e-12f);
        const DSPComplex u0 = complex_scale(
            complex_sub(complex_scale(a0, q11), complex_mul(q01, a1)),
            1.0f / determinant);
        const DSPComplex u1 = complex_scale(
            complex_add(complex_scale(complex_mul(complex_conj(q01), a0), -1.0f),
                        complex_scale(a1, q00)), 1.0f / determinant);
        const DSPComplex denominator = complex_add(complex_mul(complex_conj(a0), u0),
                                                   complex_mul(complex_conj(a1), u1));
        const float denominator_real = std::max(denominator.r, 1.0e-12f);
        const DSPComplex w0 = complex_scale(u0, 1.0f / denominator_real);
        const DSPComplex w1 = complex_scale(u1, 1.0f / denominator_real);
        output_fft_[k] = complex_add(complex_mul(complex_conj(w0), x),
                                     complex_mul(complex_conj(w1), z));
    }
    for (size_t k = 1; k + 1 < bins_; ++k)
        output_fft_[frame_size_ - k] = complex_conj(output_fft_[k]);
    fft_fixed(output_fft_, frame_size_, true);
    for (size_t n = 0; n < frame_size_; ++n) {
        ola_[n] += output_fft_[n].r * window_[n];
        ola_norm_[n] += window_[n] * window_[n];
    }
    if (pending_count_ + hop_size_ > 512) return false;
    for (size_t n = 0; n < hop_size_; ++n) {
        const float norm = ola_norm_[n];
        const size_t slot = (pending_head_ + pending_count_) % 512;
        pending_[slot] = norm > 1.0e-8f ? ola_[n] / norm : 0.0f;
        ++pending_count_;
    }
    for (size_t n = 0; n + hop_size_ < frame_size_; ++n) {
        ola_[n] = ola_[n + hop_size_];
        ola_norm_[n] = ola_norm_[n + hop_size_];
    }
    for (size_t n = frame_size_ - hop_size_; n < frame_size_; ++n) {
        ola_[n] = 0.0f;
        ola_norm_[n] = 0.0f;
    }
    return true;
}

bool StreamingMVDR::process(const float *input1, const float *input2,
                            float *output, size_t sample_count)
{
    if (!initialized_ || !input1 || !input2 || !output) return false;
    for (size_t i = 0; i < sample_count; ++i) {
        frame1_[frame_fill_] = input1[i];
        frame2_[frame_fill_] = input2[i];
        ++frame_fill_;
        if (frame_fill_ == frame_size_) {
            if (!process_frame()) return false;
            for (size_t n = 0; n + hop_size_ < frame_size_; ++n) {
                frame1_[n] = frame1_[n + hop_size_];
                frame2_[n] = frame2_[n + hop_size_];
            }
            frame_fill_ = frame_size_ - hop_size_;
        }
        if (pending_count_ > 0) {
            output[i] = pending_[pending_head_];
            pending_head_ = (pending_head_ + 1) % 512;
            --pending_count_;
        } else {
            output[i] = 0.0f;
        }
    }
    return true;
}

}  // namespace IcsDSP
