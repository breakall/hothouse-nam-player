#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>

#include "daisy.h"
#include "daisysp.h"
#include "daisysp-lgpl.h"
#if HOTHOUSE_USE_IR
#include "embedded_ir_bank.h"
#endif
#include "embedded_model.h"
#include "capture_loader.h"
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
using daisysp::ReverbSc;
using daisysp::fonepole;
using hothouse_nam::ByteRing;
using hothouse_nam::CaptureFormat;
using hothouse_nam::CaptureInfo;
using hothouse_nam::CaptureStore;
using hothouse_nam::Command;
using hothouse_nam::CommandType;

namespace
{
constexpr size_t kAudioBlockSize = 48;
#if HOTHOUSE_USE_IR
constexpr size_t kMaxIrLength = 1024;
#endif
constexpr uint32_t kCycleBudget = 480000;
constexpr float kInputGainMinimum = 0.25f;
constexpr float kInputGainOctaves = 4.0f;
constexpr float kBassLowpassCoefficient = 0.032195f;
constexpr float kTrebleLowpassCoefficient = 0.279675f;
constexpr float kRoomFeedback = 0.78f;
constexpr float kRoomDampingHz = 12000.0f;
constexpr float kHallFeedback = 0.92f;
constexpr float kHallDampingHz = 7000.0f;
#if HOTHOUSE_USE_IR
static_assert(embedded_ir_bank::kCount > 0 && embedded_ir_bank::kCount <= 2,
              "IR bank must contain one or two entries");
static_assert(embedded_ir_bank::kSampleRate == 48000,
              "Cabinet IRs must be 48 kHz");
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
SmoothedFloat reverb_mix_smoothed = {0.0f, 0.0f};
SmoothedFloat bass_gain_smoothed = {1.0f, 1.0f};
SmoothedFloat mid_gain_smoothed = {1.0f, 1.0f};
SmoothedFloat treble_gain_smoothed = {1.0f, 1.0f};
float bass_lowpass_state = 0.0f;
float treble_lowpass_state = 0.0f;

#if HOTHOUSE_USE_IR
FIR<kMaxIrLength, kAudioBlockSize> cabinet_irs[2];
constexpr uint8_t kIrOff = 0xff;
volatile uint8_t active_ir_index = kIrOff;
#endif
ReverbSc DSY_SDRAM_BSS reverb;
Led led_effect;
Led led_status;

NAM_SAMPLE mono_in[kAudioBlockSize];
NAM_SAMPLE mono_out[kAudioBlockSize];
float ir_out[kAudioBlockSize];
constexpr size_t kMaximumA1CaptureSize = 64U * 1024U;
DSY_SDRAM_BSS alignas(4) uint8_t model_blob[kMaximumA1CaptureSize];

class DaisyFlashBackend
{
 public:
  explicit DaisyFlashBackend(daisy::QSPIHandle& qspi) : qspi_(qspi) {}

  const uint8_t* Data(uint32_t offset) const
  {
    return offset < 0x00800000U
        ? static_cast<const uint8_t*>(qspi_.GetData(offset)) : nullptr;
  }

  bool Read(uint32_t offset, uint8_t* output, size_t length) const
  {
    const uint8_t* source = Data(offset);
    if(source == nullptr || output == nullptr || length > 0x00800000U - offset)
      return false;
    std::memcpy(output, source, length);
    return true;
  }

  bool Write(uint32_t offset, const uint8_t* input, size_t length)
  {
    return System::GetProgramMemoryRegion() != System::MemoryRegion::QSPI
        && input != nullptr && offset <= 0x00800000U
        && length <= 0x00800000U - offset
        && qspi_.Write(offset, static_cast<uint32_t>(length),
                       const_cast<uint8_t*>(input)) == daisy::QSPIHandle::OK;
  }

  bool Erase(uint32_t offset, uint32_t length)
  {
    return System::GetProgramMemoryRegion() != System::MemoryRegion::QSPI
        && length != 0 && offset <= 0x00800000U
        && length <= 0x00800000U - offset
        && qspi_.Erase(offset, offset + length) == daisy::QSPIHandle::OK;
  }

