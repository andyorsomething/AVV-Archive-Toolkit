/**
 * @file archive_reader.cpp
 * @brief Reads and unpacks AVV4 (single-file) and AVV5 (split) archives.
 */
#include "archive_reader.h"
#include "../third_party/lz4/lz4frame.h"
#include "crypto_utils.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace vfs {

ArchiveReader::ArchiveReader() = default;
ArchiveReader::~ArchiveReader() = default;

// ---------------------------------------------------------------------------
// chunk_path_for
// ---------------------------------------------------------------------------

/// @brief Resolves the filesystem path containing a given chunk's payload.
///        For single-file archives returns archive_path_; for split archives
///        constructs `<chunk_dir_>/<chunk_stem_>_NNN.avv`.
std::filesystem::path
ArchiveReader::chunk_path_for(uint16_t chunk_index) const {
  if (!is_split_)
    return archive_path_;
  char buf[8];
  std::snprintf(buf, sizeof(buf), "_%03u", static_cast<unsigned>(chunk_index));
  return chunk_dir_ / (chunk_stem_ + buf + ".avv");
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

/// @brief Opens an archive from disk. Detects `_dir.avv` suffix for AVV5
///        split sets and populates chunk_dir_ / chunk_stem_ accordingly.
Result<void> ArchiveReader::open(const std::filesystem::path &archive_file) {
  if (!std::filesystem::exists(archive_file) ||
      !std::filesystem::is_regular_file(archive_file))
    return vfs::unexpected<ErrorCode>(ErrorCode::FileNotFound);

  archive_path_ = archive_file;

  const std::string fname = archive_file.filename().string();
  constexpr std::string_view DIR_SUFFIX = "_dir.avv";
  if (fname.size() > DIR_SUFFIX.size() &&
      fname.substr(fname.size() - DIR_SUFFIX.size()) == DIR_SUFFIX) {
    is_split_ = true;
    chunk_dir_ = archive_file.parent_path();
    chunk_stem_ = fname.substr(0, fname.size() - DIR_SUFFIX.size());
  } else {
    is_split_ = false;
    chunk_stem_ = "";
  }

  // Verify archive hash
  std::ifstream hash_in(archive_file, std::ios::binary);
  if (hash_in) {
    ArchiveHeader hdr;
    hash_in.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
    const std::string_view magic_sv(hdr.magic, 4);
    if (magic_sv != "AVV4" && magic_sv != "AVV5") {
      return vfs::unexpected<ErrorCode>(ErrorCode::InvalidMagic);
    }

    default_compression_level_ = hdr.default_compression_level;

    uint64_t expected = from_disk64(hdr.directory_hash);
    if (expected != 0) {
      char buf[8192];
      Fnv1a64 hasher;
      while (hash_in.read(buf, sizeof(buf))) {
        hasher.update(buf, static_cast<size_t>(hash_in.gcount()));
      }
      if (hash_in.gcount() > 0) {
        hasher.update(buf, static_cast<size_t>(hash_in.gcount()));
      }
      if (hasher.digest() != expected) {
        return vfs::unexpected<ErrorCode>(ErrorCode::HashMismatch);
      }
    }
  }

  return read_central_directory();
}

// ---------------------------------------------------------------------------
// read_central_directory
// ---------------------------------------------------------------------------

/// @brief Parses the archive footer and central directory. Handles V4
///        (28-byte CDE) and V5 (32-byte CDE with chunk_index) formats.
Result<void> ArchiveReader::read_central_directory() {
  std::ifstream in(archive_path_, std::ios::binary);
  if (!in)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  ArchiveHeader header;
  in.read(reinterpret_cast<char *>(&header), sizeof(header));
  const std::string_view magic_sv(header.magic, 4);
  if (!in || (magic_sv != "AVV4" && magic_sv != "AVV5"))
    return vfs::unexpected<ErrorCode>(ErrorCode::InvalidMagic);

  const uint32_t version = from_disk32(header.version);
  if (version != 4 && version != 5)
    return vfs::unexpected<ErrorCode>(ErrorCode::UnsupportedVersion);

  in.seekg(-static_cast<std::streamoff>(sizeof(ArchiveFooter)), std::ios::end);
  if (!in.good())
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

  const std::streamoff footer_pos = in.tellg();
  ArchiveFooter footer;
  in.read(reinterpret_cast<char *>(&footer), sizeof(footer));

  const std::string_view eof_magic(footer.magic_end, 8);
  if (!in || (eof_magic != "4VVA_EOF" && eof_magic != "5VVA_EOF"))
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

  const uint64_t dir_offset = from_disk64(footer.directory_offset);
  in.seekg(static_cast<std::streamoff>(dir_offset), std::ios::beg);
  if (!in.good())
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

  const uint64_t directory_size =
      static_cast<uint64_t>(footer_pos) - dir_offset;
  uint64_t bytes_read = 0;
  entries_.clear();

  while (bytes_read < directory_size) {
    if (version == 4) {
      CentralDirectoryEntryBase base;
      in.read(reinterpret_cast<char *>(&base), sizeof(base));
      if (!in)
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
      bytes_read += sizeof(base);

      const uint16_t path_len = from_disk16(base.path_length);
      if (path_len == 0 || bytes_read + path_len > directory_size)
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

      std::string path(path_len, '\0');
      in.read(&path[0], path_len);
      if (!in)
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
      bytes_read += path_len;

      entries_.push_back({std::move(path), from_disk16(base.flags), 0,
                          from_disk64(base.size_offset), from_disk64(base.size),
                          from_disk64(base.compressed_size)});
    } else {
      CentralDirectoryEntryBaseV3 base;
      in.read(reinterpret_cast<char *>(&base), sizeof(base));
      if (!in)
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
      bytes_read += sizeof(base);

      const uint16_t path_len = from_disk16(base.path_length);
      if (path_len == 0 || bytes_read + path_len > directory_size)
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

      std::string path(path_len, '\0');
      in.read(&path[0], path_len);
      if (!in)
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
      bytes_read += path_len;

      entries_.push_back({std::move(path), from_disk16(base.flags),
                          from_disk16(base.chunk_index),
                          from_disk64(base.size_offset), from_disk64(base.size),
                          from_disk64(base.compressed_size)});
    }
  }

  is_open_ = true;
  return {};
}

// ---------------------------------------------------------------------------
// read_entry_data
// ---------------------------------------------------------------------------

/// @brief Reads and decompresses a single FileEntry from its data stream.
///        Handles both LZ4 Frame and raw paths; shared by extract_file and
///        read_file_data.
static Result<std::vector<char>>
read_entry_data(const ArchiveReader::FileEntry &entry,
                const std::filesystem::path &data_file,
                const std::string &password) {
  if (entry.size == 0)
    return std::vector<char>{};

  std::ifstream in(data_file, std::ios::binary);
  if (!in)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  in.seekg(static_cast<std::streamoff>(entry.size_offset), std::ios::beg);
  if (!in.good())
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

  std::vector<char> result(static_cast<size_t>(entry.size));

  std::vector<char> comp(static_cast<size_t>(entry.compressed_size));
  if (!in.read(comp.data(), static_cast<std::streamsize>(comp.size())))
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

  const CipherAlgorithm cipher = cde_cipher_id(entry.flags);
  if (cipher != CipherAlgorithm::None) {
    if (password.empty())
      return vfs::unexpected<ErrorCode>(ErrorCode::DecryptionFailed);

    std::span<uint8_t> data_span(reinterpret_cast<uint8_t *>(comp.data()),
                                 comp.size());
    if (cipher == CipherAlgorithm::Xor) {
      CryptoUtils::xor_cipher(data_span, password, 0);
    } else if (cipher == CipherAlgorithm::Aes256Ctr) {
      if (comp.size() < 16)
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
      std::vector<uint8_t> iv(16);
      std::memcpy(iv.data(), comp.data(), 16);
      auto derived_key = CryptoUtils::derive_aes256_key(password);
      std::span<uint8_t> encrypted_payload(
          reinterpret_cast<uint8_t *>(comp.data() + 16), comp.size() - 16);
      CryptoUtils::aes256_ctr_cipher(encrypted_payload, derived_key, iv, 0);
      comp.erase(comp.begin(), comp.begin() + 16);
    }
  }

  if (cde_is_lz4(entry.flags)) {
    LZ4F_dctx *dctx = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION)))
      return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

    size_t dst_size = result.size();
    size_t src_size = comp.size();
    const size_t ret = LZ4F_decompress(dctx, result.data(), &dst_size,
                                       comp.data(), &src_size, nullptr);
    LZ4F_freeDecompressionContext(dctx);

    if (LZ4F_isError(ret) || dst_size != entry.size)
      return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
  } else {
    // Already in `comp`, need to move to `result`
    if (comp.size() != entry.size)
      return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
    std::memcpy(result.data(), comp.data(), comp.size());
  }
  return result;
}

