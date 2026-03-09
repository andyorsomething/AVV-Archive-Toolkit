#include "path_utils.h"
#include <filesystem>

namespace vfs {

namespace {

Result<std::string> normalize_impl(std::string_view raw_path, bool require_rooted,
                                   bool allow_root_result) {
  std::string path(raw_path);
  for (char &ch : path) {
    if (ch == '\\')
      ch = '/';
  }

  if (path.empty())
    path = require_rooted ? "/" : ".";

  std::filesystem::path fs_path(path);
  const std::filesystem::path normalized = fs_path.lexically_normal();
  std::string out = normalized.generic_string();

  if (require_rooted) {
    if (out.empty())
      out = "/";
    if (out.front() != '/')
      out.insert(out.begin(), '/');
  } else {
    if (normalized.has_root_name() || normalized.has_root_directory() ||
        normalized.is_absolute()) {
      return unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
    }
    if (out == ".")
      out.clear();
  }

  if (!allow_root_result && (out.empty() || out == "/"))
    return unexpected<ErrorCode>(ErrorCode::InvalidMountPoint);

  std::filesystem::path final_path(out);
  for (const auto &part : final_path) {
    const std::string piece = part.generic_string();
    if (piece == "." || piece == "..")
      return unexpected<ErrorCode>(require_rooted ? ErrorCode::FileNotFound
                                                  : ErrorCode::CorruptedArchive);
  }

  return out;
}

} // namespace

bool path_policy_is_case_sensitive(PathCasePolicy policy) noexcept {
  switch (policy) {
  case PathCasePolicy::ArchiveExact:
  case PathCasePolicy::ForceSensitive:
    return true;
  case PathCasePolicy::ForceInsensitive:
    return false;
  case PathCasePolicy::HostNative:
#ifdef _WIN32
    return false;
#else
    return true;
#endif
  }
  return true;
}

Result<std::string> normalize_virtual_path(std::string_view raw_path,
                                           bool require_rooted) {
  return normalize_impl(raw_path, require_rooted, true);
}

Result<std::string> normalize_mount_point(std::string_view raw_mount_point) {
  return normalize_impl(raw_mount_point, true, true);
}

Result<std::string> normalize_source_relative_path(std::string_view raw_path) {
  auto res = normalize_impl(raw_path, false, true);
  if (!res)
    return unexpected<ErrorCode>(res.error());
  if (res.value().empty())
    return unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
  return res;
}

Result<std::string>
join_mount_point_and_relative_path(std::string_view mount_point,
                                   std::string_view relative_path) {
  auto mount_res = normalize_mount_point(mount_point);
  if (!mount_res)
    return unexpected<ErrorCode>(mount_res.error());
  auto rel_res = normalize_source_relative_path(relative_path);
  if (!rel_res)
    return unexpected<ErrorCode>(rel_res.error());

  if (mount_res.value() == "/")
    return normalize_virtual_path("/" + rel_res.value(), true);
  return normalize_virtual_path(mount_res.value() + "/" + rel_res.value(), true);
}

std::vector<std::string>
enumerate_parent_directories(std::string_view rooted_virtual_path) {
  std::vector<std::string> result;
  std::string cur(rooted_virtual_path);
  if (cur.empty() || cur == "/")
    return result;

  std::size_t pos = cur.find_last_of('/');
  while (pos != std::string::npos && pos > 0) {
    result.push_back(cur.substr(0, pos));
    pos = cur.find_last_of('/', pos - 1);
  }
  result.push_back("/");
  return result;
}

std::string ascii_lower_copy(std::string_view input) {
  std::string out(input);
  for (char &ch : out) {
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch - 'A' + 'a');
  }
  return out;
}

std::string canonicalize_virtual_path(std::string_view rooted_path,
                                      PathCasePolicy policy) {
  return path_policy_is_case_sensitive(policy) ? std::string(rooted_path)
                                               : ascii_lower_copy(rooted_path);
}

} // namespace vfs
