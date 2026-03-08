/**
 * @file archive_reader.cpp
 * @brief Reads and unpacks AVV4 (single-file) and AVV5 (split) archives.
 */
#include "archive_reader.h"
#include "../third_party/lz4/lz4frame.h"
#include "crypto_utils.h"
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <set>
#include <vector>

namespace vfs {

namespace {

ErrorCode map_io_error(const std::error_code &ec) {
  if (ec == std::errc::no_such_file_or_directory)
    return ErrorCode::FileNotFound;
  if (ec == std::errc::permission_denied)
    return ErrorCode::PermissionDenied;
  return ErrorCode::IOError;
}

bool is_safe_archive_entry_path(const std::string &raw_path) {
  if (raw_path.empty())
    return false;

  const std::filesystem::path normalized =
      std::filesystem::path(raw_path).lexically_normal();
  if (normalized.empty() || normalized == "." || normalized.has_root_name() ||
      normalized.has_root_directory() || normalized.is_absolute() ||
      normalized.filename().empty()) {
    return false;
  }

  for (const auto &part : normalized) {
    const std::string piece = part.generic_string();
    if (piece.empty() || piece == "." || piece == "..")
      return false;
  }

  return true;
}

Result<void> hash_file_range(const std::filesystem::path &path, uint64_t offset,
                             Fnv1a64 &hasher) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (ec)
    return vfs::unexpected<ErrorCode>(map_io_error(ec));
  if (!exists)
    return vfs::unexpected<ErrorCode>(ErrorCode::FileNotFound);

  std::ifstream in(path, std::ios::binary);
  if (!in)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  in.seekg(0, std::ios::end);
  const std::streamoff end = in.tellg();
  if (end < 0 || static_cast<uint64_t>(end) < offset)
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  char buf[8192];
  while (in.read(buf, sizeof(buf))) {
    hasher.update(buf, static_cast<size_t>(in.gcount()));
  }
  if (in.bad())
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);
  if (in.gcount() > 0)
    hasher.update(buf, static_cast<size_t>(in.gcount()));
  return Result<void>{};
}

} // namespace

#ifndef _WIN32
// Stub POSIX implementation
struct PlatformFileHandle {
  void *handle = nullptr;
};
Result<void> platform_read_at(PlatformFileHandle *, uint64_t, void *, size_t) {
  return vfs::unexpected<ErrorCode>(ErrorCode::IOError);
}
PlatformFileHandle *ArchiveReader::get_platform_handle(uint16_t) {
  return nullptr;
}
void destroy_platform_handle(PlatformFileHandle *h) { delete h; }
#else
Result<void> platform_read_at(PlatformFileHandle *handle, uint64_t offset,
                              void *buffer, size_t size);
void destroy_platform_handle(PlatformFileHandle *h);
#endif

ArchiveReader::ArchiveReader() : thread_pool_(std::make_unique<ThreadPool>()) {}
ArchiveReader::~ArchiveReader() { close(); }

void ArchiveReader::reset_file_state() {
  // Drain pending callbacks before releasing handles to avoid any dangling
  // references from already-completed tasks.
  pump_callbacks();

  std::lock_guard<std::mutex> lock(handles_mutex_);
  for (auto *h : platform_handles_) {
    destroy_platform_handle(h);
  }
  platform_handles_.clear();
  is_open_ = false;
  is_split_ = false;
  archive_path_.clear();
  chunk_dir_.clear();
  chunk_stem_.clear();
  default_compression_level_ = 3;
  entries_.clear();
  entry_lookup_.clear();
}

