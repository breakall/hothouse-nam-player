#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../capture_loader.h"

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

  char begin_line[] = "HNAM BEGIN a2_weights_f32 4 b63cfbcd 54657374";
  Command command;
  assert(ParseCommand(begin_line, command));
  assert(command.type == CommandType::Begin);
  assert(command.format == CaptureFormat::A2WeightsF32);
  assert(command.size == 4);
  assert(command.crc32 == 0xb63cfbcdU);
  assert(std::strcmp(command.name, "Test") == 0);

  char data_line[] = "HNAM DATA 0 01020304";
  assert(ParseCommand(data_line, command));
  assert(command.type == CommandType::Data);
  assert(command.data_length == 4);

  MemoryFlash flash(1024);
  CaptureStore<MemoryFlash, 0, 1024> store(flash);
  const uint8_t payload[] = {1, 2, 3, 4};
  const uint32_t crc = Crc32Update(0xffffffffU, payload, sizeof(payload))
      ^ 0xffffffffU;
  assert(store.Begin("Test", CaptureFormat::A2WeightsF32,
                     sizeof(payload), crc));
  assert(store.WriteChunk(0, payload, sizeof(payload)));
  CaptureInfo info;
  assert(store.Commit(info));
  assert(store.ReadInfo(info));
  assert(info.size == sizeof(payload));
  uint8_t copy[4] = {};
  assert(store.ReadPayload(info, copy, sizeof(copy)));
  assert(std::memcmp(payload, copy, sizeof(payload)) == 0);

  flash.bytes[CaptureStore<MemoryFlash, 0, 1024>::HeaderAreaSize] ^= 1U;
  assert(!store.ReadInfo(info));
  return 0;
}
