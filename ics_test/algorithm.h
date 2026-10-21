# ifndef ALGORITHM_H_
# define ALGORITHM_H_
#include <stddef.h>
#include <stdint.h>

namespace IcsDSP {

// DSP 模块内部统一使用的圆周率常量，避免污染其他模块的宏命名空间。
constexpr float PI = 3.14159265358979323846f;

// 离线与流式 DSP 共用的复数存储类型。
struct DSPComplex {
    float r;
    float i;
};

// 离线 MVDR 波束形成参数；帧长须为 2 的幂。
struct MVDRConfig {
    size_t frame_size;
    size_t hop_size;
    float sample_rate;
    float microphone_spacing_m;
    float sound_speed_mps;
    float target_angle_deg;
    float covariance_alpha;
    float diagonal_loading;
    MVDRConfig();
};

// 对两路输入做离线 MVDR 波束形成，成功时写入 sample_count 个输出点。
bool mvdr_process(const float *input1, const float *input2, float *output,
                  size_t sample_count, const MVDRConfig &config);

// 单通道频域 WPE（加权预测误差）离线处理参数。
struct WPEConfig {
    size_t frame_size;
    size_t hop_size;
    float sample_rate;
    size_t prediction_delay_frames;
    size_t filter_taps;
    size_t iterations;
    float covariance_alpha;
    float regularization;
    float power_floor;
    WPEConfig();
};

// 对单路输入做离线 WPE 降混响，输入和输出缓冲区不可重叠使用。
bool wpe_process(const float *input, float *output, size_t sample_count,
                 const WPEConfig &config);

// I2S 原始数据转浮点音频时使用的增益、直流阻断配置。
struct AudioFilterConfig {
    int raw_shift;
    int dc_block_shift;
    AudioFilterConfig();
};

class AudioFilter {
public:
    // 初始化滤波器配置，并将内部延迟线清零。
    explicit AudioFilter(const AudioFilterConfig &config = AudioFilterConfig());
    // 清除直流阻断器和抗混叠 FIR 的历史状态。
    void reset();
    // 将一个 32 位 I2S 槽位转换为缩放、去直流并低通后的浮点采样。
    float process(int32_t raw_sample);

private:
    AudioFilterConfig config_;
    float previous_input_;
    float previous_output_;
    float history_[5];
    size_t history_position_;
};

// 固定容量的低延迟流式 WPE；数组容量受限，适合 MCU 按块持续调用。
class StreamingWPE {
public:
    // 构造未初始化的流式处理器；调用 init 后才能处理数据。
    StreamingWPE();
    // 检查配置是否落在固定数组容量内，并初始化帧参数。
    bool init(const WPEConfig &config);
    // 清空重叠、历史频谱、协方差和待输出数据。
    void reset();
    // 输入任意长度的一段数据，输出对应长度的流式处理结果。
    bool process(const float *input, float *output, size_t sample_count);
    bool ready() const { return initialized_; }

private:
    bool process_frame();
    bool initialized_;
    size_t frame_size_;
    size_t bins_;
    size_t hop_size_;
    size_t delay_frames_;
    size_t filter_taps_;
    float covariance_alpha_;
    float regularization_;
    float power_floor_;
    size_t frame_fill_;
    size_t history_count_;
    size_t history_position_;
    size_t pending_head_;
    size_t pending_count_;
    float window_[256];
    float frame_[256];
    float ola_[256];
    float ola_norm_[256];
    float pending_[512];
    DSPComplex fft_[256];
    DSPComplex history_[8][129];
    DSPComplex covariance_[129][4][4];
    DSPComplex cross_[129][4];
    DSPComplex filter_[129][4];
};

// 固定容量的双通道流式 MVDR；输入通道需同步或由外部完成时间对齐。
class StreamingMVDR {
public:
    // 构造未初始化的双通道流式波束形成器。
    StreamingMVDR();
    // 设置帧长、帧移、麦克风间距和目标方向。
    bool init(const MVDRConfig &config);
    // 清除协方差、帧缓存和重叠相加状态。
    void reset();
    // 同步处理两路输入并产生等长的单路波束形成输出。
    bool process(const float *input1, const float *input2, float *output,
                 size_t sample_count);
    bool ready() const { return initialized_; }

private:
    bool process_frame();
    bool initialized_;
    size_t frame_size_;
    size_t bins_;
    size_t hop_size_;
    float sample_rate_;
    float spacing_m_;
    float sound_speed_mps_;
    float target_angle_deg_;
    float covariance_alpha_;
    float diagonal_loading_;
    size_t frame_fill_;
    size_t pending_head_;
    size_t pending_count_;
    float window_[256];
    float frame1_[256];
    float frame2_[256];
    float ola_[256];
    float ola_norm_[256];
    float pending_[512];
    DSPComplex fft1_[256];
    DSPComplex fft2_[256];
    DSPComplex output_fft_[256];
    DSPComplex covariance00_[129];
    DSPComplex covariance01_[129];
    DSPComplex covariance11_[129];
};

}  // namespace IcsDSP

# endif
