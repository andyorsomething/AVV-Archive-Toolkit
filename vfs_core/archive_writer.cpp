/**
 * @file archive_writer.cpp
 * @brief Implements single-file (AVV2) and split (AVV3) packing with
 *        configurable LZ4 compression level and per-file progress callbacks.
 */
#include "archive_writer.h"
#include "../third_party/lz4/lz4frame.h"
#include "crypto_utils.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <unordered_map>
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
// Journaling Structures & Helpers
// ---------------------------------------------------------------------------

struct TempEntry {
  std::string path;
  uint16_t flags;
  uint16_t chunk_index; // 0 for single-file AVV2
  uint64_t size_offset, size, compressed_size;
};

struct JournalHeader {
  char magic[8]; // "AVVJRNL1"
  uint64_t last_hash;
};

struct JournalRecord {
  uint16_t path_len;
  uint16_t flags;
  uint16_t chunk_index;
  uint64_t size_offset;
  uint64_t size;
  uint64_t compressed_size;
  uint64_t new_hash;
};

static Result<std::unordered_map<std::string, TempEntry>>
load_journal(const std::filesystem::path &journal_path, uint64_t &out_last_hash,
             uint64_t &out_valid_size) {
  out_last_hash = Fnv1a64{}.digest(); // default
  out_valid_size = 0;
  std::unordered_map<std::string, TempEntry> entries;

  std::ifstream in(journal_path, std::ios::binary);
  if (!in)
    return entries; // empty

  JournalHeader hdr;
  if (!in.read(reinterpret_cast<char *>(&hdr), sizeof(hdr)))
    return entries;

  if (std::memcmp(hdr.magic, "AVVJRNL1", 8) != 0)
    return entries; // Invalid journal, ignore

  out_last_hash = hdr.last_hash;
  out_valid_size = sizeof(JournalHeader);

  while (in.good()) {
    JournalRecord rec;
    if (!in.read(reinterpret_cast<char *>(&rec), sizeof(rec)))
      break;

    std::string path(rec.path_len, '\0');
    if (!in.read(path.data(), rec.path_len))
      break;

    out_last_hash = rec.new_hash;
    out_valid_size += sizeof(JournalRecord) + rec.path_len;
    TempEntry e;
    e.path = std::move(path);
    e.flags = rec.flags;
    e.chunk_index = rec.chunk_index;
    e.size_offset = rec.size_offset;
    e.size = rec.size;
    e.compressed_size = rec.compressed_size;
    entries[e.path] = std::move(e);
  }
  return entries;
}

static void append_to_journal(std::ofstream &jrn, const TempEntry &e,
                              uint64_t running_hash) {
  JournalRecord rec;
  rec.path_len = static_cast<uint16_t>(e.path.size());
  rec.flags = e.flags;
  rec.chunk_index = e.chunk_index;
  rec.size_offset = e.size_offset;
  rec.size = e.size;
  rec.compressed_size = e.compressed_size;
  rec.new_hash = running_hash;

  jrn.write(reinterpret_cast<const char *>(&rec), sizeof(rec));
  jrn.write(e.path.data(), e.path.size());
}

// ---------------------------------------------------------------------------
// Single-file packing (AVV2)
// ---------------------------------------------------------------------------

