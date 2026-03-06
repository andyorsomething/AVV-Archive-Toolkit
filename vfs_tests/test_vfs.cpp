/**
 * @file test_vfs.cpp
 * @brief Catch2 unit tests for the VFS core library.
 *
 * Covers:
 *  - Endianness helpers
 *  - Single-file AVV2 pack/unpack round-trip
 *  - LZ4HC compression correctness and fallback
 *  - Split AVV3 pack/unpack round-trip (single and multi-chunk)
 *  - Single-file extraction APIs (extract_file, read_file_data)
 *  - Error handling: bad magic, truncated file, unsupported version,
 *    missing file, zero-size entry, FileNotFound on missing path
 */
#define CATCH_CONFIG_MAIN
#include "../vfs_core/archive_reader.h"
#include "../vfs_core/archive_writer.h"
#include "catch2/catch.hpp"
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace vfs;

// ===========================================================================
// Helpers
// ===========================================================================

/// @brief Writes a string to a binary file.
/// @param p       Destination path.
/// @param content Data to write.
static void write_file(const std::filesystem::path &p,
                       const std::string &content) {
  std::ofstream out(p, std::ios::binary);
  REQUIRE(out.is_open());
  out.write(content.data(), content.size());
}

/// @brief Reads all bytes from a binary file into a string.
/// @param p Source path.
/// @return The file contents as a string.
static std::string read_file(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  return {(std::istreambuf_iterator<char>(in)),
          std::istreambuf_iterator<char>()};
}

// ===========================================================================
// Endianness
// ===========================================================================

/**
 * @brief Verifies that endianness swap helpers are bijective
 *        and that the round-trip is identity on the host.
 */
TEST_CASE("Endianness helpers", "[vfs_types]") {
  uint32_t val = 0x12345678;
  REQUIRE(to_disk32(val) == val);              // little-endian host: no-op
  REQUIRE(from_disk32(to_disk32(val)) == val); // always identity
  REQUIRE(from_disk64(to_disk64(0xDEADBEEFCAFEBABEULL)) ==
          0xDEADBEEFCAFEBABEULL);
}

// ===========================================================================
// AVV2 — Single-file pack / unpack
// ===========================================================================

/**
 * @brief Full AVV2 round-trip: pack a two-file directory, verify entry count,
 *        unpack, and compare file contents byte-for-byte.
 */
TEST_CASE("AVV2 Pack and Unpack Roundtrip", "[vfs_core]") {
  const std::filesystem::path test_dir = "rt2_in";
  const std::filesystem::path out_dir = "rt2_out";
  const std::filesystem::path archive = "rt2.avv";

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove_all(out_dir);
  std::filesystem::remove(archive);
  std::filesystem::create_directories(test_dir / "subdir");

  write_file(test_dir / "file1.txt", "Hello VFS!");
  write_file(test_dir / "subdir" / "file2.bin", "\x01\x02\x03\x04");

  SECTION("Pack") {
    ArchiveWriter writer;
    REQUIRE(writer.pack_directory(test_dir, archive).has_value());
    REQUIRE(std::filesystem::exists(archive));
    REQUIRE(std::filesystem::file_size(archive) > 0);

    SECTION("Unpack") {
      ArchiveReader reader;
      REQUIRE(reader.open(archive).has_value());
      REQUIRE(reader.get_entries().size() == 2);

      REQUIRE(reader.unpack_all(out_dir).has_value());
      REQUIRE(read_file(out_dir / "file1.txt") == "Hello VFS!");
      REQUIRE(read_file(out_dir / "subdir" / "file2.bin") ==
              "\x01\x02\x03\x04");
    }
  }

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove_all(out_dir);
  std::filesystem::remove(archive);
}

// ===========================================================================
// LZ4HC Compression
// ===========================================================================

/**
 * @brief Verifies LZ4HC compression is applied when beneficial (ratio < 1)
 *        and that the round-trip produces identical payload bytes.
 *        Also checks that incompressible data is stored raw (fallback).
 */
