#include <cstdint>
#include <cstring>
#include <memory>

#include "daisy.h"
#include "daisysp.h"
#if HOTHOUSE_USE_IR
#include "embedded_ir.h"
#endif
#include "embedded_model.h"
#include "hothouse.h"
#include "NAM/activations.h"
#include "namb/get_dsp_namb.h"

using clevelandmusicco::Hothouse;
using daisy::AudioHandle;
using daisy::Led;
using daisy::SaiHandle;
using daisy::System;
#if HOTHOUSE_USE_IR
using daisysp::FIR;
#endif
using daisysp::fonepole;

namespace
{
constexpr size_t kAudioBlockSize = 48;
#if HOTHOUSE_USE_IR
constexpr size_t kMaxIrLength = 1024;
#endif
constexpr uint32_t kCycleBudget = 480000;
#if HOTHOUSE_USE_IR
static_assert(embedded_ir::kLength > 0, "Embedded IR cannot be empty");
static_assert(embedded_ir::kLength <= kMaxIrLength, "Embedded IR exceeds FIR capacity");
static_assert(embedded_ir::kSampleRate == 48000, "Embedded IR must be 48 kHz");
#endif

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

Hothouse hw;
std::unique_ptr<nam::DSP> model;

SmoothedFloat input_gain_smoothed = {1.0f, 1.0f};
SmoothedFloat output_smoothed = {0.8f, 0.8f};

#if HOTHOUSE_USE_IR
FIR<kMaxIrLength, kAudioBlockSize> cabinet_ir;
#endif
Led led_effect;
Led led_status;

NAM_SAMPLE mono_in[kAudioBlockSize];
NAM_SAMPLE mono_out[kAudioBlockSize];
float ir_out[kAudioBlockSize];
alignas(4) uint8_t model_blob[embedded_model::kNambSize];

volatile bool effect_enabled = false;
volatile uint32_t cb_process_cycles = 0;
volatile uint32_t cb_max_cycles = 0;

float sample_rate_hz = 48000.0f;

void CheckStartupRecovery()
{
  // Debounce briefly, then keep servicing the DFU gesture before any model
  // allocation or prewarming can stall. Hold both switches while powering on.
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
  led_status.Set((!model || cb_max_cycles >= kCycleBudget) ? 1.0f : 0.0f);
  led_effect.Update();
  led_status.Update();
}

bool LoadEmbeddedModel()
{
  try
  {
    if(embedded_model::kIsNamb)
    {
      if(embedded_model::kNambSize == 0)
        return false;
      // Match nam-pedal's SD-card path: deserialize from aligned SRAM, not
      // directly from the execute-in-place QSPI code/data region.
      std::memcpy(model_blob, embedded_model::kNambData, embedded_model::kNambSize);
      model = nam::get_dsp_namb(model_blob, embedded_model::kNambSize);
      led_effect.Set(1.0f);
      led_effect.Update();
    }
    else
      return false;
  }
  catch(const std::exception& e)
  {
    hw.seed.PrintLine("model load failed: %s", e.what());
    model.reset();
    return false;
  }

  if(!model)
    return false;

  model->ResetAndPrewarm(sample_rate_hz, kAudioBlockSize);
  return true;
}

void BenchmarkModel()
{
  if(!model)
    return;

  for(size_t i = 0; i < kAudioBlockSize; ++i)
    mono_in[i] = 0.0f;

  NAM_SAMPLE* input_ptr = mono_in;
  NAM_SAMPLE* output_ptr = mono_out;

  DWT->CYCCNT = 0;
  model->process(&input_ptr, &output_ptr, static_cast<int>(kAudioBlockSize));
  hw.seed.PrintLine("benchmark cycles=%lu",
                    static_cast<unsigned long>(DWT->CYCCNT));
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
  __set_FPSCR(__get_FPSCR() | (1U << 24) | (1U << 25));

  if(effect_enabled && model)
  {
    for(size_t i = 0; i < size; ++i)
    {
      const float input_gain = input_gain_smoothed.Tick();
      mono_in[i] = in[0][i] * input_gain;
    }

    NAM_SAMPLE* input_ptr = mono_in;
    NAM_SAMPLE* output_ptr = mono_out;

    const uint32_t cyc0 = DWT->CYCCNT;
    model->process(&input_ptr, &output_ptr, static_cast<int>(size));
#if HOTHOUSE_USE_IR
    cabinet_ir.ProcessBlock(mono_out, ir_out, size);
#else
    for(size_t i = 0; i < size; ++i)
      ir_out[i] = mono_out[i];
#endif
    const uint32_t elapsed = DWT->CYCCNT - cyc0;
    cb_process_cycles = elapsed;
    if(elapsed > cb_max_cycles)
      cb_max_cycles = elapsed;

    // Recover immediately from a model that cannot meet the audio deadline.
    // Without this trip, back-to-back audio interrupts can starve the control
    // loop and make an oversized capture appear to lock the pedal.
    if(elapsed >= kCycleBudget)
      effect_enabled = false;

    for(size_t i = 0; i < size; ++i)
    {
      const float output = output_smoothed.Tick();
      const float processed = ir_out[i] * output;
      out[0][i] = processed;
      out[1][i] = processed;
    }
  }
  else
  {
    for(size_t i = 0; i < size; ++i)
    {
      const float output = output_smoothed.Tick();
      const float dry = in[0][i] * output;
      out[0][i] = dry;
      out[1][i] = dry;
    }
  }
}

} // namespace

