#include "inference/EmbeddedPayload.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace zsoda::inference {
namespace {

constexpr std::string_view kPayloadHeaderMagic = "ZSODA_PAYLOAD_V1";
constexpr std::string_view kPayloadFooterMagic = "ZSODA_FOOTER_V1";
constexpr const char* kPayloadReadyMarker = ".zsoda_payload_ready";
constexpr const char* kPayloadCacheRootEnv = "ZSODA_PAYLOAD_CACHE_ROOT";
constexpr std::size_t kPayloadMagicFieldSize = 16U;
constexpr std::size_t kPayloadDigestSize = 32U;

struct EmbeddedPayloadFooter {
  std::uint64_t payload_size = 0;
  std::array<std::uint8_t, kPayloadDigestSize> payload_sha256 = {};
};

bool ReadExact(std::ifstream* stream, char* buffer, std::size_t size) {
  if (stream == nullptr || buffer == nullptr) {
    return false;
  }
  stream->read(buffer, static_cast<std::streamsize>(size));
  return stream->good() || stream->gcount() == static_cast<std::streamsize>(size);
}

bool ReadU32LE(std::ifstream* stream, std::uint32_t* value) {
  std::array<std::uint8_t, 4> buffer = {};
  if (!ReadExact(stream, reinterpret_cast<char*>(buffer.data()), buffer.size())) {
    return false;
  }
  *value = static_cast<std::uint32_t>(buffer[0]) |
           (static_cast<std::uint32_t>(buffer[1]) << 8U) |
           (static_cast<std::uint32_t>(buffer[2]) << 16U) |
           (static_cast<std::uint32_t>(buffer[3]) << 24U);
  return true;
}

bool ReadU64LE(std::ifstream* stream, std::uint64_t* value) {
  std::array<std::uint8_t, 8> buffer = {};
  if (!ReadExact(stream, reinterpret_cast<char*>(buffer.data()), buffer.size())) {
    return false;
  }
  *value = static_cast<std::uint64_t>(buffer[0]) |
           (static_cast<std::uint64_t>(buffer[1]) << 8U) |
           (static_cast<std::uint64_t>(buffer[2]) << 16U) |
           (static_cast<std::uint64_t>(buffer[3]) << 24U) |
           (static_cast<std::uint64_t>(buffer[4]) << 32U) |
           (static_cast<std::uint64_t>(buffer[5]) << 40U) |
           (static_cast<std::uint64_t>(buffer[6]) << 48U) |
           (static_cast<std::uint64_t>(buffer[7]) << 56U);
  return true;
}

std::string HexEncode(const std::array<std::uint8_t, 32>& bytes) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(bytes.size() * 2U);
  for (const std::uint8_t value : bytes) {
    encoded.push_back(kHexDigits[(value >> 4U) & 0x0FU]);
    encoded.push_back(kHexDigits[value & 0x0FU]);
  }
  return encoded;
}

void AppendPayloadTrace(const char* stage, std::string_view detail = {}) {
#if defined(_WIN32)
  std::error_code ec;
  const auto temp_root = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return;
  }

  std::ofstream stream(temp_root / "ZSoda_AE_Runtime.log", std::ios::binary | std::ios::app);
  if (!stream.is_open()) {
    return;
  }

  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_tm = {};
  if (localtime_s(&local_tm, &now_time) != 0) {
    return;
  }

  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  stream << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setw(3) << std::setfill('0') << millis.count()
         << " | PayloadTrace | stage="
         << (stage != nullptr ? stage : "<null>")
         << ", detail="
         << (!detail.empty() ? detail : "<none>")
         << "\r\n";
#else
  (void)stage;
  (void)detail;
#endif
}

std::filesystem::path ResolveLegacyPayloadCacheRoot() {
#if defined(_WIN32)
  const char* local_appdata = std::getenv("LOCALAPPDATA");
  if (local_appdata != nullptr && local_appdata[0] != '\0') {
    return std::filesystem::path(local_appdata) / "ZSoda" / "PayloadCache";
  }
#endif
  return {};
}

std::filesystem::path ResolveSecondaryWindowsPayloadCacheRoot() {
#if defined(_WIN32)
  const char* user_profile = std::getenv("USERPROFILE");
  if (user_profile != nullptr && user_profile[0] != '\0') {
    return std::filesystem::path(user_profile) / "ZS";
  }
#endif
  return {};
}