Result<void>
ArchiveWriter::pack_directory(const std::filesystem::path &input_dir,
                              const std::filesystem::path &output_file,
                              int compression_level, ProgressCallback progress,
                              const EncryptionOptions &encryption,
                              bool enable_journal) {
  if (!std::filesystem::exists(input_dir) ||
      !std::filesystem::is_directory(input_dir))
    return vfs::unexpected<ErrorCode>(ErrorCode::FileNotFound);

  std::filesystem::path tmp_file = output_file.string() + ".tmp";
  std::filesystem::path journal_file = output_file.string() + "-journal";

  std::unordered_map<std::string, TempEntry> completed_entries;
  uint64_t running_hash = Fnv1a64{}.digest();
  uint64_t valid_journal_bytes = 0;

  if (enable_journal && std::filesystem::exists(journal_file) &&
      std::filesystem::exists(tmp_file)) {
    if (auto res =
            load_journal(journal_file, running_hash, valid_journal_bytes);
        res) {
      completed_entries = std::move(res.value());
      std::error_code ec;
      std::filesystem::resize_file(journal_file, valid_journal_bytes, ec);
    }
  }

  std::vector<TempEntry> entries;
  for (const auto &kv : completed_entries)
    entries.push_back(kv.second);

  std::sort(entries.begin(), entries.end(),
            [](const TempEntry &a, const TempEntry &b) {
              return a.size_offset < b.size_offset;
            });

  std::ios_base::openmode mode_out = std::ios::binary;
  if (!completed_entries.empty()) {
    mode_out |= std::ios::in | std::ios::out;
    uint64_t expected_size = 16; // Header size
    if (!entries.empty()) {
      expected_size =
          entries.back().size_offset + entries.back().compressed_size;
    }
    std::error_code ec;
    std::filesystem::resize_file(tmp_file, expected_size, ec);
    mode_out |= std::ios::ate;
  } else {
    mode_out |= std::ios::in | std::ios::out | std::ios::trunc;
  }

  std::fstream out(tmp_file, mode_out);
  if (!out)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  if (completed_entries.empty()) {
    ArchiveHeader header;
    header.version = to_disk32(2);
    header.reserved = to_disk64(0);
    out.write(reinterpret_cast<const char *>(&header), sizeof(header));
    if (!out.good())
      return vfs::unexpected<ErrorCode>(ErrorCode::IOError);
    if (enable_journal) {
      std::ofstream j_init(journal_file, std::ios::binary | std::ios::trunc);
      JournalHeader jhdr;
      std::memcpy(jhdr.magic, "AVVJRNL1", 8);
      jhdr.last_hash = running_hash;
      j_init.write(reinterpret_cast<const char *>(&jhdr), sizeof(jhdr));
    }
  }

  std::ofstream journal_out;
  if (enable_journal) {
    journal_out.open(journal_file, std::ios::binary | std::ios::app);
  }

  const uint32_t total = progress ? count_regular_files(input_dir) : 0;
  uint32_t current = static_cast<uint32_t>(completed_entries.size());

  size_t jrn_buffered = 0;

  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(input_dir)) {
    if (!entry.is_regular_file())
      continue;

    std::string rel_path =
        std::filesystem::relative(entry.path(), input_dir).generic_string();
    if (completed_entries.find(rel_path) != completed_entries.end())
      continue; // Skip already packed files

    const uint64_t file_size = std::filesystem::file_size(entry.path());
    auto pr = compress_file(entry.path(), file_size, compression_level);
    if (!pr)
      return vfs::unexpected<ErrorCode>(pr.error());
    auto &payload = pr.value();

    std::vector<uint8_t> iv;
    if (encryption.algorithm != EncryptionAlgorithm::None &&
        !payload.data.empty()) {
      std::span<uint8_t> data_span(
          reinterpret_cast<uint8_t *>(payload.data.data()),
          payload.data.size());
      if (encryption.algorithm == EncryptionAlgorithm::Xor) {
        CryptoUtils::xor_cipher(data_span, encryption.key, 0);
        payload.flags |= 0x04;
      } else if (encryption.algorithm == EncryptionAlgorithm::Aes256Ctr) {
        iv.resize(16);
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint16_t> dist(0, 255);
        for (int i = 0; i < 16; ++i)
          iv[i] = static_cast<uint8_t>(dist(gen));
        auto derived_key = CryptoUtils::derive_aes256_key(encryption.key);
        CryptoUtils::aes256_ctr_cipher(data_span, derived_key, iv, 0);
        payload.flags |= 0x08;
      }
    }

    TempEntry cde;
    cde.path = rel_path;
    cde.size = file_size;
    cde.flags = payload.flags;
    cde.chunk_index = 0;
    cde.compressed_size = payload.stored_size + iv.size();
    cde.size_offset = static_cast<uint64_t>(out.tellp());

    Fnv1a64 block_hasher(running_hash);
    if (!iv.empty()) {
      out.write(reinterpret_cast<const char *>(iv.data()),
                static_cast<std::streamsize>(iv.size()));
      block_hasher.update(iv.data(), iv.size());
    }
    if (!payload.data.empty()) {
      out.write(payload.data.data(),
                static_cast<std::streamsize>(payload.data.size()));
      block_hasher.update(payload.data.data(), payload.data.size());
    }
    if (!out.good())
      return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

    running_hash = block_hasher.digest();

    if (enable_journal && journal_out) {
      append_to_journal(journal_out, cde, running_hash);
      jrn_buffered += cde.compressed_size;
      if (jrn_buffered > 16 * 1024 * 1024) { // flush every ~16MB
        journal_out.flush();
        jrn_buffered = 0;
      }
    }

    entries.push_back(std::move(cde));

    if (progress)
      progress(++current, total, entries.back().path);
  }

  if (journal_out)
    journal_out.close();

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
  if (!out.good())
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  // Instead of recalculating the whole file, we have the running payload hash.
  Fnv1a64 hasher(running_hash);
  // Hash the central directory and footer that we just wrote
  out.clear();
  out.seekg(dir_offset, std::ios::beg);
  char buf[8192];
  while (out.read(buf, sizeof(buf))) {
    hasher.update(buf, static_cast<size_t>(out.gcount()));
  }
  if (out.gcount() > 0) {
    hasher.update(buf, static_cast<size_t>(out.gcount()));
  }
  uint64_t final_hash = hasher.digest();

  // Overwrite `reserved` field with hash
  out.clear();
  out.seekp(8, std::ios::beg); // `reserved` is exactly 8 bytes into the file
                               // (version is 4 bytes, magic 4)
  uint64_t disk_hash = to_disk64(final_hash);
  out.write(reinterpret_cast<const char *>(&disk_hash), sizeof(disk_hash));

  out.close();

  std::filesystem::rename(tmp_file, output_file);
  std::filesystem::remove(journal_file);

  return Result<void>{};
}

