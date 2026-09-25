#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hothouse_nam
{

class CaptureTransitionController
{
 public:
  static constexpr uint8_t NoSlot = 0xffU;
  static constexpr uint32_t SettleMilliseconds = 60U;
  static constexpr uint32_t FadeSamples = 960U;

  enum class State : uint8_t
  {
    Normal,
    MuteRequested,
    Muted,
    FadeIn,
  };

  void Initialize(uint8_t slot)
  {
    active_observation_ = slot;
    pending_slot_ = NoSlot;
    state_ = State::Normal;
    fade_samples_remaining_ = 0;
    last_output_mono_ = 0.0f;
  }

  void ObserveSlot(uint8_t slot, uint32_t now_ms)
  {
    if(slot >= 3U || slot == active_observation_)
      return;
    active_observation_ = slot;
    pending_slot_ = slot;
    stable_since_ms_ = now_ms;
    if(state_ == State::Normal || state_ == State::FadeIn)
      state_ = State::MuteRequested;
  }

  bool TakeSettledSlot(uint32_t now_ms, uint8_t& slot)
  {
    if(pending_slot_ >= 3U || state_ != State::Muted
       || now_ms - stable_since_ms_ < SettleMilliseconds)
      return false;
    slot = pending_slot_;
    pending_slot_ = NoSlot;
    return true;
  }

  void BeginFadeIn()
  {
    fade_samples_remaining_ = FadeSamples;
    state_ = State::FadeIn;
  }

  void ApplyOutput(float* left, float* right, size_t size)
  {
    if(left == nullptr || right == nullptr || size == 0)
      return;

    const State state = state_;
    if(state == State::MuteRequested)
    {
      size_t cut_index = 0;
      size_t quietest_index = 0;
      float quietest_level = INFINITY;
      float quietest_crossing_level = INFINITY;
      float previous_mono = last_output_mono_;
      bool found_crossing = false;
      for(size_t i = 0; i < size; ++i)
      {
        const float mono = 0.5f * (left[i] + right[i]);
        const float level = std::fmax(std::fabs(left[i]), std::fabs(right[i]));
        if(level < quietest_level)
        {
          quietest_level = level;
          quietest_index = i;
        }
        if((previous_mono <= 0.0f && mono >= 0.0f)
           || (previous_mono >= 0.0f && mono <= 0.0f))
        {
          if(level < quietest_crossing_level)
          {
            quietest_crossing_level = level;
            cut_index = i;
            found_crossing = true;
          }
        }
        previous_mono = mono;
      }
      if(!found_crossing)
        cut_index = quietest_index;
      for(size_t i = cut_index; i < size; ++i)
      {
        left[i] = 0.0f;
        right[i] = 0.0f;
      }
      last_output_mono_ = 0.0f;
      state_ = State::Muted;
      return;
    }

    if(state == State::Muted)
    {
      for(size_t i = 0; i < size; ++i)
      {
        left[i] = 0.0f;
        right[i] = 0.0f;
      }
      last_output_mono_ = 0.0f;
      return;
    }

    if(state == State::FadeIn)
    {
      uint32_t remaining = fade_samples_remaining_;
      for(size_t i = 0; i < size; ++i)
      {
        if(remaining != 0)
        {
          const float gain = static_cast<float>(FadeSamples - remaining + 1U)
              / static_cast<float>(FadeSamples);
          left[i] *= gain;
          right[i] *= gain;
          --remaining;
        }
      }
      fade_samples_remaining_ = remaining;
      if(remaining == 0 && state_ == State::FadeIn)
        state_ = State::Normal;
    }

    last_output_mono_ = 0.5f * (left[size - 1] + right[size - 1]);
  }

  State GetState() const { return state_; }
  uint8_t PendingSlot() const { return pending_slot_; }
  uint32_t FadeSamplesRemaining() const { return fade_samples_remaining_; }

 private:
  volatile State state_ = State::Normal;
  volatile uint32_t fade_samples_remaining_ = 0;
  uint8_t active_observation_ = NoSlot;
  uint8_t pending_slot_ = NoSlot;
  uint32_t stable_since_ms_ = 0;
  float last_output_mono_ = 0.0f;
};

} // namespace hothouse_nam