std::filesystem::path ResolvePayloadCacheRoot() {
  const char* override_root = std::getenv(kPayloadCacheRootEnv);
  if (override_root != nullptr && override_root[0] != '\0') {
    return std::filesystem::path(override_root);
  }

#if defined(_WIN32)
  const char* local_appdata = std::getenv("LOCALAPPDATA");
  if (local_appdata != nullptr && local_appdata[0] != '\0') {
    // Keep the Windows payload cache root intentionally short. After Effects does not
    // advertise long-path awareness, and the bundled Python runtime contains deep
    // package trees that can otherwise push extraction paths past MAX_PATH.
    return std::filesystem::path(local_appdata) / "ZS";
  }
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  if (home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / "Library" / "Application Support" / "ZSoda" /
           "PayloadCache";
  }
#else
  const char* xdg = std::getenv("XDG_DATA_HOME");
  if (xdg != nullptr && xdg[0] != '\0') {
    return std::filesystem::path(xdg) / "ZSoda" / "PayloadCache";
  }
  const char* home = std::getenv("HOME");
  if (home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / ".local" / "share" / "ZSoda" / "PayloadCache";
  }
#endif

  std::error_code ec;
  const auto temp_root = std::filesystem::temp_directory_path(ec);
  if (!ec) {
    return temp_root / "ZSoda" / "PayloadCache";
  }
  return std::filesystem::path("ZSodaPayloadCache");
}

bool PathEscapesRoot(const std::filesystem::path& relative_path) {
  if (relative_path.empty() || relative_path.is_absolute()) {
    return true;
  }
  for (const auto& part : relative_path) {
    if (part == "..") {
      return true;
    }
  }
  return false;
}

template <std::size_t N>
bool MatchesPaddedMagic(const std::array<char, N>& field, std::string_view expected) {
  if (expected.size() > N) {
    return false;
  }
  const std::string_view prefix(field.data(), expected.size());
  if (prefix != expected) {
    return false;
  }
  for (std::size_t index = expected.size(); index < N; ++index) {
    if (field[index] != '\0') {
      return false;
    }
  }
  return true;
}

bool ReadPayloadFooter(const std::filesystem::path& module_path,
                       EmbeddedPayloadFooter* footer,
                       std::uint64_t* payload_offset,
                       std::string* error) {
  if (footer == nullptr || payload_offset == nullptr) {
    if (error != nullptr) {
      *error = "internal error: payload footer output is null";
    }
    return false;
  }

  std::ifstream stream(module_path, std::ios::binary);
  if (!stream.is_open()) {
    if (error != nullptr) {
      *error = "failed to open module for embedded payload scan: " + module_path.string();
    }
    return false;
  }

  stream.seekg(0, std::ios::end);
  const std::streamoff file_size = stream.tellg();
  constexpr std::streamoff footer_size =
      static_cast<std::streamoff>(kPayloadMagicFieldSize + sizeof(std::uint64_t) + kPayloadDigestSize);
  if (file_size < footer_size) {
    return false;
  }

  stream.seekg(file_size - footer_size, std::ios::beg);
  std::array<char, kPayloadMagicFieldSize> footer_magic = {};
  if (!ReadExact(&stream, footer_magic.data(), footer_magic.size())) {
    if (error != nullptr) {
      *error = "failed to read embedded payload footer";
    }
    return false;
  }
  if (!MatchesPaddedMagic(footer_magic, kPayloadFooterMagic)) {
    return false;
  }
  if (!ReadU64LE(&stream, &footer->payload_size)) {
    if (error != nullptr) {
      *error = "failed to read embedded payload footer size";
    }
    return false;
  }
  if (!ReadExact(&stream,
                 reinterpret_cast<char*>(footer->payload_sha256.data()),
                 footer->payload_sha256.size())) {
    if (error != nullptr) {
      *error = "failed to read embedded payload footer digest";
    }
    return false;
  }

  if (footer->payload_size == 0U ||
      static_cast<std::uint64_t>(file_size) < footer->payload_size + static_cast<std::uint64_t>(footer_size)) {
    if (error != nullptr) {
      *error = "embedded payload footer is inconsistent";
    }
    return false;
  }

  *payload_offset =
      static_cast<std::uint64_t>(file_size) - footer->payload_size - static_cast<std::uint64_t>(footer_size);
  return true;
}