void ArchiveReader::close() {
  if (thread_pool_) {
    // Join all workers so no task can touch a handle after we destroy them.
    thread_pool_.reset();
  }
  reset_file_state();
}

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
  // Fully quiesce any prior async work before switching archives.
  close();

  if (!thread_pool_) {
    thread_pool_ = std::make_unique<ThreadPool>();
  }

  std::error_code ec;
  const bool exists = std::filesystem::exists(archive_file, ec);
  if (ec)
    return vfs::unexpected<ErrorCode>(map_io_error(ec));
  if (!exists)
    return vfs::unexpected<ErrorCode>(ErrorCode::FileNotFound);

  ec.clear();
  const bool regular = std::filesystem::is_regular_file(archive_file, ec);
  if (ec)
    return vfs::unexpected<ErrorCode>(map_io_error(ec));
  if (!regular)
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

  if (auto dir_res = read_central_directory(); !dir_res) {
    reset_file_state();
    return dir_res;
  }

  if (auto hash_res = verify_archive_hash(); !hash_res) {
    reset_file_state();
    return hash_res;
  }

  return Result<void>{};
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
  default_compression_level_ = header.default_compression_level;

  in.seekg(-static_cast<std::streamoff>(sizeof(ArchiveFooter)), std::ios::end);
  if (!in.good())
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

  const std::streamoff footer_pos = in.tellg();
  ArchiveFooter footer;
  in.read(reinterpret_cast<char *>(&footer), sizeof(footer));

  const std::string_view eof_magic(footer.magic_end, 8);
  const std::string_view expected_footer =
      (version == 4) ? std::string_view("4VVA_EOF", 8)
                     : std::string_view("5VVA_EOF", 8);
  if (!in || eof_magic != expected_footer)
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

  const uint64_t dir_offset = from_disk64(footer.directory_offset);
  if (dir_offset < sizeof(ArchiveHeader) ||
      dir_offset > static_cast<uint64_t>(footer_pos))
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
  in.seekg(static_cast<std::streamoff>(dir_offset), std::ios::beg);
  if (!in.good())
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

  const uint64_t directory_size =
      static_cast<uint64_t>(footer_pos) - dir_offset;
  uint64_t bytes_read = 0;
  entries_.clear();
  entry_lookup_.clear();

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
      if (!cde_flags_valid(from_disk16(base.flags)) ||
          !is_safe_archive_entry_path(path))
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

      entries_.push_back({std::move(path), from_disk16(base.flags), 0,
                          from_disk64(base.size_offset), from_disk64(base.size),
                          from_disk64(base.compressed_size)});
      if (!entry_lookup_.emplace(entries_.back().path, entries_.size() - 1)
               .second) {
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
      }
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
      const uint16_t flags = from_disk16(base.flags);
      const uint16_t chunk_index = from_disk16(base.chunk_index);
      if (!cde_flags_valid(flags) || base._reserved != 0 ||
          chunk_index == 0xFFFFu || !is_safe_archive_entry_path(path))
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

      entries_.push_back({std::move(path), flags, chunk_index,
                          from_disk64(base.size_offset), from_disk64(base.size),
                          from_disk64(base.compressed_size)});
      if (!entry_lookup_.emplace(entries_.back().path, entries_.size() - 1)
               .second) {
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
      }
    }
  }

  is_open_ = true;
  return {};
}

Result<void> ArchiveReader::verify_archive_hash() const {
  std::ifstream in(archive_path_, std::ios::binary);
  if (!in)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  ArchiveHeader header;
  in.read(reinterpret_cast<char *>(&header), sizeof(header));
  if (!in)
    return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);

  const uint64_t expected = from_disk64(header.directory_hash);
  if (expected == 0)
    return Result<void>{};

  Fnv1a64 hasher;
  if (!is_split_) {
    auto hash_res = hash_file_range(archive_path_, sizeof(ArchiveHeader), hasher);
    if (!hash_res)
      return hash_res;
  } else {
    std::set<uint16_t> chunk_indices;
    for (const auto &entry : entries_) {
      chunk_indices.insert(entry.chunk_index);
    }
    for (uint16_t chunk_index : chunk_indices) {
      auto hash_res = hash_file_range(chunk_path_for(chunk_index), 0, hasher);
      if (!hash_res)
        return vfs::unexpected<ErrorCode>(
            hash_res.error() == ErrorCode::FileNotFound
                ? ErrorCode::CorruptedArchive
                : hash_res.error());
    }

    auto hash_res = hash_file_range(archive_path_, sizeof(ArchiveHeader), hasher);
    if (!hash_res)
      return hash_res;
  }

  if (hasher.digest() != expected)
    return vfs::unexpected<ErrorCode>(ErrorCode::HashMismatch);

  return Result<void>{};
}

// ---------------------------------------------------------------------------
// read_entry_data
// ---------------------------------------------------------------------------