TEST_CASE("LZ4HC Compression Flag and Roundtrip", "[vfs_core]") {
  const std::filesystem::path test_dir = "lz4_in";
  const std::filesystem::path out_dir = "lz4_out";
  const std::filesystem::path archive = "lz4.avv";

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove_all(out_dir);
  std::filesystem::remove(archive);
  std::filesystem::create_directories(test_dir);

  // Highly compressible
  std::string compressible;
  for (int i = 0; i < 1000; ++i)
    compressible += "Compressible repeating payload string. ";

  // Intentionally incompressible: Mersenne Twister output (fixed seed for
  // reproducibility). MT19937 output has no exploitable structure at any LZ4
  // compression level.
  std::string incompressible(2048, '\0');
  {
    std::mt19937 rng(0xDEADBEEF);
    for (auto &c : incompressible)
      c = static_cast<char>(rng() & 0xFF);
  }

  write_file(test_dir / "compressible.txt", compressible);
  write_file(test_dir / "incompressible.bin", incompressible);

  ArchiveWriter writer;
  REQUIRE(writer.pack_directory(test_dir, archive).has_value());

  ArchiveReader reader;
  REQUIRE(reader.open(archive).has_value());
  const auto &entries = reader.get_entries();
  REQUIRE(entries.size() == 2);

  for (const auto &e : entries) {
    if (e.path == "compressible.txt") {
      CHECK((e.flags & 0x01) != 0);      // must be LZ4
      CHECK(e.compressed_size < e.size); // must actually shrink
    } else {
      CHECK((e.flags & 0x01) == 0); // raw fallback
      CHECK(e.compressed_size == e.size);
    }
  }

  REQUIRE(reader.unpack_all(out_dir).has_value());
  CHECK(read_file(out_dir / "compressible.txt") == compressible);
  CHECK(read_file(out_dir / "incompressible.bin") == incompressible);

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove_all(out_dir);
  std::filesystem::remove(archive);
}

// ===========================================================================
// AVV3 Split — Single chunk (all files fit in one chunk)
// ===========================================================================

/**
 * @brief Verifies that pack_directory_split produces a _dir.avv file plus at
 *        least one _000.avv chunk file, and that unpack_all restores content.
 */
TEST_CASE("AVV3 Split Pack Roundtrip - Single Chunk", "[vfs_core][split]") {
  const std::filesystem::path test_dir = "sp1_in";
  const std::filesystem::path out_dir = "sp1_out";
  const std::string stem = "sp1_archive";
  const std::string dir_avv = stem + "_dir.avv";

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove_all(out_dir);
  std::filesystem::remove(dir_avv);
  std::filesystem::remove(stem + "_000.avv");
  std::filesystem::create_directories(test_dir / "subdir");

  write_file(test_dir / "alpha.txt", "Alpha payload");
  write_file(test_dir / "subdir" / "beta.bin", "Beta payload");

  ArchiveWriter writer;
  // Use a large chunk limit so everything lands in _000.avv
  REQUIRE(writer.pack_directory_split(test_dir, stem, 1024ULL * 1024 * 1024)
              .has_value());

  REQUIRE(std::filesystem::exists(dir_avv));
  REQUIRE(std::filesystem::exists(stem + "_000.avv"));

  ArchiveReader reader;
  REQUIRE(reader.open(dir_avv).has_value());
  const auto &entries = reader.get_entries();
  REQUIRE(entries.size() == 2);
  // All entries should be in chunk 0
  for (const auto &e : entries)
    CHECK(e.chunk_index == 0);

  REQUIRE(reader.unpack_all(out_dir).has_value());
  CHECK(read_file(out_dir / "alpha.txt") == "Alpha payload");
  CHECK(read_file(out_dir / "subdir" / "beta.bin") == "Beta payload");

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove_all(out_dir);
  std::filesystem::remove(dir_avv);
  std::filesystem::remove(stem + "_000.avv");
}

// ===========================================================================
// AVV3 Split — Forced multi-chunk rollover
// ===========================================================================

/**
 * @brief Forces chunk rollover by using a tiny 1-byte chunk limit.
 *        Verifies each file gets a distinct chunk_index and that all
 *        payloads decompress correctly.
 */