bool ExtractPayloadEntries(const std::filesystem::path& module_path,
                           std::uint64_t payload_offset,
                           std::uint64_t payload_size,
                           const std::filesystem::path& output_root,
                           std::string* error) {
  std::ifstream stream(module_path, std::ios::binary);
  if (!stream.is_open()) {
    if (error != nullptr) {
      *error = "failed to reopen module for embedded payload extraction";
    }
    return false;
  }

  stream.seekg(static_cast<std::streamoff>(payload_offset), std::ios::beg);
  std::array<char, kPayloadMagicFieldSize> header_magic = {};
  if (!ReadExact(&stream, header_magic.data(), header_magic.size())) {
    if (error != nullptr) {
      *error = "failed to read embedded payload header";
    }
    return false;
  }
  if (!MatchesPaddedMagic(header_magic, kPayloadHeaderMagic)) {
    if (error != nullptr) {
      *error = "embedded payload header magic mismatch";
    }
    return false;
  }

  std::uint32_t entry_count = 0;
  std::uint32_t reserved = 0;
  if (!ReadU32LE(&stream, &entry_count) || !ReadU32LE(&stream, &reserved)) {
    if (error != nullptr) {
      *error = "failed to read embedded payload header metadata";
    }
    return false;
  }
  (void)reserved;

  std::error_code fs_error;
  std::filesystem::remove_all(output_root, fs_error);
  fs_error.clear();
  std::filesystem::create_directories(output_root, fs_error);
  if (fs_error) {
    if (error != nullptr) {
      *error = "failed to create embedded payload output root: " + output_root.string();
    }
    return false;
  }

  constexpr std::size_t kCopyBufferSize = 1U << 16U;
  std::array<char, kCopyBufferSize> buffer = {};
  for (std::uint32_t entry_index = 0; entry_index < entry_count; ++entry_index) {
    std::uint32_t path_length = 0;
    std::uint64_t file_size = 0;
    if (!ReadU32LE(&stream, &path_length) || !ReadU64LE(&stream, &file_size)) {
      if (error != nullptr) {
        *error = "failed to read embedded payload entry header";
      }
      std::filesystem::remove_all(output_root, fs_error);
      return false;
    }
    if (path_length == 0U || path_length > 4096U) {
      if (error != nullptr) {
        *error = "embedded payload entry path length is invalid";
      }
      std::filesystem::remove_all(output_root, fs_error);
      return false;
    }

    std::string path_utf8(path_length, '\0');
    if (!ReadExact(&stream, path_utf8.data(), path_utf8.size())) {
      if (error != nullptr) {
        *error = "failed to read embedded payload entry path";
      }
      std::filesystem::remove_all(output_root, fs_error);
      return false;
    }

    const std::filesystem::path relative_path(path_utf8);
    if (PathEscapesRoot(relative_path)) {
      if (error != nullptr) {
        *error = "embedded payload entry path is invalid: " + path_utf8;
      }
      std::filesystem::remove_all(output_root, fs_error);
      return false;
    }

    const auto destination = output_root / relative_path;
    std::filesystem::create_directories(destination.parent_path(), fs_error);
    if (fs_error) {
      if (error != nullptr) {
        *error = "failed to create embedded payload parent directory: " +
                 destination.parent_path().string();
      }
      std::filesystem::remove_all(output_root, fs_error);
      return false;
    }

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      if (error != nullptr) {
        *error = "failed to open embedded payload output file: " + destination.string();
      }
      std::filesystem::remove_all(output_root, fs_error);
      return false;
    }

    std::uint64_t remaining = file_size;
    while (remaining > 0U) {
      const std::size_t chunk_size =
          static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
      if (!ReadExact(&stream, buffer.data(), chunk_size)) {
        if (error != nullptr) {
          *error = "failed to read embedded payload entry bytes: " + path_utf8;
        }
        output.close();
        std::filesystem::remove_all(output_root, fs_error);
        return false;
      }
      output.write(buffer.data(), static_cast<std::streamsize>(chunk_size));
      if (!output.good()) {
        if (error != nullptr) {
          *error = "failed to write embedded payload entry bytes: " + destination.string();
        }
        output.close();
        std::filesystem::remove_all(output_root, fs_error);
        return false;
      }
      remaining -= static_cast<std::uint64_t>(chunk_size);
    }
  }

  const auto expected_end =
      static_cast<std::streamoff>(payload_offset + payload_size);
  if (stream.tellg() != expected_end) {
    if (error != nullptr) {
      *error = "embedded payload size mismatch after extraction";
    }
    std::filesystem::remove_all(output_root, fs_error);
    return false;
  }

  std::ofstream marker(output_root / kPayloadReadyMarker, std::ios::binary | std::ios::trunc);
  if (!marker.is_open()) {
    if (error != nullptr) {
      *error = "failed to create embedded payload ready marker";
    }
    std::filesystem::remove_all(output_root, fs_error);
    return false;
  }
  return true;
}

}  // namespace

