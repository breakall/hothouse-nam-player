// HybridSpaceReverb -- sparse stereo early reflections plus a compact FDN tail.
//
// This is intentionally self-contained: it has no DaisySP dependency and does
// not allocate memory.  Place an instance in SDRAM on the Hothouse target.
#ifndef HOTHOUSE_NAM_REVERBS_HYBRID_SPACE_REVERB_H_
#define HOTHOUSE_NAM_REVERBS_HYBRID_SPACE_REVERB_H_

#include <cmath>

namespace hothouse_nam
{
namespace reverb
{

class HybridSpaceReverb
{
  public:
    // The fixed storage supports 48 kHz at the documented delay times, and
    // degrades gracefully (by clipping the longest delays) at higher rates.
    static constexpr unsigned kMaxDelaySamples = 8192;
    static constexpr unsigned kEarlyBufferSamples = 8192;

    bool Init(float sample_rate)
    {
        if(!std::isfinite(sample_rate) || sample_rate < 8000.0f
           || sample_rate > 52000.0f)
            return false;

        sample_rate_ = sample_rate;
        Reset();
        SetParameters(0.58f, 0.35f, 0.55f, 0.20f);
        return true;
    }

    void Reset()
    {
        for(unsigned line = 0; line < kLineCount; ++line)
        {
            for(unsigned sample = 0; sample < kMaxDelaySamples; ++sample)
                delay_[line][sample] = 0.0f;
            damping_state_[line] = 0.0f;
            lfo_phase_[line] = 0.137f * static_cast<float>(line + 1);
        }
        for(unsigned sample = 0; sample < kEarlyBufferSamples; ++sample)
            early_buffer_[sample] = 0.0f;
        write_index_       = 0;
        early_write_index_ = 0;
    }

    // All parameters are normalized to [0, 1].
    // decay: tail duration; damping: high-frequency absorption; size: room
    // scale; modulation: subtle delay modulation in the late FDN.
    void SetParameters(float decay, float damping, float size, float modulation)
    {
        decay_      = Clamp01(decay);
        modulation_ = Clamp01(modulation);

        // Keep feedback safely below unity.  0.985 is deliberately a little
        // conservative: it leaves headroom for parameter changes and avoids
        // an accidental infinite tail on a full-scale input.
        feedback_ = 0.52f + 0.465f * decay_;

        // One-pole low-pass coefficient in each feedback branch.  Larger
        // damping values make high frequencies disappear sooner.
        damping_coefficient_ = 0.74f - 0.66f * Clamp01(damping);

        const float rate_scale = sample_rate_ / 48000.0f;
        const float room_scale = 0.62f + 0.70f * Clamp01(size);
        static constexpr float kBaseDelays[kLineCount]
            = {1423.0f, 1861.0f, 2417.0f, 2999.0f};
        for(unsigned line = 0; line < kLineCount; ++line)
        {
            float samples = kBaseDelays[line] * rate_scale * room_scale;
            if(samples < 32.0f)
                samples = 32.0f;
            if(samples > static_cast<float>(kMaxDelaySamples - 3))
                samples = static_cast<float>(kMaxDelaySamples - 3);
            delay_samples_[line] = samples;

            // Different very-low LFO rates keep line-to-line correlations low.
            const float hertz = 0.071f + 0.019f * static_cast<float>(line);
            lfo_increment_[line] = hertz / sample_rate_;
        }
    }

