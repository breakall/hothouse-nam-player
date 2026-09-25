#pragma once

#include <cstddef>
#include <cstdint>

#include "nam_engine.h"

namespace hothouse_nam::model_engine::a1
{
constexpr size_t PayloadCapacity() { return 64U * 1024U; }
bool AcceptsPayloadSize(size_t size);
void Initialize(double sample_rate, size_t block_size);
void Clear();
LoadMetrics Load(const uint8_t* payload, size_t payload_size);
const char* LastError();
bool IsLoaded();
void ProcessBlock48(float* input, float* output);
} // namespace hothouse_nam::model_engine::a1

namespace hothouse_nam::model_engine::a2
{
constexpr size_t PayloadCapacity() { return 1871U * sizeof(float); }
bool AcceptsPayloadSize(size_t size);
void Initialize(double sample_rate, size_t block_size);
void Clear();
LoadMetrics Load(const uint8_t* payload, size_t payload_size);
const char* LastError();
bool IsLoaded();
void ProcessBlock48(float* input, float* output);
} // namespace hothouse_nam::model_engine::a2
