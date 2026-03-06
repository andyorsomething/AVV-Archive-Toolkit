// ============================================================
// vfs_browser -- Virtual File Browser
// Dear ImGui frontend for the VFS / .avv archive system.
// ============================================================
#include "../vfs_core/archive_reader.h"
#include "../vfs_core/archive_writer.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "imgui.h"
#include "win32_dragdrop.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image/stb_image.h"

// ============================================================
// Helpers
// ============================================================

/// @brief Formats a raw byte count into a human-readable string (e.g. "1.5
/// MB").
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

// ============================================================
// Application State
// ============================================================

/**
 * @struct DirectoryNode
 * @brief Represents a single level in the virtual file system's folder
 * hierarchy.
 */
struct DirectoryNode {
  std::string name; ///< Name of this specific directory (e.g., "textures").
  std::vector<const vfs::ArchiveReader::FileEntry *>
      files; ///< Pointers to files residing within this directory.
  std::vector<std::unique_ptr<DirectoryNode>>
      subdirs; ///< Child directories nested under this node.
  DirectoryNode *parent = nullptr; ///< Pointer to parent node (null for root).
};

/**
 * @struct AppState
 * @brief Central application state shared across all VFB UI panels.
 */
struct AppState {
  /// @brief Heap-allocated reader so it can be safely re-created on new open().
  std::unique_ptr<vfs::ArchiveReader> reader =
      std::make_unique<vfs::ArchiveReader>();
  bool archive_open = false; ///< True once an archive is successfully opened.
  std::string archive_path_str; ///< Display path for the status bar.

  std::optional<vfs::ArchiveReader::FileEntry>
      selected_entry; ///< Currently selected file.
  std::vector<char>
      preview_data; ///< Raw bytes of the selected file (plus \0 sentinel).
  std::string drag_out_pending; ///< Filename pending extraction and drag out.
  char search_buf[256] = {};    ///< Text filter for the explorer table.
  char global_password[128] =
      {}; ///< Decryption password if archive is encrypted.
  char status_msg[512] = "No archive open."; ///< Persistent status bar text.

  GLuint preview_texture = 0; ///< OpenGL texture ID for the image preview.
  int preview_width = 0;      ///< Width of the decoded preview image.
  int preview_height = 0;     ///< Height of the decoded preview image.

  std::unique_ptr<DirectoryNode> root_dir; ///< Virtual directory tree root.
  DirectoryNode *current_dir = nullptr;    ///< Current active directory.

  // ---- Extraction progress state ----
  std::atomic<bool> extracting{false};
  std::atomic<uint32_t> extract_current{0};
  std::atomic<uint32_t> extract_total{0};
  std::string extract_current_file; // protected by extract_mutex
  std::mutex extract_mutex;
  std::thread extract_thread;
  bool extract_succeeded = false;

