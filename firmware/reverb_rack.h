#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <new>

#include "daisysp-lgpl.h"
#include "reverb_config.h"
#include "reverbs/dattorro_reverb.h"
#include "reverbs/fdn16_reverb.h"
#include "reverbs/hybrid_space_reverb.h"

namespace hothouse_nam
{

// Exactly two engine state blocks are constructed in SDRAM: the engines mapped
// to the physical UP and DOWN positions. All other compiled algorithms occupy
// flash only. Reconfiguring slots happens from the foreground USB handler
// while audio is stopped, never from the audio interrupt.
class ReverbRack
{
 public:
  enum class Position : uint8_t { Off, Up, Down };

  bool Init(float sample_rate, const ReverbSlotConfig& config)
  {
    if(!std::isfinite(sample_rate) || sample_rate < 8000.0f
       || sample_rate > 52000.0f)
      return false;
    sample_rate_ = sample_rate;
    quiet_samples_required_ = static_cast<uint32_t>(sample_rate_);
    maximum_tail_samples_ = static_cast<uint32_t>(sample_rate_ * 30.0f);
    return Configure(config);
  }

  bool Configure(const ReverbSlotConfig& config)
  {
    if(!ValidReverbSlotConfig(config))
      return false;
    DestroySlot(up_);
    DestroySlot(down_);
    config_ = config;
    last_active_ = Position::Off;
    quiet_samples_ = 0;
    tail_samples_remaining_ = 0;
    const bool up_ready = ConstructSlot(up_, config.up);
    const bool down_ready = ConstructSlot(down_, config.down);
    if(!up_ready || !down_ready)
    {
      DestroySlot(up_);
      DestroySlot(down_);
      return false;
    }
    return true;
  }

  const ReverbSlotConfig& Config() const { return config_; }
  bool IsReady() const { return up_.ready && down_.ready; }

  // `active` selects which physical toggle slot receives input. When it is
  // Off, the last active engine receives zero input so its tail can drain.
  // The caller remains responsible for wet/dry mixing and mix smoothing.
  void Process(Position active, float input, float* left, float* right)
  {
    if(left == nullptr || right == nullptr)
      return;
    *left = 0.0f;
    *right = 0.0f;
    if(active == Position::Up)
    {
      ProcessConfiguredSlot(up_, input, left, right);
      last_active_ = Position::Up;
      quiet_samples_ = 0;
      tail_samples_remaining_ = maximum_tail_samples_;
    }
    else if(active == Position::Down)
    {
      ProcessConfiguredSlot(down_, input, left, right);
      last_active_ = Position::Down;
      quiet_samples_ = 0;
      tail_samples_remaining_ = maximum_tail_samples_;
    }
    else if(last_active_ != Position::Off && tail_samples_remaining_ != 0)
    {
      if(last_active_ == Position::Up)
        ProcessConfiguredSlot(up_, 0.0f, left, right);
      else
        ProcessConfiguredSlot(down_, 0.0f, left, right);

      --tail_samples_remaining_;
      if(std::fabs(*left) < 0.00001f && std::fabs(*right) < 0.00001f)
        ++quiet_samples_;
      else
        quiet_samples_ = 0;

      // A full second below -100 dB is longer than every delay line in the
      // catalogue, so no hidden echo remains to wake up later. The hard limit
      // also lets infinite/near-infinite settings eventually release CPU.
      if(quiet_samples_ >= quiet_samples_required_
         || tail_samples_remaining_ == 0)
        last_active_ = Position::Off;
    }
  }

 private:
  // Fdn16Reverb is currently the largest implementation (about 640 KiB).
  // A static storage slot makes allocation deterministic and lets the rack
  // instantiate only two selected engines rather than every compiled engine.
  static constexpr size_t kEngineStorageBytes = sizeof(reverb::Fdn16Reverb);
  static constexpr size_t kEngineStorageAlign = alignof(reverb::Fdn16Reverb);
  static_assert(sizeof(reverb::DattorroReverb) <= kEngineStorageBytes,
                "Increase ReverbRack engine storage");
  static_assert(sizeof(reverb::HybridSpaceReverb) <= kEngineStorageBytes,
                "Increase ReverbRack engine storage");
  static_assert(sizeof(daisysp::ReverbSc) <= kEngineStorageBytes,
                "Increase ReverbRack engine storage");
  static_assert(alignof(reverb::DattorroReverb) <= kEngineStorageAlign
                    && alignof(reverb::HybridSpaceReverb) <= kEngineStorageAlign
                    && alignof(daisysp::ReverbSc) <= kEngineStorageAlign,
                "Increase ReverbRack engine alignment");

