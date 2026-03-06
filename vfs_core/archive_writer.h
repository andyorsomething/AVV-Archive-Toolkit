#pragma once

#include "vfs_types.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace vfs {

/// @brief Default LZ4 compression level. Level 3 is the fastest LZ4-fast
///        level before the HC codepath, offering a good speed/ratio balance.
constexpr int DEFAULT_COMPRESSION_LEVEL = 3;

/// @brief Default chunk size (12 GiB) used in drag-and-drop split-pack mode.
constexpr uint64_t DEFAULT_CHUNK_BYTES = 12ULL * 1024 * 1024 * 1024;

/// @brief Per-file progress callback.
/// @param current  1-based index of the file just processed.
/// @param total    Total number of files.
/// @param path     Virtual path of the file.
using ProgressCallback = std::function<void(uint32_t current, uint32_t total,
                                            const std::string &path)>;

/**
 * @class ArchiveWriter
 * @brief Packs directories into .avv archives (single-file or split).
 */
class ArchiveWriter {
public:
  ArchiveWriter();
  ~ArchiveWriter();
  ArchiveWriter(const ArchiveWriter &) = delete;
  ArchiveWriter &operator=(const ArchiveWriter &) = delete;

  /// @brief Packs into a single AVV2 archive.
  [[nodiscard]] Result<void>
  pack_directory(const std::filesystem::path &input_dir,
                 const std::filesystem::path &output_file,
                 int compression_level = DEFAULT_COMPRESSION_LEVEL,
                 ProgressCallback progress = nullptr);

  /// @brief Packs into a VPK-style split AVV3 archive set.
  [[nodiscard]] Result<void>
  pack_directory_split(const std::filesystem::path &input_dir,
                       const std::filesystem::path &output_stem,
                       uint64_t max_chunk_bytes = DEFAULT_CHUNK_BYTES,
                       int compression_level = DEFAULT_COMPRESSION_LEVEL,
                       ProgressCallback progress = nullptr);
};

} // namespace vfs