  /// @brief Attempt to open an archive. Updates status_msg on failure.
  void open(const std::string &path) {
    reader = std::make_unique<vfs::ArchiveReader>(); // fresh instance
    archive_open = false;
    selected_entry.reset();
    preview_data.clear();

    auto result = reader->open(std::filesystem::path(path));
    if (result) {
      archive_open = true;
      archive_path_str = path;
      std::snprintf(status_msg, sizeof(status_msg), "Opened: %s  (%zu files)",
                    path.c_str(), reader->get_entries().size());

      root_dir = std::make_unique<DirectoryNode>();
      root_dir->name = "<root>";
      current_dir = root_dir.get();

      for (const auto &e : reader->get_entries()) {
        std::string p = e.path;
        std::replace(p.begin(), p.end(), '\\', '/');
        size_t pos = 0;
        DirectoryNode *curr = root_dir.get();
        while ((pos = p.find('/')) != std::string::npos) {
          std::string dir = p.substr(0, pos);
          p.erase(0, pos + 1);
          auto it = std::find_if(curr->subdirs.begin(), curr->subdirs.end(),
                                 [&](const std::unique_ptr<DirectoryNode> &n) {
                                   return n->name == dir;
                                 });
          if (it == curr->subdirs.end()) {
            auto node = std::make_unique<DirectoryNode>();
            node->name = dir;
            node->parent = curr;
            curr->subdirs.push_back(std::move(node));
            curr = curr->subdirs.back().get();
          } else {
            curr = it->get();
          }
        }
        if (!p.empty())
          curr->files.push_back(&e);
      }

      std::function<void(DirectoryNode *)> sort_tree =
          [&](DirectoryNode *node) {
            std::sort(node->subdirs.begin(), node->subdirs.end(),
                      [](const std::unique_ptr<DirectoryNode> &a,
                         const std::unique_ptr<DirectoryNode> &b) {
                        return a->name < b->name;
                      });
            std::sort(
                node->files.begin(), node->files.end(),
                [](const vfs::ArchiveReader::FileEntry *a,
                   const vfs::ArchiveReader::FileEntry *b) {
                  return std::filesystem::path(a->path).filename().string() <
                         std::filesystem::path(b->path).filename().string();
                });
            for (auto &child : node->subdirs) {
              sort_tree(child.get());
            }
          };
      sort_tree(root_dir.get());
    } else {
      std::snprintf(status_msg, sizeof(status_msg),
                    "Failed to open '%s' (error %d)", path.c_str(),
                    static_cast<int>(result.error()));
    }
  }

  /// @brief Load the selected entry's raw bytes into preview_data.
  void load_preview(const vfs::ArchiveReader::FileEntry &entry) {
    selected_entry = entry;
    preview_data.clear();

    if (preview_texture) {
      glDeleteTextures(1, &preview_texture);
      preview_texture = 0;
    }

    auto res = reader->read_file_data(entry.path, global_password);
    if (res) {
      preview_data = std::move(res.value());

      std::string ext = std::filesystem::path(entry.path).extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") {
        int channels = 0;
        unsigned char *img = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc *>(preview_data.data()),
            static_cast<int>(preview_data.size()), &preview_width,
            &preview_height, &channels, 4);
        if (img) {
          glGenTextures(1, &preview_texture);
          glBindTexture(GL_TEXTURE_2D, preview_texture);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#if defined(GL_UNPACK_ROW_LENGTH)
          glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, preview_width, preview_height,
                       0, GL_RGBA, GL_UNSIGNED_BYTE, img);
          stbi_image_free(img);
        }
      }
      preview_data.push_back('\0'); // null-terminate for ImGui text display
    } else if (res.error() == vfs::ErrorCode::DecryptionFailed) {
      std::string msg = "Access Denied (Decryption Failed): Enter correct "
                        "password in the top bar.";
      preview_data.assign(msg.begin(), msg.end());
      preview_data.push_back('\0');
    }
  }

  /// @brief Launches threaded unpack_all with progress modal.
  void start_extract_all(const std::filesystem::path &out_dir) {
    if (extracting.load())
      return;
    if (extract_thread.joinable())
      extract_thread.join();

    extract_current.store(0);
    extract_total.store(static_cast<uint32_t>(reader->get_entries().size()));
    extracting.store(true);
    extract_succeeded = false;

    std::string pass = global_password;
    extract_thread = std::thread([this, out_dir, pass]() {
      auto cb = [this](uint32_t cur, uint32_t tot, const std::string &path) {
        extract_current.store(cur);
        extract_total.store(tot);
        std::lock_guard<std::mutex> lk(extract_mutex);
        extract_current_file = path;
      };
      auto r = reader->unpack_all(out_dir, cb, pass);
      extract_succeeded = r.has_value();
      extracting.store(false);
    });
  }

  ~AppState() {
    if (extract_thread.joinable())
      extract_thread.join();
    if (preview_texture)
      glDeleteTextures(1, &preview_texture);
  }
};

