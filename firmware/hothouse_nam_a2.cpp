#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>

#include "daisy.h"
#include "daisysp.h"
#include "daisysp-lgpl.h"
#include "embedded_a2_model.h"
#include "hothouse.h"
#include "nam_a2_runtime.h"

using clevelandmusicco::Hothouse;
using daisy::AudioHandle;
using daisy::Led;
using daisy::PersistentStorage;
using daisy::SaiHandle;
using daisy::System;
using daisysp::ReverbSc;
using daisysp::fonepole;

namespace
{
constexpr size_t kAudioBlockSize = nam_a2_daisy::kBlockSize;
constexpr uint32_t kCycleBudget = 480000;
constexpr float kInputGainMinimum = 0.25f;
constexpr float kInputGainOctaves = 4.0f;
constexpr float kGateOpenThreshold = 0.001f;
constexpr float kGateCloseThreshold = 0.0005f;
constexpr float kBassLowpassCoefficient = 0.032195f;  // 250 Hz at 48 kHz
constexpr float kTrebleLowpassCoefficient = 0.279675f; // 2.5 kHz at 48 kHz
constexpr float kRoomFeedback = 0.78f;
constexpr float kRoomDampingHz = 12000.0f;
constexpr float kHallFeedback = 0.92f;
constexpr float kHallDampingHz = 7000.0f;
constexpr uint32_t kPresetMagic = 0x48505253; // "HPRS"
constexpr uint32_t kPresetVersion = 1;
constexpr uint32_t kPresetQspiOffset = 0x007ff000;
constexpr uint32_t kPresetHoldMs = 1500;
constexpr uint32_t kPresetSavedIndicationMs = 1000;
constexpr float kPresetKnobMovementThreshold = 0.02f;
static_assert(kAudioBlockSize == 48, "A2-Lite runtime requires 48-sample blocks");
static_assert(embedded_a2_model::kWeightCount == nam_a2_daisy::kA2WeightCount,
              "Embedded model does not match the A2-Lite runtime");

struct SmoothedFloat
{
  float current = 0.0f;
  float target = 0.0f;

  float Tick(float coefficient = 0.002f)
  {
    fonepole(current, target, coefficient);
    return current;
  }
};

struct PresetSettings
{
  uint32_t magic;
  uint32_t version;
  float knobs[Hothouse::KNOB_LAST];
  uint32_t toggles[3];

  bool operator!=(const PresetSettings& other) const
  {
    if(magic != other.magic || version != other.version)
      return true;
    for(size_t i = 0; i < Hothouse::KNOB_LAST; ++i)
      if(knobs[i] != other.knobs[i])
        return true;
    for(size_t i = 0; i < 3; ++i)
      if(toggles[i] != other.toggles[i])
        return true;
    return false;
  }
};

Hothouse hw;
NAM_A2_STATE_DATA static nam_a2_daisy::A2Player model;
ReverbSc DSY_SDRAM_BSS reverb;
PersistentStorage<PresetSettings> preset_storage(hw.seed.qspi);
PresetSettings saved_preset = {};

SmoothedFloat input_gain_smoothed = {1.0f, 1.0f};
SmoothedFloat output_smoothed = {0.8f, 0.8f};
SmoothedFloat reverb_mix_smoothed = {0.0f, 0.0f};
SmoothedFloat bass_gain_smoothed = {1.0f, 1.0f};
SmoothedFloat mid_gain_smoothed = {1.0f, 1.0f};
SmoothedFloat treble_gain_smoothed = {1.0f, 1.0f};
float input_envelope = 0.0f;
float input_gate_gain = 0.0f;
bool input_gate_open = false;
float bass_lowpass_state = 0.0f;
float treble_lowpass_state = 0.0f;

Led led_effect;
Led led_status;
float mono_in[kAudioBlockSize];
float mono_out[kAudioBlockSize];
char usb_log_buffers[2][192] = {};
uint8_t usb_log_active_buffer = 1;

volatile bool effect_enabled = false;
volatile bool model_loaded = false;
volatile bool reverb_ready = false;
volatile bool reverb_enabled = false;
volatile uint32_t cb_process_cycles = 0;
volatile uint32_t cb_max_cycles = 0;
bool preset_engaged = false;
bool preset_press_active = false;
bool preset_long_press_handled = false;
bool preset_chord_cancelled = false;
uint32_t preset_saved_indication_until = 0;
float preset_panel_knobs[Hothouse::KNOB_LAST] = {};
Hothouse::ToggleswitchPosition preset_panel_toggles[3] = {};
float preset_working_knobs[Hothouse::KNOB_LAST] = {};
Hothouse::ToggleswitchPosition preset_working_toggles[3] = {};
bool preset_knob_overridden[Hothouse::KNOB_LAST] = {};
bool preset_toggle_overridden[3] = {};
#if HOTHOUSE_A2_DIAGNOSTIC
volatile uint8_t diagnostic_mode = 0;
#endif

void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t size);

