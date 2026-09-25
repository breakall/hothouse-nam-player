// Dattorro-inspired stereo plate reverb for the Hothouse NAM player.
//
// This is an original, self-contained implementation of the topology
// described by Jon Dattorro in "Effect Design, Part 1: Reverberator and
// Other Filters" (JAES, 1997): a series allpass input diffuser followed by
// two cross-coupled, damped tank loops.  It intentionally does not copy code
// from a GPL implementation.  The API and implementation are suitable for
// bare-metal C++17; no heap allocation is performed.
//
// Place an instance in SDRAM (for example with DSY_SDRAM_BSS) because its
// delay storage is about 128 KiB.  The maximum delays target 48 kHz.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>

namespace hothouse_nam {
namespace reverb {

namespace detail {

inline float Clamp(float value, float low, float high) {
  return value < low ? low : (value > high ? high : value);
}

template <std::size_t kMaximumSamples>
class FixedDelayLine {
 public:
  void Init(std::size_t length) {
    length_ = length < 4 ? 4 : (length > kMaximumSamples ? kMaximumSamples : length);
    Reset();
  }

  void Reset() {
    for (std::size_t i = 0; i < kMaximumSamples; ++i) buffer_[i] = 0.0f;
    write_index_ = 0;
  }

  void Write(float value) {
    buffer_[write_index_] = value;
    if (++write_index_ == length_) write_index_ = 0;
  }

  // Linear interpolation is sufficient for the very shallow tank modulation.
  float Read(float delay_samples) const {
    delay_samples = Clamp(delay_samples, 1.0f, static_cast<float>(length_ - 2));
    float position = static_cast<float>(write_index_) - delay_samples;
    while (position < 0.0f) position += static_cast<float>(length_);
    while (position >= static_cast<float>(length_)) position -= static_cast<float>(length_);
    const std::size_t first = static_cast<std::size_t>(position);
    const std::size_t second = first + 1 == length_ ? 0 : first + 1;
    const float fraction = position - static_cast<float>(first);
    return buffer_[first] + (buffer_[second] - buffer_[first]) * fraction;
  }

 private:
  float buffer_[kMaximumSamples]{};
  std::size_t length_ = kMaximumSamples;
  std::size_t write_index_ = 0;
};

template <std::size_t kMaximumSamples>
class Allpass {
 public:
  void Init(std::size_t length) { delay_.Init(length); }
  void Reset() { delay_.Reset(); }

  float Process(float input, float delay_samples, float gain) {
    const float delayed = delay_.Read(delay_samples);
    const float write = input + gain * delayed;
    delay_.Write(write);
    return delayed - gain * write;
  }

 private:
  FixedDelayLine<kMaximumSamples> delay_;
};

class OnePoleLowpass {
 public:
  void Reset() { state_ = 0.0f; }
  float Process(float input, float coefficient) {
    state_ += (input - state_) * coefficient;
    return state_;
  }

 private:
  float state_ = 0.0f;
};

}  // namespace detail

class DattorroReverb {
 public:
  // Returns false for invalid sample rates or rates whose scaled delay lines
  // do not fit the fixed 48 kHz storage allocation.
  bool Init(float sample_rate) {
    if (!std::isfinite(sample_rate) || !(sample_rate > 1000.0f)
        || sample_rate > 48000.0f)
      return false;
    sample_rate_ = sample_rate;
    phase_increment_ = 6.28318530718f / sample_rate_;
    ConfigureDelayStorage();
    SetParameters(0.65f, 0.35f, 1.0f, 0.25f);
    return true;
  }

  void Reset() {
    diffuser_1_.Reset(); diffuser_2_.Reset(); diffuser_3_.Reset(); diffuser_4_.Reset();
    tank_left_allpass_.Reset(); tank_right_allpass_.Reset();
    tank_left_a_.Reset(); tank_left_b_.Reset();
    tank_right_a_.Reset(); tank_right_b_.Reset();
    damping_left_.Reset(); damping_right_.Reset();
    lfo_sine_ = 0.0f;
    lfo_cosine_ = 1.0f;
    lfo_renorm_counter_ = 0;
  }

