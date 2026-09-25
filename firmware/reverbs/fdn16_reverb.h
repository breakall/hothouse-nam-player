#pragma once

// A compact, allocation-free 16-line feedback delay network (FDN) hall.
//
// The network uses a Householder feedback matrix, which is orthogonal and
// therefore spreads energy between every delay line without an expensive 16 x
// 16 matrix multiply.  The delay store is intentionally part of this object:
// put the object in SDRAM (DSY_SDRAM_BSS) in the application, not on the
// stack.  At its maximum size it occupies 655,360 bytes plus a small amount
// of state.

#include <cmath>
#include <cstddef>
#include <cstring>

namespace hothouse_nam
{
namespace reverb
{

class Fdn16Reverb
{
  public:
    static constexpr std::size_t kNumLines        = 16;
    static constexpr std::size_t kMaxDelaySamples = 10240;

    // Designed for the pedal's 48 kHz audio rate.  The fixed delay store
    // safely accommodates rates through 52 kHz at maximum Size.
    bool Init(float sample_rate)
    {
        if(!(sample_rate > 1000.0f) || sample_rate > 52000.0f)
            return false;

        sample_rate_ = sample_rate;
        Reset();
        SetParameters(0.55f, 0.35f, 0.50f, 0.20f);
        return true;
    }

    void Reset()
    {
        std::memset(delay_, 0, sizeof(delay_));
        std::memset(damp_state_, 0, sizeof(damp_state_));
        write_index_ = 0;
        lfo_phase_   = 0.0f;
    }

    // All parameters are nominally in [0, 1].
    // decay: 0.35 to 16 s RT60; damping: bright to dark; size: 0.5 to 1.8x;
    // modulation: 0 to approximately +/- 18 samples at 48 kHz.
    void SetParameters(float decay, float damping, float size, float modulation)
    {
        decay      = Clamp(decay, 0.0f, 1.0f);
        damping    = Clamp(damping, 0.0f, 1.0f);
        size       = Clamp(size, 0.0f, 1.0f);
        modulation = Clamp(modulation, 0.0f, 1.0f);

        // Exponential range makes the useful part of the decay control much
        // easier to set than a linear 0--16 second mapping.
        const float rt60_seconds = 0.35f * std::pow(45.7f, decay);
        const float rate_scale   = sample_rate_ / 48000.0f;
        const float size_scale   = 0.50f + 1.30f * size;
        modulation_depth_       = modulation * (4.0f + 14.0f * size);

        // Damping is a one-pole low-pass inside each feedback path.  This
        // maps to roughly 18 kHz (bright) through 1.1 kHz (dark) at 48 kHz.
        const float cutoff = 18000.0f * std::pow(0.061f, damping);
        damping_alpha_     = 1.0f - std::exp(-6.28318530718f * cutoff / sample_rate_);

        for(std::size_t i = 0; i < kNumLines; ++i)
        {
            const float delay = kBaseDelaySamples48[i] * rate_scale * size_scale;
            // Two samples remain for fractional interpolation and sufficient
            // extra room remains for the modulation excursion.
            delay_samples_[i] = Clamp(delay, 32.0f, float(kMaxDelaySamples - 24));
            feedback_gain_[i] = std::pow(10.0f,
                                         -3.0f * (delay_samples_[i] / sample_rate_)
                                             / rt60_seconds);
        }
    }