void UsbLog(const char* format, ...)
{
  const uint8_t candidate = usb_log_active_buffer ^ 1U;
  char* buffer = usb_log_buffers[candidate];

  va_list args;
  va_start(args, format);
  const int result = std::vsnprintf(buffer, sizeof(usb_log_buffers[0]) - 2,
                                    format, args);
  va_end(args);
  if(result < 0)
    return;

  size_t length = static_cast<size_t>(result);
  if(length > sizeof(usb_log_buffers[0]) - 2)
    length = sizeof(usb_log_buffers[0]) - 2;
  buffer[length++] = '\r';
  buffer[length++] = '\n';

  if(hw.seed.usb_handle.TransmitInternal(
         reinterpret_cast<uint8_t*>(buffer), length)
     == daisy::UsbHandle::Result::OK)
    usb_log_active_buffer = candidate;
}

float InputGainFromKnob(float knob)
{
  // 0.25x, 0.5x, 1x, 2x, 4x across the knob's quarter points.
  return kInputGainMinimum * std::pow(2.0f, knob * kInputGainOctaves);
}

PresetSettings DefaultPreset()
{
  PresetSettings settings = {};
  settings.magic = kPresetMagic;
  settings.version = kPresetVersion;
  settings.knobs[Hothouse::KNOB_1] = 0.5f;
  settings.knobs[Hothouse::KNOB_2] = 0.2f;
  settings.knobs[Hothouse::KNOB_3] = 0.8f;
  settings.knobs[Hothouse::KNOB_4] = 0.5f;
  settings.knobs[Hothouse::KNOB_5] = 0.5f;
  settings.knobs[Hothouse::KNOB_6] = 0.5f;
  settings.toggles[Hothouse::TOGGLESWITCH_1] = Hothouse::TOGGLESWITCH_MIDDLE;
  settings.toggles[Hothouse::TOGGLESWITCH_2] = Hothouse::TOGGLESWITCH_MIDDLE;
  settings.toggles[Hothouse::TOGGLESWITCH_3] = Hothouse::TOGGLESWITCH_MIDDLE;
  return settings;
}

bool PresetIsValid(const PresetSettings& settings)
{
  if(settings.magic != kPresetMagic || settings.version != kPresetVersion)
    return false;
  for(size_t i = 0; i < Hothouse::KNOB_LAST; ++i)
    if(!std::isfinite(settings.knobs[i]) || settings.knobs[i] < 0.0f
       || settings.knobs[i] > 1.0f)
      return false;
  for(size_t i = 0; i < 3; ++i)
    if(settings.toggles[i] > Hothouse::TOGGLESWITCH_DOWN)
      return false;
  return true;
}

float ActiveKnob(Hothouse::Knob knob)
{
  return preset_engaged && !preset_knob_overridden[knob]
      ? saved_preset.knobs[knob]
      : preset_engaged ? preset_working_knobs[knob] : hw.GetKnobValue(knob);
}

Hothouse::ToggleswitchPosition ActiveToggle(Hothouse::Toggleswitch toggle)
{
  if(!preset_engaged || preset_toggle_overridden[toggle])
    return preset_engaged ? preset_working_toggles[toggle]
                          : hw.GetToggleswitchPosition(toggle);
  return static_cast<Hothouse::ToggleswitchPosition>(saved_preset.toggles[toggle]);
}

void BeginPresetRecall()
{
  for(size_t i = 0; i < Hothouse::KNOB_LAST; ++i)
  {
    preset_panel_knobs[i] = hw.GetKnobValue(static_cast<Hothouse::Knob>(i));
    preset_working_knobs[i] = saved_preset.knobs[i];
    preset_knob_overridden[i] = false;
  }
  for(size_t i = 0; i < 3; ++i)
  {
    preset_panel_toggles[i]
        = hw.GetToggleswitchPosition(static_cast<Hothouse::Toggleswitch>(i));
    preset_working_toggles[i]
        = static_cast<Hothouse::ToggleswitchPosition>(saved_preset.toggles[i]);
    preset_toggle_overridden[i] = false;
  }
  preset_engaged = true;
}