EmbeddedPayloadInfo EnsureEmbeddedPayloadAvailable(const std::string& module_path,
                                                   std::string* error) {
  EmbeddedPayloadInfo info;
  if (error != nullptr) {
    error->clear();
  }
  if (module_path.empty()) {
    return info;
  }

  EmbeddedPayloadFooter footer;
  std::uint64_t payload_offset = 0;
  std::string footer_error;
  if (!ReadPayloadFooter(std::filesystem::path(module_path), &footer, &payload_offset, &footer_error)) {
    if (!footer_error.empty() && error != nullptr) {
      *error = footer_error;
    }
    if (!footer_error.empty()) {
      AppendPayloadTrace("footer_read_failed", footer_error);
    }
    return info;
  }

  info.has_payload = true;
  info.payload_id = HexEncode(footer.payload_sha256);
  const std::filesystem::path extraction_root = ResolvePayloadCacheRoot() / info.payload_id;
  const std::filesystem::path secondary_cache_root = ResolveSecondaryWindowsPayloadCacheRoot();
  const std::filesystem::path secondary_root =
      secondary_cache_root.empty() ? std::filesystem::path{} : (secondary_cache_root / info.payload_id);
  info.asset_root = extraction_root.string();
  AppendPayloadTrace("payload_detected", info.asset_root);

  std::error_code ec;
  if (std::filesystem::is_regular_file(extraction_root / kPayloadReadyMarker, ec) && !ec) {
    info.extracted = true;
    AppendPayloadTrace("payload_ready_reused", info.asset_root);
    return info;
  }

  if (!secondary_root.empty() && secondary_root != extraction_root &&
      std::filesystem::is_regular_file(secondary_root / kPayloadReadyMarker, ec) && !ec) {
    info.extracted = true;
    info.asset_root = secondary_root.string();
    AppendPayloadTrace("payload_ready_reused_secondary", info.asset_root);
    return info;
  }

  const std::filesystem::path legacy_root = ResolveLegacyPayloadCacheRoot() / info.payload_id;
  if (!legacy_root.empty() && legacy_root != extraction_root &&
      std::filesystem::is_regular_file(legacy_root / kPayloadReadyMarker, ec) && !ec) {
    info.extracted = true;
    info.asset_root = legacy_root.string();
    AppendPayloadTrace("payload_ready_reused_legacy", info.asset_root);
    return info;
  }

  std::string extract_error;
  if (!ExtractPayloadEntries(std::filesystem::path(module_path),
                             payload_offset,
                             footer.payload_size,
                             extraction_root,
                             &extract_error)) {
    if (!secondary_root.empty() && secondary_root != extraction_root) {
      AppendPayloadTrace("payload_extract_retry_secondary", secondary_root.string());
      std::string secondary_error;
      if (ExtractPayloadEntries(std::filesystem::path(module_path),
                                payload_offset,
                                footer.payload_size,
                                secondary_root,
                                &secondary_error)) {
        info.asset_root = secondary_root.string();
        info.extracted = true;
        AppendPayloadTrace("payload_extract_ok_secondary", info.asset_root);
        return info;
      }
      if (error != nullptr) {
        *error = extract_error;
        if (!secondary_error.empty()) {
          error->append(" | secondary_root=");
          error->append(secondary_error);
        }
      }
      if (!secondary_error.empty()) {
        AppendPayloadTrace("payload_extract_secondary_failed", secondary_error);
      }
    } else if (error != nullptr) {
      *error = extract_error;
    }
    if (!extract_error.empty()) {
      AppendPayloadTrace("payload_extract_failed", extract_error);
    }
    info.asset_root.clear();
    info.extracted = false;
    return info;
  }

  info.extracted = true;
  AppendPayloadTrace("payload_extract_ok", info.asset_root);
  return info;
}

}  // namespace zsoda::inference
