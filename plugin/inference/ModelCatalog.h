#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace zsoda::inference {

struct ModelAssetSpec {
  std::string relative_path;
  std::string download_url;
};

struct ResolvedModelAsset {
  std::string relative_path;
  std::string download_url;
  std::string absolute_path;
};

struct ModelSpec {
  std::string id;
  std::string display_name;
  std::string relative_path;
  std::string download_url;
  std::vector<ModelAssetSpec> auxiliary_assets;
  bool preferred_default = false;
};

struct ManifestLoadResult {
  std::size_t added = 0;
  std::size_t updated = 0;
};

class ModelCatalog {
 public:
  ModelCatalog();

  [[nodiscard]] static const char* DefaultManifestFilename();
  [[nodiscard]] const std::vector<ModelSpec>& models() const;
  [[nodiscard]] const ModelSpec* FindById(std::string_view model_id) const;
  [[nodiscard]] std::string DefaultModelId() const;
  [[nodiscard]] std::string ResolveModelPath(const std::string& model_root,
                                             const std::string& model_id) const;
  [[nodiscard]] std::vector<ResolvedModelAsset> ResolveModelAssets(
      const std::string& model_root,
      const std::string& model_id) const;
  bool RegisterModel(ModelSpec spec, std::string* error, bool* updated = nullptr);
  bool LoadManifestFile(const std::string& manifest_path,
                        std::string* error,
                        ManifestLoadResult* result = nullptr);
  bool LoadManifestFromRoot(const std::string& model_root,
                            std::string* error,
                            ManifestLoadResult* result = nullptr);

 private:
  std::vector<ModelSpec> models_;
};

}  // namespace zsoda::inference