void UpdatePresetPanelOverrides()
{
  if(!preset_engaged)
    return;

  for(size_t i = 0; i < Hothouse::KNOB_LAST; ++i)
  {
    const float physical = hw.GetKnobValue(static_cast<Hothouse::Knob>(i));
    if(!preset_knob_overridden[i]
       && std::fabs(physical - preset_panel_knobs[i]) >= kPresetKnobMovementThreshold)
      preset_knob_overridden[i] = true;
    if(preset_knob_overridden[i])
    {
      const float baseline = preset_panel_knobs[i];
      const float saved = saved_preset.knobs[i];
      if(physical >= baseline)
      {
        const float available = 1.0f - baseline;
        preset_working_knobs[i] = available > 0.001f
            ? saved + (physical - baseline) * (1.0f - saved) / available
            : saved;
      }
      else
      {
        preset_working_knobs[i] = baseline > 0.001f
            ? saved * physical / baseline
            : saved;
      }
    }
  }
  for(size_t i = 0; i < 3; ++i)
  {
    const Hothouse::ToggleswitchPosition physical
        = hw.GetToggleswitchPosition(static_cast<Hothouse::Toggleswitch>(i));
    if(!preset_toggle_overridden[i] && physical != preset_panel_toggles[i])
      preset_toggle_overridden[i] = true;
    if(preset_toggle_overridden[i])
      preset_working_toggles[i] = physical;
  }
}

void CapturePhysicalPanel(PresetSettings& settings)
{
  settings.magic = kPresetMagic;
  settings.version = kPresetVersion;
  for(size_t i = 0; i < Hothouse::KNOB_LAST; ++i)
    settings.knobs[i] = hw.GetKnobValue(static_cast<Hothouse::Knob>(i));
  for(size_t i = 0; i < 3; ++i)
    settings.toggles[i] = static_cast<uint32_t>(
        hw.GetToggleswitchPosition(static_cast<Hothouse::Toggleswitch>(i)));
}

float EqGainFromKnob(float knob)
{
  // Give the physical noon position a small unity-gain deadband.
  if(std::fabs(knob - 0.5f) < 0.025f)
    return 1.0f;
  const float decibels = (knob - 0.5f) * 20.0f;
  return std::pow(10.0f, decibels / 20.0f);
}

float ProcessToneStack(float input)
{
  bass_lowpass_state += kBassLowpassCoefficient * (input - bass_lowpass_state);
  treble_lowpass_state += kTrebleLowpassCoefficient * (input - treble_lowpass_state);

  const float low = bass_lowpass_state;
  const float mid = treble_lowpass_state - bass_lowpass_state;
  const float high = input - treble_lowpass_state;
  return low * bass_gain_smoothed.Tick()
       + mid * mid_gain_smoothed.Tick()
       + high * treble_gain_smoothed.Tick();
}

void UpdateReverbControls()
{
  const Hothouse::ToggleswitchPosition position
      = ActiveToggle(Hothouse::TOGGLESWITCH_1);

  switch(position)
  {
    case Hothouse::TOGGLESWITCH_UP:
      reverb.SetFeedback(kRoomFeedback);
      reverb.SetLpFreq(kRoomDampingHz);
      reverb_enabled = reverb_ready;
      break;
    case Hothouse::TOGGLESWITCH_DOWN:
      reverb.SetFeedback(kHallFeedback);
      reverb.SetLpFreq(kHallDampingHz);
      reverb_enabled = reverb_ready;
      break;
    case Hothouse::TOGGLESWITCH_MIDDLE:
    case Hothouse::TOGGLESWITCH_UNKNOWN:
    default:
      reverb_enabled = false;
      break;
  }

  reverb_mix_smoothed.target
      = reverb_enabled ? ActiveKnob(Hothouse::KNOB_2) : 0.0f;
}

void CheckStartupRecovery()
{
  for(size_t i = 0; i < 10; ++i)
  {
    hw.ProcessAllControls();
    System::Delay(2);
  }

  while(hw.switches[Hothouse::FOOTSWITCH_1].Pressed()
        && hw.switches[Hothouse::FOOTSWITCH_2].Pressed())
  {
    hw.ProcessAllControls();
    hw.CheckResetToBootloader();
    System::Delay(5);
  }
}

void UpdateLedState()
{
  led_effect.Set(effect_enabled ? 1.0f : 0.0f);
  const uint32_t now_ms = System::GetNow();
  const bool fault = !model_loaded || cb_max_cycles >= kCycleBudget;
  bool status_on = fault ? ((now_ms / 125U) & 1U) != 0U : preset_engaged;
  if(!fault && static_cast<int32_t>(preset_saved_indication_until - now_ms) > 0)
    status_on = ((now_ms / 100U) & 1U) != 0U;
#if HOTHOUSE_A2_DIAGNOSTIC
  if(!status_on && diagnostic_mode == 1)
    status_on = true;
  else if(!status_on && diagnostic_mode == 2)
    status_on = ((System::GetNow() / 250U) & 1U) != 0U;
#endif
  led_status.Set(status_on ? 1.0f : 0.0f);
  led_effect.Update();
  led_status.Update();
}

