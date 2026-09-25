#include "nam_engine.h"

#include "daisy.h"
#include "nam_engine_backends.h"

namespace hothouse_nam::model_engine
{
namespace
{
DSY_SDRAM_BSS alignas(32) uint8_t payload[a1::PayloadCapacity()];
CaptureFormat active_format = CaptureFormat::Unknown;
CaptureFormat last_attempted_format = CaptureFormat::Unknown;
}

const char* BackendId()
{
  return "a1_a2";
}

const char* ActiveBackendId()
{
  switch(active_format)
  {
    case CaptureFormat::A1Namb: return "a1_nano_relu";
    case CaptureFormat::A2WeightsF32: return "a2_lite";
    default: return "none";
  }
}

size_t PayloadCapacity()
{
  return sizeof(payload);
}

bool AcceptsPayload(CaptureFormat format, size_t size)
{
  switch(format)
  {
    case CaptureFormat::A1Namb: return a1::AcceptsPayloadSize(size);
    case CaptureFormat::A2WeightsF32: return a2::AcceptsPayloadSize(size);
    default: return false;
  }
}

uint8_t* PayloadBuffer()
{
  return payload;
}

void Initialize(double sample_rate, size_t block_size)
{
  a1::Initialize(sample_rate, block_size);
  a2::Initialize(sample_rate, block_size);
}

void Clear()
{
  a1::Clear();
  a2::Clear();
  active_format = CaptureFormat::Unknown;
}

LoadMetrics Load(CaptureFormat format, size_t payload_size)
{
  Clear();
  last_attempted_format = format;
  LoadMetrics metrics;
  if(!AcceptsPayload(format, payload_size))
    return metrics;

  switch(format)
  {
    case CaptureFormat::A1Namb:
      metrics = a1::Load(payload, payload_size);
      break;
    case CaptureFormat::A2WeightsF32:
      metrics = a2::Load(payload, payload_size);
      break;
    default:
      break;
  }
  if(metrics.loaded)
    active_format = format;
  return metrics;
}

const char* LastError()
{
  return last_attempted_format == CaptureFormat::A1Namb ? a1::LastError() : "";
}

bool IsLoaded()
{
  switch(active_format)
  {
    case CaptureFormat::A1Namb: return a1::IsLoaded();
    case CaptureFormat::A2WeightsF32: return a2::IsLoaded();
    default: return false;
  }
}

void ProcessBlock48(float* input, float* output)
{
  switch(active_format)
  {
    case CaptureFormat::A1Namb:
      a1::ProcessBlock48(input, output);
      break;
    case CaptureFormat::A2WeightsF32:
      a2::ProcessBlock48(input, output);
      break;
    default:
      break;
  }
}

} // namespace hothouse_nam::model_engine
