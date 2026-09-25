#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../capture_loader.h"
#include "../reverb_config.h"

struct MemoryFlash
{
  explicit MemoryFlash(size_t size) : bytes(size, 0xffU) {}

  const uint8_t* Data(uint32_t offset) const
  {
    return offset < bytes.size() ? bytes.data() + offset : nullptr;
  }

  bool Read(uint32_t offset, uint8_t* output, size_t length) const
  {
    if(output == nullptr || offset > bytes.size() || length > bytes.size() - offset)
      return false;
    std::memcpy(output, bytes.data() + offset, length);
    return true;
  }

  bool Write(uint32_t offset, const uint8_t* input, size_t length)
  {
    if(input == nullptr || offset > bytes.size() || length > bytes.size() - offset)
      return false;
    for(size_t i = 0; i < length; ++i)
      bytes[offset + i] &= input[i];
    return true;
  }

  bool Erase(uint32_t offset, uint32_t length)
  {
    if(offset > bytes.size() || length > bytes.size() - offset)
      return false;
    std::fill(bytes.begin() + offset, bytes.begin() + offset + length, 0xffU);
    return true;
  }

  std::vector<uint8_t> bytes;
};

int main()
{
  using namespace hothouse_nam;

  assert(ParseCaptureFormat("a2_weights_f32") == CaptureFormat::A2WeightsF32);
  assert(ParseCaptureFormat("other") == CaptureFormat::Unknown);
  uint32_t parsed = 0;
  assert(ParseUnsigned("4294967295", parsed, 10) && parsed == 0xffffffffU);
  assert(!ParseUnsigned("4294967296", parsed, 10));
  assert(!ParseUnsigned("12x", parsed, 10));

  ByteRing<4> ring;
  const uint8_t ring_input[] = {'a', 'b', 'c', 'd'};
  ring.PushFromInterrupt(ring_input, sizeof(ring_input));
  assert(ring.Dropped() == 1);
  char ring_value = 0;
  assert(ring.Pop(ring_value) && ring_value == 'a');
  assert(ring.Pop(ring_value) && ring_value == 'b');
  assert(ring.Pop(ring_value) && ring_value == 'c');
  assert(!ring.Pop(ring_value));

  char begin_line[] = "HNAM BEGIN B a2_weights_f32 4 b63cfbcd 54657374";
  Command command;
  assert(ParseCommand(begin_line, command));
  assert(command.type == CommandType::Begin);
  assert(command.slot == 1);
  assert(command.format == CaptureFormat::A2WeightsF32);
  assert(command.size == 4);
  assert(command.crc32 == 0xb63cfbcdU);
  assert(std::strcmp(command.name, "Test") == 0);

  char data_line[] = "HNAM DATA 0 01020304";
  assert(ParseCommand(data_line, command));
  assert(command.type == CommandType::Data);
  assert(command.data_length == 4);
  char delete_line[] = "HNAM DELETE C";
  assert(ParseCommand(delete_line, command));
  assert(command.type == CommandType::Delete);
  assert(command.slot == 2);
  char slots_line[] = "HNAM SLOTS";
  assert(ParseCommand(slots_line, command));
  assert(command.type == CommandType::Slots);
  char slot_line[] = "HNAM SLOT a";
  assert(ParseCommand(slot_line, command));
  assert(command.type == CommandType::Slot);
  assert(command.slot == 0);
  char legacy_begin_line[] = "HNAM BEGIN a2_weights_f32 4 b63cfbcd 54657374";
  assert(!ParseCommand(legacy_begin_line, command));
  char extra_info[] = "HNAM INFO extra";
  assert(!ParseCommand(extra_info, command));
  char invalid_slot[] = "HNAM SLOT D";
  assert(!ParseCommand(invalid_slot, command));
  char invalid_hex[] = "HNAM DATA 0 012x";
  assert(!ParseCommand(invalid_hex, command));
  char oversized_data[300] = "HNAM DATA 0 ";
  std::memset(oversized_data + 12, '0', 258);
  oversized_data[270] = '\0';
  assert(!ParseCommand(oversized_data, command));

  MemoryFlash flash(4096);
  CaptureStore<MemoryFlash, 0, 1536, 512> store(flash);
  const uint8_t payload[] = {1, 2, 3, 4};
  const uint32_t crc = Crc32Update(0xffffffffU, payload, sizeof(payload))
      ^ 0xffffffffU;
  assert(!store.Begin(3, "Test", CaptureFormat::A2WeightsF32,
                      sizeof(payload), crc));
  assert(!store.Begin(0, "Test", CaptureFormat::Unknown,
                      sizeof(payload), crc));
  assert(!store.Begin(0, "Test", CaptureFormat::A2WeightsF32, 0, crc));
  assert(!store.Begin(0, "", CaptureFormat::A2WeightsF32,
                      sizeof(payload), crc));
  assert(store.Begin("Test", CaptureFormat::A2WeightsF32,
                     sizeof(payload), crc));
  assert(!store.Begin("Other", CaptureFormat::A2WeightsF32,
                      sizeof(payload), crc));
  assert(!store.EraseSlot(1));
  assert(!store.WriteChunk(1, payload, sizeof(payload)));
  assert(!store.WriteChunk(0, payload, sizeof(payload) + 1));
  assert(store.WriteChunk(0, payload, sizeof(payload)));
  CaptureInfo info;
  assert(store.Commit(info));
  assert(store.ReadInfo(info));
  assert(info.size == sizeof(payload));
  uint8_t copy[4] = {};
  assert(store.ReadPayload(info, copy, sizeof(copy)));
  assert(std::memcmp(payload, copy, sizeof(payload)) == 0);
  assert(!store.ReadPayload(3, info, copy, sizeof(copy)));
  assert(!store.ReadPayload(info, copy, sizeof(copy) - 1));

  assert(store.Begin(1, "Incomplete", CaptureFormat::A2WeightsF32,
                     sizeof(payload), crc));
  assert(store.WriteChunk(0, payload, 2));
  assert(!store.Commit(info));
  assert(store.LastCommitStatus() == CaptureCommitStatus::Incomplete);
  assert(store.WriteChunk(2, payload + 2, 2));
  assert(store.Commit(info));

  assert(store.Begin(1, "Bad CRC", CaptureFormat::A2WeightsF32,
                     sizeof(payload), crc ^ 1U));
  assert(store.WriteChunk(0, payload, sizeof(payload)));
  assert(!store.Commit(info));
  assert(store.LastCommitStatus() == CaptureCommitStatus::CrcMismatch);
  store.Cancel();
  assert(!store.Commit(info));
  assert(store.LastCommitStatus() == CaptureCommitStatus::NotUploading);

  assert(store.Begin(2, "Third", CaptureFormat::A2WeightsF32,
                     sizeof(payload), crc));
  assert(store.WriteChunk(0, payload, sizeof(payload)));
  CaptureInfo third_info;
  assert(store.Commit(third_info));
  assert(store.ReadInfo(2, third_info));
  assert(store.Inspect(1) == CaptureSlotState::Empty);
  assert(store.EraseSlot(2));
  assert(store.Inspect(2) == CaptureSlotState::Empty);
  assert(store.Inspect(0) == CaptureSlotState::Valid);

  flash.bytes[CaptureStore<MemoryFlash, 0, 1536, 512>::HeaderAreaSize] ^= 1U;
  assert(!store.ReadInfo(info));
  assert(store.Inspect(0) == CaptureSlotState::Invalid);

  ReverbConfigStore<MemoryFlash, 512, 128> reverb_store(flash);
  ReverbSlotConfig reverb_config = {};
  assert(reverb_config.up == ReverbId::HybridSpace);
  assert(reverb_config.down == ReverbId::Dattorro);
  assert(!reverb_store.Load(reverb_config));
  reverb_config.up = ReverbId::HybridSpace;
  reverb_config.down = ReverbId::Dattorro;
  assert(reverb_store.Save(reverb_config));
  reverb_config = {};
  assert(reverb_store.Load(reverb_config));
  assert(reverb_config.up == ReverbId::HybridSpace);
  assert(reverb_config.down == ReverbId::Dattorro);
  assert(ParseReverbId("fdn16") == ReverbId::Fdn16);
  assert(ParseReverbId("not-a-reverb") == ReverbId::Invalid);
  assert(!ValidReverbSlotConfig({ReverbId::Dattorro, ReverbId::Dattorro}));
  return 0;
}
