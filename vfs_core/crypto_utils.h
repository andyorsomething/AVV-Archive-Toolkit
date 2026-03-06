#pragma once

#include "vfs_types.h"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vfs {

/**
 * @brief FNV-1a 64-bit hash calculator for file integrity.
 */
class Fnv1a64 {
  uint64_t hash_ = 0xcbf29ce484222325ULL;

public:
  Fnv1a64() = default;
  explicit Fnv1a64(uint64_t initial) : hash_(initial) {}

  /**
   * @brief Updates the running hash with new data.
   * @param data Pointer to the buffer containing data to hash.
   * @param size Number of bytes in the buffer.
   */
  void update(const void *data, size_t size);

  /**
   * @brief Retrieves the final computed hash.
   * @return uint64_t The 64-bit FNV-1a hash.
   */
  [[nodiscard]] uint64_t digest() const { return hash_; }
};

/**
 * @brief Utility class for encryption and decryption of VFS payloads.
 */
class CryptoUtils {
public:
  /**
   * @brief Applies an XOR cipher to the given span of bytes in place.
   * @param data The data to encrypt/decrypt.
   * @param key The key to use.
   * @param offset Optional offset for stream continuity (byte offset).
   */
  static void xor_cipher(std::span<uint8_t> data, const std::string &key,
                         uint64_t offset = 0);

  /**
   * @brief Derives a 32-byte AES key from a user password using FNV-1a
   * recursion.
   * @param password User's plaintext password.
   * @return std::vector<uint8_t> A 32-byte key.
   */
  [[nodiscard]] static std::vector<uint8_t>
  derive_aes256_key(const std::string &password);

  /**
   * @brief Encrypts/Decrypts a block of data using AES-256-CTR.
   * @param data The data to en/decrypt in place.
   * @param key 32-byte key.
   * @param iv 16-byte initialization vector.
   * @param offset Byte offset (simulates seek skipping within the stream).
   */
  static void aes256_ctr_cipher(std::span<uint8_t> data,
                                const std::vector<uint8_t> &key,
                                const std::vector<uint8_t> &iv,
                                uint64_t offset = 0);
};

} // namespace vfs