TEST_CASE("AVV3 Split Pack Roundtrip - Multi Chunk", "[vfs_core][split]") {
  const std::filesystem::path test_dir = "sp2_in";
  const std::filesystem::path out_dir = "sp2_out";
  const std::string stem = "sp2_archive";
  const std::string dir_avv = stem + "_dir.avv";

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove_all(out_dir);
  // Remove any leftover chunk files
  for (int i = 0; i < 10; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "_%03d", i);
    std::filesystem::remove(stem + buf + ".avv");
  }
  std::filesystem::remove(dir_avv);
  std::filesystem::create_directories(test_dir);

  const std::string payload_a = "File A content for split test.";
  const std::string payload_b = "File B content for split test.";
  const std::string payload_c = "File C content for split test.";

  write_file(test_dir / "a.txt", payload_a);
  write_file(test_dir / "b.txt", payload_b);
  write_file(test_dir / "c.txt", payload_c);

  ArchiveWriter writer;
  // 1-byte limit forces each file into its own chunk
  REQUIRE(writer.pack_directory_split(test_dir, stem, 1).has_value());

  REQUIRE(std::filesystem::exists(dir_avv));

  ArchiveReader reader;
  REQUIRE(reader.open(dir_avv).has_value());
  const auto &entries = reader.get_entries();
  REQUIRE(entries.size() == 3);

  // Each file should land in a different chunk
  std::vector<uint16_t> chunk_indices;
  for (const auto &e : entries)
    chunk_indices.push_back(e.chunk_index);
  // All three must be distinct
  std::sort(chunk_indices.begin(), chunk_indices.end());
  REQUIRE(std::unique(chunk_indices.begin(), chunk_indices.end()) ==
          chunk_indices.end());

  REQUIRE(reader.unpack_all(out_dir).has_value());
  // Content must match regardless of which chunk they landed in
  const std::string got_a = read_file(out_dir / "a.txt");
  const std::string got_b = read_file(out_dir / "b.txt");
  const std::string got_c = read_file(out_dir / "c.txt");
  CHECK(got_a == payload_a);
  CHECK(got_b == payload_b);
  CHECK(got_c == payload_c);

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove_all(out_dir);
  std::filesystem::remove(dir_avv);
  for (int i = 0; i < 10; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "_%03d", i);
    std::filesystem::remove(stem + buf + ".avv");
  }
}

// ===========================================================================
// AVV3 Split — read_file_data and extract_file APIs
// ===========================================================================

/**
 * @brief Verifies that the single-file read/extract APIs work correctly
 *        against an AVV3 split archive, including FileNotFound for bad paths.
 */
TEST_CASE("AVV3 Split Single-File API", "[vfs_core][split]") {
  const std::filesystem::path test_dir = "sp3_in";
  const std::string stem = "sp3_archive";
  const std::string dir_avv = stem + "_dir.avv";

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove(dir_avv);
  std::filesystem::remove(stem + "_000.avv");
  std::filesystem::create_directories(test_dir);

  const std::string content = "Single-file API test payload for AVV3.";
  write_file(test_dir / "payload.txt", content);

  ArchiveWriter writer;
  REQUIRE(writer.pack_directory_split(test_dir, stem).has_value());

  ArchiveReader reader;
  REQUIRE(reader.open(dir_avv).has_value());

  SECTION("read_file_data returns correct bytes") {
    auto res = reader.read_file_data("payload.txt");
    REQUIRE(res.has_value());
    std::string got(res.value().begin(), res.value().end());
    CHECK(got == content);
  }

  SECTION("read_file_data returns FileNotFound for missing path") {
    auto res = reader.read_file_data("no_such_file.txt");
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == ErrorCode::FileNotFound);
  }

  SECTION("extract_file writes correct bytes to disk") {
    const std::filesystem::path out_path = "sp3_extracted.txt";
    std::filesystem::remove(out_path);

    REQUIRE(reader.extract_file("payload.txt", out_path).has_value());
    REQUIRE(std::filesystem::exists(out_path));
    CHECK(read_file(out_path) == content);

    std::filesystem::remove(out_path);
  }

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove(dir_avv);
  std::filesystem::remove(stem + "_000.avv");
}

// ===========================================================================
// Error Handling
// ===========================================================================

/**
 * @brief A file with non-AVV magic is rejected with InvalidMagic.
 */
TEST_CASE("Error Handling - Corrupted Magic", "[vfs_core]") {
  const std::filesystem::path f = "bad_magic.avv";
  {
    std::ofstream out(f, std::ios::binary);
    out.write("BAD_DATA_NOT_AVV", 16);
  }
  ArchiveReader reader;
  auto res = reader.open(f);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == ErrorCode::InvalidMagic);
  std::filesystem::remove(f);
}

/**
 * @brief An archive file that does not exist is rejected with FileNotFound.
 */
