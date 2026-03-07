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
  bool is_open_ = false;
  bool is_split_ = false;
  uint8_t default_compression_level_ = 3;

  [[nodiscard]] Result<void> read_central_directory();
  [[nodiscard]] std::filesystem::path
  chunk_path_for(uint16_t chunk_index) const;
};

} // namespace vfs
