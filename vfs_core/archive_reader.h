/**
 * @file archive_reader.h
 * @brief Public API for reading, extracting, and asynchronously streaming AVV
 * archives.
 */
#pragma once

#include "thread_pool.h"
#include "vfs_types.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace vfs {

/// @brief Re-exported progress callback so consumers only need this header.
using ProgressCallback = std::function<void(uint32_t current, uint32_t total,
                                            const std::string &path)>;

/// @brief Opaque platform-specific file handle wrapper.
struct PlatformFileHandle;

/**
 * @class ArchiveReader
 * @brief Parses and unpacks AVV4 single-file and AVV5 split archives.
 */
class ArchiveReader {
public:
  /**
   * @struct FileEntry
   * @brief In-memory metadata for a single file stored inside an archive.
   */
  struct FileEntry {
    std::string path; ///< Relative virtual path within the archive.
    uint16_t flags; ///< Per-file flags. Bit 0 (CDE_FLAG_LZ4) = LZ4 compressed.
                    ///<  Bits [11:8] = CipherAlgorithm discriminant.
                    ///<  Use cde_is_lz4() and cde_cipher_id() to read.
    uint16_t chunk_index;     ///< Data chunk index (AVV5 split archives only;
                              ///< always 0 for AVV4).
    uint64_t size_offset;     ///< Byte offset of the payload within its chunk.
    uint64_t size;            ///< Uncompressed file size in bytes.
    uint64_t compressed_size; ///< On-disk (compressed) size in bytes.
  };

  /// @brief Constructs a new, empty ArchiveReader.
  ArchiveReader();

  /// @brief Destroys the ArchiveReader and closes any open file handles.
  ~ArchiveReader();
  ArchiveReader(const ArchiveReader &) = delete;
  ArchiveReader &operator=(const ArchiveReader &) = delete;

  /// @brief Opens an archive and parses its central directory without loading
  ///        file payloads.
  /// @param archive_file Path to an AVV4 `.avv` file or AVV5 `_dir.avv` file.
  /// @return `Success` on completion, otherwise a specific `ErrorCode`.
  [[nodiscard]] Result<void> open(const std::filesystem::path &archive_file);

  /// @brief Closes the archive and releases all active file handles.
  void close();

  /// @brief Extracts every file in the archive to @p output_dir, recreating
  ///        the original directory structure. Invokes @p progress (if not null)
  ///        after each file.
  /// @param output_dir Destination root directory on the host filesystem.
  /// @param progress Optional callback invoked after each extracted file.
  /// @param password Optional archive password for encrypted entries.
  /// @return `Success` on completion, otherwise a specific `ErrorCode`.
  [[nodiscard]] Result<void> unpack_all(const std::filesystem::path &output_dir,
                                        ProgressCallback progress = nullptr,
                                        const std::string &password = "");

  /// @brief Extracts a single file identified by @p internal_path to
  ///        @p output_path on the host filesystem.
  /// @param internal_path Archive-relative path of the entry to extract.
  /// @param output_path Destination file path on the host filesystem.
  /// @param password Optional archive password for encrypted entries.
  /// @return `Success` on completion, otherwise a specific `ErrorCode`.
  [[nodiscard]] Result<void>
  extract_file(const std::string &internal_path,
               const std::filesystem::path &output_path,
               const std::string &password = "");

  /// @brief Reads a single file's decompressed bytes into memory.
  /// @param internal_path Archive-relative path of the entry to read.
  /// @param password Optional archive password for encrypted entries.
  /// @return The raw bytes on success, or an error result.
  [[nodiscard]] Result<std::vector<char>>
  read_file_data(const std::string &internal_path,
                 const std::string &password = "");

  // --- Async APIs ---

  /// @brief Asynchronously reads a file's decompressed bytes.
  /// @param internal_path Archive-relative path of the entry to read.
  /// @param on_complete Completion callback queued for `pump_callbacks()`.
  /// @param password Optional archive password for encrypted entries.
  void
  read_file_async(const std::string &internal_path,
                  std::function<void(Result<std::vector<char>>)> on_complete,
                  const std::string &password = "");

  /// @brief Asynchronously extracts a file to disk.
  /// @param internal_path Archive-relative path of the entry to extract.
  /// @param output_path Destination file path on the host filesystem.
  /// @param on_complete Completion callback queued for `pump_callbacks()`.
  /// @param password Optional archive password for encrypted entries.
  void extract_file_async(const std::string &internal_path,
                          const std::filesystem::path &output_path,
                          std::function<void(Result<void>)> on_complete,
                          const std::string &password = "");

  /// @brief Warms platform file handles for the chunks referenced by the paths.
  /// @param internal_paths Archive-relative paths to prefetch.
  void prefetch_files(const std::vector<std::string> &internal_paths);

  /// @brief Executes completed async callbacks on the calling thread.
  void pump_callbacks();

  /// @brief Returns the parsed central directory entries.
  /// @pre open() must have succeeded.

  [[nodiscard]] const std::vector<FileEntry> &get_entries() const {
    return entries_;
  }

  /// @brief Retrieves the default LZ4 compression level for new files appended
  ///        to this archive (valid for AVV4 single-file and AVV5 split).
  [[nodiscard]] uint8_t get_default_compression_level() const {
    return default_compression_level_;
  }

private:
  std::filesystem::path archive_path_;
  std::filesystem::path chunk_dir_;
  std::string chunk_stem_;
  std::vector<FileEntry> entries_;
  std::unordered_map<std::string, size_t> entry_lookup_;
  bool is_open_ = false;
  bool is_split_ = false;
  uint8_t default_compression_level_ = 3;

  std::vector<PlatformFileHandle *> platform_handles_;
  std::mutex handles_mutex_;
  std::mutex callbacks_mutex_;
  std::queue<std::function<void()>> completed_tasks_;
  std::unique_ptr<ThreadPool> thread_pool_;

  [[nodiscard]] Result<void> read_central_directory();
  [[nodiscard]] Result<void> verify_archive_hash() const;
  [[nodiscard]] std::filesystem::path
  chunk_path_for(uint16_t chunk_index) const;

  PlatformFileHandle *get_platform_handle(uint16_t chunk_index);

  /// @brief Closes open platform file handles and resets per-archive metadata.
  void reset_file_state();
};

} // namespace vfs
