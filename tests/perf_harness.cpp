#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/RenderPipeline.h"
#include "inference/InferenceEngine.h"

namespace {

using Clock = std::chrono::steady_clock;

struct CliOptions {
  std::string mode = "benchmark";
  int frames = -1;
  int width = -1;
  int height = -1;
  int quality = 1;
  int cache_cycle = -1;
  int warmup = -1;
  int spin_iters = 0;
  int cache_limit = 64;
  int tile_size = 256;
  int overlap = 16;
  bool quiet = false;
};

struct FrameInput {
  std::uint64_t hash = 0;
  zsoda::core::FrameBuffer frame;
};

struct ScenarioConfig {
  std::string name;
  int frames = 0;
  int width = 0;
  int height = 0;
  int quality = 1;
  int cache_cycle = 0;
  int warmup = 0;
  int spin_iters = 0;
  int cache_limit = 64;
  int tile_size = 256;
  int overlap = 16;
  bool cache_enabled = true;
};

struct ScenarioResult {
  int frames = 0;
  int cache_hits = 0;
  int inference = 0;
  int fallback_tiled = 0;
  int fallback_downscaled = 0;
  int safe_output = 0;
  int engine_runs = 0;
  double total_ms = 0.0;
  std::string first_error;
};

class PerfInferenceEngine final : public zsoda::inference::IInferenceEngine {
 public:
  explicit PerfInferenceEngine(int spin_iters) : spin_iters_(std::max(0, spin_iters)) {}

  const char* Name() const override { return "PerfInferenceEngine"; }

  bool Initialize(const std::string& model_id, std::string* error) override {
    if (model_id.empty()) {
      if (error != nullptr) {
        *error = "model id cannot be empty";
      }
      return false;
    }
    model_id_ = model_id;
    return true;
  }

  bool SelectModel(const std::string& model_id, std::string* error) override {
    return Initialize(model_id, error);
  }

  std::vector<std::string> ListModelIds() const override {
    return {model_id_.empty() ? std::string("depth-anything-v3-small") : model_id_};
  }

  std::string ActiveModelId() const override {
    return model_id_.empty() ? "depth-anything-v3-small" : model_id_;
  }