// ---------------------------------------------------------------------------
// Append to existing AVV2
// ---------------------------------------------------------------------------

Result<void> ArchiveWriter::append_file(
    const std::filesystem::path &source_file, const std::string &virtual_path,
    const std::filesystem::path &archive_file, int compression_level,
    const EncryptionOptions &encryption) {

  if (!std::filesystem::exists(source_file) ||
      !std::filesystem::is_regular_file(source_file)) {
    return vfs::unexpected<ErrorCode>(ErrorCode::FileNotFound);
  }

  std::string normalized_path = virtual_path;
  std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');

  std::fstream arc(archive_file,
                   std::ios::binary | std::ios::in | std::ios::out);
  if (!arc) {
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);
  }

  ArchiveHeader header;
  if (!arc.read(reinterpret_cast<char *>(&header), sizeof(header))) {
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);
  }
  if (std::memcmp(header.magic, "AVV2", 4) != 0) {
    return vfs::unexpected<ErrorCode>(
        ErrorCode::InvalidMagic); // Only support AVV2 single-file
  }

  // Move to the footer position to read the current central directory offset
  arc.seekg(-static_cast<std::streamoff>(sizeof(ArchiveFooter)), std::ios::end);
  const std::streamoff footer_pos = arc.tellg();
  ArchiveFooter footer;
  if (!arc.read(reinterpret_cast<char *>(&footer), sizeof(footer))) {
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
  }
  if (std::memcmp(footer.magic_end, "2VVA_EOF", 8) != 0) {
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
  }

  const uint64_t dir_offset = from_disk64(footer.directory_offset);
  const uint64_t directory_size =
      static_cast<uint64_t>(footer_pos) - dir_offset;

  // Read existing directory entries into memory so we can rewrite them later
  arc.seekg(static_cast<std::streamoff>(dir_offset), std::ios::beg);
  std::vector<TempEntry> entries;
  uint64_t bytes_read = 0;
  while (bytes_read < directory_size) {
    CentralDirectoryEntryBase base;
    if (!arc.read(reinterpret_cast<char *>(&base), sizeof(base)))
      break;
    bytes_read += sizeof(base);

    uint16_t path_len = from_disk16(base.path_length);
    std::string path(path_len, '\0');
    if (!arc.read(&path[0], path_len))
      break;
    bytes_read += path_len;

    TempEntry e;
    e.path = std::move(path);
    e.flags = from_disk16(base.flags);
    e.chunk_index = 0;
    e.size_offset = from_disk64(base.size_offset);
    e.size = from_disk64(base.size);
    e.compressed_size = from_disk64(base.compressed_size);
    entries.push_back(std::move(e));
  }

  // Remove existing entry with the same virtual path if it exists to allow
  // overwrite
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](const TempEntry &e) {
                                 return e.path == normalized_path;
                               }),
                entries.end());

  // Compute running hash of the old payload (everything up to dir_offset)
  // This allows us to update the archive hash without re-reading the entire
  // file
  Fnv1a64 hasher;
  arc.seekg(16, std::ios::beg); // Skip magic (4) + version (4) + hash (8)
  char buf[8192];
  uint64_t bytes_to_hash = dir_offset - 16;
  uint64_t hashed = 0;
  while (hashed < bytes_to_hash) {
    uint64_t to_read =
        std::min(static_cast<uint64_t>(sizeof(buf)), bytes_to_hash - hashed);
    arc.read(buf, static_cast<std::streamsize>(to_read));
    hasher.update(buf, static_cast<size_t>(arc.gcount()));
    hashed += arc.gcount();
  }

  // Seek back to old directory offset to overwrite it with new file data
  arc.seekp(static_cast<std::streamoff>(dir_offset), std::ios::beg);

  // Compress and encrypt new file
  const uint64_t file_size = std::filesystem::file_size(source_file);
  auto pr = compress_file(source_file, file_size, compression_level);
  if (!pr)
    return vfs::unexpected<ErrorCode>(pr.error());
  auto &payload = pr.value();

  std::vector<uint8_t> iv;
  if (encryption.algorithm != EncryptionAlgorithm::None &&
      !payload.data.empty()) {
    std::span<uint8_t> data_span(
        reinterpret_cast<uint8_t *>(payload.data.data()), payload.data.size());
    if (encryption.algorithm == EncryptionAlgorithm::Xor) {
      CryptoUtils::xor_cipher(data_span, encryption.key, 0);
      payload.flags |= 0x04;
    } else if (encryption.algorithm == EncryptionAlgorithm::Aes256Ctr) {
      iv.resize(16);
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<uint16_t> dist(0, 255);
      for (int i = 0; i < 16; ++i)
        iv[i] = static_cast<uint8_t>(dist(gen));
      auto derived_key = CryptoUtils::derive_aes256_key(encryption.key);
      CryptoUtils::aes256_ctr_cipher(data_span, derived_key, iv, 0);
      payload.flags |= 0x08;
    }
  }

  TempEntry cde;
  cde.path = normalized_path;
  cde.size = file_size;
  cde.flags = payload.flags;
  cde.chunk_index = 0;
  cde.compressed_size = payload.stored_size + iv.size();
  cde.size_offset = static_cast<uint64_t>(arc.tellp());
  entries.push_back(std::move(cde));

  if (!iv.empty()) {
    arc.write(reinterpret_cast<const char *>(iv.data()),
              static_cast<std::streamsize>(iv.size()));
    hasher.update(iv.data(), iv.size());
  }
  if (!payload.data.empty()) {
    arc.write(payload.data.data(),
              static_cast<std::streamsize>(payload.data.size()));
    hasher.update(payload.data.data(), payload.data.size());
  }
  if (!arc.good())
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  const uint64_t new_dir_offset = static_cast<uint64_t>(arc.tellp());
  for (const auto &e : entries) {
    CentralDirectoryEntryBase base{};
    base.path_length = to_disk16(static_cast<uint16_t>(e.path.size()));
    base.flags = to_disk16(e.flags);
    base.size_offset = to_disk64(e.size_offset);
    base.size = to_disk64(e.size);
    base.compressed_size = to_disk64(e.compressed_size);
    arc.write(reinterpret_cast<const char *>(&base), sizeof(base));
    arc.write(e.path.c_str(), static_cast<std::streamsize>(e.path.size()));
  }

  ArchiveFooter new_footer{};
  std::memcpy(new_footer.magic_end, "2VVA_EOF", 8);
  new_footer.directory_offset = to_disk64(new_dir_offset);
  arc.write(reinterpret_cast<const char *>(&new_footer), sizeof(new_footer));

  const uint64_t final_file_size = static_cast<uint64_t>(arc.tellp());

  // Compute final hash
  arc.seekg(new_dir_offset, std::ios::beg);
  while (arc.read(buf, sizeof(buf))) {
    hasher.update(buf, static_cast<size_t>(arc.gcount()));
  }
  if (arc.gcount() > 0) {
    hasher.update(buf, static_cast<size_t>(arc.gcount()));
  }
  const uint64_t final_hash = hasher.digest();

  arc.clear();
  arc.seekp(8, std::ios::beg);
  const uint64_t disk_hash = to_disk64(final_hash);
  arc.write(reinterpret_cast<const char *>(&disk_hash), sizeof(disk_hash));
  arc.close();

  std::error_code ec;
  std::filesystem::resize_file(archive_file, final_file_size, ec);

  return Result<void>{};
}