// ============================================================
// UI Panels
// ============================================================

/// @brief Renders the main menu bar. Returns true if the user chose to exit.
static bool render_menu_bar(AppState &state, bool &show_hex) {
  bool quit = false;
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Open Archive...", "Ctrl+O"))
        ImGui::OpenPopup("##open_dlg");
      if (state.archive_open && ImGui::MenuItem("Extract All...")) {
        auto out = std::filesystem::current_path() / "extracted";
        state.start_extract_all(out);
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Exit", "Alt+F4"))
        quit = true;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      ImGui::MenuItem("Hex Viewer", nullptr, &show_hex);
      ImGui::EndMenu();
    }
    if (state.archive_open) {
      ImGui::Spacing();
      ImGui::Spacing();
      ImGui::SetNextItemWidth(150.f);
      ImGui::InputTextWithHint(
          "##global_pass", "Password...", state.global_password,
          sizeof(state.global_password), ImGuiInputTextFlags_Password);

      float avail = ImGui::GetContentRegionAvail().x;
      ImGui::SameLine(ImGui::GetWindowWidth() - avail);
      ImGui::TextDisabled("%s", state.archive_path_str.c_str());
    }
    ImGui::EndMainMenuBar();
  }

  // Inline open-archive dialog
  if (ImGui::BeginPopup("##open_dlg")) {
    static char path_buf[512] = {};
    ImGui::Text("Archive path:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360.f);
    ImGui::InputText("##path", path_buf, sizeof(path_buf));
    ImGui::SameLine();
    if (ImGui::Button("Open") && path_buf[0]) {
      state.open(path_buf);
      path_buf[0] = '\0'; // clear for next use
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  return quit;
}

/// @brief Renders the sortable Archive Explorer table with search, right-click
///        context menu, and per-row selection.
static void render_explorer(AppState &state) {
  ImGui::Begin("Archive Explorer");

  ImGui::SetNextItemWidth(-1.f);
  ImGui::InputTextWithHint("##search", "Search files...", state.search_buf,
                           sizeof(state.search_buf));
  ImGui::Separator();

  if (!state.archive_open) {
    ImGui::TextDisabled(
        "Drag an .avv file onto the window, or use File > Open.");
    ImGui::End();
    return;
  }

  // Breadcrumbs
  if (state.current_dir) {
    std::vector<DirectoryNode *> crumbs;
    DirectoryNode *curr = state.current_dir;
    while (curr) {
      crumbs.push_back(curr);
      curr = curr->parent;
    }
    std::reverse(crumbs.begin(), crumbs.end());

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 4));
    for (size_t i = 0; i < crumbs.size(); ++i) {
      if (i > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("/");
        ImGui::SameLine();
      }
      const char *name =
          (crumbs[i]->parent == nullptr) ? "Root" : crumbs[i]->name.c_str();
      if (ImGui::SmallButton(name)) {
        state.current_dir = crumbs[i];
      }
    }
    ImGui::PopStyleVar();
    ImGui::Separator();
  }

  constexpr ImGuiTableFlags TABLE_FLAGS =
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
      ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
      ImGuiTableFlags_ScrollY;

  if (ImGui::BeginTable("##entries", 4, TABLE_FLAGS)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.f);
    ImGui::TableSetupColumn("On Disk", ImGuiTableColumnFlags_WidthFixed, 90.f);
    ImGui::TableSetupColumn("Compression", ImGuiTableColumnFlags_WidthFixed,
                            110.f);
    ImGui::TableHeadersRow();

    const auto &entries = state.reader->get_entries();
    const std::string filter(state.search_buf);

    if (!filter.empty()) {
      std::vector<const vfs::ArchiveReader::FileEntry *> visible;
      visible.reserve(entries.size());
      for (const auto &e : entries) {
        if (e.path.find(filter) != std::string::npos)
          visible.push_back(&e);
      }

      if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsCount > 0) {
          const int col = specs->Specs[0].ColumnIndex;
          const bool asc =
              (specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
          std::sort(visible.begin(), visible.end(),
                    [&](const vfs::ArchiveReader::FileEntry *a,
                        const vfs::ArchiveReader::FileEntry *b) {
                      auto cmp = [&]() -> int {
                        if (col == 0)
                          return a->path.compare(b->path);
                        if (col == 1)
                          return (a->size < b->size)   ? -1
                                 : (a->size > b->size) ? 1
                                                       : 0;
                        if (col == 2)
                          return (a->compressed_size < b->compressed_size) ? -1
                                 : (a->compressed_size > b->compressed_size)
                                     ? 1
                                     : 0;
                        return 0;
                      };
                      return asc ? cmp() < 0 : cmp() > 0;
                    });
          specs->SpecsDirty = false;
        }
      }

      for (const auto *ep : visible) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const bool selected = state.selected_entry.has_value() &&
                              state.selected_entry->path == ep->path;
        if (ImGui::Selectable(ep->path.c_str(), selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
          state.load_preview(*ep);
        }
        if (ImGui::IsItemActive() &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
          state.drag_out_pending = ep->path;
        }
        if (ImGui::BeginPopupContextItem()) {
          ImGui::Text("  %s", ep->path.c_str());
          ImGui::Separator();
          if (ImGui::MenuItem("Copy Path"))
            ImGui::SetClipboardText(ep->path.c_str());
          if (ImGui::MenuItem("Extract to CWD")) {
            auto res = state.reader->extract_file(
                ep->path,
                std::filesystem::current_path() /
                    std::filesystem::path(ep->path).filename(),
                state.global_password);
            if (res)
              std::snprintf(state.status_msg, sizeof(state.status_msg),
                            "Extracted: %s", ep->path.c_str());
            else
              std::snprintf(state.status_msg, sizeof(state.status_msg),
                            "Extract failed");
          }
          ImGui::EndPopup();
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(format_size(ep->size).c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(format_size(ep->compressed_size).c_str());
        ImGui::TableSetColumnIndex(3);
        if (ep->flags & 0x01)
          ImGui::TextColored({0.4f, 0.9f, 0.5f, 1.f}, "LZ4");
        else
          ImGui::TextDisabled("Raw");
      }
    } else if (state.current_dir) {
      if (state.current_dir->parent) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Selectable("[Folder] ..", false,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
          if (ImGui::IsMouseDoubleClicked(0)) {
            state.current_dir = state.current_dir->parent;
          }
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("-");
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted("-");
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted("-");
      }

      for (const auto &subdir : state.current_dir->subdirs) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        std::string label = "[Folder] " + subdir->name;
        if (ImGui::Selectable(label.c_str(), false,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
          if (ImGui::IsMouseDoubleClicked(0)) {
            state.current_dir = subdir.get();
          }
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("-");
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted("-");
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted("-");
      }

      for (const auto *ep : state.current_dir->files) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        std::string filename =
            std::filesystem::path(ep->path).filename().string();
        const bool selected = state.selected_entry.has_value() &&
                              state.selected_entry->path == ep->path;
        if (ImGui::Selectable(filename.c_str(), selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
          state.load_preview(*ep);
        }
        if (ImGui::IsItemActive() &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
          state.drag_out_pending = ep->path;
        }
        if (ImGui::BeginPopupContextItem()) {
          ImGui::Text("  %s", filename.c_str());
          ImGui::Separator();
          if (ImGui::MenuItem("Copy Path"))
            ImGui::SetClipboardText(ep->path.c_str());
          if (ImGui::MenuItem("Extract to CWD")) {
            auto res = state.reader->extract_file(
                ep->path, std::filesystem::current_path() / filename,
                state.global_password);
            if (res)
              std::snprintf(state.status_msg, sizeof(state.status_msg),
                            "Extracted: %s", filename.c_str());
            else
              std::snprintf(state.status_msg, sizeof(state.status_msg),
                            "Extract failed");
          }
          ImGui::EndPopup();
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(format_size(ep->size).c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(format_size(ep->compressed_size).c_str());
        ImGui::TableSetColumnIndex(3);
        if (ep->flags & 0x01)
          ImGui::TextColored({0.4f, 0.9f, 0.5f, 1.f}, "LZ4");
        else
          ImGui::TextDisabled("Raw");
      }
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

/// @brief Renders the File Details panel with text preview tab and optional
///        hex dump tab (with ImGuiListClipper for large files).
static void render_details(AppState &state, bool show_hex) {
  ImGui::Begin("File Details");

  if (!state.selected_entry) {
    ImGui::TextDisabled("Select a file in the Explorer.");
    ImGui::End();
    return;
  }

  const auto &e = *state.selected_entry;
  ImGui::Text("Path:           %s", e.path.c_str());
  ImGui::Text("Uncompressed:   %s", format_size(e.size).c_str());
  ImGui::Text("On Disk:        %s", format_size(e.compressed_size).c_str());
  ImGui::Text("Archive Offset: 0x%llX",
              static_cast<unsigned long long>(e.size_offset));
  ImGui::Text("Compression:    %s", (e.flags & 0x01) ? "LZ4 Frame" : "None");
  ImGui::Separator();

  if (ImGui::BeginTabBar("##preview_tabs")) {
    if (state.preview_texture && ImGui::BeginTabItem("Image")) {
      float max_w = ImGui::GetContentRegionAvail().x;
      float max_h = ImGui::GetContentRegionAvail().y -
                    ImGui::GetTextLineHeightWithSpacing() * 2.0f;
      float asp = (float)state.preview_width / (float)state.preview_height;
      float dis_w = (float)state.preview_width;
      float dis_h = (float)state.preview_height;
      if (dis_w > max_w) {
        dis_w = max_w;
        dis_h = dis_w / asp;
      }
      if (dis_h > max_h) {
        dis_h = max_h;
        dis_w = dis_h * asp;
      }
      ImGui::Image((void *)(intptr_t)state.preview_texture,
                   ImVec2(dis_w, dis_h));
      ImGui::Text("%dx%d Image", state.preview_width, state.preview_height);
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Text")) {
      if (!state.preview_data.empty()) {
        ImGui::InputTextMultiline("##text_preview", state.preview_data.data(),
                                  state.preview_data.size(), ImVec2(-1.f, -1.f),
                                  ImGuiInputTextFlags_ReadOnly);
      } else {
        ImGui::TextDisabled("(empty file)");
      }
      ImGui::EndTabItem();
    }

    if (show_hex && ImGui::BeginTabItem("Hex Dump")) {
      constexpr int COLS = 16;
      // Row layout: "XXXXXXXX  XX XX XX XX XX XX XX XX  XX XX XX XX XX XX XX XX
      // |................|\n" Max per row: 10 (addr+2sp) + COLS*3 + 2 (mid-gap)
      // + 1(|) + COLS + 1(|) + 1(\0) = ~78 chars
      constexpr int LINE_BUF = 96;

      ImGui::BeginChild("##hex", ImVec2(0, 0), false,
                        ImGuiWindowFlags_HorizontalScrollbar);

      const auto &d = state.preview_data;
      // Exclude the sentinel null byte we appended for ImGui text display
      const size_t data_size = d.size() > 0 ? d.size() - 1 : 0;
      const int total_lines = static_cast<int>((data_size + COLS - 1) / COLS);

      ImGuiListClipper clipper;
      clipper.Begin(total_lines);
      while (clipper.Step()) {
        for (int line = clipper.DisplayStart; line < clipper.DisplayEnd;
             ++line) {
          const size_t row_start = static_cast<size_t>(line) * COLS;
          char buf[LINE_BUF];
          int pos = 0;

          // Address column
          pos += std::snprintf(buf + pos, LINE_BUF - pos, "%08X  ",
                               static_cast<unsigned>(row_start));

          // Hex columns (two groups of 8, split by an extra space)
          for (int j = 0; j < COLS; ++j) {
            if (j == COLS / 2)
              buf[pos++] = ' '; // mid-gap for readability
            if (row_start + j < data_size)
              pos +=
                  std::snprintf(buf + pos, LINE_BUF - pos, "%02X ",
                                static_cast<unsigned char>(d[row_start + j]));
            else
              pos += std::snprintf(buf + pos, LINE_BUF - pos, "   ");
          }

          // ASCII column
          buf[pos++] = ' ';
          buf[pos++] = '|';
          for (int j = 0; j < COLS; ++j) {
            if (row_start + j < data_size) {
              const unsigned char c =
                  static_cast<unsigned char>(d[row_start + j]);
              buf[pos++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
            } else {
              buf[pos++] = ' ';
            }
          }
          buf[pos++] = '|';
          buf[pos] = '\0';

          ImGui::TextUnformatted(buf);
        }
      }
      clipper.End();

      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
}

// ============================================================
// Entry Point
// ============================================================

int main(int argc, char **argv) {
  // SDL2 setup
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) !=
      0) {
    std::cerr << "SDL_Init error: " << SDL_GetError() << "\n";
    return -1;
  }
  // Allow users to drag an .avv file directly onto the window
  SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

  SDL_Window *window = SDL_CreateWindow(
      "Virtual File Browser", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      1280, 720,
      static_cast<SDL_WindowFlags>(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                   SDL_WINDOW_ALLOW_HIGHDPI));
  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  SDL_GL_MakeCurrent(window, gl_context);
  SDL_GL_SetSwapInterval(1); // VSync

  // Dear ImGui setup
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // panel docking
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // multi-monitor tear-off

  // Modern dark styling
  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowRounding = 4.f;
  style.FrameRounding = 3.f;
  style.GrabRounding = 3.f;
  style.ScrollbarRounding = 3.f;
  style.TabRounding = 3.f;
  style.Colors[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.13f, 0.15f, 1.f);
  style.Colors[ImGuiCol_Header] = ImVec4(0.22f, 0.40f, 0.62f, 0.79f);
  style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.27f, 0.50f, 0.75f, 0.80f);
  if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    style.Colors[ImGuiCol_WindowBg].w = 1.f;

  ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
  ImGui_ImplOpenGL3_Init("#version 130");

  AppState state;
  bool show_hex = true;

  // Optional: open archive passed via command line
  if (argc >= 2)
    state.open(argv[1]);

  bool done = false;
  while (!done) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);

      if (event.type == SDL_QUIT)
        done = true;
      if (event.type == SDL_WINDOWEVENT &&
          event.window.event == SDL_WINDOWEVENT_CLOSE &&
          event.window.windowID == SDL_GetWindowID(window))
        done = true;

      // Drag-and-drop: open a dropped .avv archive, or append file to an open
      // archive
      if (event.type == SDL_DROPFILE && event.drop.file) {
        std::filesystem::path dropped_path(event.drop.file);
        if (state.archive_open && dropped_path.extension() != ".avv" &&
            std::filesystem::is_regular_file(dropped_path)) {
          vfs::ArchiveWriter writer;
          vfs::EncryptionOptions enc;
          if (state.global_password[0] != '\0') {
            enc.algorithm = vfs::EncryptionAlgorithm::Aes256Ctr;
            enc.key = state.global_password;
          }
          auto res = writer.append_file(
              dropped_path, dropped_path.filename().string(),
              state.archive_path_str, vfs::DEFAULT_COMPRESSION_LEVEL, enc);
          if (res) {
            std::snprintf(state.status_msg, sizeof(state.status_msg),
                          "Appended: %s",
                          dropped_path.filename().string().c_str());
            state.open(state.archive_path_str); // Refresh the view
          } else {
            std::snprintf(state.status_msg, sizeof(state.status_msg),
                          "Failed to append (error %d)",
                          static_cast<int>(res.error()));
          }
        } else {
          state.open(event.drop.file);
        }
        SDL_free(event.drop.file);
      }
    }

    // Ctrl+O shortcut opens the file dialog popup
    // (must be called after NewFrame so ImGui can process it)

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
      ImGui::OpenPopup("##open_dlg");

    // Full-window dockspace
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    constexpr ImGuiWindowFlags DS_FLAGS =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;
    ImGui::Begin("##DockHost", nullptr, DS_FLAGS);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(ImGui::GetID("MainDockspace"));

    if (render_menu_bar(state, show_hex))
      done = true;

    ImGui::End(); // DockHost

    render_explorer(state);
    render_details(state, show_hex);

    // ---- Extraction progress modal ----
    if (state.extracting.load() || ImGui::IsPopupOpen("Extracting")) {
      if (!ImGui::IsPopupOpen("Extracting"))
        ImGui::OpenPopup("Extracting");

      if (ImGui::BeginPopupModal("Extracting", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
        const uint32_t cur = state.extract_current.load();
        const uint32_t tot = state.extract_total.load();
        const float frac = (tot > 0) ? static_cast<float>(cur) / tot : 0.f;

        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%u / %u", cur, tot);
        ImGui::ProgressBar(frac, ImVec2(340.f, 0.f), overlay);

        {
          std::lock_guard<std::mutex> lk(state.extract_mutex);
          ImGui::TextDisabled("%s", state.extract_current_file.c_str());
        }

        if (!state.extracting.load()) {
          // Finished
          if (state.extract_succeeded)
            std::snprintf(state.status_msg, sizeof(state.status_msg),
                          "Extraction complete (%u files)", tot);
          else
            std::snprintf(state.status_msg, sizeof(state.status_msg),
                          "Extraction failed.");
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    }

    // Persistent status bar at the bottom
    ImGui::SetNextWindowPos(
        {vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - 22.f});
    ImGui::SetNextWindowSize({vp->WorkSize.x, 22.f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.18f, 0.18f, 0.20f, 1.f));
    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::TextDisabled("  %s", state.status_msg);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // Trigger Native Win32 Drag-and-Drop if requested
    if (!state.drag_out_pending.empty()) {
      std::string path_to_extract = state.drag_out_pending;
      state.drag_out_pending.clear();

      auto temp_dir = std::filesystem::temp_directory_path();
      auto full_out_path =
          temp_dir / std::filesystem::path(path_to_extract).filename();
      auto res = state.reader->extract_file(path_to_extract, full_out_path,
                                            state.global_password);
      if (res) {
#ifdef _WIN32
        std::wstring wpath = full_out_path.wstring();
        Win32DoDragDrop(wpath);
        // clear the mouse state to avoid imgui sticking to dragging after the
        // blocking DoDragDrop finishes
        ImGui::GetIO().ClearInputMouse();
#endif
        std::snprintf(state.status_msg, sizeof(state.status_msg),
                      "Extracted (Drag & Drop): %s", path_to_extract.c_str());
      } else {
        std::snprintf(state.status_msg, sizeof(state.status_msg),
                      "Drag & Drop failed (error %d)",
                      static_cast<int>(res.error()));
      }
    }

    // Render
    ImGui::Render();
    glViewport(0, 0, static_cast<int>(io.DisplaySize.x),
               static_cast<int>(io.DisplaySize.y));
    glClearColor(0.12f, 0.12f, 0.14f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Multi-viewport support
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      SDL_Window *cur_win = SDL_GL_GetCurrentWindow();
      SDL_GLContext cur_ctx = SDL_GL_GetCurrentContext();
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
      SDL_GL_MakeCurrent(cur_win, cur_ctx);
    }
    SDL_GL_SwapWindow(window);
  }

  // Cleanup
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