  bool Run(const zsoda::inference::InferenceRequest& request,
           zsoda::core::FrameBuffer* out_depth,
           std::string* error) const override {
    if (request.source == nullptr || out_depth == nullptr) {
      if (error != nullptr) {
        *error = "invalid inference request";
      }
      return false;
    }
    const auto& src = *request.source;
    if (src.empty()) {
      if (error != nullptr) {
        *error = "source frame is empty";
      }
      return false;
    }

    std::uint64_t sink = 0;
    for (int i = 0; i < spin_iters_; ++i) {
      sink ^= (static_cast<std::uint64_t>(i) + 0x9e3779b97f4a7c15ULL + (sink << 6U) + (sink >> 2U));
    }
    if (sink == 1U << 30U) {
      if (error != nullptr) {
        *error = "unexpected spin state";
      }
      return false;
    }

    auto desc = src.desc();
    desc.channels = 1;
    desc.format = zsoda::core::PixelFormat::kGray32F;
    out_depth->Resize(desc);

    const float quality_scale = static_cast<float>(std::clamp(request.quality, 1, 4));
    for (int y = 0; y < desc.height; ++y) {
      for (int x = 0; x < desc.width; ++x) {
        const float src_value = src.at(x, y, 0);
        const float jitter = static_cast<float>(((x * 17 + y * 31) & 15)) * 0.001F;
        out_depth->at(x, y, 0) = (src_value + jitter) / quality_scale;
      }
    }

    run_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  int run_count() const { return run_count_.load(std::memory_order_relaxed); }

 private:
  int spin_iters_ = 0;
  std::string model_id_ = "depth-anything-v3-small";
  mutable std::atomic<int> run_count_{0};
};

void PrintUsage() {
  std::cout << "Usage: zsoda_perf_harness [options]\n"
               "  --mode benchmark|stability\n"
               "  --frames <N>\n"
               "  --width <N>\n"
               "  --height <N>\n"
               "  --quality <N>\n"
               "  --cache-cycle <N>   (0 = all unique frame hashes)\n"
               "  --warmup <N>\n"
               "  --spin-iters <N>\n"
               "  --cache-limit <N>\n"
               "  --tile-size <N>\n"
               "  --overlap <N>\n"
               "  --quiet\n";
}

bool ParseInt(const std::string& text, int* out) {
  if (out == nullptr) {
    return false;
  }
  try {
    std::size_t consumed = 0;
    const int value = std::stoi(text, &consumed);
    if (consumed != text.size()) {
      return false;
    }
    *out = value;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool ParseArgs(int argc, char** argv, CliOptions* options, std::string* error) {
  if (options == nullptr) {
    if (error != nullptr) {
      *error = "options is null";
    }
    return false;
  }

  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      PrintUsage();
      std::exit(0);
    }
    if (arg == "--quiet") {
      options->quiet = true;
      continue;
    }

    if (i + 1 >= argc) {
      if (error != nullptr) {
        *error = "missing value for " + arg;
      }
      return false;
    }

    const std::string value(argv[++i]);
    if (arg == "--mode") {
      options->mode = value;
      continue;
    }
    if (arg == "--frames") {
      if (!ParseInt(value, &options->frames)) {
        if (error != nullptr) {
          *error = "invalid --frames value";
        }
        return false;
      }
      continue;
    }
    if (arg == "--width") {
      if (!ParseInt(value, &options->width)) {
        if (error != nullptr) {
          *error = "invalid --width value";
        }
        return false;
      }
      continue;
    }
    if (arg == "--height") {
      if (!ParseInt(value, &options->height)) {
        if (error != nullptr) {
          *error = "invalid --height value";
        }
        return false;
      }
      continue;
    }
    if (arg == "--quality") {
      if (!ParseInt(value, &options->quality)) {
        if (error != nullptr) {
          *error = "invalid --quality value";
        }
        return false;
      }
      continue;
    }
    if (arg == "--cache-cycle") {
      if (!ParseInt(value, &options->cache_cycle)) {
        if (error != nullptr) {
          *error = "invalid --cache-cycle value";
        }
        return false;
      }
      continue;
    }
    if (arg == "--warmup") {
      if (!ParseInt(value, &options->warmup)) {
        if (error != nullptr) {
          *error = "invalid --warmup value";
        }
        return false;
      }
      continue;
    }
    if (arg == "--spin-iters") {
      if (!ParseInt(value, &options->spin_iters)) {
        if (error != nullptr) {
          *error = "invalid --spin-iters value";
        }
        return false;
      }
      continue;
    }
    if (arg == "--cache-limit") {
      if (!ParseInt(value, &options->cache_limit)) {
        if (error != nullptr) {
          *error = "invalid --cache-limit value";
        }
        return false;
      }
      continue;
    }
    if (arg == "--tile-size") {
      if (!ParseInt(value, &options->tile_size)) {
        if (error != nullptr) {
          *error = "invalid --tile-size value";
        }
        return false;
      }
      continue;
    }
    if (arg == "--overlap") {
      if (!ParseInt(value, &options->overlap)) {
        if (error != nullptr) {
          *error = "invalid --overlap value";
        }
        return false;
      }
      continue;
    }

    if (error != nullptr) {
      *error = "unknown option: " + arg;
    }
    return false;
  }

  if (options->mode != "benchmark" && options->mode != "stability") {
    if (error != nullptr) {
      *error = "--mode must be benchmark or stability";
    }
    return false;
  }

  if (options->quality < 1) {
    if (error != nullptr) {
      *error = "--quality must be >= 1";
    }
    return false;
  }
  if (options->cache_limit < 1) {
    if (error != nullptr) {
      *error = "--cache-limit must be >= 1";
    }
    return false;
  }
  if (options->tile_size < 32) {
    if (error != nullptr) {
      *error = "--tile-size must be >= 32";
    }
    return false;
  }
  if (options->overlap < 0) {
    if (error != nullptr) {
      *error = "--overlap must be >= 0";
    }
    return false;
  }

  return true;
}

std::uint64_t FrameHashAt(int frame_index, int cache_cycle) {
  if (cache_cycle <= 0) {
    return static_cast<std::uint64_t>(frame_index);
  }
  return static_cast<std::uint64_t>(frame_index % cache_cycle);
}

zsoda::core::FrameBuffer MakeSyntheticFrame(int width, int height, std::uint64_t seed) {
  zsoda::core::FrameDesc desc;
  desc.width = width;
  desc.height = height;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;

  zsoda::core::FrameBuffer frame(desc);
  const std::uint64_t base = seed * 11400714819323198485ULL;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::uint64_t mix = base + static_cast<std::uint64_t>(x) * 73856093ULL +
                                static_cast<std::uint64_t>(y) * 19349663ULL;
      const float value = static_cast<float>(mix % 1024ULL) / 1023.0F;
      frame.at(x, y, 0) = value;
    }
  }
  return frame;
}