 private:
  daisy::QSPIHandle& qspi_;
};

DaisyFlashBackend capture_flash(hw.seed.qspi);
CaptureStore<DaisyFlashBackend> capture_store(capture_flash);
ByteRing<1024> capture_usb_rx;
char capture_line[320] = {};
size_t capture_line_length = 0;
char capture_reply[192] = {};
size_t capture_reply_length = 0;

volatile bool effect_enabled = false;
volatile bool reverb_ready = false;
volatile bool reverb_enabled = false;
volatile uint32_t cb_process_cycles = 0;
volatile uint32_t cb_max_cycles = 0;

float sample_rate_hz = 48000.0f;

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out,
                   size_t size);

void CaptureUsbReceive(uint8_t* bytes, uint32_t* length)
{
  if(length != nullptr)
    capture_usb_rx.PushFromInterrupt(bytes, *length);
}

void QueueCaptureReply(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  const int result = std::vsnprintf(capture_reply, sizeof(capture_reply) - 2,
                                    format, args);
  va_end(args);
  if(result < 0)
    return;
  capture_reply_length = static_cast<size_t>(result);
  if(capture_reply_length > sizeof(capture_reply) - 2)
    capture_reply_length = sizeof(capture_reply) - 2;
  capture_reply[capture_reply_length++] = '\r';
  capture_reply[capture_reply_length++] = '\n';
}

void FlushCaptureReply()
{
  if(capture_reply_length == 0)
    return;
  if(hw.seed.usb_handle.TransmitInternal(
       reinterpret_cast<uint8_t*>(capture_reply), capture_reply_length)
     == daisy::UsbHandle::Result::OK)
    capture_reply_length = 0;
}

float InputGainFromKnob(float knob)
{
  return kInputGainMinimum * std::pow(2.0f, knob * kInputGainOctaves);
}

float EqGainFromKnob(float knob)
{
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
  switch(hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1))
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
    default:
      reverb_enabled = false;
      break;
  }
  reverb_mix_smoothed.target
      = reverb_enabled ? hw.GetKnobValue(Hothouse::KNOB_2) : 0.0f;
}

#if HOTHOUSE_USE_IR
uint8_t IrIndexFromPanel()
{
  const Hothouse::ToggleswitchPosition position
      = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
  if(position == Hothouse::TOGGLESWITCH_MIDDLE
     || position == Hothouse::TOGGLESWITCH_UNKNOWN)
    return kIrOff;
  if(position == Hothouse::TOGGLESWITCH_DOWN
     && embedded_ir_bank::kCount > 1)
    return 1;
  return 0;
}

void UpdateIrSelection()
{
  const uint8_t requested = IrIndexFromPanel();
  if(requested == active_ir_index)
    return;
  if(requested != kIrOff)
    cabinet_irs[requested].Reset();
  active_ir_index = requested;
  hw.seed.PrintLine("cabinet IR: %s", requested == kIrOff
      ? "off" : embedded_ir_bank::kEntries[requested].name);
}
#endif

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

bool LoadInstalledCapture(const CaptureInfo& info, bool audio_running)
{
  if(info.format != CaptureFormat::A1Namb
     || info.size > sizeof(model_blob)
     || !capture_store.ReadPayload(info, model_blob, sizeof(model_blob)))
    return false;

  if(audio_running)
    hw.StopAudio();
  bool loaded = false;
  model.reset();
  try
  {
    model = nam::get_dsp_namb(model_blob, info.size);
    if(model)
    {
      model->ResetAndPrewarm(sample_rate_hz, kAudioBlockSize);
      loaded = true;
    }
  }
  catch(const std::exception& error)
  {
    hw.seed.PrintLine("installed model rejected: %s", error.what());
    model.reset();
  }
  effect_enabled = loaded;
  cb_max_cycles = 0;
  if(audio_running)
    hw.StartAudio(AudioCallback);
  return loaded;
}

