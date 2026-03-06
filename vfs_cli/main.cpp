/**
 * @file main.cpp
 * @brief VFS CLI — pack, packs (split), unpack, list, with progress bars.
 *
 * Flags:
 *   -v           Verbose per-file output.
 *   -s <GB>      Chunk size in GiB for split packing (default 12).
 *   -c <level>   LZ4 compression level 1..12 (default 3).
 */
#include "../vfs_core/archive_reader.h"
#include "../vfs_core/archive_writer.h"
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Formats a byte count into a human-readable string (e.g. "1.2 MB").
static std::string format_size(uint64_t bytes) {
  char buf[32];
  if (bytes >= 1024ULL * 1024 * 1024)
    std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
  else if (bytes >= 1024ULL * 1024)
    std::snprintf(buf, sizeof(buf), "%.2f MB", bytes / (1024.0 * 1024));
  else if (bytes >= 1024ULL)
    std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
  else
    std::snprintf(buf, sizeof(buf), "%llu B",
                  static_cast<unsigned long long>(bytes));
  return buf;
}

/// @brief Prints full CLI usage, commands, flags, and drag-and-drop info.
static void print_usage(const char *prog) {
  std::cout << "Usage:\n"
            << "  " << prog
            << " [-v] [-c <1..12>] pack  <output.avv> <input_dir>\n"
            << "  " << prog
            << " [-v] [-c <1..12>] [-s <GB>] packs <stem> <input_dir>\n"
            << "  " << prog
            << " [-v] unpack <input.avv|_dir.avv> <output_dir>\n"
            << "  " << prog << " list <input.avv|_dir.avv>\n"
            << "\nOptions:\n"
            << "  -v          Verbose: print each file.\n"
            << "  -c <level>  LZ4 compression level (default 3; range 1-12).\n"
            << "  -s <GB>     Chunk size for split archives (default 12).\n"
            << "  --encrypt <alg>  Algorithm to use: xor, aes\n"
            << "  --key <pass>     Password for encryption / decryption\n"
            << "  --no-journal     Disable resume journaling\n"
            << "\nDrag-and-drop:\n"
            << "  Folder → split-pack at 12 GiB chunks.\n"
            << "  .avv   → unpack beside the archive.\n";
}

// ---------------------------------------------------------------------------
// Progress bar renderer (inline, \r based)
// ---------------------------------------------------------------------------

/// @brief Renders: [████████░░░░░░░░░░░░]  42%  (128/305)  filename.ext
static void render_progress(uint32_t current, uint32_t total,
                            const std::string &path) {
  constexpr int BAR_WIDTH = 20;
  const float fraction =
      (total > 0) ? static_cast<float>(current) / total : 1.0f;
  const int filled = static_cast<int>(fraction * BAR_WIDTH);

  // Build bar string: filled = '#', empty = '-'     (ASCII-safe)
  char bar[BAR_WIDTH + 1];
  for (int i = 0; i < BAR_WIDTH; ++i)
    bar[i] = (i < filled) ? '#' : '-';
  bar[BAR_WIDTH] = '\0';

  // Truncate filename to last 30 chars
  std::string short_path = path;
  if (short_path.size() > 30)
    short_path = "..." + short_path.substr(short_path.size() - 27);

  std::printf("\r  [%s] %3d%%  (%u/%u)  %-30s", bar,
              static_cast<int>(fraction * 100.0f), current, total,
              short_path.c_str());
  std::fflush(stdout);

  if (current == total)
    std::printf("\n");
}

/// @brief Verbose listing: prints each entry's chunk, path, size, and
/// compression.
static void print_entries(const vfs::ArchiveReader &reader, const char *verb) {
  for (const auto &e : reader.get_entries()) {
    const char *comp = (e.flags & 0x01) ? "LZ4" : "Raw";
    std::printf("  %s: [%03u] %-36s %10s  [%s]\n", verb, e.chunk_index,
                e.path.c_str(), format_size(e.size).c_str(), comp);
  }
}

// ---------------------------------------------------------------------------
// Flag parsing helper
// ---------------------------------------------------------------------------

