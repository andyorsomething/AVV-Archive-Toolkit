/**
 * @file path_utils.h
 * @brief Shared virtual-path normalization and canonicalization helpers.
 */
#pragma once

#include "vfs_types.h"
#include <string>
#include <string_view>
#include <vector>

namespace vfs {

enum class PathCasePolicy : uint8_t {
  ArchiveExact,
  HostNative,
  ForceSensitive,
  ForceInsensitive
};

[[nodiscard]] bool path_policy_is_case_sensitive(PathCasePolicy policy) noexcept;

[[nodiscard]] Result<std::string>
normalize_virtual_path(std::string_view raw_path, bool require_rooted);

[[nodiscard]] Result<std::string>
normalize_mount_point(std::string_view raw_mount_point);

[[nodiscard]] Result<std::string>
normalize_source_relative_path(std::string_view raw_path);

[[nodiscard]] Result<std::string>
join_mount_point_and_relative_path(std::string_view mount_point,
                                   std::string_view relative_path);

[[nodiscard]] std::vector<std::string>
enumerate_parent_directories(std::string_view rooted_virtual_path);

[[nodiscard]] std::string canonicalize_virtual_path(std::string_view rooted_path,
                                                    PathCasePolicy policy);

[[nodiscard]] std::string ascii_lower_copy(std::string_view input);

} // namespace vfs
