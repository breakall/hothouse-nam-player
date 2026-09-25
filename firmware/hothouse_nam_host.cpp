#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "daisy.h"
#include "daisysp.h"
#include "daisysp-lgpl.h"
#include "sys/dma.h"
#include "capture_loader.h"
#include "capture_transition.h"
#include "reverb_config.h"
#include "reverb_rack.h"
#include "nam_engine.h"
#if HOTHOUSE_USE_IR
#include "embedded_ir_bank.h"
#endif
#include "hothouse.h"

using clevelandmusicco::Hothouse;
using daisy::AudioHandle;
using daisy::Led;
using daisy::PersistentStorage;
using daisy::SaiHandle;
using daisy::System;
#if HOTHOUSE_USE_IR
using daisysp::FIR;
#endif
using daisysp::fonepole;
using hothouse_nam::ByteRing;
using hothouse_nam::CaptureFormat;
using hothouse_nam::CaptureInfo;
using hothouse_nam::CaptureSlotState;
using hothouse_nam::CaptureStore;
using hothouse_nam::CaptureTransitionController;
using hothouse_nam::Command;
using hothouse_nam::CommandType;
using hothouse_nam::ReverbConfigStore;
using hothouse_nam::ReverbId;
using hothouse_nam::ReverbRack;
using hothouse_nam::ReverbSlotConfig;
namespace model_engine = hothouse_nam::model_engine;

namespace
{
constexpr size_t kAudioBlockSize = 48;
constexpr uint32_t kCycleBudget = 480000;
#if HOTHOUSE_USE_IR
constexpr size_t kMaxIrLength = 1024;
static_assert(embedded_ir_bank::kCount > 0 && embedded_ir_bank::kCount <= 2,
              "IR bank must contain one or two entries");
static_assert(embedded_ir_bank::kSampleRate == 48000,
              "Cabinet IRs must be 48 kHz");
#endif
constexpr float kInputGainMinimum = 0.25f;
constexpr float kInputGainOctaves = 4.0f;
constexpr float kGateOpenThreshold = 0.001f;
constexpr float kGateCloseThreshold = 0.0005f;
constexpr float kBassLowpassCoefficient = 0.032195f;  // 250 Hz at 48 kHz
constexpr float kTrebleLowpassCoefficient = 0.279675f; // 2.5 kHz at 48 kHz
constexpr uint32_t kPresetMagic = 0x48505253; // "HPRS"
constexpr uint32_t kPresetVersion = 1;
constexpr uint32_t kPresetQspiOffset = 0x007ff000;
constexpr uint32_t kPresetHoldMs = 1500;
constexpr uint32_t kPresetSavedIndicationMs = 1000;
constexpr float kPresetKnobMovementThreshold = 0.02f;
static_assert(kAudioBlockSize == 48, "NAM engines require 48-sample blocks");

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
DSY_SDRAM_BSS alignas(ReverbRack)
uint8_t reverb_rack_storage[sizeof(ReverbRack)];
ReverbRack* reverb_rack = nullptr;
#if HOTHOUSE_USE_IR
FIR<kMaxIrLength, kAudioBlockSize> cabinet_irs[2];
#endif
PersistentStorage<PresetSettings> preset_storage(hw.seed.qspi);
PresetSettings saved_preset = {};

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
    const bool written = System::GetProgramMemoryRegion() != System::MemoryRegion::QSPI
        && input != nullptr && offset <= 0x00800000U
        && length <= 0x00800000U - offset
        && qspi_.Write(offset, static_cast<uint32_t>(length),
                       const_cast<uint8_t*>(input)) == daisy::QSPIHandle::OK;
    if(written)
      dsy_dma_invalidate_cache_for_buffer(
          static_cast<uint8_t*>(qspi_.GetData(offset)), length);
    return written;
  }

  bool Erase(uint32_t offset, uint32_t length)
  {
    const bool erased = System::GetProgramMemoryRegion() != System::MemoryRegion::QSPI
        && length != 0 && offset <= 0x00800000U
        && length <= 0x00800000U - offset
        && qspi_.Erase(offset, offset + length) == daisy::QSPIHandle::OK;
    if(erased)
      dsy_dma_invalidate_cache_for_buffer(
          static_cast<uint8_t*>(qspi_.GetData(offset)), length);
    return erased;
  }

 private:
  daisy::QSPIHandle& qspi_;
};

