#include "nam_engine.h"

#include <cstddef>
#include <cstdint>

#include "daisy.h"

#if HOTHOUSE_SIZE_BUILD
#define NAM_A2_NOINLINE __attribute__((noinline, optimize("O2")))
#endif
#include "nam_a2_runtime.h"

namespace hothouse_nam::model_engine
{
namespace
{
constexpr size_t kPayloadSize = nam_a2_daisy::kA2WeightCount * sizeof(float);
DSY_SDRAM_BSS alignas(32) uint8_t payload[kPayloadSize];
NAM_A2_STATE_DATA nam_a2_daisy::A2Player model;
bool loaded = false;
}

const char* BackendId()
{
  return "a2_lite";
}

const char* ActiveBackendId()
{
  return loaded ? "a2_lite" : "none";
}

size_t PayloadCapacity()
{
  return sizeof(payload);
}

bool AcceptsPayload(CaptureFormat format, size_t size)
{
  return format == CaptureFormat::A2WeightsF32 && size == sizeof(payload);
}

uint8_t* PayloadBuffer()
{
  return payload;
}

void Initialize(double, size_t)
{
}

void Clear()
{
  loaded = false;
}

LoadMetrics Load(CaptureFormat format, size_t payload_size)
{
  LoadMetrics metrics;
  if(!AcceptsPayload(format, payload_size))
  {
    Clear();
    return metrics;
  }

  const uint32_t load_start = DWT->CYCCNT;
  loaded = model.load_weights(reinterpret_cast<const float*>(payload),
                              nam_a2_daisy::kA2WeightCount);
  metrics.construct_cycles = DWT->CYCCNT - load_start;
  metrics.loaded = loaded;
  return metrics;
}

const char* LastError()
{
  return "";
}

bool IsLoaded()
{
  return loaded;
}

void ProcessBlock48(float* input, float* output)
{
  model.process_block_48(input, output);
}

} // namespace hothouse_nam::model_engine
