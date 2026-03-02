#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace zsoda::inference {

struct ModelSpec {
  std::string id;
  std::string display_name;
  std::string relative_path;
  std::string download_url;
  bool preferred_default = false;
};

class ModelCatalog {
 public:
  ModelCatalog();

  [[nodiscard]] const std::vector<ModelSpec>& models() const;
  [[nodiscard]] const ModelSpec* FindById(std::string_view model_id) const;
  [[nodiscard]] std::string DefaultModelId() const;
  [[nodiscard]] std::string ResolveModelPath(const std::string& model_root,
                                             const std::string& model_id) const;

 private:
  std::vector<ModelSpec> models_;
};

}  // namespace zsoda::inference
