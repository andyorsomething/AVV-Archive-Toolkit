/**
 * @file mounted_source.h
 * @brief Source abstraction for mounted namespace backends.
 */
#pragma once

#include "archive_reader.h"
#include "path_utils.h"
#include "thread_pool.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

namespace vfs {

class IMountedSource {
public:
  struct SourceEntry {
    std::string source_path;
    uint16_t flags = 0;
    uint16_t chunk_index = 0;
    uint64_t size = 0;
    uint64_t compressed_size = 0;
    bool is_directory = false;
  };

  virtual ~IMountedSource() = default;
  virtual Result<void> open() = 0;
  virtual void close() = 0;
  [[nodiscard]] virtual const std::vector<SourceEntry> &entries() const = 0;
  [[nodiscard]] virtual Result<std::vector<char>>
  read_file_data(const std::string &source_path) = 0;
  [[nodiscard]] virtual Result<void>
  extract_file(const std::string &source_path,
               const std::filesystem::path &output_path) = 0;
  [[nodiscard]] virtual Result<SourceEntry>
  stat(const std::string &source_path) const = 0;
  virtual void read_file_async(
      const std::string &source_path,
      std::function<void(Result<std::vector<char>>)> on_complete) = 0;
  virtual void pump_callbacks() = 0;
};

class ArchiveMountedSource final : public IMountedSource {
public:
  ArchiveMountedSource(std::filesystem::path archive_path, std::string password);

  Result<void> open() override;
  void close() override;
  [[nodiscard]] const std::vector<SourceEntry> &entries() const override;
  [[nodiscard]] Result<std::vector<char>>
  read_file_data(const std::string &source_path) override;
  [[nodiscard]] Result<void>
  extract_file(const std::string &source_path,
               const std::filesystem::path &output_path) override;
  [[nodiscard]] Result<SourceEntry>
  stat(const std::string &source_path) const override;
  void read_file_async(
      const std::string &source_path,
      std::function<void(Result<std::vector<char>>)> on_complete) override;
  void pump_callbacks() override;

private:
  std::filesystem::path archive_path_;
  std::string password_;
  ArchiveReader reader_;
  std::vector<SourceEntry> entries_;
  std::unordered_map<std::string, std::size_t> entry_lookup_;
};

class HostDirectoryMountedSource final : public IMountedSource {
public:
  explicit HostDirectoryMountedSource(std::filesystem::path host_root);
  ~HostDirectoryMountedSource() override;

  Result<void> open() override;
  void close() override;
  [[nodiscard]] const std::vector<SourceEntry> &entries() const override;
  [[nodiscard]] Result<std::vector<char>>
  read_file_data(const std::string &source_path) override;
  [[nodiscard]] Result<void>
  extract_file(const std::string &source_path,
               const std::filesystem::path &output_path) override;
  [[nodiscard]] Result<SourceEntry>
  stat(const std::string &source_path) const override;
  void read_file_async(
      const std::string &source_path,
      std::function<void(Result<std::vector<char>>)> on_complete) override;
  void pump_callbacks() override;

private:
  std::filesystem::path host_root_;
  std::vector<SourceEntry> entries_;
  std::unordered_map<std::string, std::size_t> entry_lookup_;
  std::mutex callbacks_mutex_;
  std::queue<std::function<void()>> completed_tasks_;
  std::unique_ptr<ThreadPool> thread_pool_;
};

} // namespace vfs