  // All controls are normalized [0, 1].  Decay governs feedback, damping
  // makes the tail darker, size scales the tank, and modulation is deliberately
  // shallow so changing it cannot make the delay read head overrun a line.
  void SetParameters(float decay, float damping, float size, float modulation) {
    decay_ = detail::Clamp(decay, 0.0f, 1.0f);
    damping_ = detail::Clamp(damping, 0.0f, 1.0f);
    size_ = detail::Clamp(size, 0.0f, 1.0f);
    modulation_ = detail::Clamp(modulation, 0.0f, 1.0f);

    // Leave headroom below one: the two tank loops, as well as the diffuser,
    // can otherwise accumulate a large transient at maximum settings.
    feedback_ = 0.25f + decay_ * 0.70f;
    // Higher damping lowers the lowpass cutoff.  Coefficient is per sample.
    damping_coefficient_ = 0.48f - damping_ * 0.42f;
    const float lfo_increment = phase_increment_ * (0.11f + modulation_ * 0.37f);
    lfo_step_sine_ = std::sin(lfo_increment);
    lfo_step_cosine_ = std::cos(lfo_increment);

    const float scale = 0.55f + size_ * 0.45f;
    const float sr = sample_rate_ / 29761.0f;
    // Dattorro's original delay proportions, scaled for the current rate.
    diffuser_delay_[0] = 142.0f * sr * scale;
    diffuser_delay_[1] = 107.0f * sr * scale;
    diffuser_delay_[2] = 379.0f * sr * scale;
    diffuser_delay_[3] = 277.0f * sr * scale;
    left_a_delay_ = 4453.0f * sr * scale;
    left_b_delay_ = 3720.0f * sr * scale;
    right_a_delay_ = 4217.0f * sr * scale;
    right_b_delay_ = 2656.0f * sr * scale;
    left_allpass_delay_ = 672.0f * sr * scale;
    right_allpass_delay_ = 908.0f * sr * scale;

  }

  void Process(float input, float* left, float* right) {
    if (left == nullptr || right == nullptr) return;

    // Four allpasses turn the direct input into a compact cloud before the
    // cross-coupled tank.  Alternating polarity reduces the allpass's obvious
    // comb colouration and follows the classic plate input diffuser approach.
    float diffuse = input * 0.30f;
    diffuse = diffuser_1_.Process(diffuse, diffuser_delay_[0], 0.75f);
    diffuse = diffuser_2_.Process(diffuse, diffuser_delay_[1], -0.75f);
    diffuse = diffuser_3_.Process(diffuse, diffuser_delay_[2], 0.625f);
    diffuse = diffuser_4_.Process(diffuse, diffuser_delay_[3], -0.625f);

    const float l_tail = damping_left_.Process(tank_left_b_.Read(left_b_delay_), damping_coefficient_);
    const float r_tail = damping_right_.Process(tank_right_b_.Read(right_b_delay_), damping_coefficient_);
    const float tank_l_input = diffuse + feedback_ * r_tail;
    const float tank_r_input = diffuse + feedback_ * l_tail;

    const float sine = lfo_sine_;
    const float cosine = lfo_cosine_;
    const float l_mod = sine * modulation_ * 10.0f;
    const float r_mod = cosine * modulation_ * 10.0f;

    tank_left_a_.Write(tank_l_input);
    const float l_a = tank_left_a_.Read(left_a_delay_ + l_mod);
    const float l_ap = tank_left_allpass_.Process(l_a, left_allpass_delay_ + l_mod * 0.25f, 0.70f);
    tank_left_b_.Write(l_ap);

    tank_right_a_.Write(tank_r_input);
    const float r_a = tank_right_a_.Read(right_a_delay_ + r_mod);
    const float r_ap = tank_right_allpass_.Process(r_a, right_allpass_delay_ + r_mod * 0.25f, 0.70f);
    tank_right_b_.Write(r_ap);

    // Decorrelated tap combinations produce a stable stereo plate image.
    *left = 0.28f * (l_a + tank_left_b_.Read(left_b_delay_ * 0.58f)
                     - r_a + tank_right_b_.Read(right_b_delay_ * 0.73f));
    *right = 0.28f * (r_a + tank_right_b_.Read(right_b_delay_ * 0.61f)
                      - l_a + tank_left_b_.Read(left_b_delay_ * 0.79f));

    // A recursive oscillator avoids expensive transcendental calls in the
    // audio callback.  Re-normalise occasionally to eliminate accumulated
    // floating-point drift.
    const float next_sine = lfo_sine_ * lfo_step_cosine_ + lfo_cosine_ * lfo_step_sine_;
    lfo_cosine_ = lfo_cosine_ * lfo_step_cosine_ - lfo_sine_ * lfo_step_sine_;
    lfo_sine_ = next_sine;
    if ((++lfo_renorm_counter_ & 1023u) == 0u) {
      const float inverse_magnitude = 1.0f / std::sqrt(lfo_sine_ * lfo_sine_ + lfo_cosine_ * lfo_cosine_);
      lfo_sine_ *= inverse_magnitude;
      lfo_cosine_ *= inverse_magnitude;
    }
  }

