#include "inference/ModelCatalog.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
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

bool ParseAuxiliaryAssets(std::string_view input,
                          int line_number,
                          std::vector<ModelAssetSpec>* out_assets,
                          std::string* error) {
  if (out_assets == nullptr) {
    if (error) {
      *error = "internal error: auxiliary asset output pointer is null";
    }
    return false;
  }
  out_assets->clear();

  const std::string trimmed = TrimCopy(input);
  if (trimmed.empty()) {
    return true;
  }

  const std::string_view text(trimmed);
  std::size_t start = 0;
  int entry_index = 0;
  while (start <= text.size()) {
    const std::size_t delimiter = text.find(';', start);
    const std::size_t length =
        (delimiter == std::string_view::npos) ? (text.size() - start) : (delimiter - start);
    const std::string token = TrimCopy(text.substr(start, length));
    if (!token.empty()) {
      ++entry_index;
      const std::size_t separator = token.find("::");
      if (separator == std::string::npos) {
        if (error) {
          std::ostringstream oss;
          oss << "invalid auxiliary_assets entry at line " << line_number << " (entry "
              << entry_index << "): expected '<relative_path>::<download_url>'";
          *error = oss.str();
        }
        return false;
      }

      ModelAssetSpec asset;
      asset.relative_path = TrimCopy(std::string_view(token).substr(0, separator));
      asset.download_url = TrimCopy(std::string_view(token).substr(separator + 2));
      if (asset.relative_path.empty() || asset.download_url.empty()) {
        if (error) {
          std::ostringstream oss;
          oss << "invalid auxiliary_assets entry at line " << line_number << " (entry "
              << entry_index << "): relative_path/download_url cannot be empty";
          *error = oss.str();
        }
        return false;
      }
      out_assets->push_back(std::move(asset));
    }

    if (delimiter == std::string_view::npos) {
      break;
    }
    start = delimiter + 1;
  }
  return true;
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
  if (spec.download_url.empty()) {
    if (error) {
      *error = "model download_url cannot be empty: " + spec.id;
    }
    return false;
  }

  std::unordered_set<std::string> unique_asset_paths;
  unique_asset_paths.insert(spec.relative_path);
  for (const auto& asset : spec.auxiliary_assets) {
    if (asset.relative_path.empty() || asset.download_url.empty()) {
      if (error) {
        *error = "model auxiliary asset is incomplete: " + spec.id;
      }
      return false;
    }
    if (!unique_asset_paths.insert(asset.relative_path).second) {
      if (error) {
        *error = "model auxiliary asset relative_path is duplicated: " + asset.relative_path;
      }
      return false;
    }
  }
  return true;
}

}  // namespace

ModelCatalog::ModelCatalog() {
  models_.push_back({
      "distill-any-depth",
      "DistillAnyDepth Small",
      "distill-any-depth/distill_any_depth_small.onnx",
      "remote://distill-any-depth-small",
      {},
      false,
  });
  models_.push_back({
      "distill-any-depth-base",
      "DistillAnyDepth Base",
      "distill-any-depth/distill_any_depth_base.onnx",
      "remote://distill-any-depth-base",
      {},
      true,
  });
  models_.push_back({
      "distill-any-depth-large",
      "DistillAnyDepth Large",
      "distill-any-depth/distill_any_depth_large.onnx",
      "remote://distill-any-depth-large",
      {},
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

std::vector<ResolvedModelAsset> ModelCatalog::ResolveModelAssets(
    const std::string& model_root,
    const std::string& model_id) const {
  std::vector<ResolvedModelAsset> assets;
  const auto* model = FindById(model_id);
  if (model == nullptr) {
    return assets;
  }

  assets.reserve(1U + model->auxiliary_assets.size());
  const auto root = std::filesystem::path(model_root);
  assets.push_back({
      model->relative_path,
      model->download_url,
      (root / model->relative_path).string(),
  });
  for (const auto& asset : model->auxiliary_assets) {
    assets.push_back({
        asset.relative_path,
        asset.download_url,
        (root / asset.relative_path).string(),
    });
  }
  return assets;
}

bool ModelCatalog::RegisterModel(ModelSpec spec, std::string* error, bool* updated) {
  spec.id = TrimCopy(spec.id);
  spec.display_name = TrimCopy(spec.display_name);
  spec.relative_path = TrimCopy(spec.relative_path);
  spec.download_url = TrimCopy(spec.download_url);
  for (auto& asset : spec.auxiliary_assets) {
    asset.relative_path = TrimCopy(asset.relative_path);
    asset.download_url = TrimCopy(asset.download_url);
  }
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
    if (columns.size() < 4 || columns.size() > 6) {
      if (error) {
        std::ostringstream oss;
        oss << "invalid manifest line " << line_number
            << ": expected 4 to 6 pipe-separated columns";
        *error = oss.str();
      }
      return false;
    }

    ModelSpec spec;
    spec.id = columns[0];
    spec.display_name = columns[1];
    spec.relative_path = columns[2];
    spec.download_url = columns[3];

    if (columns.size() >= 5 && !columns[4].empty()) {
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
    if (columns.size() == 6 && !columns[5].empty()) {
      std::string parse_error;
      if (!ParseAuxiliaryAssets(columns[5], line_number, &spec.auxiliary_assets, &parse_error)) {
        if (error) {
          *error = parse_error;
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
