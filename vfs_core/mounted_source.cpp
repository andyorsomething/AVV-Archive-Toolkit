#include "mounted_source.h"
#include <fstream>

namespace vfs {

namespace {

ErrorCode map_io_error(const std::error_code &ec) {
  if (ec == std::errc::no_such_file_or_directory)
    return ErrorCode::FileNotFound;
  if (ec == std::errc::permission_denied)
    return ErrorCode::PermissionDenied;
  return ErrorCode::IOError;
}

Result<std::vector<char>> read_host_file(const std::filesystem::path &path) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (ec)
    return unexpected<ErrorCode>(map_io_error(ec));
  if (!exists)
    return unexpected<ErrorCode>(ErrorCode::FileNotFound);

  std::ifstream in(path, std::ios::binary);
  if (!in)
    return unexpected<ErrorCode>(ErrorCode::IOError);
  in.seekg(0, std::ios::end);
  const std::streamoff end = in.tellg();
  if (end < 0)
    return unexpected<ErrorCode>(ErrorCode::IOError);
  in.seekg(0, std::ios::beg);

  std::vector<char> data(static_cast<std::size_t>(end));
  if (!data.empty() &&
      !in.read(data.data(), static_cast<std::streamsize>(data.size()))) {
    return unexpected<ErrorCode>(ErrorCode::IOError);
  }
  return data;
}

} // namespace

ArchiveMountedSource::ArchiveMountedSource(std::filesystem::path archive_path,
                                           std::string password)
    : archive_path_(std::move(archive_path)), password_(std::move(password)) {}

Result<void> ArchiveMountedSource::open() {
  auto res = reader_.open(archive_path_);
  if (!res)
    return unexpected<ErrorCode>(res.error());

  entries_.clear();
  entry_lookup_.clear();
  const auto &archive_entries = reader_.get_entries();
  entries_.reserve(archive_entries.size());
  for (const auto &entry : archive_entries) {
    SourceEntry source;
    source.source_path = entry.path;
    source.flags = entry.flags;
    source.chunk_index = entry.chunk_index;
    source.size = entry.size;
    source.compressed_size = entry.compressed_size;
    entries_.push_back(source);
    entry_lookup_[source.source_path] = entries_.size() - 1;
  }
  return {};
}

void ArchiveMountedSource::close() {
  reader_.close();
  entries_.clear();
  entry_lookup_.clear();
}

const std::vector<IMountedSource::SourceEntry> &
ArchiveMountedSource::entries() const {
  return entries_;
}

Result<std::vector<char>>
ArchiveMountedSource::read_file_data(const std::string &source_path) {
  return reader_.read_file_data(source_path, password_);
}

Result<void>
ArchiveMountedSource::extract_file(const std::string &source_path,
                                   const std::filesystem::path &output_path) {
  return reader_.extract_file(source_path, output_path, password_);
}

Result<IMountedSource::SourceEntry>
ArchiveMountedSource::stat(const std::string &source_path) const {
  const auto it = entry_lookup_.find(source_path);
  if (it == entry_lookup_.end())
    return unexpected<ErrorCode>(ErrorCode::FileNotFound);
  return entries_[it->second];
}

void ArchiveMountedSource::read_file_async(
    const std::string &source_path,
    std::function<void(Result<std::vector<char>>)> on_complete) {
  reader_.read_file_async(source_path, std::move(on_complete), password_);
}

void ArchiveMountedSource::pump_callbacks() { reader_.pump_callbacks(); }

HostDirectoryMountedSource::HostDirectoryMountedSource(
    std::filesystem::path host_root)
    : host_root_(std::move(host_root)),
      thread_pool_(std::make_unique<ThreadPool>()) {}

HostDirectoryMountedSource::~HostDirectoryMountedSource() { close(); }

