/**
 * @file mounted_file_system.h
 * @brief Read-only mounted namespace over archives and host directories.
 */
#pragma once

#include "mounted_source.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace vfs {

class MountedFileSystem {
public:
  enum class MountSourceKind : uint8_t {
    Archive,
    HostDirectory
  };

  struct MountOptions {
    std::string mount_point = "/";
    int priority = 0;
    PathCasePolicy case_policy = PathCasePolicy::ArchiveExact;
    std::string password;
  };

  struct MountedEntry {
    std::string virtual_path;
    std::string display_name;
    std::filesystem::path source_root;
    std::string source_path;
    MountSourceKind source_kind = MountSourceKind::Archive;
    uint32_t mount_id = 0;
    uint16_t flags = 0;
    uint16_t chunk_index = 0;
    uint64_t size = 0;
    uint64_t compressed_size = 0;
    bool is_directory = false;
    int priority = 0;
  };

  MountedFileSystem();
  ~MountedFileSystem();
  MountedFileSystem(const MountedFileSystem &) = delete;
  MountedFileSystem &operator=(const MountedFileSystem &) = delete;

  [[nodiscard]] Result<uint32_t>
  mount_archive(const std::filesystem::path &archive_path,
                const MountOptions &options = {});
  [[nodiscard]] Result<uint32_t>
  mount_host_directory(const std::filesystem::path &host_dir,
                       const MountOptions &options = {});
  [[nodiscard]] Result<void> unmount(uint32_t mount_id);
  void unmount_all();

  [[nodiscard]] bool is_mounted() const;
  [[nodiscard]] Result<bool> exists(const std::string &virtual_path) const;
  [[nodiscard]] Result<MountedEntry> stat(const std::string &virtual_path) const;
  [[nodiscard]] Result<std::vector<MountedEntry>> list_all_files() const;
  [[nodiscard]] Result<std::vector<MountedEntry>>
  list_directory(const std::string &virtual_dir) const;
  [[nodiscard]] Result<std::vector<MountedEntry>>
  list_overlays(const std::string &virtual_path) const;

  [[nodiscard]] Result<std::vector<char>>
  read_file_data(const std::string &virtual_path);
  [[nodiscard]] Result<void>
  extract_file(const std::string &virtual_path,
               const std::filesystem::path &output_path);

  void read_file_async(const std::string &virtual_path,
                       std::function<void(Result<std::vector<char>>)> on_complete);
  void pump_callbacks();

public:
  struct MountRecord;
  struct VisibleEntry;
  struct NamespaceSnapshot;

private:
  [[nodiscard]] Result<uint32_t>
  mount_impl(MountSourceKind source_kind, const std::filesystem::path &source_root,
             MountOptions options);
  [[nodiscard]] Result<void> rebuild_snapshot_locked();
  [[nodiscard]] Result<std::string>
  normalize_lookup_path(const std::string &virtual_path) const;
  [[nodiscard]] Result<MountedEntry>
  to_mounted_entry(const VisibleEntry &entry) const;
  [[nodiscard]] std::shared_ptr<IMountedSource>
  source_for_mount_id(uint32_t mount_id) const;

  mutable std::mutex rebuild_mutex_;
  std::unordered_map<uint32_t, std::shared_ptr<MountRecord>> mounts_;
  std::atomic<std::shared_ptr<const NamespaceSnapshot>> snapshot_;
  std::atomic<uint32_t> next_mount_id_{1};
  std::atomic<uint64_t> next_mount_order_{1};
};

} // namespace vfs
