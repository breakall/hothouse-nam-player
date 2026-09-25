#include <cassert>
#include <cmath>
#include <cstdint>

#include "../capture_transition.h"

namespace
{
using Controller = hothouse_nam::CaptureTransitionController;

void Fill(float* values, size_t size, float value)
{
  for(size_t i = 0; i < size; ++i)
    values[i] = value;
}

void TestImmediateMuteAndSettle()
{
  Controller transition;
  transition.Initialize(0);
  transition.ObserveSlot(1, 100);
  assert(transition.GetState() == Controller::State::MuteRequested);
  assert(transition.PendingSlot() == 1);

  float left[4] = {0.5f, 0.25f, -0.1f, -0.5f};
  float right[4] = {0.5f, 0.25f, -0.1f, -0.5f};
  transition.ApplyOutput(left, right, 4);
  assert(transition.GetState() == Controller::State::Muted);
  assert(left[0] == 0.5f && left[1] == 0.25f);
  assert(left[2] == 0.0f && left[3] == 0.0f);

  uint8_t slot = Controller::NoSlot;
  assert(!transition.TakeSettledSlot(159, slot));
  assert(transition.TakeSettledSlot(160, slot));
  assert(slot == 1);
  assert(!transition.TakeSettledSlot(200, slot));
}

void TestQuietestSampleFallback()
{
  Controller transition;
  transition.Initialize(0);
  float prime_left[2] = {1.0f, 1.0f};
  float prime_right[2] = {1.0f, 1.0f};
  transition.ApplyOutput(prime_left, prime_right, 2);

  transition.ObserveSlot(1, 0);
  float left[4] = {0.8f, 0.4f, 0.1f, 0.3f};
  float right[4] = {0.7f, 0.3f, 0.2f, 0.4f};
  transition.ApplyOutput(left, right, 4);
  assert(left[0] == 0.8f && left[1] == 0.4f);
  assert(left[2] == 0.0f && left[3] == 0.0f);
  assert(right[2] == 0.0f && right[3] == 0.0f);
}

void TestRapidMovementSelectsOnlyFinalSlot()
{
  Controller transition;
  transition.Initialize(0);
  transition.ObserveSlot(1, 10);
  float left[2] = {0.2f, 0.2f};
  float right[2] = {0.2f, 0.2f};
  transition.ApplyOutput(left, right, 2);

  transition.ObserveSlot(2, 30);
  uint8_t slot = Controller::NoSlot;
  assert(!transition.TakeSettledSlot(89, slot));
  assert(transition.TakeSettledSlot(90, slot));
  assert(slot == 2);
}

void TestMutedBlocksAreSilent()
{
  Controller transition;
  transition.Initialize(0);
  transition.ObserveSlot(1, 0);
  float left[3] = {1.0f, 1.0f, 1.0f};
  float right[3] = {1.0f, 1.0f, 1.0f};
  transition.ApplyOutput(left, right, 3);
  Fill(left, 3, 0.75f);
  Fill(right, 3, -0.75f);
  transition.ApplyOutput(left, right, 3);
  for(size_t i = 0; i < 3; ++i)
    assert(left[i] == 0.0f && right[i] == 0.0f);
}

void TestFadeShapeAndCompletion()
{
  Controller transition;
  transition.Initialize(0);
  transition.BeginFadeIn();

  float left[48];
  float right[48];
  for(size_t block = 0; block < Controller::FadeSamples / 48; ++block)
  {
    Fill(left, 48, 1.0f);
    Fill(right, 48, 1.0f);
    transition.ApplyOutput(left, right, 48);
    if(block == 0)
    {
      assert(std::fabs(left[0] - 1.0f / 960.0f) < 0.000001f);
      assert(std::fabs(left[47] - 48.0f / 960.0f) < 0.000001f);
    }
  }
  assert(std::fabs(left[47] - 1.0f) < 0.000001f);
  assert(transition.FadeSamplesRemaining() == 0);
  assert(transition.GetState() == Controller::State::Normal);
}

void TestMovementInterruptsFade()
{
  Controller transition;
  transition.Initialize(0);
  transition.BeginFadeIn();
  transition.ObserveSlot(2, 10);
  assert(transition.GetState() == Controller::State::MuteRequested);
  float left[2] = {0.5f, 0.5f};
  float right[2] = {0.5f, 0.5f};
  transition.ApplyOutput(left, right, 2);
  assert(transition.GetState() == Controller::State::Muted);
  assert(left[0] == 0.0f && left[1] == 0.0f);
}

void TestTimerWraparound()
{
  Controller transition;
  transition.Initialize(0);
  transition.ObserveSlot(1, UINT32_MAX - 20U);
  float left[1] = {1.0f};
  float right[1] = {1.0f};
  transition.ApplyOutput(left, right, 1);
  uint8_t slot = Controller::NoSlot;
  assert(!transition.TakeSettledSlot(38U, slot));
  assert(transition.TakeSettledSlot(39U, slot));
  assert(slot == 1);
}
} // namespace

int main()
{
  TestImmediateMuteAndSettle();
  TestQuietestSampleFallback();
  TestRapidMovementSelectsOnlyFinalSlot();
  TestMutedBlocksAreSilent();
  TestFadeShapeAndCompletion();
  TestMovementInterruptsFade();
  TestTimerWraparound();
  return 0;
}