// ---------------------------------------------------------------------------
// unpack_all
// ---------------------------------------------------------------------------

Result<void> ArchiveReader::unpack_all(const std::filesystem::path &output_dir,
                                       ProgressCallback progress,
                                       const std::string &password) {
  if (!is_open_)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  std::filesystem::create_directories(output_dir);

  const uint32_t total = static_cast<uint32_t>(entries_.size());
  uint32_t current = 0;

  for (const auto &entry : entries_) {
    const std::filesystem::path out_path = output_dir / entry.path;
    if (out_path.has_parent_path())
      std::filesystem::create_directories(out_path.parent_path());

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out)
      return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

    if (entry.size > 0) {
      auto data =
          read_entry_data(entry, chunk_path_for(entry.chunk_index), password);
      if (!data)
        return vfs::unexpected<ErrorCode>(data.error());
      out.write(data.value().data(),
                static_cast<std::streamsize>(data.value().size()));
    }

    if (!out.good())
      return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

    if (progress)
      progress(++current, total, entry.path);
  }
  return {};
}

// ---------------------------------------------------------------------------
// read_file_data
// ---------------------------------------------------------------------------

Result<std::vector<char>>
ArchiveReader::read_file_data(const std::string &internal_path,
                              const std::string &password) {
  if (!is_open_)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  const FileEntry *found = nullptr;
  for (const auto &e : entries_)
    if (e.path == internal_path) {
      found = &e;
      break;
    }
  if (!found)
    return vfs::unexpected<ErrorCode>(ErrorCode::FileNotFound);

  return read_entry_data(*found, chunk_path_for(found->chunk_index), password);
}

// ---------------------------------------------------------------------------
// extract_file
// ---------------------------------------------------------------------------

Result<void>
ArchiveReader::extract_file(const std::string &internal_path,
                            const std::filesystem::path &output_path,
                            const std::string &password) {

  auto data_result = read_file_data(internal_path, password);
  if (!data_result)
    return vfs::unexpected<ErrorCode>(data_result.error());

  if (output_path.has_parent_path())
    std::filesystem::create_directories(output_path.parent_path());

  std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
  if (!out)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  const auto &data = data_result.value();
  if (!data.empty())
    out.write(data.data(), static_cast<std::streamsize>(data.size()));

  return out.good() ? Result<void>{}
                    : vfs::unexpected<ErrorCode>(ErrorCode::IOError);
}

} // namespace vfs
