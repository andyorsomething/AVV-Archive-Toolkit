/**
 * @file archive_writer.cpp
 * @brief Implements single-file (AVV2) and split (AVV3) packing with
 *        configurable LZ4 compression level and per-file progress callbacks.
 */
#include "archive_writer.h"
#include "../third_party/lz4/lz4frame.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace vfs {

ArchiveWriter::ArchiveWriter() = default;
ArchiveWriter::~ArchiveWriter() = default;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Builds LZ4 frame preferences at the given compression level.
/// @param level LZ4 compression level (1-12). Levels >= 4 activate LZ4HC.
static LZ4F_preferences_t make_lz4_prefs(int level) {
  LZ4F_preferences_t p{};
  p.compressionLevel = level;
  p.frameInfo.blockMode = LZ4F_blockLinked;
  return p;
}

/// @brief Result of compressing a single file payload.
struct CompressedPayload {
  std::vector<char> data; ///< Compressed or raw bytes to write to disk.
  uint16_t flags;         ///< 0x01 = LZ4 compressed; 0 = raw.
  uint64_t stored_size;   ///< Number of bytes in @c data.
};

/// @brief Reads a file, compresses it via LZ4 Frame, and returns the smaller
///        of the compressed or raw payload.
/// @param input_path        File to read.
/// @param uncompressed_size Pre-known file size.
/// @param level             LZ4 compression level.
static Result<CompressedPayload>
compress_file(const std::filesystem::path &input_path,
              uint64_t uncompressed_size, int level) {
  std::ifstream in(input_path, std::ios::binary);
  if (!in)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  std::vector<char> raw(static_cast<size_t>(uncompressed_size));
  if (uncompressed_size > 0 && !in.read(raw.data(), uncompressed_size))
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  if (uncompressed_size == 0)
    return CompressedPayload{{}, 0, 0};

  const LZ4F_preferences_t prefs = make_lz4_prefs(level);
  const size_t max_dst = LZ4F_compressFrameBound(uncompressed_size, &prefs);
  std::vector<char> comp(max_dst);

  size_t comp_sz = LZ4F_compressFrame(comp.data(), comp.size(), raw.data(),
                                      raw.size(), &prefs);
  if (LZ4F_isError(comp_sz))
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  if (comp_sz < uncompressed_size) {
    comp.resize(comp_sz);
    return CompressedPayload{std::move(comp), 0x01,
                             static_cast<uint64_t>(comp_sz)};
  }
  return CompressedPayload{std::move(raw), 0, uncompressed_size};
}

// ---------------------------------------------------------------------------
// Shared: count files in a directory tree
// ---------------------------------------------------------------------------

/// @brief Counts regular files in a directory tree (used to compute
///        progress total before iterating).
static uint32_t count_regular_files(const std::filesystem::path &dir) {
  uint32_t n = 0;
  for (const auto &e : std::filesystem::recursive_directory_iterator(dir))
    if (e.is_regular_file())
      ++n;
  return n;
}

// ---------------------------------------------------------------------------
// Single-file packing (AVV2)
// ---------------------------------------------------------------------------

Result<void>
ArchiveWriter::pack_directory(const std::filesystem::path &input_dir,
                              const std::filesystem::path &output_file,
                              int compression_level,
                              ProgressCallback progress) {
  if (!std::filesystem::exists(input_dir) ||
      !std::filesystem::is_directory(input_dir))
    return vfs::unexpected<ErrorCode>(ErrorCode::FileNotFound);

  std::ofstream out(output_file, std::ios::binary | std::ios::trunc);
  if (!out)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  ArchiveHeader header;
  header.version = to_disk32(2);
  header.reserved = to_disk64(0);
  out.write(reinterpret_cast<const char *>(&header), sizeof(header));
  if (!out.good())
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  struct TempEntry {
    std::string path;
    uint16_t flags;
    uint64_t size_offset, size, compressed_size;
  };
  std::vector<TempEntry> entries;

  const uint32_t total = progress ? count_regular_files(input_dir) : 0;
  uint32_t current = 0;

  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(input_dir)) {
    if (!entry.is_regular_file())
      continue;

    const uint64_t file_size = std::filesystem::file_size(entry.path());
    auto pr = compress_file(entry.path(), file_size, compression_level);
    if (!pr)
      return vfs::unexpected<ErrorCode>(pr.error());
    auto &payload = pr.value();

    TempEntry cde;
    cde.path =
        std::filesystem::relative(entry.path(), input_dir).generic_string();
    cde.size = file_size;
    cde.flags = payload.flags;
    cde.compressed_size = payload.stored_size;
    cde.size_offset = static_cast<uint64_t>(out.tellp());

    if (!payload.data.empty())
      out.write(payload.data.data(),
                static_cast<std::streamsize>(payload.data.size()));
    if (!out.good())
      return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

    entries.push_back(std::move(cde));

    if (progress)
      progress(++current, total, entries.back().path);
  }

  const uint64_t dir_offset = static_cast<uint64_t>(out.tellp());
  for (const auto &e : entries) {
    CentralDirectoryEntryBase base{};
    base.path_length = to_disk16(static_cast<uint16_t>(e.path.size()));
    base.flags = to_disk16(e.flags);
    base.size_offset = to_disk64(e.size_offset);
    base.size = to_disk64(e.size);
    base.compressed_size = to_disk64(e.compressed_size);
    out.write(reinterpret_cast<const char *>(&base), sizeof(base));
    out.write(e.path.c_str(), static_cast<std::streamsize>(e.path.size()));
  }

  ArchiveFooter footer{};
  footer.directory_offset = to_disk64(dir_offset);
  out.write(reinterpret_cast<const char *>(&footer), sizeof(footer));

  return out.good() ? Result<void>{}
                    : vfs::unexpected<ErrorCode>(ErrorCode::IOError);
}

