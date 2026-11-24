// Compare both kernels against the SAME NAMCore golden capture/input/output.
// This is numerical verification on the host, not a Cortex-M7 speed benchmark.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#define NAM_A2_HOT_DATA
#define NAM_A2_STATE_DATA
#define NAM_A2_HOT_STATE_DATA
#include "nam_a2_runtime.h"
extern "C" {
#include "nam_a2lite.h"
#include "namb_reader.h"
}

static nam_a2_daisy::A2Player legacy;
static nam_a2lite_weights_t weights;
static nam_a2lite_t candidate;
static nam_a2lite_state_t state;

static std::vector<unsigned char> Read(const char* path)
{
  std::ifstream file(path, std::ios::binary);
  if(!file)
  {
    std::fprintf(stderr, "Cannot open %s\n", path);
    std::exit(1);
  }
  return {std::istreambuf_iterator<char>(file), {}};
}

static bool Report(const char* name, const std::vector<float>& output,
                   const float* expected)
{
  double error = 0.0, reference = 0.0, maximum = 0.0;
  for(size_t i = 0; i < output.size(); ++i)
  {
    if(!std::isfinite(output[i]))
      return false;
    const double difference = static_cast<double>(output[i]) - expected[i];
    error += difference * difference;
    reference += static_cast<double>(expected[i]) * expected[i];
    maximum = std::max(maximum, std::fabs(difference));
  }
  const double esr = 10.0 * std::log10(error / reference);
  const bool passed = maximum < 1e-5 && esr < -100.0;
  std::printf("%s: samples=%zu max_abs_error=%.9g ESR=%.2f dB %s\n",
              name, output.size(), maximum, esr, passed ? "PASS" : "FAIL");
  return passed;
}

int main(int argc, char** argv)
{
  if(argc != 4)
    return 2;
  const auto model = Read(argv[1]);
  const auto input_bytes = Read(argv[2]);
  const auto reference_bytes = Read(argv[3]);
  if(input_bytes.size() != reference_bytes.size()
     || input_bytes.empty() || input_bytes.size() % (48 * sizeof(float)) != 0)
    return 2;
  namb_model_t parsed;
  if(namb_parse(model.data(), model.size(), &parsed) != NAMB_OK
     || namb_verify_crc(model.data(), model.size()) != NAMB_OK
     || namb_check_a2lite(&parsed) != NAMB_OK)
    return 2;
  if(!legacy.load_weights(parsed.weights, parsed.weight_count)
     || nam_a2lite_load_weights(&weights, parsed.weights, parsed.weight_count))
    return 2;
  nam_a2lite_init(&candidate, &weights, &state);
  nam_a2lite_prewarm(&candidate);
  const size_t samples = input_bytes.size() / sizeof(float);
  std::vector<float> input(samples), reference(samples);
  std::memcpy(input.data(), input_bytes.data(), input_bytes.size());
  std::memcpy(reference.data(), reference_bytes.data(), reference_bytes.size());
  std::vector<float> baseline_output(samples), candidate_output(samples);
  for(size_t offset = 0; offset < samples; offset += 48)
  {
    legacy.process_block_48(input.data() + offset, baseline_output.data() + offset);
    const float* inputs[] = {input.data() + offset};
    float* outputs[] = {candidate_output.data() + offset};
    nam_a2lite_process(&candidate, inputs, outputs, 48);
  }
  bool passed = Report("legacy vs NAMCore", baseline_output, reference.data());
  passed = Report("tone3000 vs NAMCore", candidate_output, reference.data()) && passed;
  passed = Report("tone3000 vs legacy", candidate_output, baseline_output.data()) && passed;
  std::printf("State bytes (host ABI): legacy=%zu tone3000=%zu; weights+instance tone3000=%zu\n",
              sizeof(nam_a2_daisy::A2State), sizeof(state), sizeof(weights) + sizeof(candidate));
  return passed ? 0 : 1;
}