    // Produces wet-only stereo output.  The caller owns wet/dry mixing.
    void Process(float input, float* left, float* right)
    {
        if(left == nullptr || right == nullptr)
            return;

        // These sparse taps establish the first ~150 ms of a room without an
        // IR asset or an expensive FIR.  Unequal gains form a stable stereo
        // image while keeping the early portion intentionally non-recursive.
        const float early_l = ReadEarly(431) * 0.62f
                              + ReadEarly(1297) * 0.39f
                              + ReadEarly(2773) * 0.27f
                              + ReadEarly(5501) * 0.18f;
        const float early_r = ReadEarly(673) * 0.57f
                              + ReadEarly(1789) * 0.42f
                              + ReadEarly(3641) * 0.25f
                              + ReadEarly(7117) * 0.16f;
        early_buffer_[early_write_index_] = input;
        if(++early_write_index_ == kEarlyBufferSamples)
            early_write_index_ = 0;

        float line_out[kLineCount];
        float sum = 0.0f;
        for(unsigned line = 0; line < kLineCount; ++line)
        {
            // 1.5 samples is enough to decorrelate the tail without audible
            // chorusing.  Linear interpolation is deliberately used here: on
            // this small four-line network it is a good CPU/sound tradeoff.
            const float phase = Triangle(lfo_phase_[line]);
            const float depth = modulation_ * (0.40f + 0.23f * line);
            line_out[line] = ReadDelay(line, delay_samples_[line] + phase * depth);
            sum += line_out[line];

            lfo_phase_[line] += lfo_increment_[line];
            if(lfo_phase_[line] >= 1.0f)
                lfo_phase_[line] -= 1.0f;
        }

        // A four-line Householder mixer: (sum / 2) - each line.  It is
        // orthogonal, so it spreads energy without changing it before the
        // feedback gain and damping filters are applied.
        const float shared = 0.5f * sum;
        for(unsigned line = 0; line < kLineCount; ++line)
        {
            const float mixed = shared - line_out[line];
            damping_state_[line] += damping_coefficient_
                                    * (mixed - damping_state_[line]);
            const float injection = (line & 1U) ? early_r : early_l;
            delay_[line][write_index_] = injection * 0.19f
                                         + damping_state_[line] * feedback_;
        }
        if(++write_index_ == kMaxDelaySamples)
            write_index_ = 0;

        // Four decorrelated taps.  The early reflections are included here as
        // well so a short decay still sounds like a complete stereo space.
        *left  = early_l * 0.58f + 0.34f * (line_out[0] - line_out[1]
                                             - line_out[2] + line_out[3]);
        *right = early_r * 0.58f + 0.34f * (line_out[0] + line_out[1]
                                             - line_out[2] - line_out[3]);
    }

  private:
    static constexpr unsigned kLineCount = 4;

    static float Clamp01(float value)
    {
        if(value < 0.0f)
            return 0.0f;
        if(value > 1.0f)
            return 1.0f;
        return value;
    }

    static float Triangle(float phase)
    {
        // -1 at phase zero, +1 at phase 0.5, then back to -1.
        return phase < 0.5f ? (-1.0f + 4.0f * phase)
                            : (3.0f - 4.0f * phase);
    }

    float ReadEarly(unsigned delay_samples) const
    {
        unsigned index = early_write_index_;
        if(index < delay_samples)
            index += kEarlyBufferSamples;
        return early_buffer_[index - delay_samples];
    }

    float ReadDelay(unsigned line, float delay_samples) const
    {
        float index = static_cast<float>(write_index_) - delay_samples;
        if(index < 0.0f)
            index += static_cast<float>(kMaxDelaySamples);
        if(index < 0.0f)
            index += static_cast<float>(kMaxDelaySamples);
        const unsigned first = static_cast<unsigned>(index);
        const unsigned second = (first + 1U == kMaxDelaySamples) ? 0U : first + 1U;
        const float fraction = index - static_cast<float>(first);
        return delay_[line][first] + fraction * (delay_[line][second] - delay_[line][first]);
    }

    float delay_[kLineCount][kMaxDelaySamples];
    float early_buffer_[kEarlyBufferSamples];
    float damping_state_[kLineCount];
    float delay_samples_[kLineCount];
    float lfo_phase_[kLineCount];
    float lfo_increment_[kLineCount];
    float sample_rate_ = 48000.0f;
    float feedback_ = 0.78f;
    float damping_coefficient_ = 0.50f;
    float decay_ = 0.58f;
    float modulation_ = 0.20f;
    unsigned write_index_ = 0;
    unsigned early_write_index_ = 0;
};

} // namespace reverb
} // namespace hothouse_nam

#endif // HOTHOUSE_NAM_REVERBS_HYBRID_SPACE_REVERB_H_
