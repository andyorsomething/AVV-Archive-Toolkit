#include "mounted_file_system.h"
#include <algorithm>
#include <set>

namespace vfs {

struct MountedFileSystem::MountRecord {
  uint32_t mount_id = 0;
  uint64_t mount_order = 0;
  MountSourceKind source_kind = MountSourceKind::Archive;
  std::filesystem::path source_root;
  std::string mount_point;
  int priority = 0;
  PathCasePolicy case_policy = PathCasePolicy::ArchiveExact;
  std::shared_ptr<IMountedSource> source;
};

struct MountedFileSystem::VisibleEntry {
  uint32_t mount_id = 0;
  uint64_t mount_order = 0;
  int priority = 0;
  std::string virtual_path;
  std::string source_path;
  MountSourceKind source_kind = MountSourceKind::Archive;
  std::filesystem::path source_root;
  PathCasePolicy case_policy = PathCasePolicy::ArchiveExact;
  uint16_t flags = 0;
  uint16_t chunk_index = 0;
  uint64_t size = 0;
  uint64_t compressed_size = 0;
  bool is_directory = false;
};

struct MountedFileSystem::NamespaceSnapshot {
  std::unordered_map<std::string, VisibleEntry> visible_entries_by_key;
  std::unordered_map<std::string, std::vector<VisibleEntry>> overlays_by_key;
  std::unordered_map<std::string, std::vector<std::string>>
      directory_children_by_key;
  std::unordered_map<std::string, std::string> canonical_to_display_path;
};

namespace {

bool entry_precedes(const MountedFileSystem::VisibleEntry &lhs,
                    const MountedFileSystem::VisibleEntry &rhs) {
  if (lhs.priority != rhs.priority)
    return lhs.priority > rhs.priority;
  return lhs.mount_order > rhs.mount_order;
}

MountedFileSystem::MountedEntry make_mounted_entry(
    const MountedFileSystem::VisibleEntry &entry) {
  MountedFileSystem::MountedEntry result;
  result.virtual_path = entry.virtual_path;
  result.source_root = entry.source_root;
  result.source_path = entry.source_path;
  result.source_kind = entry.source_kind;
  result.mount_id = entry.mount_id;
  result.flags = entry.flags;
  result.chunk_index = entry.chunk_index;
  result.size = entry.size;
  result.compressed_size = entry.compressed_size;
  result.is_directory = entry.is_directory;
  result.priority = entry.priority;
  if (entry.virtual_path == "/") {
    result.display_name = "/";
  } else {
    const std::size_t slash = entry.virtual_path.find_last_of('/');
    result.display_name = (slash == std::string::npos)
                              ? entry.virtual_path
                              : entry.virtual_path.substr(slash + 1);
  }
  return result;
}

std::string alternate_lookup_key(const std::string &normalized_path) {
  return ascii_lower_copy(normalized_path);
}

} // namespace

MountedFileSystem::MountedFileSystem()
    : snapshot_(std::shared_ptr<const NamespaceSnapshot>(
          std::make_shared<NamespaceSnapshot>())) {}

MountedFileSystem::~MountedFileSystem() { unmount_all(); }

Result<uint32_t>
MountedFileSystem::mount_archive(const std::filesystem::path &archive_path,
                                 const MountOptions &options) {
  return mount_impl(MountSourceKind::Archive, archive_path, options);
}

Result<uint32_t>
MountedFileSystem::mount_host_directory(const std::filesystem::path &host_dir,
                                        const MountOptions &options) {
  return mount_impl(MountSourceKind::HostDirectory, host_dir, options);
}

Result<uint32_t> MountedFileSystem::mount_impl(
    MountSourceKind source_kind, const std::filesystem::path &source_root,
    MountOptions options) {
  auto mount_point_res = normalize_mount_point(options.mount_point);
  if (!mount_point_res)
    return unexpected<ErrorCode>(ErrorCode::InvalidMountPoint);

  if (source_kind == MountSourceKind::Archive &&
      options.case_policy == PathCasePolicy::HostNative) {
    options.case_policy = PathCasePolicy::ArchiveExact;
  }
  if (source_kind == MountSourceKind::HostDirectory &&
      options.case_policy == PathCasePolicy::ArchiveExact) {
    options.case_policy = PathCasePolicy::HostNative;
  }

  std::shared_ptr<IMountedSource> source;
  if (source_kind == MountSourceKind::Archive) {
    source = std::make_shared<ArchiveMountedSource>(source_root, options.password);
  } else {
    source = std::make_shared<HostDirectoryMountedSource>(source_root);
  }

  auto open_res = source->open();
  if (!open_res)
    return unexpected<ErrorCode>(open_res.error());

  auto record = std::make_shared<MountRecord>();
  record->mount_id = next_mount_id_.fetch_add(1);
  record->mount_order = next_mount_order_.fetch_add(1);
  record->source_kind = source_kind;
  record->source_root = source_root;
  record->mount_point = mount_point_res.value();
  record->priority = options.priority;
  record->case_policy = options.case_policy;
  record->source = std::move(source);

  std::lock_guard<std::mutex> lock(rebuild_mutex_);
  mounts_[record->mount_id] = record;
  auto rebuild_res = rebuild_snapshot_locked();
  if (!rebuild_res) {
    mounts_.erase(record->mount_id);
    return unexpected<ErrorCode>(rebuild_res.error());
  }
  return record->mount_id;
}

Result<void> MountedFileSystem::unmount(uint32_t mount_id) {
  std::shared_ptr<MountRecord> removed_mount;
  Result<void> rebuild_res;
  {
    std::lock_guard<std::mutex> lock(rebuild_mutex_);
    const auto it = mounts_.find(mount_id);
    if (it == mounts_.end())
      return unexpected<ErrorCode>(ErrorCode::MountNotFound);
    removed_mount = it->second;
    mounts_.erase(it);
    rebuild_res = rebuild_snapshot_locked();
  }
  removed_mount.reset();
  return rebuild_res;
}

void MountedFileSystem::unmount_all() {
  decltype(mounts_) removed_mounts;
  {
    std::lock_guard<std::mutex> lock(rebuild_mutex_);
    removed_mounts.swap(mounts_);
    snapshot_.store(std::shared_ptr<const NamespaceSnapshot>(
        std::make_shared<NamespaceSnapshot>()));
  }
  removed_mounts.clear();
}

bool MountedFileSystem::is_mounted() const {
  auto snap = snapshot_.load();
  return snap && !snap->visible_entries_by_key.empty();
}

Result<void> MountedFileSystem::rebuild_snapshot_locked() {
  auto snap = std::make_shared<NamespaceSnapshot>();
  std::vector<VisibleEntry> all_entries;

  for (const auto &[mount_id, mount] : mounts_) {
    for (const auto &entry : mount->source->entries()) {
      auto virtual_path_res =
          join_mount_point_and_relative_path(mount->mount_point, entry.source_path);
      if (!virtual_path_res)
        return unexpected<ErrorCode>(virtual_path_res.error());

      VisibleEntry visible;
      visible.mount_id = mount_id;
      visible.mount_order = mount->mount_order;
      visible.priority = mount->priority;
      visible.virtual_path = virtual_path_res.value();
      visible.source_path = entry.source_path;
      visible.source_kind = mount->source_kind;
      visible.source_root = mount->source_root;
      visible.case_policy = mount->case_policy;
      visible.flags = entry.flags;
      visible.chunk_index = entry.chunk_index;
      visible.size = entry.size;
      visible.compressed_size = entry.compressed_size;
      all_entries.push_back(std::move(visible));
    }
  }

  std::sort(all_entries.begin(), all_entries.end(),
            [](const VisibleEntry &lhs, const VisibleEntry &rhs) {
              if (lhs.virtual_path != rhs.virtual_path)
                return lhs.virtual_path < rhs.virtual_path;
              if (lhs.priority != rhs.priority)
                return lhs.priority > rhs.priority;
              return lhs.mount_order > rhs.mount_order;
            });

  for (const auto &entry : all_entries) {
    const std::string key =
        canonicalize_virtual_path(entry.virtual_path, entry.case_policy);
    auto &overlays = snap->overlays_by_key[key];
    overlays.push_back(entry);
    std::sort(overlays.begin(), overlays.end(),
              [](const VisibleEntry &lhs, const VisibleEntry &rhs) {
                return entry_precedes(lhs, rhs);
              });
    snap->visible_entries_by_key[key] = overlays.front();
    snap->canonical_to_display_path[key] = overlays.front().virtual_path;
  }

  std::set<std::string> visible_file_paths;
  std::set<std::string> visible_directory_paths;

  for (const auto &[key, entry] : snap->visible_entries_by_key) {
    visible_file_paths.insert(key);
    const auto parents = enumerate_parent_directories(entry.virtual_path);
    std::string child_key = key;
    for (const auto &parent : parents) {
      const std::string parent_key =
          canonicalize_virtual_path(parent, entry.case_policy);
      if (visible_file_paths.count(parent_key))
        return unexpected<ErrorCode>(ErrorCode::PathConflict);
      visible_directory_paths.insert(parent_key);
      if (snap->canonical_to_display_path.find(parent_key) ==
          snap->canonical_to_display_path.end()) {
        snap->canonical_to_display_path[parent_key] = parent;
      }
      auto &children = snap->directory_children_by_key[parent_key];
      if (std::find(children.begin(), children.end(), child_key) == children.end())
        children.push_back(child_key);
      child_key = parent_key;
    }
  }

  for (const auto &dir_key : visible_directory_paths) {
    if (visible_file_paths.count(dir_key))
      return unexpected<ErrorCode>(ErrorCode::PathConflict);
  }

  for (const auto &[key, entry] : snap->visible_entries_by_key) {
    const std::size_t slash = entry.virtual_path.find_last_of('/');
    const std::string parent_path =
        (slash == 0 || slash == std::string::npos) ? "/" : entry.virtual_path.substr(0, slash);
    const std::string parent_key =
        canonicalize_virtual_path(parent_path, entry.case_policy);
    auto &children = snap->directory_children_by_key[parent_key];
    if (std::find(children.begin(), children.end(), key) == children.end())
      children.push_back(key);
  }

  snapshot_.store(std::shared_ptr<const NamespaceSnapshot>(std::move(snap)));
  return {};
}

Result<std::string>
MountedFileSystem::normalize_lookup_path(const std::string &virtual_path) const {
  return normalize_virtual_path(virtual_path, true);
}

std::shared_ptr<IMountedSource>
MountedFileSystem::source_for_mount_id(uint32_t mount_id) const {
  std::lock_guard<std::mutex> lock(rebuild_mutex_);
  const auto it = mounts_.find(mount_id);
  return (it == mounts_.end()) ? nullptr : it->second->source;
}

Result<MountedFileSystem::MountedEntry>
MountedFileSystem::to_mounted_entry(const VisibleEntry &entry) const {
  return make_mounted_entry(entry);
}

Result<bool> MountedFileSystem::exists(const std::string &virtual_path) const {
  auto path_res = normalize_lookup_path(virtual_path);
  if (!path_res)
    return unexpected<ErrorCode>(path_res.error());
  auto snap = snapshot_.load();
  const std::string exact_key = path_res.value();
  const std::string folded_key = alternate_lookup_key(path_res.value());
  if (snap->visible_entries_by_key.count(exact_key) > 0 ||
      snap->visible_entries_by_key.count(folded_key) > 0 ||
      snap->canonical_to_display_path.count(exact_key) > 0 ||
      snap->canonical_to_display_path.count(folded_key) > 0)
    return true;
  return false;
}

Result<MountedFileSystem::MountedEntry>
MountedFileSystem::stat(const std::string &virtual_path) const {
  auto path_res = normalize_lookup_path(virtual_path);
  if (!path_res)
    return unexpected<ErrorCode>(path_res.error());
  auto snap = snapshot_.load();
  const std::string exact_key = path_res.value();
  const std::string folded_key = alternate_lookup_key(path_res.value());
  auto file_it = snap->visible_entries_by_key.find(exact_key);
  if (file_it == snap->visible_entries_by_key.end())
    file_it = snap->visible_entries_by_key.find(folded_key);
  if (file_it != snap->visible_entries_by_key.end())
    return to_mounted_entry(file_it->second);

  auto dir_it = snap->canonical_to_display_path.find(exact_key);
  if (dir_it == snap->canonical_to_display_path.end())
    dir_it = snap->canonical_to_display_path.find(folded_key);
  if (dir_it != snap->canonical_to_display_path.end()) {
    VisibleEntry dir_entry;
    dir_entry.virtual_path = dir_it->second;
    dir_entry.is_directory = true;
    return to_mounted_entry(dir_entry);
  }
  return unexpected<ErrorCode>(ErrorCode::FileNotFound);
}

Result<std::vector<MountedFileSystem::MountedEntry>>
MountedFileSystem::list_all_files() const {
  auto snap = snapshot_.load();
  std::vector<MountedEntry> result;
  result.reserve(snap->visible_entries_by_key.size());
  for (const auto &[key, entry] : snap->visible_entries_by_key) {
    result.push_back(make_mounted_entry(entry));
  }
  std::sort(result.begin(), result.end(),
            [](const MountedEntry &lhs, const MountedEntry &rhs) {
              return lhs.virtual_path < rhs.virtual_path;
            });
  return result;
}

Result<std::vector<MountedFileSystem::MountedEntry>>
MountedFileSystem::list_directory(const std::string &virtual_dir) const {
  auto path_res = normalize_lookup_path(virtual_dir);
  if (!path_res)
    return unexpected<ErrorCode>(path_res.error());
  auto snap = snapshot_.load();

  std::string dir_key = path_res.value();
  if (snap->canonical_to_display_path.count(dir_key) == 0) {
    dir_key = alternate_lookup_key(path_res.value());
  }
  if (path_res.value() != "/" &&
      snap->visible_entries_by_key.count(dir_key) > 0 &&
      snap->directory_children_by_key.count(dir_key) == 0) {
    return unexpected<ErrorCode>(ErrorCode::NotADirectory);
  }
  if (snap->canonical_to_display_path.count(dir_key) == 0 && path_res.value() != "/")
    return unexpected<ErrorCode>(ErrorCode::NotADirectory);

  const auto child_it = snap->directory_children_by_key.find(dir_key);
  if (child_it == snap->directory_children_by_key.end())
    return std::vector<MountedEntry>{};

  std::vector<MountedEntry> result;
  for (const auto &child_key : child_it->second) {
    const auto file_it = snap->visible_entries_by_key.find(child_key);
    if (file_it != snap->visible_entries_by_key.end()) {
      result.push_back(make_mounted_entry(file_it->second));
      continue;
    }
    const auto display_it = snap->canonical_to_display_path.find(child_key);
    if (display_it != snap->canonical_to_display_path.end()) {
      VisibleEntry dir_entry;
      dir_entry.virtual_path = display_it->second;
      dir_entry.is_directory = true;
      result.push_back(make_mounted_entry(dir_entry));
    }
  }

  std::sort(result.begin(), result.end(),
            [](const MountedEntry &lhs, const MountedEntry &rhs) {
              if (lhs.is_directory != rhs.is_directory)
                return lhs.is_directory > rhs.is_directory;
              return lhs.virtual_path < rhs.virtual_path;
            });
  result.erase(std::unique(result.begin(), result.end(),
                           [](const MountedEntry &lhs, const MountedEntry &rhs) {
                             return lhs.virtual_path == rhs.virtual_path &&
                                    lhs.is_directory == rhs.is_directory;
                           }),
               result.end());
  return result;
}

Result<std::vector<MountedFileSystem::MountedEntry>>
MountedFileSystem::list_overlays(const std::string &virtual_path) const {
  auto path_res = normalize_lookup_path(virtual_path);
  if (!path_res)
    return unexpected<ErrorCode>(path_res.error());
  auto snap = snapshot_.load();
  std::string key = path_res.value();
  auto it = snap->overlays_by_key.find(key);
  if (it == snap->overlays_by_key.end()) {
    key = alternate_lookup_key(path_res.value());
    it = snap->overlays_by_key.find(key);
  }
  if (it != snap->overlays_by_key.end()) {
    std::vector<MountedEntry> result;
    result.reserve(it->second.size());
    for (const auto &entry : it->second) {
      result.push_back(make_mounted_entry(entry));
    }
    return result;
  }
  return unexpected<ErrorCode>(ErrorCode::FileNotFound);
}

Result<std::vector<char>>
MountedFileSystem::read_file_data(const std::string &virtual_path) {
  auto stat_res = stat(virtual_path);
  if (!stat_res)
    return unexpected<ErrorCode>(stat_res.error());
  if (stat_res.value().is_directory)
    return unexpected<ErrorCode>(ErrorCode::NotADirectory);
  auto source = source_for_mount_id(stat_res.value().mount_id);
  if (!source)
    return unexpected<ErrorCode>(ErrorCode::MountNotFound);
  return source->read_file_data(stat_res.value().source_path);
}

Result<void>
MountedFileSystem::extract_file(const std::string &virtual_path,
                                const std::filesystem::path &output_path) {
  auto stat_res = stat(virtual_path);
  if (!stat_res)
    return unexpected<ErrorCode>(stat_res.error());
  if (stat_res.value().is_directory)
    return unexpected<ErrorCode>(ErrorCode::NotADirectory);
  auto source = source_for_mount_id(stat_res.value().mount_id);
  if (!source)
    return unexpected<ErrorCode>(ErrorCode::MountNotFound);
  return source->extract_file(stat_res.value().source_path, output_path);
}

void MountedFileSystem::read_file_async(
    const std::string &virtual_path,
    std::function<void(Result<std::vector<char>>)> on_complete) {
  auto stat_res = stat(virtual_path);
  if (!stat_res) {
    on_complete(unexpected<ErrorCode>(stat_res.error()));
    return;
  }
  if (stat_res.value().is_directory) {
    on_complete(unexpected<ErrorCode>(ErrorCode::NotADirectory));
    return;
  }
  auto source = source_for_mount_id(stat_res.value().mount_id);
  if (!source) {
    on_complete(unexpected<ErrorCode>(ErrorCode::MountNotFound));
    return;
  }
  source->read_file_async(stat_res.value().source_path, std::move(on_complete));
}

void MountedFileSystem::pump_callbacks() {
  std::vector<std::shared_ptr<IMountedSource>> sources;
  {
    std::lock_guard<std::mutex> lock(rebuild_mutex_);
    sources.reserve(mounts_.size());
    for (const auto &[id, mount] : mounts_) {
      sources.push_back(mount->source);
    }
  }
  for (const auto &source : sources) {
    source->pump_callbacks();
  }
}

} // namespace vfs
