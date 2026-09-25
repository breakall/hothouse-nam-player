#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hothouse_nam
{

enum class CaptureFormat : uint8_t
{
  Unknown = 0,
  A2WeightsF32 = 1,
};

inline const char* CaptureFormatName(CaptureFormat format)
{
  switch(format)
  {
    case CaptureFormat::A2WeightsF32: return "a2_weights_f32";
    default: return "unknown";
  }
}

inline CaptureFormat ParseCaptureFormat(const char* text)
{
  if(text != nullptr && std::strcmp(text, "a2_weights_f32") == 0)
    return CaptureFormat::A2WeightsF32;
  return CaptureFormat::Unknown;
}

inline uint32_t Crc32Update(uint32_t crc, const uint8_t* bytes, size_t length)
{
  for(size_t i = 0; i < length; ++i)
  {
    crc ^= bytes[i];
    for(size_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return crc;
}

template <size_t Capacity>
class ByteRing
{
 public:
  void PushFromInterrupt(const uint8_t* bytes, size_t length)
  {
    if(bytes == nullptr)
      return;
    for(size_t i = 0; i < length; ++i)
    {
      const size_t next = (head_ + 1U) % Capacity;
      if(next == tail_)
      {
        ++dropped_;
        continue;
      }
      data_[head_] = static_cast<char>(bytes[i]);
      head_ = next;
    }
  }

  bool Pop(char& value)
  {
    if(tail_ == head_)
      return false;
    value = data_[tail_];
    tail_ = (tail_ + 1U) % Capacity;
    return true;
  }

  uint32_t Dropped() const { return dropped_; }

 private:
  char data_[Capacity] = {};
  volatile size_t head_ = 0;
  volatile size_t tail_ = 0;
  volatile uint32_t dropped_ = 0;
};

#pragma pack(push, 1)
struct CaptureHeader
{
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t payload_size;
  uint32_t payload_crc32;
  uint32_t header_crc32;
  uint8_t format;
  uint8_t reserved[3];
  char name[64];
};
#pragma pack(pop)

struct CaptureInfo
{
  uint32_t size = 0;
  uint32_t crc32 = 0;
  CaptureFormat format = CaptureFormat::Unknown;
  char name[64] = {};
};

enum class CaptureSlotState : uint8_t
{
  Empty,
  Valid,
  Invalid,
};

enum class CaptureCommitStatus : uint8_t
{
  Ok,
  NotUploading,
  Incomplete,
  CrcMismatch,
  PayloadVerifyFailed,
  HeaderWriteFailed,
  HeaderVerifyFailed,
};

inline const char* CaptureCommitStatusName(CaptureCommitStatus status)
{
  switch(status)
  {
    case CaptureCommitStatus::NotUploading: return "not_uploading";
    case CaptureCommitStatus::Incomplete: return "incomplete";
    case CaptureCommitStatus::CrcMismatch: return "crc_mismatch";
    case CaptureCommitStatus::PayloadVerifyFailed: return "payload_verify_failed";
    case CaptureCommitStatus::HeaderWriteFailed: return "header_write_failed";
    case CaptureCommitStatus::HeaderVerifyFailed: return "header_verify_failed";
    default: return "ok";
  }
}

// The final 256 KiB before 0x007f0000 is reserved for three capture slots.
// 0x007ff000 remains untouched for the existing preset store.
template <typename Backend,
          uint32_t RegionOffset = 0x007b0000U,
          uint32_t RegionSize = 0x00040000U,
          uint32_t CaptureSlotSize = 0x00015000U>
class CaptureStore
{
 public:
  static constexpr uint8_t SlotCount = 3;
  static constexpr uint32_t HeaderAreaSize = 256U;
  // Three independently erasable 84 KiB slots consume 252 KiB, leaving one
  // 4 KiB sector unused before the reverb configuration sector.
  static constexpr uint32_t SlotSize = CaptureSlotSize;
  static constexpr uint32_t MaximumCaptureSize = SlotSize - HeaderAreaSize;
  static_assert(SlotSize > HeaderAreaSize,
                "Capture slot must fit its header and a payload");
  static_assert(SlotSize * SlotCount <= RegionSize,
                "Capture slots exceed reserved QSPI region");

  explicit CaptureStore(Backend& backend) : backend_(backend) {}

  CaptureSlotState Inspect(uint8_t slot, CaptureInfo* info = nullptr,
                           bool verify_payload = true) const
  {
    if(slot >= SlotCount)
      return CaptureSlotState::Invalid;
    CaptureHeader header = {};
    if(!backend_.Read(SlotOffset(slot), reinterpret_cast<uint8_t*>(&header),
                      sizeof(header)))
      return CaptureSlotState::Invalid;
    if(header.magic == 0xffffffffU)
      return CaptureSlotState::Empty;
    if(!ValidHeader(header)
       || (verify_payload && !PayloadCrcMatches(slot, header)))
      return CaptureSlotState::Invalid;
    if(info != nullptr)
    {
      info->size = header.payload_size;
      info->crc32 = header.payload_crc32;
      info->format = static_cast<CaptureFormat>(header.format);
      std::memcpy(info->name, header.name, sizeof(info->name));
    }
    return CaptureSlotState::Valid;
  }

  bool ReadInfo(uint8_t slot, CaptureInfo& info,
                bool verify_payload = true) const
  {
    return Inspect(slot, &info, verify_payload) == CaptureSlotState::Valid;
  }

  bool ReadInfo(CaptureInfo& info, bool verify_payload = true) const
  {
    return ReadInfo(0, info, verify_payload);
  }

  bool Begin(uint8_t slot, const char* name, CaptureFormat format, uint32_t size,
             uint32_t crc32)
  {
    if(uploading_ || slot >= SlotCount || format == CaptureFormat::Unknown || size == 0
       || size > MaximumCaptureSize || !ValidName(name))
      return false;
    if(!backend_.Erase(SlotOffset(slot), SlotSize))
      return false;

    pending_ = {};
    pending_.magic = Magic;
    pending_.version = Version;
    pending_.header_size = sizeof(CaptureHeader);
    pending_.payload_size = size;
    pending_.payload_crc32 = crc32;
    pending_.format = static_cast<uint8_t>(format);
    CopyName(pending_.name, name);
    offset_ = 0;
    crc_ = 0xffffffffU;
    pending_slot_ = slot;
    uploading_ = true;
    return true;
  }

  bool Begin(const char* name, CaptureFormat format, uint32_t size,
             uint32_t crc32)
  {
    return Begin(0, name, format, size, crc32);
  }

  bool WriteChunk(uint32_t offset, const uint8_t* bytes, size_t length)
  {
    if(!uploading_ || bytes == nullptr || length == 0 || offset != offset_
       || length > pending_.payload_size - offset_)
      return false;
    if(!backend_.Write(SlotOffset(pending_slot_) + HeaderAreaSize + offset,
                       bytes, length))
      return false;
    crc_ = Crc32Update(crc_, bytes, length);
    offset_ += static_cast<uint32_t>(length);
    return true;
  }

  bool Commit(CaptureInfo& info)
  {
    if(!uploading_)
    {
      last_commit_status_ = CaptureCommitStatus::NotUploading;
      return false;
    }
    if(offset_ != pending_.payload_size)
    {
      last_commit_status_ = CaptureCommitStatus::Incomplete;
      return false;
    }
    if((crc_ ^ 0xffffffffU) != pending_.payload_crc32)
    {
      last_commit_status_ = CaptureCommitStatus::CrcMismatch;
      return false;
    }
    if(!PayloadCrcMatches(pending_slot_, pending_))
    {
      last_commit_status_ = CaptureCommitStatus::PayloadVerifyFailed;
      return false;
    }
    pending_.header_crc32 = HeaderCrc(pending_);
    if(!backend_.Write(SlotOffset(pending_slot_),
                       reinterpret_cast<const uint8_t*>(&pending_),
                       sizeof(pending_)))
    {
      last_commit_status_ = CaptureCommitStatus::HeaderWriteFailed;
      return false;
    }
    uploading_ = false;
    if(!ReadInfo(pending_slot_, info, false))
    {
      last_commit_status_ = CaptureCommitStatus::HeaderVerifyFailed;
      return false;
    }
    last_commit_status_ = CaptureCommitStatus::Ok;
    return true;
  }

  void Cancel() { uploading_ = false; }

  bool ReadPayload(uint8_t slot, const CaptureInfo& info, uint8_t* output,
                   size_t capacity) const
  {
    return slot < SlotCount && output != nullptr && info.size <= capacity
        && backend_.Read(SlotOffset(slot) + HeaderAreaSize, output, info.size);
  }

  bool ReadPayload(const CaptureInfo& info, uint8_t* output,
                   size_t capacity) const
  {
    return ReadPayload(0, info, output, capacity);
  }

  bool EraseSlot(uint8_t slot)
  {
    return !uploading_ && slot < SlotCount
        && backend_.Erase(SlotOffset(slot), SlotSize);
  }

  uint32_t UploadOffset() const { return offset_; }
  uint8_t UploadSlot() const { return pending_slot_; }
  CaptureCommitStatus LastCommitStatus() const { return last_commit_status_; }

 private:
  static constexpr uint32_t Magic = 0x4d414e48U; // "HNAM"
  static constexpr uint16_t Version = 2;

  static bool ValidName(const char* name)
  {
    if(name == nullptr || name[0] == '\0')
      return false;
    for(size_t i = 0; i < sizeof(CaptureHeader::name); ++i)
    {
      const uint8_t c = static_cast<uint8_t>(name[i]);
      if(c == 0)
        return true;
      if(c < 0x20U || c > 0x7eU)
        return false;
    }
    return false;
  }

  static void CopyName(char* destination, const char* source)
  {
    size_t length = 0;
    while(length + 1U < sizeof(CaptureHeader::name) && source[length] != '\0')
      ++length;
    std::memcpy(destination, source, length);
    destination[length] = '\0';
  }

  static uint32_t HeaderCrc(CaptureHeader header)
  {
    header.header_crc32 = 0;
    return Crc32Update(0xffffffffU,
                       reinterpret_cast<const uint8_t*>(&header),
                       sizeof(header)) ^ 0xffffffffU;
  }

  static bool ValidHeader(const CaptureHeader& header)
  {
    return header.magic == Magic && header.version == Version
        && header.header_size == sizeof(CaptureHeader)
        && header.payload_size > 0
        && header.payload_size <= MaximumCaptureSize
        && header.format == static_cast<uint8_t>(CaptureFormat::A2WeightsF32)
        && header.name[0] != '\0'
        && header.name[sizeof(header.name) - 1U] == '\0'
        && HeaderCrc(header) == header.header_crc32;
  }

  static constexpr uint32_t SlotOffset(uint8_t slot)
  {
    return RegionOffset + static_cast<uint32_t>(slot) * SlotSize;
  }

  bool PayloadCrcMatches(uint8_t slot, const CaptureHeader& header) const
  {
    const uint8_t* bytes = backend_.Data(SlotOffset(slot) + HeaderAreaSize);
    if(bytes == nullptr)
      return false;
    return (Crc32Update(0xffffffffU, bytes, header.payload_size)
            ^ 0xffffffffU) == header.payload_crc32;
  }

  Backend& backend_;
  CaptureHeader pending_ = {};
  uint32_t offset_ = 0;
  uint32_t crc_ = 0xffffffffU;
  uint8_t pending_slot_ = 0;
  bool uploading_ = false;
  CaptureCommitStatus last_commit_status_ = CaptureCommitStatus::Ok;
};

enum class CommandType
{
  Invalid,
  Info,
  Slots,
  Slot,
  Begin,
  Data,
  Commit,
  Cancel,
  Delete,
};

struct Command
{
  CommandType type = CommandType::Invalid;
  uint8_t slot = 0;
  CaptureFormat format = CaptureFormat::Unknown;
  uint32_t size = 0;
  uint32_t crc32 = 0;
  uint32_t offset = 0;
  char name[64] = {};
  uint8_t data[128] = {};
  size_t data_length = 0;
};

inline int ParseCaptureSlot(const char* text)
{
  if(text == nullptr || text[1] != '\0')
    return -1;
  if(text[0] >= 'A' && text[0] <= 'C')
    return text[0] - 'A';
  if(text[0] >= 'a' && text[0] <= 'c')
    return text[0] - 'a';
  return -1;
}

inline int HexNibble(char c)
{
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline bool EncodeHex(const uint8_t* bytes, size_t length, char* output,
                      size_t capacity)
{
  static constexpr char digits[] = "0123456789abcdef";
  if(bytes == nullptr || output == nullptr || capacity < length * 2U + 1U)
    return false;
  for(size_t i = 0; i < length; ++i)
  {
    output[i * 2U] = digits[bytes[i] >> 4U];
    output[i * 2U + 1U] = digits[bytes[i] & 0x0fU];
  }
  output[length * 2U] = '\0';
  return true;
}

inline bool DecodeHex(const char* text, uint8_t* output, size_t capacity,
                      size_t& length)
{
  length = 0;
  if(text == nullptr)
    return false;
  const size_t characters = std::strlen(text);
  if((characters & 1U) != 0 || characters / 2U > capacity)
    return false;
  for(size_t i = 0; i < characters; i += 2U)
  {
    const int high = HexNibble(text[i]);
    const int low = HexNibble(text[i + 1U]);
    if(high < 0 || low < 0)
      return false;
    output[length++] = static_cast<uint8_t>((high << 4) | low);
  }
  return length > 0;
}

inline bool DecodeName(const char* text, char* output, size_t capacity)
{
  uint8_t bytes[63] = {};
  size_t length = 0;
  if(!DecodeHex(text, bytes, sizeof(bytes), length) || length >= capacity)
    return false;
  for(size_t i = 0; i < length; ++i)
    if(bytes[i] < 0x20U || bytes[i] > 0x7eU)
      return false;
  std::memcpy(output, bytes, length);
  output[length] = '\0';
  return true;
}

inline bool ParseUnsigned(const char* text, uint32_t& value, uint32_t base)
{
  if(text == nullptr || text[0] == '\0' || (base != 10U && base != 16U))
    return false;
  uint32_t result = 0;
  for(size_t i = 0; text[i] != '\0'; ++i)
  {
    const int digit = HexNibble(text[i]);
    if(digit < 0 || static_cast<uint32_t>(digit) >= base
       || result > (0xffffffffU - static_cast<uint32_t>(digit)) / base)
      return false;
    result = result * base + static_cast<uint32_t>(digit);
  }
  value = result;
  return true;
}

inline bool ParseCommand(char* line, Command& command)
{
  command = {};
  char* save = nullptr;
  char* token = ::strtok_r(line, " ", &save);
  if(token == nullptr || std::strcmp(token, "HNAM") != 0)
    return false;
  token = ::strtok_r(nullptr, " ", &save);
  if(token == nullptr)
    return false;
  if(std::strcmp(token, "INFO") == 0)
  {
    command.type = CommandType::Info;
    return ::strtok_r(nullptr, " ", &save) == nullptr;
  }
  if(std::strcmp(token, "SLOTS") == 0)
  {
    command.type = CommandType::Slots;
    return ::strtok_r(nullptr, " ", &save) == nullptr;
  }
  if(std::strcmp(token, "SLOT") == 0)
  {
    char* slot = ::strtok_r(nullptr, " ", &save);
    const int parsed_slot = ParseCaptureSlot(slot);
    if(parsed_slot < 0 || ::strtok_r(nullptr, " ", &save) != nullptr)
      return false;
    command.slot = static_cast<uint8_t>(parsed_slot);
    command.type = CommandType::Slot;
    return true;
  }
  if(std::strcmp(token, "COMMIT") == 0)
  {
    command.type = CommandType::Commit;
    return ::strtok_r(nullptr, " ", &save) == nullptr;
  }
  if(std::strcmp(token, "CANCEL") == 0)
  {
    command.type = CommandType::Cancel;
    return ::strtok_r(nullptr, " ", &save) == nullptr;
  }
  if(std::strcmp(token, "BEGIN") == 0)
  {
    char* slot = ::strtok_r(nullptr, " ", &save);
    char* format = ::strtok_r(nullptr, " ", &save);
    char* size = ::strtok_r(nullptr, " ", &save);
    char* crc = ::strtok_r(nullptr, " ", &save);
    char* name = ::strtok_r(nullptr, " ", &save);
    const int parsed_slot = ParseCaptureSlot(slot);
    if(parsed_slot < 0 || format == nullptr || size == nullptr || crc == nullptr || name == nullptr
       || ::strtok_r(nullptr, " ", &save) != nullptr
       || !ParseUnsigned(size, command.size, 10U)
       || !ParseUnsigned(crc, command.crc32, 16U)
       || !DecodeName(name, command.name, sizeof(command.name)))
      return false;
    command.slot = static_cast<uint8_t>(parsed_slot);
    command.format = ParseCaptureFormat(format);
    command.type = CommandType::Begin;
    return command.format != CaptureFormat::Unknown;
  }
  if(std::strcmp(token, "DELETE") == 0)
  {
    char* slot = ::strtok_r(nullptr, " ", &save);
    const int parsed_slot = ParseCaptureSlot(slot);
    if(parsed_slot < 0 || ::strtok_r(nullptr, " ", &save) != nullptr)
      return false;
    command.slot = static_cast<uint8_t>(parsed_slot);
    command.type = CommandType::Delete;
    return true;
  }
  if(std::strcmp(token, "DATA") == 0)
  {
    char* offset = ::strtok_r(nullptr, " ", &save);
    char* data = ::strtok_r(nullptr, " ", &save);
    if(offset == nullptr || data == nullptr
       || ::strtok_r(nullptr, " ", &save) != nullptr
       || !ParseUnsigned(offset, command.offset, 10U)
       || !DecodeHex(data, command.data, sizeof(command.data),
                     command.data_length))
      return false;
    command.type = CommandType::Data;
    return true;
  }
  return false;
}

} // namespace hothouse_nam