static Result<std::vector<char>>
read_entry_data(const ArchiveReader::FileEntry &entry,
                PlatformFileHandle *handle, const std::string &password) {
  if (entry.size == 0)
    return std::vector<char>{};

  if (!handle)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  const bool is_compressed = cde_is_lz4(entry.flags);
  const CipherAlgorithm cipher = cde_cipher_id(entry.flags);
  const size_t comp_size = static_cast<size_t>(entry.compressed_size);
  const size_t uncomp_size = static_cast<size_t>(entry.size);

  std::vector<char> result(uncomp_size);

  if (!is_compressed && cipher == CipherAlgorithm::None) {
    // Fast path: uncompressed, unencrypted — read directly into result.
    // No staging buffer needed; halves peak allocation.
    if (comp_size != uncomp_size)
      return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
    auto read_res = platform_read_at(handle, entry.size_offset, result.data(),
                                     result.size());
    if (!read_res)
      return vfs::unexpected<ErrorCode>(read_res.error());
    return result;
  }

  // General path: needs staging buffer for decryption / decompression.
  // Try the thread-local arena first (avoids heap churn for small files).
  std::vector<char> comp_heap; // only used when arena is too small
  char *comp_ptr = nullptr;

  MemoryArena &arena = ThreadPool::get_local_arena();
  arena.reset();
  void *arena_buf = arena.allocate(comp_size, 16);
  if (arena_buf) {
    comp_ptr = static_cast<char *>(arena_buf);
  } else {
    comp_heap.resize(comp_size);
    comp_ptr = comp_heap.data();
  }

  auto read_res =
      platform_read_at(handle, entry.size_offset, comp_ptr, comp_size);
  if (!read_res)
    return vfs::unexpected<ErrorCode>(read_res.error());

  if (cipher != CipherAlgorithm::None) {
    if (password.empty())
      return vfs::unexpected<ErrorCode>(ErrorCode::DecryptionFailed);

    std::span<uint8_t> data_span(reinterpret_cast<uint8_t *>(comp_ptr),
                                 comp_size);
    if (cipher == CipherAlgorithm::Xor) {
      CryptoUtils::xor_cipher(data_span, password, 0);
    } else if (cipher == CipherAlgorithm::Aes256Ctr) {
      if (comp_size < 16)
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
      std::vector<uint8_t> iv(comp_ptr, comp_ptr + 16);
      auto derived_key = CryptoUtils::derive_aes256_key(password);
      std::span<uint8_t> encrypted_payload(
          reinterpret_cast<uint8_t *>(comp_ptr + 16), comp_size - 16);
      CryptoUtils::aes256_ctr_cipher(encrypted_payload, derived_key, iv, 0);
      // Shift past the IV so decompression sees clean payload
      comp_ptr += 16;
      // comp_size shrinks by 16 bytes
      const size_t stripped = comp_size - 16;

      if (!is_compressed) {
        if (stripped != uncomp_size)
          return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
        std::memcpy(result.data(), comp_ptr, stripped);
        return result;
      }

      LZ4F_dctx *dctx = nullptr;
      if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION)))
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
      size_t dst_size = uncomp_size;
      size_t src_size = stripped;
      const size_t ret = LZ4F_decompress(dctx, result.data(), &dst_size,
                                         comp_ptr, &src_size, nullptr);
      LZ4F_freeDecompressionContext(dctx);
      if (LZ4F_isError(ret) || dst_size != uncomp_size)
        return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
      return result;
    }
  }

  if (is_compressed) {
    LZ4F_dctx *dctx = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION)))
      return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
    size_t dst_size = uncomp_size;
    size_t src_size = comp_size;
    const size_t ret = LZ4F_decompress(dctx, result.data(), &dst_size, comp_ptr,
                                       &src_size, nullptr);
    LZ4F_freeDecompressionContext(dctx);
    if (LZ4F_isError(ret) || dst_size != uncomp_size)
      return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
  } else {
    // XOR-only encrypted, uncompressed — data is already decrypted in place
    if (comp_size != uncomp_size)
      return vfs::unexpected<ErrorCode>(ErrorCode::CorruptedArchive);
    std::memcpy(result.data(), comp_ptr, comp_size);
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

  std::error_code ec;
  std::filesystem::create_directories(output_dir, ec);
  if (ec)
    return vfs::unexpected<ErrorCode>(map_io_error(ec));

  const uint32_t total = static_cast<uint32_t>(entries_.size());
  uint32_t current = 0;

  for (const auto &entry : entries_) {
    const std::filesystem::path out_path = output_dir / entry.path;
    if (out_path.has_parent_path()) {
      ec.clear();
      std::filesystem::create_directories(out_path.parent_path(), ec);
      if (ec)
        return vfs::unexpected<ErrorCode>(map_io_error(ec));
    }

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out)
      return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

    if (entry.size > 0) {
      PlatformFileHandle *handle = get_platform_handle(entry.chunk_index);
      if (!handle)
        return vfs::unexpected<ErrorCode>(ErrorCode::IOError);
      auto data = read_entry_data(entry, handle, password);
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

  const auto it = entry_lookup_.find(internal_path);
  if (it == entry_lookup_.end())
    return vfs::unexpected<ErrorCode>(ErrorCode::FileNotFound);
  const FileEntry *found = &entries_[it->second];

  PlatformFileHandle *handle = get_platform_handle(found->chunk_index);
  if (!handle)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);
  return read_entry_data(*found, handle, password);
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

  if (output_path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);
    if (ec)
      return vfs::unexpected<ErrorCode>(map_io_error(ec));
  }

  std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
  if (!out)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

  const auto &data = data_result.value();
  if (!data.empty())
    out.write(data.data(), static_cast<std::streamsize>(data.size()));

  return out.good() ? Result<void>{}
                    : vfs::unexpected<ErrorCode>(ErrorCode::IOError);
}

