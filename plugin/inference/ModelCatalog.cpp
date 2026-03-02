#include "inference/ModelCatalog.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace zsoda::inference {
namespace {

constexpr const char* kDefaultManifestFilename = "models.manifest";

std::string TrimCopy(std::string_view input) {
  std::size_t begin = 0;
  while (begin < input.size() &&
         std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
    ++begin;
  }

  std::size_t end = input.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    --end;
  }
  return std::string(input.substr(begin, end - begin));
}

std::string StripInlineComment(std::string_view input) {
  const std::size_t comment_pos = input.find('#');
  if (comment_pos == std::string_view::npos) {
    return std::string(input);
  }
  return std::string(input.substr(0, comment_pos));
}

bool ParseBool(std::string_view input, bool* out_value) {
  std::string normalized;
  normalized.reserve(input.size());
  for (const char ch : input) {
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  if (normalized == "1" || normalized == "true" || normalized == "yes" ||
      normalized == "on") {
    *out_value = true;
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "no" ||
      normalized == "off") {
    *out_value = false;
    return true;
  }
  return false;
}

std::vector<std::string> SplitManifestColumns(std::string_view line) {
  std::vector<std::string> columns;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t delimiter = line.find('|', start);
    if (delimiter == std::string_view::npos) {
      columns.push_back(TrimCopy(line.substr(start)));
      break;
    }
    columns.push_back(TrimCopy(line.substr(start, delimiter - start)));
    start = delimiter + 1;
  }
  return columns;
}

bool ValidateSpec(const ModelSpec& spec, std::string* error) {
  if (spec.id.empty()) {
    if (error) {
      *error = "model id cannot be empty";
    }
    return false;
  }
  if (spec.display_name.empty()) {
    if (error) {
      *error = "model display_name cannot be empty: " + spec.id;
    }
    return false;
  }
  if (spec.relative_path.empty()) {
    if (error) {
      *error = "model relative_path cannot be empty: " + spec.id;
    }
    return false;
  }
  return true;
}

}  // namespace

ModelCatalog::ModelCatalog() {
  models_.push_back({
      "depth-anything-v3-small",
      "Depth Anything v3 Small",
      "depth-anything-v3/depth_anything_v3_small.onnx",
      "https://huggingface.co/depth-anything/Depth-Anything-V3/resolve/main/depth_anything_v3_small.onnx",
      true,
  });
  models_.push_back({
      "depth-anything-v3-base",
      "Depth Anything v3 Base",
      "depth-anything-v3/depth_anything_v3_base.onnx",
      "https://huggingface.co/depth-anything/Depth-Anything-V3/resolve/main/depth_anything_v3_base.onnx",
      false,
  });
  models_.push_back({
      "depth-anything-v3-large",
      "Depth Anything v3 Large",
      "depth-anything-v3/depth_anything_v3_large.onnx",
      "https://huggingface.co/depth-anything/Depth-Anything-V3/resolve/main/depth_anything_v3_large.onnx",
      false,
  });
  models_.push_back({
      "midas-dpt-large",
      "MiDaS DPT Large",
      "midas/dpt_large_384.onnx",
      "https://github.com/isl-org/MiDaS/releases/download/v3_1/dpt_large_384.onnx",
      false,
  });
}

const char* ModelCatalog::DefaultManifestFilename() {
  return kDefaultManifestFilename;
}

const std::vector<ModelSpec>& ModelCatalog::models() const {
  return models_;
}

const ModelSpec* ModelCatalog::FindById(std::string_view model_id) const {
  for (const auto& model : models_) {
    if (model.id == model_id) {
      return &model;
    }
  }
  return nullptr;
}

std::string ModelCatalog::DefaultModelId() const {
  for (const auto& model : models_) {
    if (model.preferred_default) {
      return model.id;
    }
  }
  return models_.empty() ? "" : models_.front().id;
}

