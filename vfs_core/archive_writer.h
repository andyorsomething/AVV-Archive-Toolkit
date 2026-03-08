/**
 * @file archive_writer.h
 * @brief Public API for creating and mutating AVV archives.
 */
#pragma once

#include "vfs_types.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace vfs {

/// @brief Alias for CipherAlgorithm used by pack and append operations.
/// Values: None, Xor, Aes256Ctr - defined in `vfs_types.h`.
using EncryptionAlgorithm = CipherAlgorithm;

/**
 * @struct EncryptionOptions
 * @brief Per-operation encryption settings for pack and append APIs.
 */
struct EncryptionOptions {
  EncryptionAlgorithm algorithm =
      EncryptionAlgorithm::None; ///< The cipher to use.
  std::string key; ///< Plaintext password; key bytes are derived via FNV-1a.
};

/// @brief Default LZ4 compression level. Level 3 is the fastest LZ4-fast
///        level before the HC codepath, offering a good speed/ratio balance.
constexpr int DEFAULT_COMPRESSION_LEVEL = 3;

/// @brief Default chunk size (12 GiB) used in drag-and-drop split-pack mode.
constexpr uint64_t DEFAULT_CHUNK_BYTES = 12ULL * 1024 * 1024 * 1024;

/// @brief Per-file progress callback.
/// @param current 1-based index of the file just processed.
/// @param total Total number of files.
/// @param path Virtual path of the file.
using ProgressCallback = std::function<void(uint32_t current, uint32_t total,
                                            const std::string &path)>;

/**
 * @class ArchiveWriter
 * @brief Packs directories into `.avv` archives and mutates existing AVV4
 * archives.
 */
class ArchiveWriter {
public:
  /// @brief Constructs a new ArchiveWriter instance.
  ArchiveWriter();

  /// @brief Destroys the ArchiveWriter instance.
  ~ArchiveWriter();
  ArchiveWriter(const ArchiveWriter &) = delete;
  ArchiveWriter &operator=(const ArchiveWriter &) = delete;

  /// @brief Packs a directory tree into a single AVV4 archive.
  /// @param input_dir Source directory to traverse recursively.
  /// @param output_file Destination archive path.
  /// @param compression_level LZ4 compression level in the supported range.
  /// @param progress Optional callback invoked after each file is processed.
  /// @param encryption Optional per-file encryption settings.
  /// @param enable_journal Enables best-effort crash recovery during writes.
  /// @return `Success` on completion, otherwise a specific `ErrorCode`.
  [[nodiscard]] Result<void>
  pack_directory(const std::filesystem::path &input_dir,
                 const std::filesystem::path &output_file,
                 int compression_level = DEFAULT_COMPRESSION_LEVEL,
                 ProgressCallback progress = nullptr,
                 const EncryptionOptions &encryption = {},
                 bool enable_journal = true);

  /// @brief Appends a single file to an existing AVV4 single-file archive.
  /// @param source_file Path to the raw file on the OS disk.
  /// @param virtual_path The path/name the file will have inside the archive.
  /// @param archive_file The existing `.avv` archive to modify.
  /// @param compression_level LZ4 compression level.
  /// @param encryption Optional encryption settings.
  /// @return `Success` on completion, otherwise a specific `ErrorCode`.
  [[nodiscard]] Result<void>
  append_file(const std::filesystem::path &source_file,
              const std::string &virtual_path,
              const std::filesystem::path &archive_file,
              int compression_level = DEFAULT_COMPRESSION_LEVEL,
              const EncryptionOptions &encryption = {});

  /// @brief Deletes a single file from an existing AVV4 archive.
  /// @param virtual_path The path/name of the file to remove.
  /// @param archive_file The existing `.avv` archive to modify.
  /// @return `Success` on completion, otherwise a specific `ErrorCode`.
  [[nodiscard]] Result<void>
  delete_file(const std::string &virtual_path,
              const std::filesystem::path &archive_file);

  /// @brief Packs a directory tree into a split AVV5 archive set.
  /// @param input_dir Source directory to traverse recursively.
  /// @param output_stem Base path used to generate `_dir.avv` and `_NNN.avv`.
  /// @param max_chunk_bytes Maximum payload bytes per chunk file.
  /// @param compression_level LZ4 compression level in the supported range.
  /// @param progress Optional callback invoked after each file is processed.
  /// @param encryption Optional per-file encryption settings.
  /// @param enable_journal Enables best-effort crash recovery during writes.
  /// @return `Success` on completion, otherwise a specific `ErrorCode`.
  [[nodiscard]] Result<void>
  pack_directory_split(const std::filesystem::path &input_dir,
                       const std::filesystem::path &output_stem,
                       uint64_t max_chunk_bytes = DEFAULT_CHUNK_BYTES,
                       int compression_level = DEFAULT_COMPRESSION_LEVEL,
                       ProgressCallback progress = nullptr,
                       const EncryptionOptions &encryption = {},
                       bool enable_journal = true);
};

} // namespace vfs
