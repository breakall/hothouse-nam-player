#include <cassert>
#include <cmath>
#include <limits>

#include "../reverbs/dattorro_reverb.h"
#include "../reverbs/fdn16_reverb.h"
#include "../reverbs/hybrid_space_reverb.h"

template <typename Engine>
void VerifyEngine()
{
  Engine engine;
  assert(engine.Init(48000.0f));
  engine.SetParameters(0.72f, 0.36f, 0.68f, 0.25f);
  float left = 0.0f;
  float right = 0.0f;
  for(int i = 0; i < 12000; ++i)
  {
    engine.Process(i == 0 ? 1.0f : 0.0f, &left, &right);
    assert(std::isfinite(left));
    assert(std::isfinite(right));
    assert(std::fabs(left) < 16.0f);
    assert(std::fabs(right) < 16.0f);
  }
}

template <typename Engine>
void VerifyInvalidSampleRates()
{
  Engine engine;
  assert(!engine.Init(std::numeric_limits<float>::quiet_NaN()));
  assert(!engine.Init(std::numeric_limits<float>::infinity()));
}

int main()
{
  VerifyEngine<hothouse_nam::reverb::DattorroReverb>();
  VerifyEngine<hothouse_nam::reverb::Fdn16Reverb>();
  VerifyEngine<hothouse_nam::reverb::HybridSpaceReverb>();
  VerifyInvalidSampleRates<hothouse_nam::reverb::DattorroReverb>();
  VerifyInvalidSampleRates<hothouse_nam::reverb::Fdn16Reverb>();
  VerifyInvalidSampleRates<hothouse_nam::reverb::HybridSpaceReverb>();
  return 0;
}