void HandleCaptureCommand(char* line)
{
  Command command;
  if(!hothouse_nam::ParseCommand(line, command))
  {
    QueueCaptureReply("HNAM ERR bad_command");
    return;
  }
  if(command.type == CommandType::Info)
  {
    CaptureInfo info;
    if(capture_store.ReadInfo(info) && info.format == CaptureFormat::A1Namb)
      QueueCaptureReply("HNAM OK INFO a1_nano_relu installed %lu %08lx",
                        static_cast<unsigned long>(info.size),
                        static_cast<unsigned long>(info.crc32));
    else
      QueueCaptureReply("HNAM OK INFO a1_nano_relu factory 0 00000000");
  }
  else if(command.type == CommandType::Begin)
  {
    if(command.format != CaptureFormat::A1Namb
       || command.size > sizeof(model_blob))
      QueueCaptureReply("HNAM ERR incompatible_capture");
    else if(!capture_store.Begin(command.name, command.format, command.size,
                                 command.crc32))
      QueueCaptureReply("HNAM ERR begin_failed");
    else
      QueueCaptureReply("HNAM OK BEGIN 0");
  }
  else if(command.type == CommandType::Data)
  {
    if(!capture_store.WriteChunk(command.offset, command.data,
                                 command.data_length))
      QueueCaptureReply("HNAM ERR data_failed");
    else
      QueueCaptureReply("HNAM OK DATA %lu",
                        static_cast<unsigned long>(capture_store.UploadOffset()));
  }
  else if(command.type == CommandType::Commit)
  {
    CaptureInfo info;
    if(!capture_store.Commit(info))
      QueueCaptureReply("HNAM ERR commit_failed");
    else if(!LoadInstalledCapture(info, true))
    {
      hw.StopAudio();
      const bool factory_loaded = LoadEmbeddedModel();
      effect_enabled = factory_loaded;
      hw.StartAudio(AudioCallback);
      QueueCaptureReply("HNAM ERR activate_failed");
    }
    else
      QueueCaptureReply("HNAM OK COMMIT %08lx",
                        static_cast<unsigned long>(info.crc32));
  }
  else if(command.type == CommandType::Cancel)
  {
    capture_store.Cancel();
    QueueCaptureReply("HNAM OK CANCEL");
  }
}

void ProcessCaptureUsb()
{
  FlushCaptureReply();
  if(capture_reply_length != 0)
    return;
  char byte = 0;
  while(capture_usb_rx.Pop(byte))
  {
    if(byte == '\r')
      continue;
    if(byte == '\n')
    {
      if(capture_line_length != 0)
      {
        capture_line[capture_line_length] = '\0';
        HandleCaptureCommand(capture_line);
        capture_line_length = 0;
        return;
      }
      continue;
    }
    if(capture_line_length + 1U >= sizeof(capture_line))
    {
      capture_line_length = 0;
      QueueCaptureReply("HNAM ERR line_too_long");
      return;
    }
    capture_line[capture_line_length++] = byte;
  }
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
    const uint8_t ir_index = active_ir_index;
    if(ir_index == kIrOff)
      for(size_t i = 0; i < size; ++i)
        ir_out[i] = mono_out[i];
    else
      cabinet_irs[ir_index].ProcessBlock(mono_out, ir_out, size);
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
      const float dry = ProcessToneStack(ir_out[i]);
      float wet_left = 0.0f;
      float wet_right = 0.0f;
      if(reverb_ready)
      {
        const float send = reverb_enabled ? dry : 0.0f;
        reverb.Process(send, send, &wet_left, &wet_right);
      }
      const float mix = reverb_mix_smoothed.Tick();
      const float output = output_smoothed.Tick();
      out[0][i] = (dry * (1.0f - mix) + wet_left * mix) * output;
      out[1][i] = (dry * (1.0f - mix) + wet_right * mix) * output;
    }
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
  hw.seed.usb_handle.SetReceiveCallback(CaptureUsbReceive,
                                        daisy::UsbHandle::FS_INTERNAL);
  hw.seed.PrintLine("HothouseNAM boot");

  hw.StartAdc();
  CheckStartupRecovery();
  hw.ProcessAllControls();

  input_gain_smoothed.current = input_gain_smoothed.target
      = InputGainFromKnob(hw.GetKnobValue(Hothouse::KNOB_1));
  output_smoothed.current = output_smoothed.target = hw.GetKnobValue(Hothouse::KNOB_3);
  bass_gain_smoothed.current = bass_gain_smoothed.target
      = EqGainFromKnob(hw.GetKnobValue(Hothouse::KNOB_4));
  mid_gain_smoothed.current = mid_gain_smoothed.target
      = EqGainFromKnob(hw.GetKnobValue(Hothouse::KNOB_5));
  treble_gain_smoothed.current = treble_gain_smoothed.target
      = EqGainFromKnob(hw.GetKnobValue(Hothouse::KNOB_6));

  led_effect.Init(hw.seed.GetPin(Hothouse::LED_1), false);
  led_status.Init(hw.seed.GetPin(Hothouse::LED_2), false);
  led_status.Set(1.0f);
  led_status.Update();