// ---------------------------------------------------------------------------
// Async APIs
// ---------------------------------------------------------------------------

void ArchiveReader::pump_callbacks() {
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

void ArchiveReader::prefetch_files(const std::vector<std::string> &internal_paths) {
  if (!is_open_)
    return;

  std::set<uint16_t> chunk_indices;
  for (const auto &path : internal_paths) {
    const auto it = entry_lookup_.find(path);
    if (it != entry_lookup_.end())
      chunk_indices.insert(entries_[it->second].chunk_index);
  }

  for (uint16_t chunk_index : chunk_indices) {
    (void)get_platform_handle(chunk_index);
  }
}

void ArchiveReader::read_file_async(
    const std::string &internal_path,
    std::function<void(Result<std::vector<char>>)> on_complete,
    const std::string &password) {
  if (!is_open_) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    completed_tasks_.push([on_complete = std::move(on_complete)]() {
      on_complete(vfs::unexpected<ErrorCode>(ErrorCode::IOError));
    });
    return;
  }

  const auto it = entry_lookup_.find(internal_path);
  if (it == entry_lookup_.end()) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    completed_tasks_.push([on_complete = std::move(on_complete)]() {
      on_complete(vfs::unexpected<ErrorCode>(ErrorCode::FileNotFound));
    });
    return;
  }

  FileEntry entry = entries_[it->second];
  std::string pwd = password;
  PlatformFileHandle *handle = get_platform_handle(entry.chunk_index);
  if (!handle) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    completed_tasks_.push([on_complete = std::move(on_complete)]() {
      on_complete(vfs::unexpected<ErrorCode>(ErrorCode::IOError));
    });
    return;
  }

  try {
    thread_pool_->enqueue(
        [this, entry, handle, pwd, on_complete = std::move(on_complete)]() {
          auto result = read_entry_data(entry, handle, pwd);
          std::lock_guard<std::mutex> lock(callbacks_mutex_);
          completed_tasks_.push([on_complete = std::move(on_complete),
                                 result = std::move(result)]() mutable {
            on_complete(std::move(result));
          });
        });
  } catch (const std::exception &) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    completed_tasks_.push([on_complete = std::move(on_complete)]() {
      on_complete(vfs::unexpected<ErrorCode>(ErrorCode::IOError));
    });
  }
}

void ArchiveReader::extract_file_async(
    const std::string &internal_path, const std::filesystem::path &output_path,
    std::function<void(Result<void>)> on_complete,
    const std::string &password) {
  if (!is_open_) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    completed_tasks_.push([on_complete = std::move(on_complete)]() {
      on_complete(vfs::unexpected<ErrorCode>(ErrorCode::IOError));
    });
    return;
  }

  const auto it = entry_lookup_.find(internal_path);
  if (it == entry_lookup_.end()) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    completed_tasks_.push([on_complete = std::move(on_complete)]() {
      on_complete(vfs::unexpected<ErrorCode>(ErrorCode::FileNotFound));
    });
    return;
  }

  FileEntry entry = entries_[it->second];
  std::string pwd = password;
  PlatformFileHandle *handle = get_platform_handle(entry.chunk_index);
  if (!handle) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    completed_tasks_.push([on_complete = std::move(on_complete)]() {
      on_complete(vfs::unexpected<ErrorCode>(ErrorCode::IOError));
    });
    return;
  }

  try {
    thread_pool_->enqueue([this, entry, handle, pwd, output_path,
                           on_complete = std::move(on_complete)]() {
      auto data_res = read_entry_data(entry, handle, pwd);
      Result<void> final_res;
      if (!data_res) {
        final_res = vfs::unexpected<ErrorCode>(data_res.error());
      } else {
        try {
          if (output_path.has_parent_path())
            std::filesystem::create_directories(output_path.parent_path());
          std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
          if (!out) {
            final_res = vfs::unexpected<ErrorCode>(ErrorCode::IOError);
          } else {
            const auto &data = data_res.value();
            if (!data.empty())
              out.write(data.data(), static_cast<std::streamsize>(data.size()));
            final_res = out.good()
                            ? Result<void>{}
                            : vfs::unexpected<ErrorCode>(ErrorCode::IOError);
          }
        } catch (...) {
          final_res = vfs::unexpected<ErrorCode>(ErrorCode::IOError);
        }
      }

      std::lock_guard<std::mutex> lock(callbacks_mutex_);
      completed_tasks_.push(
          [on_complete = std::move(on_complete),
           res = std::move(final_res)]() mutable { on_complete(res); });
    });
  } catch (const std::exception &) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    completed_tasks_.push([on_complete = std::move(on_complete)]() {
      on_complete(vfs::unexpected<ErrorCode>(ErrorCode::IOError));
    });
  }
}

} // namespace vfs
