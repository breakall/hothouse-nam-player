#include "nam_engine_backends.h"

#include <cstdio>
#include <exception>
#include <memory>

#include "daisy.h"
#include "NAM/activations.h"
#include "NAM/dsp.h"
#include "namb/get_dsp_namb.h"

namespace hothouse_nam::model_engine::a1
{
namespace
{
std::unique_ptr<nam::DSP> model;
double configured_sample_rate = 48000.0;
size_t configured_block_size = 48;
char last_error[128] = {};
}

bool AcceptsPayloadSize(size_t size)
{
  return size != 0 && size <= PayloadCapacity();
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

LoadMetrics Load(const uint8_t* payload, size_t payload_size)
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

} // namespace hothouse_nam::model_engine::a1