void SavePreset()
{
  CapturePhysicalPanel(saved_preset);
  hw.StopAudio();
  preset_storage.GetSettings() = saved_preset;
  preset_storage.Save();
  hw.StartAudio(AudioCallback);
  BeginPresetRecall();
  preset_saved_indication_until = System::GetNow() + kPresetSavedIndicationMs;
  UsbLog("preset saved");
}

void ProcessPresetFootswitch()
{
  daisy::Switch& footswitch = hw.switches[Hothouse::FOOTSWITCH_2];
  if(footswitch.RisingEdge())
  {
    preset_press_active = true;
    preset_long_press_handled = false;
    preset_chord_cancelled = hw.switches[Hothouse::FOOTSWITCH_1].Pressed();
  }

  if(preset_press_active && hw.switches[Hothouse::FOOTSWITCH_1].Pressed())
    preset_chord_cancelled = true;

  if(preset_press_active && !preset_long_press_handled && !preset_chord_cancelled
     && footswitch.TimeHeldMs() >= kPresetHoldMs)
  {
    SavePreset();
    preset_long_press_handled = true;
  }

  if(footswitch.FallingEdge())
  {
    if(preset_press_active && !preset_long_press_handled && !preset_chord_cancelled)
    {
      if(preset_engaged)
        preset_engaged = false;
      else
        BeginPresetRecall();
      UsbLog("preset %s", preset_engaged ? "engaged" : "live panel");
    }
    preset_press_active = false;
  }
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
  __set_FPSCR(__get_FPSCR() | (1U << 24) | (1U << 25));

  if(effect_enabled && model_loaded && size == kAudioBlockSize)
  {
    const uint32_t cyc0 = DWT->CYCCNT;
    for(size_t i = 0; i < size; ++i)
    {
      const float input = in[0][i];
      const float magnitude = std::fabs(input);
      const float envelope_coefficient = magnitude > input_envelope ? 0.05f : 0.0002f;
      fonepole(input_envelope, magnitude, envelope_coefficient);

      if(input_gate_open)
      {
        if(input_envelope < kGateCloseThreshold)
          input_gate_open = false;
      }
      else if(input_envelope > kGateOpenThreshold)
        input_gate_open = true;

      const float gate_target = input_gate_open ? 1.0f : 0.0f;
      fonepole(input_gate_gain, gate_target, input_gate_open ? 0.05f : 0.001f);
      const float gained_input = input * input_gain_smoothed.Tick() * input_gate_gain;
#if HOTHOUSE_A2_DIAGNOSTIC
      mono_in[i] = diagnostic_mode == 0 ? gained_input : 0.0f;
#else
      mono_in[i] = gained_input;
#endif
    }

    model.process_block_48(mono_in, mono_out);

    for(size_t i = 0; i < size; ++i)
    {
      const float dry = ProcessToneStack(mono_out[i]);
      float wet_left = 0.0f;
      float wet_right = 0.0f;
      if(reverb_ready)
      {
        const float send = reverb_enabled ? dry : 0.0f;
        reverb.Process(send, send, &wet_left, &wet_right);
      }

      const float mix = reverb_mix_smoothed.Tick();
      const float dry_mix = 1.0f - mix;
      const float output = output_smoothed.Tick();
      float processed_left = (dry * dry_mix + wet_left * mix) * output;
      float processed_right = (dry * dry_mix + wet_right * mix) * output;
#if HOTHOUSE_A2_DIAGNOSTIC
      if(diagnostic_mode == 2)
      {
        processed_left = 0.0f;
        processed_right = 0.0f;
      }
#endif
      out[0][i] = processed_left;
      out[1][i] = processed_right;
    }

    const uint32_t elapsed = DWT->CYCCNT - cyc0;
    cb_process_cycles = elapsed;
    if(elapsed > cb_max_cycles)
      cb_max_cycles = elapsed;

    if(elapsed >= kCycleBudget)
      effect_enabled = false;
  }
  else
  {
    for(size_t i = 0; i < size; ++i)
    {
      if(reverb_ready)
      {
        float discarded_left;
        float discarded_right;
        reverb.Process(0.0f, 0.0f, &discarded_left, &discarded_right);
      }
      const float dry = in[0][i] * output_smoothed.Tick();
      out[0][i] = dry;
      out[1][i] = dry;
    }
  }
}