int main()
{
  hw.Init(true);
  hw.SetAudioBlockSize(kAudioBlockSize);
  hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
  sample_rate_hz = hw.AudioSampleRate();

  uint32_t fpscr = __get_FPSCR();
  fpscr |= (1U << 24) | (1U << 25);
  __set_FPSCR(fpscr);
  volatile uint32_t* FPDSCR = reinterpret_cast<volatile uint32_t*>(0xE000EF3C);
  *FPDSCR |= (1U << 24) | (1U << 25);

  hw.seed.StartLog(false);
  hw.seed.PrintLine("HothouseNAM boot");

  hw.StartAdc();
  CheckStartupRecovery();
  hw.ProcessAllControls();

  input_gain_smoothed.current = input_gain_smoothed.target
      = 0.25f + hw.GetKnobValue(Hothouse::KNOB_1) * 3.75f;
  output_smoothed.current = output_smoothed.target = hw.GetKnobValue(Hothouse::KNOB_6);

  led_effect.Init(hw.seed.GetPin(Hothouse::LED_1), false);
  led_status.Init(hw.seed.GetPin(Hothouse::LED_2), false);
  led_status.Set(1.0f);
  led_status.Update();
#if HOTHOUSE_USE_IR
  cabinet_ir.Init(embedded_ir::kData, embedded_ir::kLength, true);
#endif

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  // The Cortex-M7 cannot afford libm tanh for every WaveNet activation.
  nam::activations::Activation::enable_fast_tanh();

  // Start in dry mode before model construction. A corrupt or oversized model
  // must never prevent the pedal from passing audio or servicing interrupts.
  hw.StartAudio(AudioCallback);

  const bool model_loaded = LoadEmbeddedModel();
  effect_enabled = model_loaded;
  hw.seed.PrintLine("embedded model: %s (%s)",
                    model_loaded ? "ok" : "failed",
                    embedded_model::kName);
#if HOTHOUSE_USE_IR
  hw.seed.PrintLine("embedded IR: %s (%u taps)",
                    embedded_ir::kName,
                    static_cast<unsigned>(embedded_ir::kLength));
#else
  hw.seed.PrintLine("embedded IR: disabled (capture includes cabinet)");
#endif
  BenchmarkModel();

  uint32_t last_log_ms = System::GetNow();
  while(true)
  {
    hw.ProcessAllControls();
    const uint32_t now_ms = System::GetNow();

    input_gain_smoothed.target = 0.25f + hw.GetKnobValue(Hothouse::KNOB_1) * 3.75f;
    output_smoothed.target = hw.GetKnobValue(Hothouse::KNOB_6);

    if(hw.switches[Hothouse::FOOTSWITCH_1].RisingEdge())
      effect_enabled = !effect_enabled;

    UpdateLedState();
    hw.CheckResetToBootloader();

    if(now_ms - last_log_ms >= 1000U)
    {
#if HOTHOUSE_USE_IR
      const char* ir_name = embedded_ir::kName;
#else
      const char* ir_name = "disabled";
#endif
      hw.seed.PrintLine("cycles=%lu max=%lu model=%s ir=%s",
                        static_cast<unsigned long>(cb_process_cycles),
                        static_cast<unsigned long>(cb_max_cycles),
                        model ? "ready" : "missing",
                        ir_name);
      last_log_ms = now_ms;
    }

    System::Delay(5);
  }
}