std::string ModelCatalog::ResolveModelPath(const std::string& model_root,
                                           const std::string& model_id) const {
  const auto* model = FindById(model_id);
  if (model == nullptr) {
    return {};
  }
  return (std::filesystem::path(model_root) / model->relative_path).string();
}

bool ModelCatalog::RegisterModel(ModelSpec spec, std::string* error, bool* updated) {
  spec.id = TrimCopy(spec.id);
  spec.display_name = TrimCopy(spec.display_name);
  spec.relative_path = TrimCopy(spec.relative_path);
  spec.download_url = TrimCopy(spec.download_url);
  if (!ValidateSpec(spec, error)) {
    return false;
  }

  if (spec.preferred_default) {
    for (auto& model : models_) {
      model.preferred_default = false;
    }
  }

  for (auto& model : models_) {
    if (model.id == spec.id) {
      model = std::move(spec);
      if (updated) {
        *updated = true;
      }
      return true;
    }
  }

  models_.push_back(std::move(spec));
  if (updated) {
    *updated = false;
  }
  return true;
}

bool ModelCatalog::LoadManifestFile(const std::string& manifest_path,
                                    std::string* error,
                                    ManifestLoadResult* result) {
  if (manifest_path.empty()) {
    if (error) {
      *error = "manifest path cannot be empty";
    }
    return false;
  }

  std::ifstream stream(manifest_path);
  if (!stream.is_open()) {
    if (error) {
      *error = "manifest file not found: " + manifest_path;
    }
    return false;
  }

  ManifestLoadResult local_result;
  std::vector<ModelSpec> parsed_specs;
  std::string line;
  int line_number = 0;
  while (std::getline(stream, line)) {
    ++line_number;
    std::string stripped = TrimCopy(StripInlineComment(line));
    if (stripped.empty()) {
      continue;
    }

    const auto columns = SplitManifestColumns(stripped);
    if (columns.size() < 4 || columns.size() > 5) {
      if (error) {
        std::ostringstream oss;
        oss << "invalid manifest line " << line_number
            << ": expected 4 or 5 pipe-separated columns";
        *error = oss.str();
      }
      return false;
    }

    ModelSpec spec;
    spec.id = columns[0];
    spec.display_name = columns[1];
    spec.relative_path = columns[2];
    spec.download_url = columns[3];

    if (columns.size() == 5 && !columns[4].empty()) {
      if (!ParseBool(columns[4], &spec.preferred_default)) {
        if (error) {
          std::ostringstream oss;
          oss << "invalid preferred_default value at line " << line_number
              << ": " << columns[4];
          *error = oss.str();
        }
        return false;
      }
    }

    std::string validation_error;
    if (!ValidateSpec(spec, &validation_error)) {
      if (error) {
        std::ostringstream oss;
        oss << "manifest line " << line_number << " rejected: " << validation_error;
        *error = oss.str();
      }
      return false;
    }
    parsed_specs.push_back(std::move(spec));
  }

  for (auto& spec : parsed_specs) {
    bool was_updated = false;
    std::string register_error;
    if (!RegisterModel(std::move(spec), &register_error, &was_updated)) {
      if (error) {
        *error = register_error;
      }
      return false;
    }
    if (was_updated) {
      ++local_result.updated;
    } else {
      ++local_result.added;
    }
  }

  if (result) {
    *result = local_result;
  }
  if (error) {
    error->clear();
  }
  return true;
}

bool ModelCatalog::LoadManifestFromRoot(const std::string& model_root,
                                        std::string* error,
                                        ManifestLoadResult* result) {
  if (model_root.empty()) {
    if (error) {
      *error = "model root cannot be empty";
    }
    return false;
  }

  const std::filesystem::path manifest_path =
      std::filesystem::path(model_root) / DefaultManifestFilename();
  if (!std::filesystem::exists(manifest_path)) {
    if (result) {
      *result = {};
    }
    if (error) {
      error->clear();
    }
    return true;
  }
  return LoadManifestFile(manifest_path.string(), error, result);
}

}  // namespace zsoda::inference