std::vector<FrameInput> BuildInputBank(const ScenarioConfig& config) {
  const int unique_count =
      config.cache_cycle > 0 ? std::min(config.frames, config.cache_cycle) : config.frames;

  std::vector<FrameInput> bank;
  bank.reserve(static_cast<std::size_t>(unique_count));
  for (int i = 0; i < unique_count; ++i) {
    FrameInput entry;
    entry.hash = static_cast<std::uint64_t>(i);
    entry.frame = MakeSyntheticFrame(config.width, config.height, entry.hash);
    bank.push_back(std::move(entry));
  }
  return bank;
}

ScenarioResult RunScenario(const ScenarioConfig& config) {
  ScenarioResult result;
  result.frames = config.frames;

  auto engine = std::make_shared<PerfInferenceEngine>(config.spin_iters);
  std::string error;
  if (!engine->Initialize("depth-anything-v3-small", &error)) {
    result.safe_output = config.frames;
    result.first_error = error.empty() ? "engine initialize failed" : error;
    return result;
  }

  zsoda::core::RenderPipeline pipeline(engine);
  pipeline.SetCacheLimit(static_cast<std::size_t>(config.cache_limit));

  const auto input_bank = BuildInputBank(config);
  if (input_bank.empty()) {
    result.safe_output = config.frames;
    result.first_error = "input bank is empty";
    return result;
  }

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.quality = config.quality;
  params.cache_enabled = config.cache_enabled;
  params.output_mode = zsoda::core::OutputMode::kDepthMap;
  params.tile_size = config.tile_size;
  params.overlap = config.overlap;

  for (int i = 0; i < config.warmup; ++i) {
    const std::uint64_t hash = FrameHashAt(i, config.cache_cycle);
    const auto& src = input_bank[hash % input_bank.size()].frame;
    params.frame_hash = hash;
    params.cache_enabled = false;
    (void)pipeline.Render(src, params);
  }

  pipeline.PurgeCache();
  params.cache_enabled = config.cache_enabled;
  const int run_count_before_timed = engine->run_count();

  const auto start = Clock::now();
  for (int i = 0; i < config.frames; ++i) {
    const std::uint64_t hash = FrameHashAt(i, config.cache_cycle);
    const auto& src = input_bank[hash % input_bank.size()].frame;
    params.frame_hash = hash;

    const auto output = pipeline.Render(src, params);
    switch (output.status) {
      case zsoda::core::RenderStatus::kCacheHit:
        ++result.cache_hits;
        break;
      case zsoda::core::RenderStatus::kInference:
        ++result.inference;
        break;
      case zsoda::core::RenderStatus::kFallbackTiled:
        ++result.fallback_tiled;
        break;
      case zsoda::core::RenderStatus::kFallbackDownscaled:
        ++result.fallback_downscaled;
        break;
      case zsoda::core::RenderStatus::kSafeOutput:
        ++result.safe_output;
        if (result.first_error.empty()) {
          result.first_error = output.message;
        }
        break;
    }
  }
  const auto end = Clock::now();
  result.total_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
  result.engine_runs = engine->run_count() - run_count_before_timed;
  return result;
}

