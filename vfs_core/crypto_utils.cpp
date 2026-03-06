#include "crypto_utils.h"

// Tell tiny-AES-c to compile for AES256 CTR
#ifndef AES256
#define AES256 1
#endif
#ifndef CTR
#define CTR 1
#endif
#ifndef CBC
#define CBC 0
#endif
#ifndef ECB
#define ECB 0
#endif

extern "C" {
#include "../third_party/tiny-aes-c/aes.h"
}

#include <cstring>

namespace vfs {

void Fnv1a64::update(const void *data, size_t size) {
  const uint8_t *ptr = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; ++i) {
    hash_ ^= ptr[i];
    hash_ *= 0x100000001b3ULL;
  }
}

void CryptoUtils::xor_cipher(std::span<uint8_t> data, const std::string &key,
                             uint64_t offset) {
  if (key.empty() || data.empty())
    return;
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] ^= key[(offset + i) % key.size()];
  }
}

std::vector<uint8_t>
CryptoUtils::derive_aes256_key(const std::string &password) {
  std::vector<uint8_t> key(32, 0);
  if (password.empty())
    return key;

  Fnv1a64 h1, h2, h3, h4;

  h1.update(password.data(), password.size());
  uint64_t d1 = to_disk64(h1.digest());

  h2.update(&d1, sizeof(d1));
  h2.update(password.data(), password.size());
  uint64_t d2 = to_disk64(h2.digest());

  h3.update(&d2, sizeof(d2));
  h3.update(password.data(), password.size());
  uint64_t d3 = to_disk64(h3.digest());

  h4.update(&d3, sizeof(d3));
  h4.update(password.data(), password.size());
  uint64_t d4 = to_disk64(h4.digest());

  std::memcpy(key.data(), &d1, 8);
  std::memcpy(key.data() + 8, &d2, 8);
  std::memcpy(key.data() + 16, &d3, 8);
  std::memcpy(key.data() + 24, &d4, 8);

  return key;
}

void CryptoUtils::aes256_ctr_cipher(std::span<uint8_t> data,
                                    const std::vector<uint8_t> &key,
                                    const std::vector<uint8_t> &iv,
                                    uint64_t offset) {
  if (key.size() != 32 || iv.size() != 16 || data.empty())
    return;

  struct AES_ctx ctx;
  AES_init_ctx_iv(&ctx, key.data(), iv.data());

  // Simulate skip by advancing the CTR cipher
  if (offset > 0) {
    uint8_t dummy[4096];
    uint64_t remaining = offset;
    while (remaining > 0) {
      uint32_t step = static_cast<uint32_t>(
          remaining > sizeof(dummy) ? sizeof(dummy) : remaining);
      std::memset(dummy, 0, step);
      AES_CTR_xcrypt_buffer(&ctx, dummy, step);
      remaining -= step;
    }
  }

  AES_CTR_xcrypt_buffer(&ctx, data.data(), static_cast<uint32_t>(data.size()));
}

} // namespace vfs