 private:
  template <std::size_t kMaximum>
  static std::size_t DelayLength(float delay) {
    const std::size_t requested = static_cast<std::size_t>(delay + 3.0f);
    return requested < 4 ? 4 : (requested > kMaximum ? kMaximum : requested);
  }

  // Storage is set once at Init.  The delay times themselves can subsequently
  // change with the size control without clearing the tail.
  void ConfigureDelayStorage() {
    const float sr = sample_rate_ / 29761.0f;
    diffuser_1_.Init(DelayLength<768>(142.0f * sr));
    diffuser_2_.Init(DelayLength<768>(107.0f * sr));
    diffuser_3_.Init(DelayLength<768>(379.0f * sr));
    diffuser_4_.Init(DelayLength<768>(277.0f * sr));
    // Include positive LFO excursion in the active ring length. Without this
    // headroom, FixedDelayLine::Read clamps half of each modulation cycle.
    constexpr float kTankModulationHeadroom = 10.0f;
    constexpr float kAllpassModulationHeadroom = 3.0f;
    tank_left_a_.Init(DelayLength<7680>(4453.0f * sr
                                        + kTankModulationHeadroom));
    tank_left_b_.Init(DelayLength<6400>(3720.0f * sr));
    tank_right_a_.Init(DelayLength<7200>(4217.0f * sr
                                         + kTankModulationHeadroom));
    tank_right_b_.Init(DelayLength<4608>(2656.0f * sr));
    tank_left_allpass_.Init(DelayLength<1536>(672.0f * sr
                                               + kAllpassModulationHeadroom));
    tank_right_allpass_.Init(DelayLength<1536>(908.0f * sr
                                                + kAllpassModulationHeadroom));
  }

  float sample_rate_ = 48000.0f;
  float phase_increment_ = 6.28318530718f / 48000.0f;
  float lfo_sine_ = 0.0f, lfo_cosine_ = 1.0f;
  float lfo_step_sine_ = 0.0f, lfo_step_cosine_ = 1.0f;
  uint32_t lfo_renorm_counter_ = 0;
  float decay_ = 0.65f, damping_ = 0.35f, size_ = 1.0f, modulation_ = 0.25f;
  float feedback_ = 0.70f, damping_coefficient_ = 0.33f;
  float diffuser_delay_[4]{};
  float left_a_delay_ = 7180.0f, left_b_delay_ = 6000.0f;
  float right_a_delay_ = 6805.0f, right_b_delay_ = 4285.0f;
  float left_allpass_delay_ = 1084.0f, right_allpass_delay_ = 1464.0f;

  detail::Allpass<768> diffuser_1_, diffuser_2_, diffuser_3_, diffuser_4_;
  detail::Allpass<1536> tank_left_allpass_, tank_right_allpass_;
  detail::FixedDelayLine<7680> tank_left_a_;
  detail::FixedDelayLine<6400> tank_left_b_;
  detail::FixedDelayLine<7200> tank_right_a_;
  detail::FixedDelayLine<4608> tank_right_b_;
  detail::OnePoleLowpass damping_left_, damping_right_;
};

}  // namespace reverb
}  // namespace hothouse_nam