void PrintScenarioResult(const ScenarioConfig& config, const ScenarioResult& result, bool quiet) {
  const double avg_ms = result.frames > 0 ? result.total_ms / static_cast<double>(result.frames) : 0.0;
  const double hit_rate = result.frames > 0
                              ? (100.0 * static_cast<double>(result.cache_hits) /
                                 static_cast<double>(result.frames))
                              : 0.0;

  std::cout << std::fixed << std::setprecision(3);
  if (!quiet) {
    std::cout << config.name << "\n";
    std::cout << "  frames=" << result.frames << " size=" << config.width << "x" << config.height
              << " quality=" << config.quality << " cache_enabled=" << (config.cache_enabled ? 1 : 0)
              << " cache_cycle=" << config.cache_cycle << "\n";
    std::cout << "  total_ms=" << result.total_ms << " avg_ms_per_frame=" << avg_ms
              << " cache_hit_rate=" << hit_rate << "%\n";
    std::cout << "  status_count cache_hit=" << result.cache_hits << " inference=" << result.inference
              << " fallback_tiled=" << result.fallback_tiled
              << " fallback_downscaled=" << result.fallback_downscaled
              << " safe_output=" << result.safe_output << " engine_runs=" << result.engine_runs << "\n";
    if (!result.first_error.empty()) {
      std::cout << "  first_error=" << result.first_error << "\n";
    }
    return;
  }

  std::cout << config.name << " frames=" << result.frames << " total_ms=" << result.total_ms
            << " avg_ms_per_frame=" << avg_ms << " cache_hit_rate=" << hit_rate
            << "% cache_hits=" << result.cache_hits << " inference=" << result.inference
            << " fallback_tiled=" << result.fallback_tiled
            << " fallback_downscaled=" << result.fallback_downscaled
            << " safe_output=" << result.safe_output << " engine_runs=" << result.engine_runs << "\n";
}

bool ValidateCommon(const ScenarioConfig& config, const ScenarioResult& result, std::string* error) {
  if (result.frames <= 0) {
    if (error != nullptr) {
      *error = config.name + ": no frames executed";
    }
    return false;
  }
  if (result.safe_output > 0) {
    if (error != nullptr) {
      *error = config.name + ": safe output encountered: " + result.first_error;
    }
    return false;
  }
  if (result.fallback_tiled > 0 || result.fallback_downscaled > 0) {
    if (error != nullptr) {
      *error = config.name + ": fallback path used unexpectedly";
    }
    return false;
  }
  return true;
}

ScenarioConfig BuildBenchmarkNoCacheConfig(const CliOptions& cli) {
  ScenarioConfig config;
  config.name = "[benchmark:no-cache]";
  config.frames = cli.frames > 0 ? cli.frames : 240;
  config.width = cli.width > 0 ? cli.width : 320;
  config.height = cli.height > 0 ? cli.height : 180;
  config.quality = cli.quality;
  config.cache_cycle = 0;
  config.warmup = cli.warmup >= 0 ? cli.warmup : 24;
  config.spin_iters = cli.spin_iters;
  config.cache_limit = cli.cache_limit;
  config.tile_size = cli.tile_size;
  config.overlap = cli.overlap;
  config.cache_enabled = false;
  return config;
}

ScenarioConfig BuildBenchmarkCachedConfig(const CliOptions& cli) {
  ScenarioConfig config;
  config.name = "[benchmark:cache]";
  config.frames = cli.frames > 0 ? cli.frames : 240;
  config.width = cli.width > 0 ? cli.width : 320;
  config.height = cli.height > 0 ? cli.height : 180;
  config.quality = cli.quality;
  config.cache_cycle = cli.cache_cycle >= 0 ? cli.cache_cycle : 12;
  config.warmup = cli.warmup >= 0 ? cli.warmup : 24;
  config.spin_iters = cli.spin_iters;
  config.cache_limit = cli.cache_limit;
  config.tile_size = cli.tile_size;
  config.overlap = cli.overlap;
  config.cache_enabled = true;
  return config;
}

