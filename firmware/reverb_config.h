#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "capture_loader.h"

namespace hothouse_nam
{

// Algorithms compiled into the firmware. A USB configuration chooses which
// two occupy the physical UP and DOWN switch positions; it never uploads DSP
// code to the pedal.
enum class ReverbId : uint8_t
{
  ReverbSc = 0,
  Dattorro = 1,
  Fdn16 = 2,
  HybridSpace = 3,
  Count,
  Invalid = 0xff,
};

inline const char* ReverbIdName(ReverbId id)
{
  switch(id)
  {
    case ReverbId::ReverbSc: return "reverbsc";
    case ReverbId::Dattorro: return "dattorro";
    case ReverbId::Fdn16: return "fdn16";
    case ReverbId::HybridSpace: return "hybrid";
    default: return "invalid";
  }
}

inline ReverbId ParseReverbId(const char* name)
{
  if(name == nullptr)
    return ReverbId::Invalid;
  if(std::strcmp(name, "reverbsc") == 0)
    return ReverbId::ReverbSc;
  if(std::strcmp(name, "dattorro") == 0)
    return ReverbId::Dattorro;
  if(std::strcmp(name, "fdn16") == 0)
    return ReverbId::Fdn16;
  if(std::strcmp(name, "hybrid") == 0)
    return ReverbId::HybridSpace;
  return ReverbId::Invalid;
}

struct ReverbSlotConfig
{
  // Factory voicing: an articulate early-reflection room on UP and a dense,
  // modulated hall on DOWN. USB configuration can override either slot.
  ReverbId up = ReverbId::HybridSpace;
  ReverbId down = ReverbId::Dattorro;
};

inline bool ValidReverbSlotConfig(const ReverbSlotConfig& config)
{
  return config.up < ReverbId::Count && config.down < ReverbId::Count
      && config.up != config.down;
}

// This is deliberately separate from the capture store (0x007b0000..) and
// A2 preset page (0x007ff000). It consumes one 4 KiB QSPI sector in the gap.
// A bad or interrupted write simply falls back to the safe built-in mapping.
template <typename Backend,
          uint32_t RegionOffset = 0x007f0000U,
          uint32_t RegionSize = 0x00001000U>
class ReverbConfigStore
{
 public:
  explicit ReverbConfigStore(Backend& backend) : backend_(backend) {}

  bool Load(ReverbSlotConfig& config) const
  {
    Header header = {};
    if(!backend_.Read(RegionOffset, reinterpret_cast<uint8_t*>(&header),
                      sizeof(header))
       || !ValidHeader(header))
      return false;
    config.up = static_cast<ReverbId>(header.up);
    config.down = static_cast<ReverbId>(header.down);
    return true;
  }

  bool Save(const ReverbSlotConfig& config)
  {
    if(!ValidReverbSlotConfig(config) || !backend_.Erase(RegionOffset, RegionSize))
      return false;
    Header header = {};
    header.magic = Magic;
    header.version = Version;
    header.header_size = sizeof(Header);
    header.up = static_cast<uint8_t>(config.up);
    header.down = static_cast<uint8_t>(config.down);
    header.crc32 = HeaderCrc(header);
    return backend_.Write(RegionOffset, reinterpret_cast<const uint8_t*>(&header),
                          sizeof(header));
  }

 private:
  struct Header
  {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint8_t up;
    uint8_t down;
    uint8_t reserved[2];
    uint32_t crc32;
  };

  static constexpr uint32_t Magic = 0x42565248U; // "HRVB"
  static constexpr uint16_t Version = 1;

  static uint32_t HeaderCrc(Header header)
  {
    header.crc32 = 0;
    return Crc32Update(0xffffffffU,
                       reinterpret_cast<const uint8_t*>(&header),
                       sizeof(header)) ^ 0xffffffffU;
  }

  static bool ValidHeader(const Header& header)
  {
    const ReverbSlotConfig config = {
        static_cast<ReverbId>(header.up), static_cast<ReverbId>(header.down)};
    return header.magic == Magic && header.version == Version
        && header.header_size == sizeof(Header) && ValidReverbSlotConfig(config)
        && HeaderCrc(header) == header.crc32;
  }

  Backend& backend_;
};

} // namespace hothouse_nam