// ---------------------------------------------------------------------------
// Split packing (AVV3)
// ---------------------------------------------------------------------------

Result<void> ArchiveWriter::pack_directory_split(
    const std::filesystem::path &input_dir,
    const std::filesystem::path &output_stem, uint64_t max_chunk_bytes,
    int compression_level, ProgressCallback progress,
    const EncryptionOptions &encryption, bool enable_journal) {

  if (!std::filesystem::exists(input_dir) ||
      !std::filesystem::is_directory(input_dir))
    return vfs::unexpected<ErrorCode>(ErrorCode::FileNotFound);

  std::filesystem::path dir_filename_tmp =
      output_stem.string() + "_dir.avv.tmp";
  std::filesystem::path journal_file = output_stem.string() + "-journal";

  std::unordered_map<std::string, TempEntry> completed_entries;
  uint64_t running_hash = Fnv1a64{}.digest();
  uint64_t valid_journal_bytes = 0;

  if (enable_journal && std::filesystem::exists(journal_file)) {
    if (auto res =
            load_journal(journal_file, running_hash, valid_journal_bytes);
        res) {
      completed_entries = std::move(res.value());
      std::error_code ec;
      std::filesystem::resize_file(journal_file, valid_journal_bytes, ec);
    }
  }

  std::vector<TempEntry> entries;
  for (const auto &kv : completed_entries)
    entries.push_back(kv.second);
  std::sort(entries.begin(), entries.end(),
            [](const TempEntry &a, const TempEntry &b) {
              if (a.chunk_index != b.chunk_index)
                return a.chunk_index < b.chunk_index;
              return a.size_offset < b.size_offset;
            });

  uint32_t cur_chunk_idx = 0;
  uint64_t cur_chunk_used = 0;

  if (!entries.empty()) {
    cur_chunk_idx = entries.back().chunk_index;
    cur_chunk_used =
        entries.back().size_offset + entries.back().compressed_size;
  }

  auto chunk_path = [&](uint32_t idx) -> std::filesystem::path {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "_%03u", idx);
    return std::filesystem::path(output_stem.string() + buf + ".avv");
  };

  std::ofstream cur_chunk;
  if (!completed_entries.empty() &&
      std::filesystem::exists(chunk_path(cur_chunk_idx))) {
    std::error_code ec;
    std::filesystem::resize_file(chunk_path(cur_chunk_idx), cur_chunk_used, ec);
    cur_chunk.open(chunk_path(cur_chunk_idx), std::ios::binary | std::ios::in |
                                                  std::ios::out |
                                                  std::ios::ate);
  } else {
    cur_chunk.open(chunk_path(cur_chunk_idx),
                   std::ios::binary | std::ios::trunc);
  }
  if (!cur_chunk)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  if (completed_entries.empty() && enable_journal) {
    std::ofstream j_init(journal_file, std::ios::binary | std::ios::trunc);
    JournalHeader jhdr;
    std::memcpy(jhdr.magic, "AVVJRNL1", 8);
    jhdr.last_hash = running_hash;
    j_init.write(reinterpret_cast<const char *>(&jhdr), sizeof(jhdr));
  }

  std::ofstream journal_out;
  if (enable_journal) {
    journal_out.open(journal_file, std::ios::binary | std::ios::app);
  }

  const uint32_t total = progress ? count_regular_files(input_dir) : 0;
  uint32_t current = static_cast<uint32_t>(completed_entries.size());
  size_t jrn_buffered = 0;

  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(input_dir)) {
    if (!entry.is_regular_file())
      continue;

    std::string rel_path =
        std::filesystem::relative(entry.path(), input_dir).generic_string();
    if (completed_entries.find(rel_path) != completed_entries.end())
      continue;

    const uint64_t file_size = std::filesystem::file_size(entry.path());
    auto pr = compress_file(entry.path(), file_size, compression_level);
    if (!pr)
      return vfs::unexpected<ErrorCode>(pr.error());
    auto &payload = pr.value();

    std::vector<uint8_t> iv;
    if (encryption.algorithm != EncryptionAlgorithm::None &&
        !payload.data.empty()) {
      std::span<uint8_t> data_span(
          reinterpret_cast<uint8_t *>(payload.data.data()),
          payload.data.size());
      if (encryption.algorithm == EncryptionAlgorithm::Xor) {
        CryptoUtils::xor_cipher(data_span, encryption.key, 0);
        payload.flags |= 0x04;
      } else if (encryption.algorithm == EncryptionAlgorithm::Aes256Ctr) {
        iv.resize(16);
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint16_t> dist(0, 255);
        for (int i = 0; i < 16; ++i)
          iv[i] = static_cast<uint8_t>(dist(gen));
        auto derived_key = CryptoUtils::derive_aes256_key(encryption.key);
        CryptoUtils::aes256_ctr_cipher(data_span, derived_key, iv, 0);
        payload.flags |= 0x08;
      }
    }

    if (cur_chunk_used > 0 &&
        cur_chunk_used + payload.stored_size + iv.size() > max_chunk_bytes) {
      cur_chunk.close();
      ++cur_chunk_idx;
      cur_chunk_used = 0;
      cur_chunk.open(chunk_path(cur_chunk_idx),
                     std::ios::binary | std::ios::trunc);
      if (!cur_chunk)
        return vfs::unexpected<ErrorCode>(ErrorCode::IOError);
    }

    const uint64_t offset = cur_chunk_used;
    Fnv1a64 block_hasher(running_hash);
    if (!iv.empty()) {
      cur_chunk.write(reinterpret_cast<const char *>(iv.data()),
                      static_cast<std::streamsize>(iv.size()));
      block_hasher.update(iv.data(), iv.size());
    }
    if (!payload.data.empty()) {
      cur_chunk.write(payload.data.data(),
                      static_cast<std::streamsize>(payload.data.size()));
      block_hasher.update(payload.data.data(), payload.data.size());
    }
    if (!cur_chunk.good())
      return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

    running_hash = block_hasher.digest();

    cur_chunk_used += payload.stored_size + iv.size();

    TempEntry cde;
    cde.path = rel_path;
    cde.size = file_size;
    cde.flags = payload.flags;
    cde.chunk_index = static_cast<uint16_t>(cur_chunk_idx);
    cde.size_offset = offset;
    cde.compressed_size = payload.stored_size + iv.size();

    if (enable_journal && journal_out) {
      append_to_journal(journal_out, cde, running_hash);
      jrn_buffered += cde.compressed_size;
      if (jrn_buffered > 16 * 1024 * 1024) {
        journal_out.flush();
        jrn_buffered = 0;
      }
    }

    entries.push_back(std::move(cde));

    if (progress)
      progress(++current, total, entries.back().path);
  }
  cur_chunk.close();
  if (journal_out)
    journal_out.close();

  // Directory file
  const std::string dir_filename = output_stem.string() + "_dir.avv";
  std::fstream dir_out(dir_filename_tmp, std::ios::binary | std::ios::in |
                                             std::ios::out | std::ios::trunc);
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
  // Compute final hash from the newly written directory file
  // (Notice: we do not use running_hash, as the reader expects a fresh hash for
  // AVV3 _dir.avv)
  Fnv1a64 hasher;
  dir_out.seekg(16, std::ios::beg);
  char buf[8192];
  while (dir_out.read(buf, sizeof(buf))) {
    hasher.update(buf, static_cast<size_t>(dir_out.gcount()));
  }
  if (dir_out.gcount() > 0) {
    hasher.update(buf, static_cast<size_t>(dir_out.gcount()));
  }
  uint64_t final_hash = hasher.digest();

  // Overwrite `reserved` field
  dir_out.clear();
  dir_out.seekp(8, std::ios::beg);
  uint64_t disk_hash = to_disk64(final_hash);
  dir_out.write(reinterpret_cast<const char *>(&disk_hash), sizeof(disk_hash));
  dir_out.close();

  std::filesystem::rename(dir_filename_tmp, dir_filename);
  std::filesystem::remove(journal_file);

  return Result<void>{};
}

} // namespace vfs