    // Mono input, true-stereo wet output.  Outputs are wet-only and expected
    // to be mixed with dry signal by the caller.
    void Process(float input, float* left, float* right)
    {
        if(left == nullptr || right == nullptr)
            return;

        float line[kNumLines];
        float sum = 0.0f;

        // A common LFO with decorrelated phase offsets keeps the tail moving
        // without 16 expensive sin() calls per sample.  The triangle shape is
        // deliberately tiny (sub-millisecond), so its corners are inaudible.
        lfo_phase_ += 0.11f / sample_rate_;
        if(lfo_phase_ >= 1.0f)
            lfo_phase_ -= 1.0f;

        for(std::size_t i = 0; i < kNumLines; ++i)
        {
            const float phase = Wrap01(lfo_phase_ + kPhaseOffsets[i]);
            const float mod   = modulation_depth_ * Triangle(phase);
            const float read  = float(write_index_) - delay_samples_[i] - mod;
            const float x     = ReadInterpolated(i, read);

            // Frequency-dependent decay belongs inside the feedback loop.
            damp_state_[i] += damping_alpha_ * (x - damp_state_[i]);
            line[i] = damp_state_[i];
            sum += line[i];
        }

        // Householder H = (2/N)11' - I.  For N=16, 2/N is exactly 1/8.
        // The resulting matrix is orthogonal: no artificial gain is added by
        // the matrix itself, while every delay line immediately talks to all
        // the other delay lines.
        const float common = 0.125f * sum;
        float       out_l  = 0.0f;
        float       out_r  = 0.0f;
        for(std::size_t i = 0; i < kNumLines; ++i)
        {
            const float feedback = feedback_gain_[i] * (common - line[i]);
            // A Walsh input vector avoids exciting every line in phase.
            const float injection = ((i & 1u) ? -0.125f : 0.125f) * input;
            delay_[i][write_index_] = feedback + injection;

            // Two different orthogonal Walsh rows form the stereo decoder.
            out_l += ((i & 2u) ? -line[i] : line[i]);
            out_r += ((i & 4u) ? -line[i] : line[i]);
        }

        if(++write_index_ == kMaxDelaySamples)
            write_index_ = 0;

        // 1/sqrt(16) keeps the RMS level of an orthogonal output row near a
        // single delay-line level.  The conservative gain leaves mix headroom.
        *left  = 0.18f * out_l;
        *right = 0.18f * out_r;
    }

  private:
    static float Clamp(float value, float minimum, float maximum)
    {
        return value < minimum ? minimum : (value > maximum ? maximum : value);
    }

    static float Wrap01(float phase)
    {
        return phase >= 1.0f ? phase - 1.0f : phase;
    }

    static float Triangle(float phase)
    {
        return phase < 0.5f ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
    }

    float ReadInterpolated(std::size_t line, float position) const
    {
        // position is at most one buffer length before write_index_, so a
        // single wrap in each direction is sufficient.
        if(position < 0.0f)
            position += float(kMaxDelaySamples);
        else if(position >= float(kMaxDelaySamples))
            position -= float(kMaxDelaySamples);

        const std::size_t index = static_cast<std::size_t>(position);
        const std::size_t next  = (index + 1u == kMaxDelaySamples) ? 0u : index + 1u;
        const float       frac  = position - float(index);
        return delay_[line][index] + frac * (delay_[line][next] - delay_[line][index]);
    }

    // Mutually prime-ish line lengths at 48 kHz.  Their wide distribution
    // prevents low-order repeating modes when Size is changed.
    static constexpr float kBaseDelaySamples48[kNumLines] = {
        1427.0f, 1559.0f, 1747.0f, 1871.0f, 2053.0f, 2237.0f, 2423.0f, 2689.0f,
        2917.0f, 3181.0f, 3469.0f, 3779.0f, 4127.0f, 4481.0f, 4871.0f, 5279.0f};
    static constexpr float kPhaseOffsets[kNumLines] = {
        0.000f, 0.062f, 0.125f, 0.188f, 0.250f, 0.312f, 0.375f, 0.438f,
        0.500f, 0.562f, 0.625f, 0.688f, 0.750f, 0.812f, 0.875f, 0.938f};

    float       delay_[kNumLines][kMaxDelaySamples];
    float       damp_state_[kNumLines]{};
    float       delay_samples_[kNumLines]{};
    float       feedback_gain_[kNumLines]{};
    float       sample_rate_      = 48000.0f;
    float       damping_alpha_    = 0.25f;
    float       modulation_depth_ = 0.0f;
    float       lfo_phase_        = 0.0f;
    std::size_t write_index_      = 0;
};

} // namespace reverb
} // namespace hothouse_nam
