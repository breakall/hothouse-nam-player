// Host-only relative cost benchmark. This deliberately does not report
// Cortex-M7 cycles: use the firmware DWT counter for that. It catches major
// regressions and ranks engines before flashing hardware.

#include <chrono>
#include <cstdio>

#include "../../HothouseExamples/DaisySP/DaisySP-LGPL/Source/Effects/reverbsc.h"
#include "../reverbs/dattorro_reverb.h"
#include "../reverbs/fdn16_reverb.h"
#include "../reverbs/hybrid_space_reverb.h"

namespace
{
constexpr int kBlocks = 30000;
constexpr int kFramesPerBlock = 48;
volatile float sink = 0.0f;

template <typename Process>
double Benchmark(Process process)
{
  const auto start = std::chrono::steady_clock::now();
  float left = 0.0f;
  float right = 0.0f;
  for(int block = 0; block < kBlocks; ++block)
    for(int frame = 0; frame < kFramesPerBlock; ++frame)
    {
      const float input = ((block + frame * 13) % 251 == 0) ? 0.5f : 0.0f;
      process(input, &left, &right);
      sink += left + right;
    }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::micro>(elapsed).count()
       / static_cast<double>(kBlocks);
}

template <typename Engine>
double BenchmarkEngine(Engine& engine)
{
  return Benchmark([&engine](float input, float* left, float* right) {
    engine.Process(input, left, right);
  });
}

} // namespace

int main()
{
  daisysp::ReverbSc reverbsc;
  reverbsc.Init(48000.0f);
  reverbsc.SetFeedback(0.86f);
  reverbsc.SetLpFreq(9000.0f);

  hothouse_nam::reverb::DattorroReverb dattorro;
  dattorro.Init(48000.0f);
  dattorro.SetParameters(0.62f, 0.42f, 0.72f, 0.25f);

  hothouse_nam::reverb::Fdn16Reverb fdn16;
  fdn16.Init(48000.0f);
  fdn16.SetParameters(0.62f, 0.42f, 0.72f, 0.25f);

  hothouse_nam::reverb::HybridSpaceReverb hybrid;
  hybrid.Init(48000.0f);
  hybrid.SetParameters(0.62f, 0.42f, 0.72f, 0.25f);

  const double reverbsc_us = Benchmark([&reverbsc](float input, float* left,
                                                    float* right) {
    reverbsc.Process(input, input, left, right);
  });
  const double dattorro_us = BenchmarkEngine(dattorro);
  const double fdn16_us = BenchmarkEngine(fdn16);
  const double hybrid_us = BenchmarkEngine(hybrid);

  std::printf("Host-relative time per 48-frame block (lower is better)\n");
  std::printf("ReverbSc  %8.2f us  1.00x\n", reverbsc_us);
  std::printf("Dattorro  %8.2f us  %.2fx\n", dattorro_us,
              dattorro_us / reverbsc_us);
  std::printf("FDN16     %8.2f us  %.2fx\n", fdn16_us, fdn16_us / reverbsc_us);
  std::printf("Hybrid    %8.2f us  %.2fx\n", hybrid_us,
              hybrid_us / reverbsc_us);
  std::printf("sink=%f\n", static_cast<float>(sink));
  return 0;
}
