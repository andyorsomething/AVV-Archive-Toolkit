#pragma once

#include <bit>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

namespace vfs {

/**
 * @enum ErrorCode
 * @brief Represents strongly-typed errors that can occur during VFS operations.
 */
enum class ErrorCode {
  Success = 0,  ///< Operation completed successfully.
  FileNotFound, ///< The specified file or directory could not be found on disk.
  InvalidMagic, ///< The archive header does not contain the expected magic
                ///< bytes.
  UnsupportedVersion, ///< The archive version is not supported by this reader.
  CorruptedArchive,   ///< The archive is truncated or structurally invalid.
  IOError, ///< A fundamental OS-level I/O error occurred (e.g., failure to
           ///< read/write).
  PermissionDenied, ///< Insufficient permissions to read or write the target
                    ///< path.
  HashMismatch,     ///< The archive's computed integrity hash does not match
                    ///< the stored hash.
  DecryptionFailed  ///< The file payload could not be decrypted (e.g., wrong
                    ///< password).
};

/**
 * @brief Converts an ErrorCode to a human-readable string.
 */
inline const char *error_code_to_string(ErrorCode code) {
  switch (code) {
  case ErrorCode::Success:
    return "Success";
  case ErrorCode::FileNotFound:
    return "File Not Found";
  case ErrorCode::InvalidMagic:
    return "Invalid Magic";
  case ErrorCode::UnsupportedVersion:
    return "Unsupported Version";
  case ErrorCode::CorruptedArchive:
    return "Corrupted Archive";
  case ErrorCode::IOError:
    return "I/O Error";
  case ErrorCode::PermissionDenied:
    return "Permission Denied";
  case ErrorCode::HashMismatch:
    return "Hash Mismatch (Integrity Error)";
  case ErrorCode::DecryptionFailed:
    return "Decryption Failed";
  default:
    return "Unknown Error";
  }
}

/**
 * @brief Custom unexpected type for C++20 compatibility.
 *
 * Used to explicitly represent an error value to be returned inside a `Result`.
 * This mirrors the proposed C++23 `std::unexpected`.
 *
 * @tparam E The error type (usually ErrorCode).
 */
template <typename E> class unexpected {
  E err_;

public:
  /**
   * @brief Constructs an unexpected error value.
   * @param e The error code to store.
   */
  constexpr explicit unexpected(E e) : err_(std::move(e)) {}

  /**
   * @brief Retrieves the stored error value.
   * @return const E& The stored error.
   */
  constexpr const E &error() const { return err_; }
};

/**
 * @brief Custom Result type for C++20 compatibility, mirroring std::expected.
 *
 * Represents either a successful value of type `T` or an error of type
 * `ErrorCode`. Does not dynamically allocate unless type `T` does.
 *
 * @tparam T The type of the value to return upon success.
 */
template <typename T> class Result {
  bool has_val_;
  union {
    T val_;
    ErrorCode err_;
  };

public:
  /// @brief Constructs a Result containing a successful value.
  Result(T v) : has_val_(true) { new (&val_) T(std::move(v)); }

  /// @brief Constructs a Result containing an error code.
  Result(unexpected<ErrorCode> e) : has_val_(false), err_(e.error()) {}

  ~Result() {
    if (has_val_)
      val_.~T();
  }

  Result(Result &&other) noexcept : has_val_(other.has_val_) {
    if (has_val_)
      new (&val_) T(std::move(other.val_));
    else
      err_ = other.err_;
  }

  /// @brief Returns true if the Result contains a value, false if it contains
  /// an error.
  bool has_value() const { return has_val_; }
  explicit operator bool() const { return has_val_; }

  /// @brief Accesses the stored value. Behavior is undefined if has_value() is
  /// false.
  T &value() { return val_; }
  const T &value() const { return val_; }

  /// @brief Accesses the stored error code. Behavior is undefined if
  /// has_value() is true.
  ErrorCode error() const { return err_; }
};

/**
 * @brief Template specialization of Result for functions that return no value
 * on success.
 */
template <> class Result<void> {
  bool has_val_;
  ErrorCode err_;

public:
  /// @brief Constructs a successful void Result.
  Result() : has_val_(true), err_(ErrorCode::Success) {}

  /// @brief Constructs an unsuccessful void Result from an unexpected error.
  Result(unexpected<ErrorCode> e) : has_val_(false), err_(e.error()) {}

  /// @brief Returns true if the operation succeeded, false otherwise.
  bool has_value() const { return has_val_; }
  explicit operator bool() const { return has_val_; }

  /// @brief Retrieves the error code. Undefined if the operation succeeded.
  ErrorCode error() const { return err_; }
};

#pragma pack(push, 1)

/**
 * @brief Main header of an .avv archive. Fixed 16 bytes.
 */
struct ArchiveHeader {
  char magic[4] = {
      'A', 'V', 'V',
      '2'}; ///< Four-byte identifier. Must equal "AVV2" for version 2 archives.
  uint32_t version = 2;  ///< Archive format version. Currently 2.
  uint64_t reserved = 0; ///< Reserved for future use; must be zero.
};
static_assert(sizeof(ArchiveHeader) == 16, "Header size mismatch");

/**
 * @brief Central Directory entry on-disk base struct (AVV version 1 and 2).
 * Note: Immediately following this struct in binary format is the path string.
 */
struct CentralDirectoryEntryBase {
  uint16_t path_length;
  uint16_t flags;           ///< Bitmask. 0x01=LZ4, 0x04=XOR, 0x08=AES.
  uint64_t size_offset;     ///< Byte offset of the file data in the archive.
  uint64_t size;            ///< Uncompressed size in bytes.
  uint64_t compressed_size; ///< On-disk size (compressed or equal to size).
};
static_assert(sizeof(CentralDirectoryEntryBase) == 28,
              "CDE base size mismatch");

/**
 * @brief Central Directory entry on-disk base struct for AVV version 3.
 *
 * Extends V2 with a `chunk_index` field that identifies which data chunk file
 * (`_000.avv`, `_001.avv`, ...) the file's payload resides in.
 * A `chunk_index` of 0xFFFF is reserved and must not be used.
 * In a non-split (single-file) V3 archive, `chunk_index` is always 0.
 */
struct CentralDirectoryEntryBaseV3 {
  uint16_t path_length;
  uint16_t flags;           ///< Bitmask. 0x01=LZ4, 0x04=XOR, 0x08=AES.
  uint16_t chunk_index;     ///< Which _NNN data chunk this file lives in.
  uint16_t _reserved;       ///< Padding — must be zero.
  uint64_t size_offset;     ///< Byte offset within the chunk file.
  uint64_t size;            ///< Uncompressed size in bytes.
  uint64_t compressed_size; ///< On-disk size in the chunk file.
};
static_assert(sizeof(CentralDirectoryEntryBaseV3) == 32,
              "CDEv3 base size mismatch");

/**
 * @brief Footer of an .avv archive containing the offset to the Central
 * Directory. Fixed 16 bytes.
 */
struct ArchiveFooter {
  uint64_t directory_offset;
  char magic_end[8] = {'2', 'V', 'V', 'A', '_', 'E', 'O', 'F'};
};
static_assert(sizeof(ArchiveFooter) == 16, "Footer size mismatch");

#pragma pack(pop)

/**
 * @brief Determines at compile-time whether the host system is little-endian.
 *
 * The AVV archive format strictly uses little-endian byte order for all binary
 * metadata to ensure archives are portable across different architectures.
 *
 * @return true If the host system is little-endian.
 * @return false If the host system is big-endian.
 */
inline constexpr bool is_little_endian() {
  return std::endian::native == std::endian::little;
}

/// @brief Byte-swaps a 16-bit value. Used on big-endian hosts to produce
/// little-endian disk layout.
[[nodiscard]] inline constexpr uint16_t bswap16(uint16_t value) {
  return static_cast<uint16_t>((value << 8) | (value >> 8));
}

/// @brief Byte-swaps a 32-bit value by composing two bswap16 calls.
[[nodiscard]] inline constexpr uint32_t bswap32(uint32_t value) {
  return (static_cast<uint32_t>(bswap16(static_cast<uint16_t>(value & 0xFFFF)))
          << 16) |
         bswap16(static_cast<uint16_t>(value >> 16));
}

/// @brief Byte-swaps a 64-bit value by composing two bswap32 calls.
[[nodiscard]] inline constexpr uint64_t bswap64(uint64_t value) {
  return (static_cast<uint64_t>(
              bswap32(static_cast<uint32_t>(value & 0xFFFFFFFF)))
          << 32) |
         bswap32(static_cast<uint32_t>(value >> 32));
}

/**
 * @brief Converts a 16-bit unsigned integer to disk (little-endian) format.
 * @param value The 16-bit integer in host byte order.
 * @return inline constexpr uint16_t The integer in disk byte order.
 */
[[nodiscard]] inline constexpr uint16_t to_disk16(uint16_t value) {
  if constexpr (is_little_endian())
    return value;
  else
    return bswap16(value);
}

/**
 * @brief Converts a 32-bit unsigned integer to disk (little-endian) format.
 * @param value The 32-bit integer in host byte order.
 * @return inline constexpr uint32_t The integer in disk byte order.
 */
[[nodiscard]] inline constexpr uint32_t to_disk32(uint32_t value) {
  if constexpr (is_little_endian())
    return value;
  else
    return bswap32(value);
}

/**
 * @brief Converts a 64-bit unsigned integer to disk (little-endian) format.
 * @param value The 64-bit integer in host byte order.
 * @return inline constexpr uint64_t The integer in disk byte order.
 */
[[nodiscard]] inline constexpr uint64_t to_disk64(uint64_t value) {
  if constexpr (is_little_endian())
    return value;
  else
    return bswap64(value);
}

/**
 * @brief Converts a 16-bit unsigned integer from disk (little-endian) format
 * back to host byte order.
 * @param value The 16-bit integer in disk byte order.
 * @return inline constexpr uint16_t The integer in host byte order.
 */
[[nodiscard]] inline constexpr uint16_t from_disk16(uint16_t value) {
  return to_disk16(value); // Symmetric operation
}

/**
 * @brief Converts a 32-bit unsigned integer from disk (little-endian) format
 * back to host byte order.
 * @param value The 32-bit integer in disk byte order.
 * @return inline constexpr uint32_t The integer in host byte order.
 */
[[nodiscard]] inline constexpr uint32_t from_disk32(uint32_t value) {
  return to_disk32(value);
}

/**
 * @brief Converts a 64-bit unsigned integer from disk (little-endian) format
 * back to host byte order.
 * @param value The 64-bit integer in disk byte order.
 * @return inline constexpr uint64_t The integer in host byte order.
 */
[[nodiscard]] inline constexpr uint64_t from_disk64(uint64_t value) {
  return to_disk64(value);
}

} // namespace vfs