void BenchmarkModel()
{
  if(!model_loaded)
    return;

  for(float& sample : mono_in)
    sample = 0.0f;

  DWT->CYCCNT = 0;
  model.process_block_48(mono_in, mono_out);
  UsbLog("A2-Lite benchmark cycles=%lu",
         static_cast<unsigned long>(DWT->CYCCNT));
}

} // namespace

int main()
{
  hw.Init(true);
  hw.SetAudioBlockSize(kAudioBlockSize);
  hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

  uint32_t fpscr = __get_FPSCR();
  fpscr |= (1U << 24) | (1U << 25);
  __set_FPSCR(fpscr);
  volatile uint32_t* FPDSCR = reinterpret_cast<volatile uint32_t*>(0xE000EF3C);
  *FPDSCR |= (1U << 24) | (1U << 25);

  hw.seed.usb_handle.Init(daisy::UsbHandle::FS_INTERNAL);
  UsbLog("HothouseNAM A2-Lite boot");

  hw.StartAdc();
  CheckStartupRecovery();
  hw.ProcessAllControls();

  const PresetSettings default_preset = DefaultPreset();
  preset_storage.Init(default_preset, kPresetQspiOffset);
  saved_preset = preset_storage.GetSettings();
  if(!PresetIsValid(saved_preset))
  {
    preset_storage.RestoreDefaults();
    saved_preset = preset_storage.GetSettings();
  }

  input_gain_smoothed.current = input_gain_smoothed.target
      = InputGainFromKnob(ActiveKnob(Hothouse::KNOB_1));
  output_smoothed.current = output_smoothed.target = ActiveKnob(Hothouse::KNOB_3);
  bass_gain_smoothed.current = bass_gain_smoothed.target
      = EqGainFromKnob(ActiveKnob(Hothouse::KNOB_4));
  mid_gain_smoothed.current = mid_gain_smoothed.target
      = EqGainFromKnob(ActiveKnob(Hothouse::KNOB_5));
  treble_gain_smoothed.current = treble_gain_smoothed.target
      = EqGainFromKnob(ActiveKnob(Hothouse::KNOB_6));

  led_effect.Init(hw.seed.GetPin(Hothouse::LED_1), false);
  led_status.Init(hw.seed.GetPin(Hothouse::LED_2), false);
  led_status.Set(1.0f);
  led_status.Update();

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  reverb_ready = reverb.Init(hw.AudioSampleRate()) == 0;
  UpdateReverbControls();
  reverb_mix_smoothed.current = reverb_mix_smoothed.target;

  // Start dry so model initialization can never block audio or recovery.
  hw.StartAudio(AudioCallback);

  model_loaded = model.load_weights(embedded_a2_model::kWeights,
                                    embedded_a2_model::kWeightCount);
  effect_enabled = model_loaded;
  UsbLog("embedded A2-Lite model: %s (%s, submodel=%d)",
         model_loaded ? "ok" : "failed",
         embedded_a2_model::kName,
         embedded_a2_model::kSourceSubmodel);
  BenchmarkModel();

  uint32_t last_log_ms = System::GetNow();
  while(true)
  {
    hw.ProcessAllControls();
    const uint32_t now_ms = System::GetNow();

    UpdatePresetPanelOverrides();

    input_gain_smoothed.target = InputGainFromKnob(ActiveKnob(Hothouse::KNOB_1));
    output_smoothed.target = ActiveKnob(Hothouse::KNOB_3);
    bass_gain_smoothed.target = EqGainFromKnob(ActiveKnob(Hothouse::KNOB_4));
    mid_gain_smoothed.target = EqGainFromKnob(ActiveKnob(Hothouse::KNOB_5));
    treble_gain_smoothed.target = EqGainFromKnob(ActiveKnob(Hothouse::KNOB_6));
    UpdateReverbControls();

    if(hw.switches[Hothouse::FOOTSWITCH_1].RisingEdge() && model_loaded)
      effect_enabled = !effect_enabled;

    ProcessPresetFootswitch();

#if HOTHOUSE_A2_DIAGNOSTIC
    diagnostic_mode = 0;
#endif

    UpdateLedState();
    hw.CheckResetToBootloader();

    if(now_ms - last_log_ms >= 1000U)
    {
      UsbLog("A2-Lite cycles=%lu max=%lu model=%s",
             static_cast<unsigned long>(cb_process_cycles),
             static_cast<unsigned long>(cb_max_cycles),
             model_loaded ? "ready" : "missing");
      last_log_ms = now_ms;
    }

    System::Delay(5);
  }
}