ScenarioConfig BuildStabilityConfig(const CliOptions& cli) {
  ScenarioConfig config;
  config.name = "[stability]";
  config.frames = cli.frames > 0 ? cli.frames : 1200;
  config.width = cli.width > 0 ? cli.width : 64;
  config.height = cli.height > 0 ? cli.height : 36;
  config.quality = cli.quality;
  config.cache_cycle = cli.cache_cycle >= 0 ? cli.cache_cycle : 0;
  config.warmup = cli.warmup >= 0 ? cli.warmup : 12;
  config.spin_iters = cli.spin_iters;
  config.cache_limit = cli.cache_limit;
  config.tile_size = cli.tile_size;
  config.overlap = cli.overlap;
  config.cache_enabled = true;
  return config;
}

bool RunBenchmarkMode(const CliOptions& cli, std::string* error) {
  const auto no_cache_config = BuildBenchmarkNoCacheConfig(cli);
  const auto cached_config = BuildBenchmarkCachedConfig(cli);

  if (cached_config.cache_cycle <= 0) {
    if (error != nullptr) {
      *error = "benchmark mode requires --cache-cycle > 0 to measure cache hit behavior";
    }
    return false;
  }

  const auto no_cache = RunScenario(no_cache_config);
  const auto cached = RunScenario(cached_config);

  PrintScenarioResult(no_cache_config, no_cache, cli.quiet);
  PrintScenarioResult(cached_config, cached, cli.quiet);

  std::string validation_error;
  if (!ValidateCommon(no_cache_config, no_cache, &validation_error) ||
      !ValidateCommon(cached_config, cached, &validation_error)) {
    if (error != nullptr) {
      *error = validation_error;
    }
    return false;
  }
  if (no_cache.cache_hits != 0) {
    if (error != nullptr) {
      *error = "benchmark no-cache scenario produced cache hits";
    }
    return false;
  }
  if (cached.cache_hits <= 0) {
    if (error != nullptr) {
      *error = "benchmark cache scenario did not produce cache hits";
    }
    return false;
  }
  if (cached.engine_runs >= cached.frames) {
    if (error != nullptr) {
      *error = "benchmark cache scenario did not reduce inference runs";
    }
    return false;
  }

  const double no_cache_avg = no_cache.total_ms / static_cast<double>(no_cache.frames);
  const double cached_avg = cached.total_ms / static_cast<double>(cached.frames);
  const double speedup = cached_avg > 0.0 ? no_cache_avg / cached_avg : 0.0;

  std::cout << std::fixed << std::setprecision(3);
  std::cout << "[benchmark] speedup_vs_no_cache=" << speedup << "x" << "\n";
  return true;
}

bool RunStabilityMode(const CliOptions& cli, std::string* error) {
  const auto config = BuildStabilityConfig(cli);
  if (config.frames < 1000) {
    if (error != nullptr) {
      *error = "stability mode requires --frames >= 1000";
    }
    return false;
  }

  const auto result = RunScenario(config);
  PrintScenarioResult(config, result, cli.quiet);

  std::string validation_error;
  if (!ValidateCommon(config, result, &validation_error)) {
    if (error != nullptr) {
      *error = validation_error;
    }
    return false;
  }

  if (config.cache_cycle > 0 && config.cache_cycle <= config.cache_limit && result.cache_hits == 0) {
    if (error != nullptr) {
      *error = "stability mode expected at least one cache hit";
    }
    return false;
  }

  return true;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions cli;
  std::string error;
  if (!ParseArgs(argc, argv, &cli, &error)) {
    std::cerr << "argument error: " << error << "\n";
    PrintUsage();
    return 2;
  }

  bool ok = false;
  if (cli.mode == "benchmark") {
    ok = RunBenchmarkMode(cli, &error);
  } else {
    ok = RunStabilityMode(cli, &error);
  }

  if (!ok) {
    std::cerr << "perf harness failed: " << error << "\n";
    return 1;
  }
  return 0;
}
