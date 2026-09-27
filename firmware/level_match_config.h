#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>

#include "capture_loader.h"

namespace hothouse_nam
{

// NAM's loudness metadata is expressed in dB. Millidecibels avoid parsing or
// storing floating-point values in the USB protocol and persistent settings.
constexpr int32_t kUnknownLoudnessMillidb = INT32_MIN;

struct LevelMatchConfig
{
  bool enabled = false;
  int32_t loudness_millidb[3] = {
      kUnknownLoudnessMillidb, kUnknownLoudnessMillidb, kUnknownLoudnessMillidb};
};

inline bool ValidLoudnessMillidb(int32_t loudness)
{
  // NAM loudness is normally negative and well inside this deliberately broad
  // range. Keeping bogus metadata out prevents unexpectedly large boosts.
  return loudness >= -120000 && loudness <= 24000;
}

inline bool ClearSlotLoudness(LevelMatchConfig& config, uint8_t slot)
{
  if(slot >= 3)
    return false;
  config.loudness_millidb[slot] = kUnknownLoudnessMillidb;
  return true;
}

inline float LevelMatchGainForSlot(const LevelMatchConfig& config, uint8_t slot,
                                   const bool valid_slots[3],
                                   float maximum_boost_db = 12.0f)
{
  if(!config.enabled || slot >= 3 || valid_slots == nullptr || !valid_slots[slot]
     || config.loudness_millidb[slot] == kUnknownLoudnessMillidb)
    return 1.0f;

  int32_t reference = kUnknownLoudnessMillidb;
  for(size_t i = 0; i < 3; ++i)
  {
    const int32_t loudness = config.loudness_millidb[i];
    if(valid_slots[i] && loudness != kUnknownLoudnessMillidb
       && (reference == kUnknownLoudnessMillidb || loudness > reference))
      reference = loudness;
  }
  if(reference == kUnknownLoudnessMillidb)
    return 1.0f;

  const float trim_db = std::fmin(maximum_boost_db,
      static_cast<float>(reference - config.loudness_millidb[slot]) / 1000.0f);
  return std::pow(10.0f, trim_db / 20.0f);
}

// The final 4 KiB of the capture region is unused by the three 84 KiB capture
// slots. Keep level-match data here, separate from both reverb and presets.
template <typename Backend,
          uint32_t RegionOffset = 0x007ef000U,
          uint32_t RegionSize = 0x00001000U>
class LevelMatchConfigStore
{
 public:
  explicit LevelMatchConfigStore(Backend& backend) : backend_(backend) {}

  bool Load(LevelMatchConfig& config) const
  {
    Header header = {};
    if(!backend_.Read(RegionOffset, reinterpret_cast<uint8_t*>(&header),
                      sizeof(header)) || !ValidHeader(header))
      return false;
    config.enabled = header.enabled != 0;
    for(size_t i = 0; i < 3; ++i)
      config.loudness_millidb[i] = header.loudness_millidb[i];
    return true;
  }

  bool Save(const LevelMatchConfig& config)
  {
    if(!backend_.Erase(RegionOffset, RegionSize))
      return false;
    Header header = {};
    header.magic = Magic;
    header.version = Version;
    header.header_size = sizeof(Header);
    header.enabled = config.enabled ? 1U : 0U;
    for(size_t i = 0; i < 3; ++i)
      header.loudness_millidb[i] = config.loudness_millidb[i];
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
    uint8_t enabled;
    uint8_t reserved[3];
    int32_t loudness_millidb[3];
    uint32_t crc32;
  };

  static constexpr uint32_t Magic = 0x4c4d4e48U; // "HNML"
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
    if(header.magic != Magic || header.version != Version
       || header.header_size != sizeof(Header) || header.enabled > 1U
       || HeaderCrc(header) != header.crc32)
      return false;
    for(const int32_t loudness : header.loudness_millidb)
      if(loudness != kUnknownLoudnessMillidb && !ValidLoudnessMillidb(loudness))
        return false;
    return true;
  }

  Backend& backend_;
};

} // namespace hothouse_nam