Result<void> HostDirectoryMountedSource::open() {
  std::error_code ec;
  const bool exists = std::filesystem::exists(host_root_, ec);
  if (ec)
    return unexpected<ErrorCode>(map_io_error(ec));
  if (!exists)
    return unexpected<ErrorCode>(ErrorCode::FileNotFound);
  if (!std::filesystem::is_directory(host_root_, ec))
    return unexpected<ErrorCode>(ec ? map_io_error(ec) : ErrorCode::NotADirectory);

  entries_.clear();
  entry_lookup_.clear();

  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(host_root_)) {
    if (!entry.is_regular_file())
      continue;
    const auto rel_path =
        std::filesystem::relative(entry.path(), host_root_).generic_string();
    auto rel_res = normalize_source_relative_path(rel_path);
    if (!rel_res)
      return unexpected<ErrorCode>(rel_res.error());

    SourceEntry source;
    source.source_path = rel_res.value();
    source.size = entry.file_size();
    source.compressed_size = source.size;
    entries_.push_back(source);
    entry_lookup_[source.source_path] = entries_.size() - 1;
  }
  return {};
}

void HostDirectoryMountedSource::close() {
  if (thread_pool_) {
    thread_pool_.reset();
  }
  entries_.clear();
  entry_lookup_.clear();
  std::queue<std::function<void()>> pending;
  {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    pending.swap(completed_tasks_);
  }
  while (!pending.empty()) {
    pending.front()();
    pending.pop();
  }
}

const std::vector<IMountedSource::SourceEntry> &
HostDirectoryMountedSource::entries() const {
  return entries_;
}

Result<std::vector<char>>
HostDirectoryMountedSource::read_file_data(const std::string &source_path) {
  auto stat_res = stat(source_path);
  if (!stat_res)
    return unexpected<ErrorCode>(stat_res.error());
  return read_host_file(host_root_ / source_path);
}

Result<void>
HostDirectoryMountedSource::extract_file(const std::string &source_path,
                                         const std::filesystem::path &output_path) {
  auto data = read_file_data(source_path);
  if (!data)
    return unexpected<ErrorCode>(data.error());

  std::error_code ec;
  if (output_path.has_parent_path())
    std::filesystem::create_directories(output_path.parent_path(), ec);
  if (ec)
    return unexpected<ErrorCode>(map_io_error(ec));

  std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
  if (!out)
    return unexpected<ErrorCode>(ErrorCode::IOError);
  if (!data.value().empty())
    out.write(data.value().data(), static_cast<std::streamsize>(data.value().size()));
  return out.good() ? Result<void>{}
                    : unexpected<ErrorCode>(ErrorCode::IOError);
}

Result<IMountedSource::SourceEntry>
HostDirectoryMountedSource::stat(const std::string &source_path) const {
  const auto it = entry_lookup_.find(source_path);
  if (it == entry_lookup_.end())
    return unexpected<ErrorCode>(ErrorCode::FileNotFound);
  return entries_[it->second];
}

void HostDirectoryMountedSource::read_file_async(
    const std::string &source_path,
    std::function<void(Result<std::vector<char>>)> on_complete) {
  if (!thread_pool_) {
    on_complete(unexpected<ErrorCode>(ErrorCode::IOError));
    return;
  }

  try {
    thread_pool_->enqueue([this, source_path,
                           on_complete = std::move(on_complete)]() mutable {
      auto result = read_file_data(source_path);
      std::lock_guard<std::mutex> lock(callbacks_mutex_);
      completed_tasks_.push(
          [on_complete = std::move(on_complete),
           result = std::move(result)]() mutable { on_complete(std::move(result)); });
    });
  } catch (const std::exception &) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    completed_tasks_.push([on_complete = std::move(on_complete)]() {
      on_complete(unexpected<ErrorCode>(ErrorCode::IOError));
    });
  }
}

void HostDirectoryMountedSource::pump_callbacks() {
  std::queue<std::function<void()>> local;
  {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    local.swap(completed_tasks_);
  }
  while (!local.empty()) {
    local.front()();
    local.pop();
  }
}

} // namespace vfs
