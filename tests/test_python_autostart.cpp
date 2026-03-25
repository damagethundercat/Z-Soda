#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include "inference/PythonAutostart.h"
#include "inference/PythonServiceAutostart.h"

namespace {

struct ScopedTempDir {
  std::filesystem::path path;

  ScopedTempDir() {
    const auto base = std::filesystem::temp_directory_path();
    static int counter = 0;
    path = base / ("zsoda_python_service_test_" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

bool FailIfCalled(std::string_view,
                  int,
                  std::string*,
                  std::string* error) {
  if (error != nullptr) {
    *error = "unexpected health-check call";
  }
  return false;
}

void TestProbeUnavailableForEmptyPath() {
  std::string output = "stale";
  const auto capability = zsoda::inference::ProbePythonTorchCapability({}, &output);
  assert(capability == zsoda::inference::PythonTorchCapability::kUnavailable);
  assert(output.empty());
}

void TestExplicitOverrideSelectionRetainsPath() {
  zsoda::inference::RuntimeOptions options;
  options.remote_service_python = "C:/definitely/not/a/real/python.exe";

  const auto selection = zsoda::inference::ResolvePythonAutostartSelection(options);
  assert(selection.used_explicit_override);
  assert(selection.python_path.string() == options.remote_service_python);
  assert(selection.capability == zsoda::inference::PythonTorchCapability::kUnavailable);
}

void TestExplicitOverrideCommandResolution() {
  zsoda::inference::RuntimeOptions options;
  options.remote_service_python = "/tmp/not-real-python";

  const auto command = zsoda::inference::ResolvePythonCommandForAutostart(options);
  assert(command == options.remote_service_python);
}

void TestServiceLaunchFailsWhenScriptIsMissing() {
  zsoda::inference::RuntimeOptions options;
  options.remote_service_python = "python";

  zsoda::inference::PythonServiceLaunchResult launch_result;
  std::string error;
  const bool started = zsoda::inference::StartDetachedPythonService(
      options, "127.0.0.1", 0, &FailIfCalled, nullptr, &launch_result, &error);
  assert(!started);
  assert(error == "remote service script was not found");
  assert(launch_result.python_command.empty());
}

void TestServiceLaunchFailsForUnavailableExplicitPython() {
  ScopedTempDir temp_dir;
  const auto script_path = temp_dir.path / "distill_any_depth_remote_service.py";
  {
    std::ofstream stream(script_path, std::ios::binary);
    stream << "print('not used')\n";
  }

  zsoda::inference::RuntimeOptions options;
  options.remote_service_python = (temp_dir.path / "missing-python.exe").string();
  options.remote_service_script_path = script_path.string();

  zsoda::inference::PythonServiceLaunchResult launch_result;
  std::string error;
  const bool started = zsoda::inference::StartDetachedPythonService(
      options, "127.0.0.1", 0, &FailIfCalled, nullptr, &launch_result, &error);
  assert(!started);
  assert(error.find("remote inference python runtime is unavailable") != std::string::npos);
  assert(error.find(options.remote_service_python) != std::string::npos);
  assert(launch_result.python_command.empty());
}

}  // namespace

int main() {
  TestProbeUnavailableForEmptyPath();
  TestExplicitOverrideSelectionRetainsPath();
  TestExplicitOverrideCommandResolution();
  TestServiceLaunchFailsWhenScriptIsMissing();
  TestServiceLaunchFailsForUnavailableExplicitPython();
  return 0;
}