TEST_CASE("Error Handling - File Not Found", "[vfs_core]") {
  ArchiveReader reader;
  auto res = reader.open("does_not_exist_ever.avv");
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == ErrorCode::FileNotFound);
}

/**
 * @brief Packing a non-existent directory returns FileNotFound.
 */
TEST_CASE("Error Handling - Pack Missing Directory", "[vfs_core]") {
  ArchiveWriter writer;
  auto res = writer.pack_directory("no_such_directory_12345", "out.avv");
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == ErrorCode::FileNotFound);
}

/**
 * @brief An archive with a valid AVV2 magic but unsupported version number
 *        is rejected with UnsupportedVersion.
 */
TEST_CASE("Error Handling - Unsupported Version", "[vfs_core]") {
  const std::filesystem::path f = "bad_version.avv";
  {
    std::ofstream out(f, std::ios::binary);
    ArchiveHeader header; // default: magic="AVV2", version=2
    header.version = to_disk32(999);
    out.write(reinterpret_cast<const char *>(&header), sizeof(header));
  }
  ArchiveReader reader;
  auto res = reader.open(f);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == ErrorCode::UnsupportedVersion);
  std::filesystem::remove(f);
}

/**
 * @brief A file containing only an AVV2 header (no footer) is rejected
 *        with CorruptedArchive.
 */
TEST_CASE("Error Handling - Truncated Archive", "[vfs_core]") {
  const std::filesystem::path f = "truncated.avv";
  {
    std::ofstream out(f, std::ios::binary);
    ArchiveHeader header;
    out.write(reinterpret_cast<const char *>(&header), sizeof(header));
  }
  ArchiveReader reader;
  auto res = reader.open(f);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == ErrorCode::CorruptedArchive);
  std::filesystem::remove(f);
}

/**
 * @brief A zero-byte file packs and unpacks correctly without corruption.
 */
TEST_CASE("Zero-Size File Roundtrip", "[vfs_core]") {
  const std::filesystem::path test_dir = "zero_in";
  const std::filesystem::path out_dir = "zero_out";
  const std::filesystem::path archive = "zero.avv";

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove_all(out_dir);
  std::filesystem::remove(archive);
  std::filesystem::create_directories(test_dir);
  {
    std::ofstream(test_dir / "empty.txt");
  } // zero-byte file

  ArchiveWriter writer;
  REQUIRE(writer.pack_directory(test_dir, archive).has_value());

  ArchiveReader reader;
  REQUIRE(reader.open(archive).has_value());
  REQUIRE(reader.get_entries().size() == 1);
  CHECK(reader.get_entries()[0].size == 0);

  REQUIRE(reader.unpack_all(out_dir).has_value());
  REQUIRE(std::filesystem::exists(out_dir / "empty.txt"));
  CHECK(std::filesystem::file_size(out_dir / "empty.txt") == 0);

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove_all(out_dir);
  std::filesystem::remove(archive);
}

/**
 * @brief Single-file extraction APIs (AVV2): read_file_data and extract_file.
 */
TEST_CASE("AVV2 Single-File Extraction APIs", "[vfs_core]") {
  const std::filesystem::path test_dir = "api2_in";
  const std::filesystem::path archive = "api2.avv";

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove(archive);
  std::filesystem::create_directories(test_dir);

  const std::string content = "AVV2 single-file extraction payload.";
  write_file(test_dir / "payload.txt", content);

  ArchiveWriter writer;
  REQUIRE(writer.pack_directory(test_dir, archive).has_value());

  ArchiveReader reader;
  REQUIRE(reader.open(archive).has_value());

  SECTION("read_file_data returns correct bytes") {
    auto res = reader.read_file_data("payload.txt");
    REQUIRE(res.has_value());
    CHECK(std::string(res.value().begin(), res.value().end()) == content);
  }

  SECTION("read_file_data returns FileNotFound for missing path") {
    auto res = reader.read_file_data("no_such.txt");
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == ErrorCode::FileNotFound);
  }

  SECTION("extract_file writes correct bytes to disk") {
    const std::filesystem::path out_path = "api2_extracted.txt";
    std::filesystem::remove(out_path);
    REQUIRE(reader.extract_file("payload.txt", out_path).has_value());
    REQUIRE(std::filesystem::exists(out_path));
    CHECK(read_file(out_path) == content);
    std::filesystem::remove(out_path);
  }

  std::filesystem::remove_all(test_dir);
  std::filesystem::remove(archive);
}
