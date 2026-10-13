# ifndef ALGORITHM_H_
# define ALGORITHM_H_
#include <stddef.h>
#include <stdint.h>

#define PI 3.14159265358979323846f

struct DSPComplex {
    float r;
    float i;
};

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

bool mvdr_process(const float *input1, const float *input2, float *output,
                  size_t sample_count, const MVDRConfig &config);

// Single-channel frequency-domain WPE (weighted prediction error).
// The function is offline/block based and leaves the input unchanged.  For a
// two-microphone recording, call it once per channel before a spatial stage.
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

bool wpe_process(const float *input, float *output, size_t sample_count,
                 const WPEConfig &config);

struct AudioFilterConfig {
    int raw_shift;
    int dc_block_shift;
    AudioFilterConfig();
};

class AudioFilter {
public:
    explicit AudioFilter(const AudioFilterConfig &config = AudioFilterConfig());
    void reset();
    float process(int32_t raw_sample);

private:
    AudioFilterConfig config_;
    float previous_input_;
    float previous_output_;
    float history_[5];
    size_t history_position_;
};

// Fixed-capacity, low-latency WPE processor for embedded use.  The default
// streaming configuration is intentionally lower order than the offline API.
class StreamingWPE {
public:
    StreamingWPE();
    bool init(const WPEConfig &config);
    void reset();
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

// Fixed-capacity streaming two-channel MVDR processor.  It consumes the two
// synchronized (or externally aligned) channels and produces one beamformed
// stream with the configured steering angle.
class StreamingMVDR {
public:
    StreamingMVDR();
    bool init(const MVDRConfig &config);
    void reset();
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

# endif
