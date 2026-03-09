/**
 * @file main.cpp
 * @brief VFS CLI - pack, packs, unpack, list, and mounted namespace inspection.
 */
#include "../vfs_core/archive_reader.h"
#include "../vfs_core/archive_writer.h"
#include "../vfs_core/mounted_file_system.h"
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

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

static void print_usage(const char *prog) {
  std::cout << "Usage:\n"
            << "  " << prog
            << " [-v] [-c <1..12>] [--encrypt <xor|aes>] [--key <pass>] "
               "[--no-journal] pack <output.avv> <input_dir>\n"
            << "  " << prog
            << " [-v] [-c <1..12>] [-s <GB>] [--encrypt <xor|aes>] [--key "
               "<pass>] [--no-journal] packs <stem> <input_dir>\n"
            << "  " << prog
            << " [-v] [--key <pass>] unpack <input.avv|_dir.avv> <output_dir>\n"
            << "  " << prog << " list <input.avv|_dir.avv>\n"
            << "  " << prog << " vmount list <virtual_dir> <mount_spec>...\n"
            << "  " << prog << " vmount cat <virtual_path> <mount_spec>...\n"
            << "  " << prog
            << " vmount extract <virtual_path> <output_path> <mount_spec>...\n"
            << "  " << prog << " vmount stat <virtual_path> <mount_spec>...\n"
            << "  " << prog
            << " vmount overlays <virtual_path> <mount_spec>...\n"
            << "\nMounted query modifiers:\n"
            << "  [--unmount <mount-number>]...\n"
            << "\nMount specs:\n"
            << "  --archive <path> [--at <mount>] [--priority <n>] [--key "
               "<pass>] [--case <archive|host|sensitive|insensitive>]\n"
            << "  --dir <path>     [--at <mount>] [--priority <n>] [--case "
               "<archive|host|sensitive|insensitive>]\n";
}

static void render_progress(uint32_t current, uint32_t total,
                            const std::string &path) {
  constexpr int kBarWidth = 20;
  const float fraction =
      (total > 0) ? static_cast<float>(current) / total : 1.0f;
  const int filled = static_cast<int>(fraction * kBarWidth);
  char bar[kBarWidth + 1];
  for (int i = 0; i < kBarWidth; ++i)
    bar[i] = (i < filled) ? '#' : '-';
  bar[kBarWidth] = '\0';

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

static void print_entries(const vfs::ArchiveReader &reader, const char *verb) {
  for (const auto &e : reader.get_entries()) {
    const char *comp = vfs::cde_is_lz4(e.flags) ? "LZ4" : "Raw";
    std::printf("  %s: [%03u] %-36s %10s  [%s]\n", verb, e.chunk_index,
                e.path.c_str(), format_size(e.size).c_str(), comp);
  }
}

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

struct MountSpec {
  vfs::MountedFileSystem::MountSourceKind kind;
  std::filesystem::path path;
  vfs::MountedFileSystem::MountOptions options;
};

struct ParsedMountPlan {
  std::vector<MountSpec> specs;
  std::vector<size_t> unmount_indices;
};

static bool parse_int_arg(const char *name, const std::string &value,
                          int &out) {
  const char *begin = value.data();
  const char *end = begin + value.size();
  auto [ptr, ec] = std::from_chars(begin, end, out);
  if (ec != std::errc{} || ptr != end) {
    std::cerr << "Error: invalid " << name << " value '" << value << "'.\n";
    return false;
  }
  return true;
}

static bool parse_case_policy(const std::string &policy,
                              vfs::PathCasePolicy &out) {
  if (policy == "archive")
    out = vfs::PathCasePolicy::ArchiveExact;
  if (policy == "host")
    out = vfs::PathCasePolicy::HostNative;
  if (policy == "sensitive")
    out = vfs::PathCasePolicy::ForceSensitive;
  if (policy == "insensitive")
    out = vfs::PathCasePolicy::ForceInsensitive;
  if (policy == "archive" || policy == "host" || policy == "sensitive" ||
      policy == "insensitive")
    return true;
  std::cerr << "Error: invalid --case policy '" << policy << "'.\n";
  return false;
}

static bool parse_mount_plan(int start, int argc, char **argv,
                             ParsedMountPlan &plan) {
  int i = start;
  while (i < argc) {
    const std::string arg = argv[i];
    if (arg == "--unmount") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --unmount requires a mount number.\n";
        return false;
      }
      int parsed_index = 0;
      if (!parse_int_arg("--unmount", argv[i + 1], parsed_index))
        return false;
      if (parsed_index <= 0) {
        std::cerr << "Error: --unmount mount number must be 1 or greater.\n";
        return false;
      }
      plan.unmount_indices.push_back(static_cast<size_t>(parsed_index - 1));
      i += 2;
      continue;
    }
    if (arg != "--archive" && arg != "--dir") {
      std::cerr << "Error: expected '--archive' or '--dir', got '" << arg
                << "'.\n";
      return false;
    }
    if (i + 1 >= argc) {
      std::cerr << "Error: mount source path missing.\n";
      return false;
    }

    MountSpec spec;
    spec.kind = (arg == "--archive")
                    ? vfs::MountedFileSystem::MountSourceKind::Archive
                    : vfs::MountedFileSystem::MountSourceKind::HostDirectory;
    spec.path = argv[++i];
    spec.options.mount_point = "/";
    spec.options.priority = 0;
    spec.options.case_policy =
        (spec.kind == vfs::MountedFileSystem::MountSourceKind::Archive)
            ? vfs::PathCasePolicy::ArchiveExact
            : vfs::PathCasePolicy::HostNative;

    ++i;
    while (i < argc) {
      const std::string opt = argv[i];
      if (opt == "--archive" || opt == "--dir" || opt == "--unmount")
        break;
      if (opt == "--at") {
        if (i + 1 >= argc) {
          std::cerr << "Error: --at requires a mount point.\n";
          return false;
        }
        spec.options.mount_point = argv[i + 1];
        i += 2;
      } else if (opt == "--priority") {
        if (i + 1 >= argc) {
          std::cerr << "Error: --priority requires a value.\n";
          return false;
        }
        if (!parse_int_arg("--priority", argv[i + 1], spec.options.priority))
          return false;
        i += 2;
      } else if (opt == "--key") {
        if (i + 1 >= argc) {
          std::cerr << "Error: --key requires a password.\n";
          return false;
        }
        spec.options.password = argv[i + 1];
        i += 2;
      } else if (opt == "--case") {
        if (i + 1 >= argc) {
          std::cerr << "Error: --case requires a policy.\n";
          return false;
        }
        if (!parse_case_policy(argv[i + 1], spec.options.case_policy))
          return false;
        i += 2;
      } else {
        std::cerr << "Error: unknown mount option '" << opt << "'.\n";
        return false;
      }
    }
    plan.specs.push_back(std::move(spec));
  }
  if (plan.specs.empty()) {
    std::cerr << "Error: at least one mount spec is required.\n";
    return false;
  }
  for (size_t mount_index : plan.unmount_indices) {
    if (mount_index >= plan.specs.size()) {
      std::cerr << "Error: --unmount mount number " << (mount_index + 1)
                << " is out of range.\n";
      return false;
    }
  }
  return true;
}

