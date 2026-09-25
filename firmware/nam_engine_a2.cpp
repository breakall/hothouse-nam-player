#include "nam_engine_backends.h"

#include <cstddef>
#include <cstdint>

#include "daisy.h"

#if HOTHOUSE_SIZE_BUILD
#define NAM_A2_NOINLINE __attribute__((noinline, optimize("O2")))
#endif
#include "nam_a2_runtime.h"

namespace hothouse_nam::model_engine::a2
{
namespace
{
NAM_A2_STATE_DATA nam_a2_daisy::A2Player model;
bool loaded = false;
}

bool AcceptsPayloadSize(size_t size)
{
  return size == PayloadCapacity();
}

void Initialize(double, size_t)
{
}

void Clear()
{
  loaded = false;
}

LoadMetrics Load(const uint8_t* payload, size_t payload_size)
{
  LoadMetrics metrics;
  if(!AcceptsPayloadSize(payload_size))
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

} // namespace hothouse_nam::model_engine::a2