// ---------------------------------------------------------------------------
// Split packing (AVV3)
// ---------------------------------------------------------------------------

Result<void> ArchiveWriter::pack_directory_split(
    const std::filesystem::path &input_dir,
    const std::filesystem::path &output_stem, uint64_t max_chunk_bytes,
    int compression_level, ProgressCallback progress) {

  if (!std::filesystem::exists(input_dir) ||
      !std::filesystem::is_directory(input_dir))
    return vfs::unexpected<ErrorCode>(ErrorCode::FileNotFound);

  struct TempEntry {
    std::string path;
    uint16_t flags;
    uint16_t chunk_index;
    uint64_t size_offset, size, compressed_size;
  };
  std::vector<TempEntry> entries;

  uint32_t cur_chunk_idx = 0;
  uint64_t cur_chunk_used = 0;

  auto chunk_path = [&](uint32_t idx) -> std::filesystem::path {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "_%03u", idx);
    return std::filesystem::path(output_stem.string() + buf + ".avv");
  };

  std::ofstream cur_chunk(chunk_path(0), std::ios::binary | std::ios::trunc);
  if (!cur_chunk)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  const uint32_t total = progress ? count_regular_files(input_dir) : 0;
  uint32_t current = 0;

  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(input_dir)) {
    if (!entry.is_regular_file())
      continue;

    const uint64_t file_size = std::filesystem::file_size(entry.path());
    auto pr = compress_file(entry.path(), file_size, compression_level);
    if (!pr)
      return vfs::unexpected<ErrorCode>(pr.error());
    auto &payload = pr.value();

    if (cur_chunk_used > 0 &&
        cur_chunk_used + payload.stored_size > max_chunk_bytes) {
      cur_chunk.close();
      ++cur_chunk_idx;
      cur_chunk_used = 0;
      cur_chunk.open(chunk_path(cur_chunk_idx),
                     std::ios::binary | std::ios::trunc);
      if (!cur_chunk)
        return vfs::unexpected<ErrorCode>(ErrorCode::IOError);
    }

    const uint64_t offset = cur_chunk_used;
    if (!payload.data.empty())
      cur_chunk.write(payload.data.data(),
                      static_cast<std::streamsize>(payload.data.size()));
    if (!cur_chunk.good())
      return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

    cur_chunk_used += payload.stored_size;

    TempEntry cde;
    cde.path =
        std::filesystem::relative(entry.path(), input_dir).generic_string();
    cde.size = file_size;
    cde.flags = payload.flags;
    cde.chunk_index = static_cast<uint16_t>(cur_chunk_idx);
    cde.size_offset = offset;
    cde.compressed_size = payload.stored_size;
    entries.push_back(std::move(cde));

    if (progress)
      progress(++current, total, entries.back().path);
  }
  cur_chunk.close();

  // Directory file
  const std::string dir_filename = output_stem.string() + "_dir.avv";
  std::ofstream dir_out(dir_filename, std::ios::binary | std::ios::trunc);
  if (!dir_out)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  ArchiveHeader header;
  std::memcpy(header.magic, "AVV3", 4);
  header.version = to_disk32(3);
  header.reserved = to_disk64(0);
  dir_out.write(reinterpret_cast<const char *>(&header), sizeof(header));

  const uint64_t dir_offset = static_cast<uint64_t>(dir_out.tellp());
  for (const auto &e : entries) {
    CentralDirectoryEntryBaseV3 base{};
    base.path_length = to_disk16(static_cast<uint16_t>(e.path.size()));
    base.flags = to_disk16(e.flags);
    base.chunk_index = to_disk16(e.chunk_index);
    base._reserved = 0;
    base.size_offset = to_disk64(e.size_offset);
    base.size = to_disk64(e.size);
    base.compressed_size = to_disk64(e.compressed_size);
    dir_out.write(reinterpret_cast<const char *>(&base), sizeof(base));
    dir_out.write(e.path.c_str(), static_cast<std::streamsize>(e.path.size()));
  }

  ArchiveFooter footer{};
  std::memcpy(footer.magic_end, "3VVA_EOF", 8);
  footer.directory_offset = to_disk64(dir_offset);
  dir_out.write(reinterpret_cast<const char *>(&footer), sizeof(footer));

  return dir_out.good() ? Result<void>{}
                        : vfs::unexpected<ErrorCode>(ErrorCode::IOError);
}

} // namespace vfs