#if HOTHOUSE_USE_IR
  for(size_t i = 0; i < embedded_ir_bank::kCount; ++i)
  {
    const embedded_ir_bank::Entry& entry = embedded_ir_bank::kEntries[i];
    cabinet_irs[i].Init(entry.data, entry.length, true);
  }
  active_ir_index = IrIndexFromPanel();
#endif
  reverb_ready = reverb.Init(sample_rate_hz) == 0;
  UpdateReverbControls();
  reverb_mix_smoothed.current = reverb_mix_smoothed.target;

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  // The Cortex-M7 cannot afford libm tanh for every WaveNet activation.
  nam::activations::Activation::enable_fast_tanh();

  // Start in dry mode before model construction. A corrupt or oversized model
  // must never prevent the pedal from passing audio or servicing interrupts.
  hw.StartAudio(AudioCallback);

  CaptureInfo installed_capture;
  const bool installed_loaded = capture_store.ReadInfo(installed_capture)
      && LoadInstalledCapture(installed_capture, false);
  const bool model_loaded = installed_loaded || LoadEmbeddedModel();
  effect_enabled = model_loaded;
  hw.seed.PrintLine("model: %s (%s)",
                    model_loaded ? "ok" : "failed",
                    installed_loaded ? installed_capture.name
                                     : embedded_model::kName);
#if HOTHOUSE_USE_IR
  hw.seed.PrintLine("embedded IR bank: %u IR(s), active=%s",
                    static_cast<unsigned>(embedded_ir_bank::kCount),
                    active_ir_index == kIrOff
                        ? "off" : embedded_ir_bank::kEntries[active_ir_index].name);
#else
  hw.seed.PrintLine("embedded IR: disabled (capture includes cabinet)");
#endif
  BenchmarkModel();

  uint32_t last_log_ms = System::GetNow();
  while(true)
  {
    ProcessCaptureUsb();
    hw.ProcessAllControls();
    const uint32_t now_ms = System::GetNow();

    input_gain_smoothed.target = InputGainFromKnob(hw.GetKnobValue(Hothouse::KNOB_1));
    output_smoothed.target = hw.GetKnobValue(Hothouse::KNOB_3);
    bass_gain_smoothed.target = EqGainFromKnob(hw.GetKnobValue(Hothouse::KNOB_4));
    mid_gain_smoothed.target = EqGainFromKnob(hw.GetKnobValue(Hothouse::KNOB_5));
    treble_gain_smoothed.target = EqGainFromKnob(hw.GetKnobValue(Hothouse::KNOB_6));
    UpdateReverbControls();
#if HOTHOUSE_USE_IR
    UpdateIrSelection();
#endif

    if(hw.switches[Hothouse::FOOTSWITCH_1].RisingEdge())
      effect_enabled = !effect_enabled;

    UpdateLedState();
    hw.CheckResetToBootloader();

    if(now_ms - last_log_ms >= 1000U)
    {
#if HOTHOUSE_USE_IR
      const char* ir_name = active_ir_index == kIrOff
          ? "off" : embedded_ir_bank::kEntries[active_ir_index].name;
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
