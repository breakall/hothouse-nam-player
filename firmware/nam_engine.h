#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "capture_loader.h"

namespace hothouse_nam::model_engine
{

static_assert(sizeof(float) == sizeof(uint32_t),
              "NAM payload validation requires 32-bit floats");

struct LoadMetrics
{
  bool loaded = false;
  uint32_t construct_cycles = 0;
  uint32_t prepare_cycles = 0;
};

// Test the IEEE-754 exponent bits directly so this remains reliable in the
// firmware's -ffast-math build, where std::isfinite may be optimized away.
inline bool WeightsAreFinite(const float* weights, size_t count)
{
  if(weights == nullptr)
    return false;
  for(size_t i = 0; i < count; ++i)
  {
    uint32_t bits = 0;
    std::memcpy(&bits, weights + i, sizeof(bits));
    if((bits & 0x7f800000U) == 0x7f800000U)
      return false;
  }
  return true;
}

const char* BackendId();
const char* ActiveBackendId();
size_t PayloadCapacity();
bool AcceptsPayload(CaptureFormat format, size_t size);
bool PayloadIsValid(CaptureFormat format, size_t size);
uint8_t* PayloadBuffer();

void Initialize(double sample_rate, size_t block_size);
void Clear();
LoadMetrics Load(CaptureFormat format, size_t payload_size);
const char* LastError();
bool IsLoaded();
void ProcessBlock48(float* input, float* output);

} // namespace hothouse_nam::model_engine
