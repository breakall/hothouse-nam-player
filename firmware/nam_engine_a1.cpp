#include "nam_engine.h"

#include <cstdio>
#include <exception>
#include <memory>

#include "daisy.h"
#include "NAM/activations.h"
#include "NAM/dsp.h"
#include "namb/get_dsp_namb.h"

namespace hothouse_nam::model_engine
{
namespace
{
constexpr size_t kMaximumCaptureSize = 64U * 1024U;
DSY_SDRAM_BSS alignas(4) uint8_t payload[kMaximumCaptureSize];
std::unique_ptr<nam::DSP> model;
double configured_sample_rate = 48000.0;
size_t configured_block_size = 48;
char last_error[128] = {};
}

const char* BackendId()
{
  return "a1_nano_relu";
}

CaptureFormat PayloadFormat()
{
  return CaptureFormat::A1Namb;
}

size_t PayloadCapacity()
{
  return sizeof(payload);
}

bool AcceptsPayloadSize(size_t size)
{
  return size != 0 && size <= sizeof(payload);
}

uint8_t* PayloadBuffer()
{
  return payload;
}

void Initialize(double sample_rate, size_t block_size)
{
  configured_sample_rate = sample_rate;
  configured_block_size = block_size;
  nam::activations::Activation::enable_fast_tanh();
}

void Clear()
{
  model.reset();
}

LoadMetrics Load(size_t payload_size)
{
  LoadMetrics metrics;
  Clear();
  last_error[0] = '\0';
  try
  {
    const uint32_t construct_start = DWT->CYCCNT;
    model = nam::get_dsp_namb(payload, payload_size);
    metrics.construct_cycles = DWT->CYCCNT - construct_start;
    if(!model)
      return metrics;

    const uint32_t prepare_start = DWT->CYCCNT;
    // The pinned NAM core's Reset() currently performs the prewarm itself.
    // ResetAndPrewarm() would consequently run the expensive settling pass twice.
    model->Reset(configured_sample_rate,
                 static_cast<int>(configured_block_size));
    metrics.prepare_cycles = DWT->CYCCNT - prepare_start;
    metrics.loaded = true;
  }
  catch(const std::exception& error)
  {
    std::snprintf(last_error, sizeof(last_error), "%s", error.what());
    Clear();
  }
  return metrics;
}

const char* LastError()
{
  return last_error;
}

bool IsLoaded()
{
  return model != nullptr;
}

void ProcessBlock48(float* input, float* output)
{
  NAM_SAMPLE* input_ptr = input;
  NAM_SAMPLE* output_ptr = output;
  model->process(&input_ptr, &output_ptr, 48);
}

} // namespace hothouse_nam::model_engine