static bool mount_all(vfs::MountedFileSystem &fs,
                      const std::vector<MountSpec> &specs,
                      std::vector<uint32_t> *mounted_ids = nullptr) {
  for (const auto &spec : specs) {
    vfs::Result<uint32_t> res =
        (spec.kind == vfs::MountedFileSystem::MountSourceKind::Archive)
            ? fs.mount_archive(spec.path, spec.options)
            : fs.mount_host_directory(spec.path, spec.options);
    if (!res) {
      std::cerr << "Error: " << vfs::error_code_to_string(res.error()) << "\n";
      return false;
    }
    if (mounted_ids)
      mounted_ids->push_back(res.value());
  }
  return true;
}

static bool apply_unmounts(vfs::MountedFileSystem &fs,
                           const ParsedMountPlan &plan) {
  std::vector<uint32_t> mounted_ids;
  if (!mount_all(fs, plan.specs, &mounted_ids))
    return false;

  for (size_t mount_index : plan.unmount_indices) {
    auto res = fs.unmount(mounted_ids[mount_index]);
    if (!res) {
      std::cerr << "Error: " << vfs::error_code_to_string(res.error()) << "\n";
      return false;
    }
  }
  return true;
}

static int handle_vmount(int argc, char **argv, int arg_offset) {
  if (arg_offset + 1 >= argc) {
    print_usage(argv[0]);
    return 1;
  }

  const std::string action = argv[arg_offset + 1];
  vfs::MountedFileSystem fs;
  ParsedMountPlan plan;

  if (action == "list") {
    if (arg_offset + 2 >= argc) {
      print_usage(argv[0]);
      return 1;
    }
    if (!parse_mount_plan(arg_offset + 3, argc, argv, plan))
      return 1;
    if (!apply_unmounts(fs, plan))
      return 1;
    auto list_res = fs.list_directory(argv[arg_offset + 2]);
    if (!list_res) {
      std::cerr << "Error: " << vfs::error_code_to_string(list_res.error())
                << "\n";
      return 1;
    }
    for (const auto &entry : list_res.value()) {
      std::printf("%c %-40s %10s %s\n", entry.is_directory ? 'D' : 'F',
                  entry.virtual_path.c_str(), format_size(entry.size).c_str(),
                  entry.is_directory ? "<dir>" : entry.source_path.c_str());
    }
    return 0;
  }

  if (action == "cat") {
    if (arg_offset + 2 >= argc) {
      print_usage(argv[0]);
      return 1;
    }
    if (!parse_mount_plan(arg_offset + 3, argc, argv, plan))
      return 1;
    if (!apply_unmounts(fs, plan))
      return 1;
    auto read_res = fs.read_file_data(argv[arg_offset + 2]);
    if (!read_res) {
      std::cerr << "Error: " << vfs::error_code_to_string(read_res.error())
                << "\n";
      return 1;
    }
    if (!read_res.value().empty())
      std::cout.write(read_res.value().data(),
                      static_cast<std::streamsize>(read_res.value().size()));
    return 0;
  }

  if (action == "extract") {
    if (arg_offset + 3 >= argc) {
      print_usage(argv[0]);
      return 1;
    }
    if (!parse_mount_plan(arg_offset + 4, argc, argv, plan))
      return 1;
    if (!apply_unmounts(fs, plan))
      return 1;
    auto res = fs.extract_file(argv[arg_offset + 2], argv[arg_offset + 3]);
    if (!res) {
      std::cerr << "Error: " << vfs::error_code_to_string(res.error()) << "\n";
      return 1;
    }
    return 0;
  }

  if (action == "stat") {
    if (arg_offset + 2 >= argc) {
      print_usage(argv[0]);
      return 1;
    }
    if (!parse_mount_plan(arg_offset + 3, argc, argv, plan))
      return 1;
    if (!apply_unmounts(fs, plan))
      return 1;
    auto stat_res = fs.stat(argv[arg_offset + 2]);
    if (!stat_res) {
      std::cerr << "Error: " << vfs::error_code_to_string(stat_res.error())
                << "\n";
      return 1;
    }
    const auto &entry = stat_res.value();
    std::cout
        << "Virtual:  " << entry.virtual_path << "\n"
        << "Type:     "
        << (entry.is_directory
                ? "Directory"
                : (entry.source_kind ==
                           vfs::MountedFileSystem::MountSourceKind::Archive
                       ? "Archive File"
                       : "Host File"))
        << "\n"
        << "Source:   " << entry.source_root << "\n"
        << "Path:     " << entry.source_path << "\n"
        << "Priority: " << entry.priority << "\n"
        << "Size:     " << format_size(entry.size) << "\n";
    return 0;
  }

  if (action == "overlays") {
    if (arg_offset + 2 >= argc) {
      print_usage(argv[0]);
      return 1;
    }
    if (!parse_mount_plan(arg_offset + 3, argc, argv, plan))
      return 1;
    if (!apply_unmounts(fs, plan))
      return 1;
    auto overlays_res = fs.list_overlays(argv[arg_offset + 2]);
    if (!overlays_res) {
      std::cerr << "Error: " << vfs::error_code_to_string(overlays_res.error())
                << "\n";
      return 1;
    }
    for (const auto &entry : overlays_res.value()) {
      std::printf("%4d  %-40s  %s\n", entry.priority,
                  entry.virtual_path.c_str(),
                  entry.source_root.string().c_str());
    }
    return 0;
  }

  std::cerr << "Unknown vmount action: " << action << "\n";
  return 1;
}

} // namespace

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
      const std::string alg = argv[++arg_offset];
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
    } else {
      break;
    }
  }

  const uint64_t chunk_bytes = chunk_gb * 1024ULL * 1024 * 1024;

  if (arg_offset >= argc) {
    print_usage(argv[0]);
    return 1;
  }

  auto is_cmd = [](const std::string &s) {
    return s == "pack" || s == "packs" || s == "unpack" || s == "list" ||
           s == "vmount";
  };

  const std::string first_arg = argv[arg_offset];
  const std::filesystem::path dropped(first_arg);
  if (!is_cmd(first_arg) && std::filesystem::exists(dropped)) {
    int rc = 0;
    if (std::filesystem::is_directory(dropped)) {
      const std::string stem =
          (dropped.parent_path() / dropped.filename()).string();
      vfs::ArchiveWriter writer;
      auto r = writer.pack_directory_split(dropped, stem, chunk_bytes,
                                           comp_level, render_progress,
                                           enc_opts, enable_journal);
      if (!r) {
        std::cerr << "Error: " << vfs::error_code_to_string(r.error()) << "\n";
        rc = 1;
      }
    } else if (dropped.extension() == ".avv") {
      std::string stem = dropped.stem().string();
      if (stem.size() > 4 && stem.substr(stem.size() - 4) == "_dir")
        stem = stem.substr(0, stem.size() - 4);
      const auto out_dir = dropped.parent_path() / stem;
      vfs::ArchiveReader reader;
      auto r = reader.open(dropped);
      if (!r) {
        std::cerr << "Error: " << vfs::error_code_to_string(r.error()) << "\n";
        rc = 1;
      } else {
        r = reader.unpack_all(out_dir, render_progress, enc_opts.key);
        if (!r) {
          std::cerr << "Error: " << vfs::error_code_to_string(r.error())
                    << "\n";
          rc = 1;
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

  const std::string command = first_arg;
  if (command == "vmount")
    return handle_vmount(argc, argv, arg_offset);

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
    uint64_t ts = 0, td = 0;
    for (const auto &e : entries) {
      std::printf("  [%03u] %-40s %10s %10s  %s\n", e.chunk_index,
                  e.path.c_str(), format_size(e.size).c_str(),
                  format_size(e.compressed_size).c_str(),
                  vfs::cde_is_lz4(e.flags) ? "LZ4" : "Raw");
      ts += e.size;
      td += e.compressed_size;
    }
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
      const std::string dir = std::string(argv[arg_offset + 1]) + "_dir.avv";
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