/// @brief Parses a numeric CLI argument following a flag (e.g. "-s 12").
/// @param name       Flag name (for error messages).
/// @param arg_offset Current argv index; advanced past the value on success.
/// @param argc       Total argument count.
/// @param argv       Argument array.
/// @param[out] out   Parsed value.
/// @return true on success, false on parse error.
static bool parse_uint_arg(const char *name, int &arg_offset, int argc,
                           char **argv, unsigned long long &out) {
  if (arg_offset + 1 >= argc) {
    std::cerr << "Error: " << name << " requires a value.\n";
    return false;
  }
  ++arg_offset;
  const char *begin = argv[arg_offset];
  const char *end = begin + std::strlen(begin);
  auto [ptr, ec] = std::from_chars(begin, end, out);
  if (ec != std::errc{} || out == 0) {
    std::cerr << "Error: invalid " << name << " value '" << argv[arg_offset]
              << "'.\n";
    return false;
  }
  ++arg_offset;
  return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  bool verbose = false;
  int comp_level = vfs::DEFAULT_COMPRESSION_LEVEL;
  uint64_t chunk_gb = 12;
  vfs::EncryptionOptions enc_opts;
  bool enable_journal = true;
  int arg_offset = 1;

  while (arg_offset < argc) {
    const std::string a = argv[arg_offset];
    if (a == "-v") {
      verbose = true;
      ++arg_offset;
    } else if (a == "-s") {
      unsigned long long val = 0;
      if (!parse_uint_arg("-s", arg_offset, argc, argv, val))
        return 1;
      chunk_gb = val;
    } else if (a == "-c") {
      unsigned long long val = 0;
      if (!parse_uint_arg("-c", arg_offset, argc, argv, val))
        return 1;
      if (val > 12) {
        std::cerr << "Error: -c max is 12.\n";
        return 1;
      }
      comp_level = static_cast<int>(val);
    } else if (a == "--encrypt") {
      if (arg_offset + 1 >= argc) {
        std::cerr << "Error: --encrypt requires 'xor' or 'aes'.\n";
        return 1;
      }
      std::string alg = argv[++arg_offset];
      if (alg == "xor")
        enc_opts.algorithm = vfs::EncryptionAlgorithm::Xor;
      else if (alg == "aes")
        enc_opts.algorithm = vfs::EncryptionAlgorithm::Aes256Ctr;
      else {
        std::cerr << "Error: Unknown algorithm '" << alg << "'.\n";
        return 1;
      }
      ++arg_offset;
    } else if (a == "--key") {
      if (arg_offset + 1 >= argc) {
        std::cerr << "Error: --key requires a password.\n";
        return 1;
      }
      enc_opts.key = argv[++arg_offset];
      ++arg_offset;
    } else if (a == "--no-journal") {
      enable_journal = false;
      ++arg_offset;
    } else
      break;
  }

  const uint64_t chunk_bytes = chunk_gb * 1024ULL * 1024 * 1024;

  if (arg_offset >= argc) {
    print_usage(argv[0]);
    return 1;
  }

  // -- Drag-and-drop -------------------------------------------------------
  auto is_cmd = [](const std::string &s) {
    return s == "pack" || s == "packs" || s == "unpack" || s == "list";
  };

  const std::string first_arg = argv[arg_offset];
  const std::filesystem::path dropped(first_arg);

  if (!is_cmd(first_arg) && std::filesystem::exists(dropped)) {
    int rc = 0;

    if (std::filesystem::is_directory(dropped)) {
      const std::string stem =
          (dropped.parent_path() / dropped.filename()).string();
      std::cout << "Drag-and-drop: split-packing " << dropped << "\n"
                << "            => " << stem
                << "_dir.avv  (chunks: " << chunk_gb << " GiB, level "
                << comp_level << ")\n\n";

      vfs::ArchiveWriter writer;
      auto r = writer.pack_directory_split(
          dropped, stem, chunk_bytes, comp_level, render_progress, {}, true);
      if (!r) {
        std::cerr << "Error: " << vfs::error_code_to_string(r.error()) << "\n";
        rc = 1;
      } else {
        std::cout << "\nDone: " << stem << "_dir.avv\n";
      }

    } else if (dropped.extension() == ".avv") {
      const std::string fname = dropped.filename().string();
      std::string stem = fname;
      if (stem.size() > 8 && stem.substr(stem.size() - 8) == "_dir.avv")
        stem = stem.substr(0, stem.size() - 8);
      else if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".avv")
        stem = stem.substr(0, stem.size() - 4);

      const auto out_dir = dropped.parent_path() / stem;
      std::cout << "Drag-and-drop: unpacking " << dropped << "\n"
                << "            => " << out_dir << "/\n\n";

      vfs::ArchiveReader reader;
      auto r = reader.open(dropped);
      if (!r) {
        std::cerr << "Error: " << vfs::error_code_to_string(r.error()) << "\n";
        rc = 1;
      } else {
        r = reader.unpack_all(out_dir, render_progress);
        if (!r) {
          std::cerr << "Error: " << vfs::error_code_to_string(r.error())
                    << "\n";
          rc = 1;
        } else {
          std::cout << "\nDone: " << out_dir << "\n";
        }
      }
    } else {
      std::cerr << "Error: not a directory or .avv file.\n";
      rc = 1;
    }

    std::cout << "\nPress Enter to close...\n";
    std::cin.get();
    return rc;
  }

  // -- Explicit commands ---------------------------------------------------
  const std::string command = first_arg;

  if (command == "list") {
    if (arg_offset + 1 >= argc) {
      std::cerr << "'list' requires an archive path.\n";
      return 1;
    }
    vfs::ArchiveReader reader;
    auto r = reader.open(argv[arg_offset + 1]);
    if (!r) {
      std::cerr << "Error: " << vfs::error_code_to_string(r.error()) << "\n";
      return 1;
    }

    const auto &entries = reader.get_entries();
    std::cout << "Archive: " << argv[arg_offset + 1] << "  (" << entries.size()
              << " files)\n";
    std::cout << "-------------------------------------------------------------"
                 "--------\n";
    uint64_t ts = 0, td = 0;
    for (const auto &e : entries) {
      const char *c = (e.flags & 0x01) ? "LZ4" : "Raw";
      std::printf("  [%03u] %-40s %10s %10s  %s\n", e.chunk_index,
                  e.path.c_str(), format_size(e.size).c_str(),
                  format_size(e.compressed_size).c_str(), c);
      ts += e.size;
      td += e.compressed_size;
    }
    std::cout << "-------------------------------------------------------------"
                 "--------\n";
    std::printf("  Total: %s uncompressed, %s on disk\n",
                format_size(ts).c_str(), format_size(td).c_str());
    return 0;
  }

  if (command == "pack") {
    if (arg_offset + 2 >= argc) {
      print_usage(argv[0]);
      return 1;
    }
    vfs::ArchiveWriter writer;
    auto r = writer.pack_directory(argv[arg_offset + 2], argv[arg_offset + 1],
                                   comp_level, render_progress, enc_opts,
                                   enable_journal);
    if (!r) {
      std::cerr << "Error: " << vfs::error_code_to_string(r.error()) << "\n";
      return 1;
    }
    if (verbose) {
      vfs::ArchiveReader rd;
      if (rd.open(argv[arg_offset + 1]))
        print_entries(rd, "packed");
    }
    std::cout << "Done.\n";
    return 0;
  }

  if (command == "packs") {
    if (arg_offset + 2 >= argc) {
      print_usage(argv[0]);
      return 1;
    }
    vfs::ArchiveWriter writer;
    auto r = writer.pack_directory_split(
        argv[arg_offset + 2], argv[arg_offset + 1], chunk_bytes, comp_level,
        render_progress, enc_opts, enable_journal);
    if (!r) {
      std::cerr << "Error: " << vfs::error_code_to_string(r.error()) << "\n";
      return 1;
    }
    if (verbose) {
      vfs::ArchiveReader rd;
      std::string dir = std::string(argv[arg_offset + 1]) + "_dir.avv";
      if (rd.open(dir))
        print_entries(rd, "packed");
    }
    std::cout << "Done.\n";
    return 0;
  }

  if (command == "unpack") {
    if (arg_offset + 2 >= argc) {
      print_usage(argv[0]);
      return 1;
    }
    vfs::ArchiveReader reader;
    auto r = reader.open(argv[arg_offset + 1]);
    if (!r) {
      std::cerr << "Error: " << vfs::error_code_to_string(r.error()) << "\n";
      return 1;
    }
    if (verbose)
      print_entries(reader, "extract");
    r = reader.unpack_all(argv[arg_offset + 2], render_progress, enc_opts.key);
    if (!r) {
      std::cerr << "Error: " << vfs::error_code_to_string(r.error()) << "\n";
      return 1;
    }
    std::cout << "Done.\n";
    return 0;
  }

  std::cerr << "Unknown command: " << command << "\n";
  print_usage(argv[0]);
  return 1;
}
