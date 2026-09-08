#include <cstddef>
#include <cstdint>

#include "daisy.h"
#include "daisysp.h"
#include "embedded_a2_model.h"
#include "hothouse.h"
#include "nam_a2_runtime.h"

using clevelandmusicco::Hothouse;
using daisy::AudioHandle;
using daisy::Led;
using daisy::SaiHandle;
using daisy::System;
using daisysp::fonepole;

namespace
{
constexpr size_t kAudioBlockSize = nam_a2_daisy::kBlockSize;
constexpr uint32_t kCycleBudget = 480000;
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

Hothouse hw;
NAM_A2_STATE_DATA static nam_a2_daisy::A2Player model;

SmoothedFloat input_gain_smoothed = {1.0f, 1.0f};
SmoothedFloat output_smoothed = {0.8f, 0.8f};

Led led_effect;
Led led_status;
float mono_in[kAudioBlockSize];
float mono_out[kAudioBlockSize];

volatile bool effect_enabled = false;
volatile bool model_loaded = false;
volatile uint32_t cb_process_cycles = 0;
volatile uint32_t cb_max_cycles = 0;

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
  led_status.Set((!model_loaded || cb_max_cycles >= kCycleBudget) ? 1.0f : 0.0f);
  led_effect.Update();
  led_status.Update();
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
  __set_FPSCR(__get_FPSCR() | (1U << 24) | (1U << 25));

  if(effect_enabled && model_loaded && size == kAudioBlockSize)
  {
    for(size_t i = 0; i < size; ++i)
      mono_in[i] = in[0][i] * input_gain_smoothed.Tick();

    const uint32_t cyc0 = DWT->CYCCNT;
    model.process_block_48(mono_in, mono_out);
    const uint32_t elapsed = DWT->CYCCNT - cyc0;
    cb_process_cycles = elapsed;
    if(elapsed > cb_max_cycles)
      cb_max_cycles = elapsed;

    if(elapsed >= kCycleBudget)
      effect_enabled = false;

    for(size_t i = 0; i < size; ++i)
    {
      const float processed = mono_out[i] * output_smoothed.Tick();
      out[0][i] = processed;
      out[1][i] = processed;
    }
  }
  else
  {
    for(size_t i = 0; i < size; ++i)
    {
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
  hw.seed.PrintLine("A2-Lite benchmark cycles=%lu",
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

  hw.seed.StartLog(false);
  hw.seed.PrintLine("HothouseNAM A2-Lite boot");

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

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  // Start dry so model initialization can never block audio or recovery.
  hw.StartAudio(AudioCallback);

  model_loaded = model.load_weights(embedded_a2_model::kWeights,
                                    embedded_a2_model::kWeightCount);
  effect_enabled = model_loaded;
  hw.seed.PrintLine("embedded A2-Lite model: %s (%s, submodel=%d)",
                    model_loaded ? "ok" : "failed",
                    embedded_a2_model::kName,
                    embedded_a2_model::kSourceSubmodel);
  BenchmarkModel();

  uint32_t last_log_ms = System::GetNow();
  while(true)
  {
    hw.ProcessAllControls();
    const uint32_t now_ms = System::GetNow();

    input_gain_smoothed.target = 0.25f + hw.GetKnobValue(Hothouse::KNOB_1) * 3.75f;
    output_smoothed.target = hw.GetKnobValue(Hothouse::KNOB_6);

    if(hw.switches[Hothouse::FOOTSWITCH_1].RisingEdge() && model_loaded)
      effect_enabled = !effect_enabled;

    UpdateLedState();
    hw.CheckResetToBootloader();

    if(now_ms - last_log_ms >= 1000U)
    {
      hw.seed.PrintLine("A2-Lite cycles=%lu max=%lu model=%s",
                        static_cast<unsigned long>(cb_process_cycles),
                        static_cast<unsigned long>(cb_max_cycles),
                        model_loaded ? "ready" : "missing");
      last_log_ms = now_ms;
    }

    System::Delay(5);
  }
}