DaisyFlashBackend capture_flash(hw.seed.qspi);
CaptureStore<DaisyFlashBackend> capture_store(capture_flash);
ReverbConfigStore<DaisyFlashBackend> reverb_config_store(capture_flash);
ByteRing<1024> capture_usb_rx;
char capture_line[320] = {};
size_t capture_line_length = 0;
char capture_reply[192] = {};
size_t capture_reply_length = 0;

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
#if HOTHOUSE_USE_IR
float cabinet_out[kAudioBlockSize];
constexpr uint8_t kIrOff = 0xff;
volatile uint8_t active_ir_index = kIrOff;
#endif
char usb_log_buffers[2][192] = {};
uint8_t usb_log_active_buffer = 1;

volatile bool effect_enabled = false;
volatile bool capture_fault = false;
volatile bool reverb_ready = false;
volatile bool reverb_enabled = false;
volatile ReverbRack::Position reverb_position = ReverbRack::Position::Off;
volatile uint32_t cb_process_cycles = 0;
volatile uint32_t cb_max_cycles = 0;
uint8_t active_capture_slot = 0xffU;
CaptureTransitionController capture_transition;
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
#if HOTHOUSE_DIAGNOSTIC
volatile uint8_t diagnostic_mode = 0;
#endif

void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t size);
void UsbLog(const char* format, ...);
bool HandleReverbCommand(char* line);

void ApplyCaptureTransition(AudioHandle::OutputBuffer out, size_t size)
{
  capture_transition.ApplyOutput(out[0], out[1], size);
}

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

uint8_t CaptureSlotFromPanel()
{
  switch(hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3))
  {
    case Hothouse::TOGGLESWITCH_UP: return 0;
    case Hothouse::TOGGLESWITCH_MIDDLE: return 1;
    case Hothouse::TOGGLESWITCH_DOWN: return 2;
    default: return 0xffU;
  }
}

bool SelectCaptureSlot(uint8_t slot, bool audio_running)
{
  if(slot >= CaptureStore<DaisyFlashBackend>::SlotCount)
    return false;

  CaptureInfo info;
  const CaptureSlotState state = capture_store.Inspect(slot, &info);
  const bool payload_ready = state == CaptureSlotState::Valid
      && model_engine::AcceptsPayload(info.format, info.size)
      && capture_store.ReadPayload(slot, info, model_engine::PayloadBuffer(),
                                   model_engine::PayloadCapacity());
  if(audio_running)
    hw.StopAudio();
  model_engine::Clear();
  model_engine::LoadMetrics metrics;
  if(payload_ready)
    metrics = model_engine::Load(info.format, info.size);
  if(payload_ready && !metrics.loaded && model_engine::LastError()[0] != '\0')
    UsbLog("capture slot %c rejected: %s", 'A' + slot,
           model_engine::LastError());
  const bool loaded = metrics.loaded;
  active_capture_slot = slot;
  capture_fault = state == CaptureSlotState::Invalid
      || (state == CaptureSlotState::Valid && !loaded);
  cb_max_cycles = 0;
  if(audio_running)
    hw.StartAudio(AudioCallback);
  UsbLog("capture slot %c: %s", 'A' + slot,
         loaded ? info.name
                : (capture_fault ? "invalid" : "empty (NAM bypass)"));
  if(loaded)
    UsbLog("capture slot %c load: construct=%lu cycles (%.2f ms), "
           "prepare=%lu cycles (%.2f ms)",
           'A' + slot,
           static_cast<unsigned long>(metrics.construct_cycles),
           metrics.construct_cycles / 480000.0f,
           static_cast<unsigned long>(metrics.prepare_cycles),
           metrics.prepare_cycles / 480000.0f);
  return loaded;
}

void QueueCaptureSlots()
{
  const char* states[3] = {};
  for(uint8_t slot = 0; slot < 3; ++slot)
  {
    CaptureInfo info;
    const CaptureSlotState state = capture_store.Inspect(slot, &info);
    const bool compatible = state == CaptureSlotState::Valid
        && model_engine::AcceptsPayload(info.format, info.size);
    states[slot] = compatible ? "installed"
        : (state == CaptureSlotState::Empty ? "empty" : "invalid");
  }
  QueueCaptureReply("HNAM OK SLOTS active=%c A=%s B=%s C=%s",
                    active_capture_slot < 3 ? 'A' + active_capture_slot : '-',
                    states[0], states[1], states[2]);
}