  struct Slot
  {
    alignas(kEngineStorageAlign) uint8_t storage[kEngineStorageBytes] = {};
    ReverbId id = ReverbId::Invalid;
    bool ready = false;
  };

  static void DestroySlot(Slot& slot)
  {
    // All current engines have trivial destructors. This explicit teardown
    // point keeps the placement-new ownership model correct for future ones.
    slot.id = ReverbId::Invalid;
    slot.ready = false;
  }

  bool ConstructSlot(Slot& slot, ReverbId id)
  {
    slot.id = id;
    switch(id)
    {
      case ReverbId::ReverbSc:
      {
        daisysp::ReverbSc* engine = new(slot.storage) daisysp::ReverbSc();
        slot.ready = engine->Init(sample_rate_) == 0;
        if(slot.ready)
        {
          engine->SetFeedback(0.86f);
          engine->SetLpFreq(9000.0f);
        }
        break;
      }
      case ReverbId::Dattorro:
        new(slot.storage) reverb::DattorroReverb();
        slot.ready = reinterpret_cast<reverb::DattorroReverb*>(slot.storage)->Init(sample_rate_);
        break;
      case ReverbId::Fdn16:
        new(slot.storage) reverb::Fdn16Reverb();
        slot.ready = reinterpret_cast<reverb::Fdn16Reverb*>(slot.storage)->Init(sample_rate_);
        break;
      case ReverbId::HybridSpace:
        new(slot.storage) reverb::HybridSpaceReverb();
        slot.ready = reinterpret_cast<reverb::HybridSpaceReverb*>(slot.storage)->Init(sample_rate_);
        break;
      default: return false;
    }
    if(!slot.ready)
      slot.id = ReverbId::Invalid;
    else
      ApplyParameters(slot, 0.62f, 0.42f, 0.72f, 0.25f);
    return slot.ready;
  }

  static void ApplyParameters(Slot& slot, float decay, float damping,
                              float size, float modulation)
  {
    switch(slot.id)
    {
      case ReverbId::ReverbSc:
      {
        daisysp::ReverbSc* engine = reinterpret_cast<daisysp::ReverbSc*>(slot.storage);
        engine->SetFeedback(0.58f + 0.39f * decay);
        engine->SetLpFreq(18000.0f - 15500.0f * damping);
        break;
      }
      case ReverbId::Dattorro:
        reinterpret_cast<reverb::DattorroReverb*>(slot.storage)->SetParameters(decay, damping, size, modulation);
        break;
      case ReverbId::Fdn16:
        reinterpret_cast<reverb::Fdn16Reverb*>(slot.storage)->SetParameters(decay, damping, size, modulation);
        break;
      case ReverbId::HybridSpace:
        reinterpret_cast<reverb::HybridSpaceReverb*>(slot.storage)->SetParameters(decay, damping, size, modulation);
        break;
      default: break;
    }
  }

  static void ProcessEngine(Slot& slot, float input, float* left, float* right)
  {
    if(!slot.ready)
      return;
    switch(slot.id)
    {
      case ReverbId::ReverbSc:
        reinterpret_cast<daisysp::ReverbSc*>(slot.storage)->Process(input, input, left, right);
        break;
      case ReverbId::Dattorro:
        reinterpret_cast<reverb::DattorroReverb*>(slot.storage)->Process(input, left, right);
        break;
      case ReverbId::Fdn16:
        reinterpret_cast<reverb::Fdn16Reverb*>(slot.storage)->Process(input, left, right);
        break;
      case ReverbId::HybridSpace:
        reinterpret_cast<reverb::HybridSpaceReverb*>(slot.storage)->Process(input, left, right);
        break;
      default: break;
    }
  }

  // The initial profiles preserve the former room/hall intent. These will be
  // exposed as per-engine USB presets in a later control pass.
  void ProcessConfiguredSlot(Slot& slot, float input, float* left, float* right)
  {
    ProcessEngine(slot, input, left, right);
  }

  Slot up_;
  Slot down_;
  ReverbSlotConfig config_ = {};
  float sample_rate_ = 48000.0f;
  Position last_active_ = Position::Off;
  uint32_t quiet_samples_ = 0;
  uint32_t quiet_samples_required_ = 48000;
  uint32_t tail_samples_remaining_ = 0;
  uint32_t maximum_tail_samples_ = 1440000;
};

} // namespace hothouse_nam
