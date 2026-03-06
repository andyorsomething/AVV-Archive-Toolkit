#pragma once

#include "vfs_types.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vfs {

// Re-export the ProgressCallback so consumers only need archive_reader.h
using ProgressCallback = std::function<void(uint32_t current, uint32_t total,
                                            const std::string &path)>;

/**
 * @class ArchiveReader
 * @brief Parses and unpacks AVV1/AVV2 single-file and AVV3 split archives.
 */
class ArchiveReader {
public:
  /**
   * @struct FileEntry
   * @brief In-memory metadata for a single file stored inside an archive.
   */
  struct FileEntry {
    std::string path;         ///< Relative virtual path within the archive.
    uint16_t flags;           ///< Compression flags. 0x01 = LZ4 Frame.
    uint16_t chunk_index;     ///< Data chunk index (AVV3 split archives only;
                              ///< always 0 for AVV1/AVV2).
    uint64_t size_offset;     ///< Byte offset of the payload within its chunk.
    uint64_t size;            ///< Uncompressed file size in bytes.
    uint64_t compressed_size; ///< On-disk (compressed) size in bytes.
  };

  ArchiveReader();
  ~ArchiveReader();
  ArchiveReader(const ArchiveReader &) = delete;
  ArchiveReader &operator=(const ArchiveReader &) = delete;

  /// @brief Opens an archive and parses its central directory without loading
  ///        file payloads. Accepts both single-file .avv and split _dir.avv.
  [[nodiscard]] Result<void> open(const std::filesystem::path &archive_file);

  /// @brief Extracts every file in the archive to @p output_dir, recreating
  ///        the original directory structure. Invokes @p progress (if not null)
  ///        after each file.
  [[nodiscard]] Result<void> unpack_all(const std::filesystem::path &output_dir,
                                        ProgressCallback progress = nullptr,
                                        const std::string &password = "");

  /// @brief Extracts a single file identified by @p internal_path to
  ///        @p output_path on the host filesystem.
  [[nodiscard]] Result<void>
  extract_file(const std::string &internal_path,
               const std::filesystem::path &output_path,
               const std::string &password = "");

  /// @brief Reads a single file's decompressed bytes into memory.
  /// @return The raw bytes, or FileNotFound / IOError / CorruptedArchive.
  [[nodiscard]] Result<std::vector<char>>
  read_file_data(const std::string &internal_path,
                 const std::string &password = "");

  /// @brief Returns the parsed central directory entries.
  /// @pre open() must have succeeded.

  [[nodiscard]] const std::vector<FileEntry> &get_entries() const {
    return entries_;
  }

private:
  std::filesystem::path archive_path_;
  std::filesystem::path chunk_dir_;
  std::string chunk_stem_;
  std::vector<FileEntry> entries_;
  bool is_open_ = false;
  bool is_split_ = false;

  [[nodiscard]] Result<void> read_central_directory();
  [[nodiscard]] std::filesystem::path
  chunk_path_for(uint16_t chunk_index) const;
};

} // namespace vfs
