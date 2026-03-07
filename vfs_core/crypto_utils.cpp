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

  // Copy the IV so we can mutate it to simulate seeking.
  uint8_t working_iv[16];
  std::memcpy(working_iv, iv.data(), 16);

  // -----------------------------------------------------------------------
  // Step 1: Advance the IV counter arithmetically.
  //
  // tiny-aes-c increments ctx->Iv as a big-endian 128-bit counter at the
  // END of each 16-byte block. To seek to byte `offset`, we need the
  // counter value that tiny-aes-c will have AFTER processing the blocks
  // before `offset`, which is simply `offset / 16` increments.
  //
  // We replicate the same big-endian increment loop used in aes.c.
  // This is O(1) regardless of offset magnitude.
  // -----------------------------------------------------------------------
  const uint64_t blocks_to_skip = offset / 16;
  for (uint64_t n = 0; n < blocks_to_skip; ++n) {
    for (int k = 15; k >= 0; --k) {
      if (++working_iv[k] != 0)
        break;
    }
  }

  struct AES_ctx ctx;
  AES_init_ctx_iv(&ctx, key.data(), working_iv);

  uint8_t *ptr = data.data();
  uint64_t remaining = data.size();

  // -----------------------------------------------------------------------
  // Step 2: Handle a partial leading block if offset is not 16-byte aligned.
  //
  // tiny-aes-c has no "start from mid-block" API; we must generate a full
  // keystream block and skip the bytes that correspond to positions before
  // our offset. At most 15 bytes are discarded this way.
  // -----------------------------------------------------------------------
  const uint64_t intra_block = offset % 16;
  if (intra_block != 0) {
    uint8_t block[16] = {};
    AES_CTR_xcrypt_buffer(&ctx, block, 16);

    // The first `intra_block` bytes of `block` belong to the previous region;
    // XOR only the tail (up to 16 - intra_block bytes) against our payload.
    const uint64_t usable = 16 - intra_block;
    const uint64_t to_xor = (remaining < usable) ? remaining : usable;
    for (uint64_t i = 0; i < to_xor; ++i)
      ptr[i] ^= block[intra_block + i];

    ptr += to_xor;
    remaining -= to_xor;
  }

  // -----------------------------------------------------------------------
  // Step 3: Process remaining data in 1 GiB aligned chunks.
  //
  // 1 GiB (1 << 30) is a clean multiple of 16, so the block counter inside
  // tiny-aes-c is always at a block boundary when each chunk call ends. This
  // prevents counter drift across chunk calls, which the old 0xFFFFFFFF cap
  // (a value that is 15 mod 16) would have caused for >4 GB payloads.
  // -----------------------------------------------------------------------
  constexpr uint64_t MAX_CHUNK = 1ULL * 1024 * 1024 * 1024; // exactly 1 GiB
  while (remaining > 0) {
    const uint64_t step = (remaining > MAX_CHUNK) ? MAX_CHUNK : remaining;
    AES_CTR_xcrypt_buffer(&ctx, ptr, static_cast<uint32_t>(step));
    ptr += step;
    remaining -= step;
  }
}

} // namespace vfs
