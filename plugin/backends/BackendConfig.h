#pragma once

namespace zsoda::backends {

enum class BackendKind {
  kCpu,
  kCuda,
  kDirectML,
  kMetal,
  kCoreML,
};

}  // namespace zsoda::backends
