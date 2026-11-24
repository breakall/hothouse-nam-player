#include "nam_engine.h"

#include <cstddef>
#include <cstdint>

#include "daisy.h"

#if HOTHOUSE_ENGINE_TONE3000
extern "C" {
#include "nam_a2lite.h"
}

// The BOOT_SRAM image stores ITCM code in its SRAM load image. Copy it before
// main (and therefore before Initialize/Load), while preserving the existing
// boot mode and QSPI capture layout.
extern "C" uint32_t _snam_itcm, _nam_itcm_start, _nam_itcm_end;
static void CopyNamCodeToItcm()
{
  const volatile uint32_t* src = &_snam_itcm;
  volatile uint32_t* dst = &_nam_itcm_start;
  while(dst < &_nam_itcm_end)
    *dst++ = *src++;
  __DSB();
  __ISB();
}
__attribute__((section(".preinit_array"), used))
static void (*const copy_nam_code)() = CopyNamCodeToItcm;
#else
#if HOTHOUSE_SIZE_BUILD
#define NAM_A2_NOINLINE __attribute__((noinline, optimize("O2")))
#endif
#include "nam_a2_runtime.h"
#endif

namespace hothouse_nam::model_engine
{
namespace
{
constexpr size_t kWeightCount = 1871;
constexpr size_t kPayloadSize = kWeightCount * sizeof(float);
DSY_SDRAM_BSS alignas(32) uint8_t payload[kPayloadSize];
#if HOTHOUSE_ENGINE_TONE3000
__attribute__((section(".dtcmram_bss"))) nam_a2lite_weights_t weights;
__attribute__((section(".dtcmram_bss"))) nam_a2lite_t model;
__attribute__((section(".nam_axi_bss"))) nam_a2lite_state_t state;
#else
NAM_A2_STATE_DATA nam_a2_daisy::A2Player model;
#endif
bool loaded = false;
const char* last_error = "";
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

bool PayloadIsValid(CaptureFormat format, size_t size)
{
  return AcceptsPayload(format, size)
      && WeightsAreFinite(reinterpret_cast<const float*>(payload),
                          kWeightCount);
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
  last_error = "";
}

LoadMetrics Load(CaptureFormat format, size_t payload_size)
{
  LoadMetrics metrics;
  if(!AcceptsPayload(format, payload_size))
  {
    Clear();
    last_error = "incompatible payload";
    return metrics;
  }
  if(!PayloadIsValid(format, payload_size))
  {
    Clear();
    last_error = "capture contains non-finite weights";
    return metrics;
  }

  const uint32_t load_start = DWT->CYCCNT;
#if HOTHOUSE_ENGINE_TONE3000
  loaded = nam_a2lite_load_weights(&weights,
      reinterpret_cast<const float*>(payload), kWeightCount) == 0;
  metrics.construct_cycles = DWT->CYCCNT - load_start;
  if(loaded)
  {
    const uint32_t prepare_start = DWT->CYCCNT;
    nam_a2lite_init(&model, &weights, &state);
    nam_a2lite_prewarm(&model);
    metrics.prepare_cycles = DWT->CYCCNT - prepare_start;
  }
#else
  loaded = model.load_weights(reinterpret_cast<const float*>(payload),
                              kWeightCount);
  metrics.construct_cycles = DWT->CYCCNT - load_start;
#endif
  last_error = loaded ? "" : "runtime rejected capture weights";
  metrics.loaded = loaded;
  return metrics;
}

const char* LastError()
{
  return last_error;
}

bool IsLoaded()
{
  return loaded;
}

void ProcessBlock48(float* input, float* output)
{
#if HOTHOUSE_ENGINE_TONE3000
  const float* inputs[] = {input};
  float* outputs[] = {output};
  nam_a2lite_process(&model, inputs, outputs, 48);
#else
  model.process_block_48(input, output);
#endif
}

} // namespace hothouse_nam::model_engine