void QueueCaptureSlot(uint8_t slot)
{
  CaptureInfo info;
  const CaptureSlotState state = capture_store.Inspect(slot, &info);
  const bool compatible = state == CaptureSlotState::Valid
      && model_engine::AcceptsPayload(info.format, info.size);
  if(!compatible)
  {
    QueueCaptureReply("HNAM OK SLOT %c %s unknown 0 00000000 -", 'A' + slot,
                      state == CaptureSlotState::Empty ? "empty" : "invalid");
    return;
  }
  char name_hex[sizeof(info.name) * 2U] = {};
  const size_t name_length = std::strlen(info.name);
  if(!hothouse_nam::EncodeHex(reinterpret_cast<const uint8_t*>(info.name),
                              name_length, name_hex, sizeof(name_hex)))
  {
    QueueCaptureReply("HNAM OK SLOT %c invalid unknown 0 00000000 -",
                      'A' + slot);
    return;
  }
  QueueCaptureReply("HNAM OK SLOT %c installed %s %lu %08lx %s", 'A' + slot,
                    hothouse_nam::CaptureFormatName(info.format),
                    static_cast<unsigned long>(info.size),
                    static_cast<unsigned long>(info.crc32), name_hex);
}

void HandleCaptureCommand(char* line)
{
  if(HandleReverbCommand(line))
    return;
  Command command;
  if(!hothouse_nam::ParseCommand(line, command))
  {
    QueueCaptureReply("HNAM ERR bad_command");
    return;
  }
  if(command.type == CommandType::Info)
  {
    CaptureInfo info;
    const CaptureSlotState state = active_capture_slot < 3
        ? capture_store.Inspect(active_capture_slot, &info)
        : CaptureSlotState::Empty;
    if(state == CaptureSlotState::Valid
       && model_engine::AcceptsPayload(info.format, info.size))
      QueueCaptureReply("HNAM OK INFO %s installed %lu %08lx",
                        model_engine::BackendId(),
                        static_cast<unsigned long>(info.size),
                        static_cast<unsigned long>(info.crc32));
    else
      QueueCaptureReply("HNAM OK INFO %s %s 0 00000000",
                        model_engine::BackendId(),
                        state == CaptureSlotState::Empty ? "empty" : "invalid");
  }
  else if(command.type == CommandType::Slots)
    QueueCaptureSlots();
  else if(command.type == CommandType::Slot)
    QueueCaptureSlot(command.slot);
  else if(command.type == CommandType::Begin)
  {
    if(!model_engine::AcceptsPayload(command.format, command.size))
      QueueCaptureReply("HNAM ERR incompatible_capture");
    else if(!capture_store.Begin(command.slot, command.name, command.format, command.size,
                                 command.crc32))
      QueueCaptureReply("HNAM ERR begin_failed");
    else
    {
      if(command.slot == active_capture_slot)
        SelectCaptureSlot(active_capture_slot, true);
      QueueCaptureReply("HNAM OK BEGIN 0");
    }
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
    const uint8_t slot = capture_store.UploadSlot();
    if(!capture_store.Commit(info))
      QueueCaptureReply("HNAM ERR commit_%s",
                        hothouse_nam::CaptureCommitStatusName(
                            capture_store.LastCommitStatus()));
    else if(slot == active_capture_slot && !SelectCaptureSlot(slot, true))
    {
      if(capture_fault)
        QueueCaptureReply("HNAM ERR activate_failed");
      else
        QueueCaptureReply("HNAM OK COMMIT %08lx",
                          static_cast<unsigned long>(info.crc32));
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
  else if(command.type == CommandType::Delete)
  {
    if(!capture_store.EraseSlot(command.slot))
      QueueCaptureReply("HNAM ERR delete_failed");
    else
    {
      if(command.slot == active_capture_slot)
        SelectCaptureSlot(command.slot, true);
      QueueCaptureReply("HNAM OK DELETE %c", 'A' + command.slot);
    }
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

#if HOTHOUSE_USE_IR
uint8_t IrIndexFromPanel()
{
  const Hothouse::ToggleswitchPosition position
      = ActiveToggle(Hothouse::TOGGLESWITCH_2);
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
  UsbLog("cabinet IR: %s", requested == kIrOff
      ? "off" : embedded_ir_bank::kEntries[requested].name);
}
#endif

void UpdateReverbControls()
{
  const Hothouse::ToggleswitchPosition position
      = ActiveToggle(Hothouse::TOGGLESWITCH_1);

  switch(position)
  {
    case Hothouse::TOGGLESWITCH_UP:
      reverb_enabled = reverb_ready;
      reverb_position = ReverbRack::Position::Up;
      break;
    case Hothouse::TOGGLESWITCH_DOWN:
      reverb_enabled = reverb_ready;
      reverb_position = ReverbRack::Position::Down;
      break;
    case Hothouse::TOGGLESWITCH_MIDDLE:
    case Hothouse::TOGGLESWITCH_UNKNOWN:
    default:
      reverb_enabled = false;
      reverb_position = ReverbRack::Position::Off;
      break;
  }

  reverb_mix_smoothed.target
      = reverb_enabled ? ActiveKnob(Hothouse::KNOB_2) : 0.0f;
}

bool HandleReverbCommand(char* line)
{
  if(std::strncmp(line, "HNAM REVERB ", 12) != 0)
    return false;
  char* save = nullptr;
  char* prefix = ::strtok_r(line, " ", &save);
  char* noun = ::strtok_r(nullptr, " ", &save);
  char* verb = ::strtok_r(nullptr, " ", &save);
  if(prefix == nullptr || noun == nullptr || verb == nullptr
     || std::strcmp(prefix, "HNAM") != 0 || std::strcmp(noun, "REVERB") != 0)
    return false;
  if(std::strcmp(verb, "LIST") == 0 && ::strtok_r(nullptr, " ", &save) == nullptr)
  {
    QueueCaptureReply("HNAM OK REVERB LIST reverbsc dattorro fdn16 hybrid");
    return true;
  }
  if(std::strcmp(verb, "INFO") == 0 && ::strtok_r(nullptr, " ", &save) == nullptr)
  {
    const ReverbSlotConfig& config = reverb_rack->Config();
    QueueCaptureReply("HNAM OK REVERB INFO up=%s down=%s",
                      hothouse_nam::ReverbIdName(config.up),
                      hothouse_nam::ReverbIdName(config.down));
    return true;
  }
  if(std::strcmp(verb, "MAP") == 0)
  {
    char* position = ::strtok_r(nullptr, " ", &save);
    char* engine = ::strtok_r(nullptr, " ", &save);
    if(position == nullptr || engine == nullptr || ::strtok_r(nullptr, " ", &save) != nullptr)
    {
      QueueCaptureReply("HNAM ERR reverb_map_syntax");
      return true;
    }
    const ReverbSlotConfig previous = reverb_rack->Config();
    ReverbSlotConfig config = previous;
    const ReverbId id = hothouse_nam::ParseReverbId(engine);
    if(id == ReverbId::Invalid || (std::strcmp(position, "UP") != 0
                                   && std::strcmp(position, "DOWN") != 0))
    {
      QueueCaptureReply("HNAM ERR unknown_reverb");
      return true;
    }
    if(std::strcmp(position, "UP") == 0)
      config.up = id;
    else
      config.down = id;
    if(!hothouse_nam::ValidReverbSlotConfig(config))
    {
      QueueCaptureReply("HNAM ERR duplicate_reverb");
      return true;
    }
    hw.StopAudio();
    const bool configured = reverb_rack->Configure(config);
    const bool saved = configured && reverb_config_store.Save(config);
    reverb_ready = configured && saved;
    if(!saved)
      reverb_ready = reverb_rack->Configure(previous);
    hw.StartAudio(AudioCallback);
    if(!saved)
      QueueCaptureReply("HNAM ERR reverb_save_failed");
    else
      QueueCaptureReply("HNAM OK REVERB MAP up=%s down=%s",
                        hothouse_nam::ReverbIdName(config.up),
                        hothouse_nam::ReverbIdName(config.down));
    return true;
  }
  QueueCaptureReply("HNAM ERR reverb_command");
  return true;
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
  const bool fault = capture_fault || cb_max_cycles >= kCycleBudget;
  bool status_on = fault ? ((now_ms / 125U) & 1U) != 0U : preset_engaged;
  if(!fault && static_cast<int32_t>(preset_saved_indication_until - now_ms) > 0)
    status_on = ((now_ms / 100U) & 1U) != 0U;
#if HOTHOUSE_DIAGNOSTIC
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

  if(effect_enabled && size == kAudioBlockSize)
  {
    const uint32_t cyc0 = DWT->CYCCNT;
    const bool model_loaded = model_engine::IsLoaded();
    for(size_t i = 0; i < size; ++i)
    {
      const float input = in[0][i];
      const float gained_input = input * input_gain_smoothed.Tick();
      if(model_loaded)
      {
        const float magnitude = std::fabs(input);
        const float envelope_coefficient
            = magnitude > input_envelope ? 0.05f : 0.0002f;
        fonepole(input_envelope, magnitude, envelope_coefficient);
        if(input_gate_open)
        {
          if(input_envelope < kGateCloseThreshold)
            input_gate_open = false;
        }
        else if(input_envelope > kGateOpenThreshold)
          input_gate_open = true;
        const float gate_target = input_gate_open ? 1.0f : 0.0f;
        fonepole(input_gate_gain, gate_target,
                 input_gate_open ? 0.05f : 0.001f);
      }
#if HOTHOUSE_DIAGNOSTIC
      mono_in[i] = diagnostic_mode == 0
          ? gained_input
              * (model_loaded ? input_gate_gain : 1.0f)
          : 0.0f;
#else
      mono_in[i] = gained_input
          * (model_loaded ? input_gate_gain : 1.0f)
          ;
#endif
    }

    if(model_loaded)
      model_engine::ProcessBlock48(mono_in, mono_out);
    else
      for(size_t i = 0; i < size; ++i)
        mono_out[i] = mono_in[i];

#if HOTHOUSE_USE_IR
    const uint8_t ir_index = active_ir_index;
    if(ir_index == kIrOff)
      for(size_t i = 0; i < size; ++i)
        cabinet_out[i] = mono_out[i];
    else
      cabinet_irs[ir_index].ProcessBlock(mono_out, cabinet_out, size);
#endif

    for(size_t i = 0; i < size; ++i)
    {
#if HOTHOUSE_USE_IR
      const float dry = ProcessToneStack(cabinet_out[i]);
#else
      const float dry = ProcessToneStack(mono_out[i]);
#endif
      float wet_left = 0.0f;
      float wet_right = 0.0f;
      if(reverb_ready)
      {
        const float send = reverb_enabled ? dry : 0.0f;
        reverb_rack->Process(reverb_position, send, &wet_left, &wet_right);
      }

      const float mix = reverb_mix_smoothed.Tick();
      const float dry_mix = 1.0f - mix;
      const float output = output_smoothed.Tick();
      float processed_left = (dry * dry_mix + wet_left * mix) * output;
      float processed_right = (dry * dry_mix + wet_right * mix) * output;
#if HOTHOUSE_DIAGNOSTIC
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
        reverb_rack->Process(ReverbRack::Position::Off, 0.0f,
                             &discarded_left, &discarded_right);
      }
      const float dry = in[0][i] * output_smoothed.Tick();
      out[0][i] = dry;
      out[1][i] = dry;
    }
  }

  if(size != 0)
    ApplyCaptureTransition(out, size);
}

void BenchmarkModel()
{
  if(!model_engine::IsLoaded())
    return;

  for(float& sample : mono_in)
    sample = 0.0f;

  DWT->CYCCNT = 0;
  model_engine::ProcessBlock48(mono_in, mono_out);
  UsbLog("%s benchmark cycles=%lu", model_engine::ActiveBackendId(),
         static_cast<unsigned long>(DWT->CYCCNT));
}

} // namespace

int main()
{
  hw.Init(true);
  reverb_rack = new(reverb_rack_storage) ReverbRack();
  hw.SetAudioBlockSize(kAudioBlockSize);
  hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

  uint32_t fpscr = __get_FPSCR();
  fpscr |= (1U << 24) | (1U << 25);
  __set_FPSCR(fpscr);
  volatile uint32_t* FPDSCR = reinterpret_cast<volatile uint32_t*>(0xE000EF3C);
  *FPDSCR |= (1U << 24) | (1U << 25);

  hw.seed.usb_handle.Init(daisy::UsbHandle::FS_INTERNAL);
  hw.seed.usb_handle.SetReceiveCallback(CaptureUsbReceive,
                                        daisy::UsbHandle::FS_INTERNAL);
  UsbLog("HothouseNAM %s boot", model_engine::BackendId());

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
  model_engine::Initialize(hw.AudioSampleRate(), kAudioBlockSize);

  ReverbSlotConfig reverb_config = {};
  reverb_config_store.Load(reverb_config);
  reverb_ready = reverb_rack->Init(hw.AudioSampleRate(), reverb_config);
#if HOTHOUSE_USE_IR
  for(size_t i = 0; i < embedded_ir_bank::kCount; ++i)
  {
    const embedded_ir_bank::Entry& entry = embedded_ir_bank::kEntries[i];
    cabinet_irs[i].Init(entry.data, entry.length, true);
  }
  active_ir_index = IrIndexFromPanel();
  UsbLog("cabinet IR: %s", active_ir_index == kIrOff
      ? "off" : embedded_ir_bank::kEntries[active_ir_index].name);
#endif
  UpdateReverbControls();
  reverb_mix_smoothed.current = reverb_mix_smoothed.target;

  const uint8_t startup_slot = CaptureSlotFromPanel();
  if(startup_slot < 3)
    SelectCaptureSlot(startup_slot, false);
  capture_transition.Initialize(startup_slot);
  // Empty capture slots bypass only NAM; the rest of the processing chain is
  // still a useful standalone IR, EQ, and reverb processor.
  effect_enabled = true;
  hw.StartAudio(AudioCallback);
  BenchmarkModel();

  uint32_t last_log_ms = System::GetNow();
  while(true)
  {
    ProcessCaptureUsb();
    hw.ProcessAllControls();
    const uint32_t now_ms = System::GetNow();

    UpdatePresetPanelOverrides();

    input_gain_smoothed.target = InputGainFromKnob(ActiveKnob(Hothouse::KNOB_1));
    output_smoothed.target = ActiveKnob(Hothouse::KNOB_3);
    bass_gain_smoothed.target = EqGainFromKnob(ActiveKnob(Hothouse::KNOB_4));
    mid_gain_smoothed.target = EqGainFromKnob(ActiveKnob(Hothouse::KNOB_5));
    treble_gain_smoothed.target = EqGainFromKnob(ActiveKnob(Hothouse::KNOB_6));
    UpdateReverbControls();
    const uint8_t requested_capture_slot = CaptureSlotFromPanel();
    capture_transition.ObserveSlot(requested_capture_slot, now_ms);
    uint8_t settled_slot = CaptureTransitionController::NoSlot;
    if(capture_transition.TakeSettledSlot(now_ms, settled_slot))
    {
      if(settled_slot != active_capture_slot)
        SelectCaptureSlot(settled_slot, true);

      input_envelope = 0.0f;
      input_gate_gain = 0.0f;
      input_gate_open = false;
      bass_lowpass_state = 0.0f;
      treble_lowpass_state = 0.0f;
      capture_transition.BeginFadeIn();
    }
#if HOTHOUSE_USE_IR
    UpdateIrSelection();
#endif

    if(hw.switches[Hothouse::FOOTSWITCH_1].RisingEdge())
      effect_enabled = !effect_enabled;

    ProcessPresetFootswitch();

#if HOTHOUSE_DIAGNOSTIC
    diagnostic_mode = 0;
#endif

    UpdateLedState();
    hw.CheckResetToBootloader();

    if(now_ms - last_log_ms >= 1000U)
    {
      UsbLog("%s cycles=%lu max=%lu model=%s",
             model_engine::ActiveBackendId(),
             static_cast<unsigned long>(cb_process_cycles),
             static_cast<unsigned long>(cb_max_cycles),
             model_engine::IsLoaded() ? "ready" : "bypassed");
      last_log_ms = now_ms;
    }

    System::Delay(5);
  }
}
