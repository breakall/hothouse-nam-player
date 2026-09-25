#pragma once

#include <cstddef>
#include <cstdint>

#include "capture_loader.h"

namespace hothouse_nam::model_engine
{

struct LoadMetrics
{
  bool loaded = false;
  uint32_t construct_cycles = 0;
  uint32_t prepare_cycles = 0;
};

const char* BackendId();
CaptureFormat PayloadFormat();
size_t PayloadCapacity();
bool AcceptsPayloadSize(size_t size);
uint8_t* PayloadBuffer();

void Initialize(double sample_rate, size_t block_size);
void Clear();
LoadMetrics Load(size_t payload_size);
const char* LastError();
bool IsLoaded();
void ProcessBlock48(float* input, float* output);

} // namespace hothouse_nam::model_engine
