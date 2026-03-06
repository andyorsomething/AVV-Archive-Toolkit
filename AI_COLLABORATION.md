# AI Collaboration Notes

This project was developed collaboratively with AI coding assistants as a portfolio demonstration of modern C++20 systems programming.

## Tools Used

- **Gemini Antigravity** — Primary AI agent for architecture, code generation, and iterative refinement
- **Multiple concurrent agents** — Separate agents handled independent workstreams (e.g., LZ4 integration ran in parallel with VFB UI development)

## What the AI Did

| Area | AI Contribution |
|---|---|
| Architecture | Designed `.avv` binary format (V1-V3), central directory layout, and split-file VPK-style architecture |
| Core Library | Generated `ArchiveWriter`, `ArchiveReader`, endianness helpers, and `Result<T>` pattern |
| Cryptography | Implemented FNV-1a hashing and AES-256-CTR encryption support bridging tiny-AES-c |
| LZ4 Integration | Integrated LZ4HC level 1-12 frame compression with automatic raw storage fallback |
| CLI Tool | Built argument parser with `pack`, `packs` (split), `unpack`, `list` commands and `-v/-s/-c` flags |
| GUI Browser | Full Dear ImGui application with docking, high-performance hex viewer, search, and threaded extraction |
| Progress UI | Implemented inline CLI progress bars and modal GUI progress bars using a shared callback system |
| Testing | Catch2 suite covering split archives, multi-chunk rollover, corruption, and single-file APIs |
| Build System | Visual Studio solution/project configuration and local dependency vendoring |
| Documentation | Complete Doxygen commentary, comprehensive `README.md`, and technical handoffs |

## What I (the Developer) Did

- Directed all architectural decisions and feature priorities
- Reviewed every generated file and found/fixed subtle bugs (Ctrl+O scope, sort ordering, null-term)
- Managed the Visual Studio solution structure and project GUIDs
- Debugged build configuration issues (duplicate `main()`, missing include paths)
- Finalized AVV3 split archive design and chunk naming convention
- Validated performance of the ImGuiListClipper hex dump on large archives

## Lessons Learned

1. **AI agents excel at boilerplate** — SDL2/ImGui setup, vcxproj XML, and Catch2 scaffolding were generated quickly and accurately.
2. **Build system configuration still needs human oversight** — duplicate source references and missing include paths required manual debugging.
3. **Parallel agents can conflict** — clear project boundaries and sequential tasking for solution/project files prevent merge conflicts.
4. **Shared callback systems simplify multi-UI development** — using a single `ProgressCallback` signature allowed identical progress logic for both CLI and GUI.
5. **Human review is critical for state-sensitive logic** — finding the Ctrl+O "out of scope" popup bug demonstrated the value of a final manual scan over AI-generated UI code.
