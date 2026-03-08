#include "archive_reader.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace vfs {

struct PlatformFileHandle {
  HANDLE handle = INVALID_HANDLE_VALUE;

  ~PlatformFileHandle() {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

void destroy_platform_handle(PlatformFileHandle *h) { delete h; }

PlatformFileHandle *ArchiveReader::get_platform_handle(uint16_t chunk_index) {
  std::lock_guard<std::mutex> lock(handles_mutex_);
  if (platform_handles_.size() <= chunk_index) {
    platform_handles_.resize(chunk_index + 1, nullptr);
  }

  if (!platform_handles_[chunk_index]) {
    std::filesystem::path p = chunk_path_for(chunk_index);
    HANDLE h = CreateFileW(
        p.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);

    if (h != INVALID_HANDLE_VALUE) {
      auto pfh = new PlatformFileHandle();
      pfh->handle = h;
      platform_handles_[chunk_index] = pfh;
    } else {
      return nullptr;
    }
  }

  return platform_handles_[chunk_index];
}

Result<void> platform_read_at(PlatformFileHandle *handle, uint64_t offset,
                              void *buffer, size_t size) {
  if (!handle || handle->handle == INVALID_HANDLE_VALUE)
    return vfs::unexpected<ErrorCode>(ErrorCode::IOError);
  if (size == 0)
    return Result<void>{};

  // ReadFile uses DWORD for sizes; loop to handle buffers larger than 4 GB.
  uint8_t *dst = static_cast<uint8_t *>(buffer);
  uint64_t remaining = size;
  uint64_t current_offset = offset;

  while (remaining > 0) {
    const DWORD chunk =
        static_cast<DWORD>(remaining > MAXDWORD ? MAXDWORD : remaining);

    OVERLAPPED overlapped = {};
    overlapped.Offset = current_offset & 0xFFFFFFFF;
    overlapped.OffsetHigh = (current_offset >> 32) & 0xFFFFFFFF;
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent)
      return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

    DWORD bytes_read = 0;
    BOOL result = ReadFile(handle->handle, dst, chunk, nullptr, &overlapped);

    if (!result && GetLastError() == ERROR_IO_PENDING) {
      result =
          GetOverlappedResult(handle->handle, &overlapped, &bytes_read, TRUE)
              ? TRUE
              : FALSE;
    } else if (result) {
      GetOverlappedResult(handle->handle, &overlapped, &bytes_read, FALSE);
    }

    CloseHandle(overlapped.hEvent);

    if (!result || bytes_read != chunk)
      return vfs::unexpected<ErrorCode>(ErrorCode::IOError);

    dst += chunk;
    remaining -= chunk;
    current_offset += chunk;
  }

  return Result<void>{};
}

} // namespace vfs

#endif
