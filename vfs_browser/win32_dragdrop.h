/**
 * @file win32_dragdrop.h
 * @brief Native Windows OLE implementation for dragging files out of the
 * browser.
 */
#pragma once

#ifdef _WIN32

#include <functional>
#include <iterator>
#include <shlobj.h>
#include <string>
#include <windows.h>
#include <commdlg.h>

/**
 * @class DropSource
 * @brief Minimal implementation of IDropSource to handle mouse-state during OLE
 * drag.
 */
// A minimal IDropSource implementation
class DropSource : public IDropSource {
  LONG m_refCount = 1;

public:
  /// @brief COM QueryInterface implementation for IDropSource.
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (riid == IID_IUnknown || riid == IID_IDropSource) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  /// @brief Increments the COM reference count.
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refCount);
  }
  /// @brief Decrements the COM reference count and deletes at zero.
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = InterlockedDecrement(&m_refCount);
    if (ref == 0)
      delete this;
    return ref;
  }
  /// @brief Continues, drops, or cancels the drag based on mouse state.
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL fEscapePressed,
                                              DWORD grfKeyState) override {
    if (fEscapePressed)
      return DRAGDROP_S_CANCEL;
    if (!(grfKeyState & MK_LBUTTON))
      return DRAGDROP_S_DROP;
    return S_OK;
  }
  /// @brief Uses default shell drag cursors.
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD dwEffect) override {
    return DRAGDROP_S_USEDEFAULTCURSORS;
  }
};

/**
 * @class DataObjectDrop
 * @brief Minimal IDataObject implementation that wraps a single file path
 * for CF_HDROP compatibility (Shell drag-and-drop).
 */
// A minimal IDataObject implementation supporting only CF_HDROP
class DataObjectDrop : public IDataObject {
  LONG m_refCount = 1;
  std::wstring m_file;
  std::function<bool()> m_materialize;
  bool m_materialized = false;

public:
  /// @brief Wraps a single filesystem path as a shell data object.
  DataObjectDrop(const std::wstring &file, std::function<bool()> materialize)
      : m_file(file), m_materialize(std::move(materialize)) {}

  /// @brief COM QueryInterface implementation for IDataObject.
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  /// @brief Increments the COM reference count.
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refCount);
  }
  /// @brief Decrements the COM reference count and deletes at zero.
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = InterlockedDecrement(&m_refCount);
    if (ref == 0)
      delete this;
    return ref;
  }
  /// @brief Produces a `CF_HDROP` payload for the wrapped path.
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *pformatetcIn,
                                    STGMEDIUM *pmedium) override {
    if (pformatetcIn->cfFormat == CF_HDROP &&
        (pformatetcIn->tymed & TYMED_HGLOBAL)) {
      if (!m_materialized && m_materialize) {
        if (!m_materialize())
          return E_FAIL;
        m_materialized = true;
      }
      size_t len = m_file.length() +
                   2; // +1 for null, +1 for double-null termination of HDROP
      HGLOBAL hGlobal =
          GlobalAlloc(GHND, sizeof(DROPFILES) + len * sizeof(wchar_t));
      if (!hGlobal)
        return E_OUTOFMEMORY;
      DROPFILES *df = (DROPFILES *)GlobalLock(hGlobal);
      df->pFiles = sizeof(DROPFILES);
      df->fWide = TRUE;
      wchar_t *ptr = (wchar_t *)((char *)df + sizeof(DROPFILES));
      wcscpy_s(ptr, len, m_file.c_str());
      ptr[m_file.length() + 1] = L'\0'; // Double null termination
      GlobalUnlock(hGlobal);
      pmedium->tymed = TYMED_HGLOBAL;
      pmedium->hGlobal = hGlobal;
      pmedium->pUnkForRelease = nullptr;
      return S_OK;
    }
    return DV_E_FORMATETC;
  }
  /// @brief Unsupported for this minimal data object.
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *pformatetc,
                                        STGMEDIUM *pmedium) override {
    return E_NOTIMPL;
  }
  /// @brief Reports support for `CF_HDROP`.
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *pformatetc) override {
    if (pformatetc->cfFormat == CF_HDROP && (pformatetc->tymed & TYMED_HGLOBAL))
      return S_OK;
    return DV_E_FORMATETC;
  }
  /// @brief Unsupported for this minimal data object.
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
      FORMATETC *pformatectIn, FORMATETC *pformatetcOut) override {
    return E_NOTIMPL;
  }
  /// @brief Unsupported for this minimal data object.
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *pformatetc, STGMEDIUM *pmedium,
                                    BOOL fRelease) override {
    return E_NOTIMPL;
  }
  /// @brief Enumerates the single supported format when requested by the shell.
  HRESULT STDMETHODCALLTYPE
  EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC **ppenumFormatEtc) override {
    if (dwDirection == DATADIR_GET) {
      FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
      return SHCreateStdEnumFmtEtc(1, &fmt, ppenumFormatEtc);
    }
    return E_NOTIMPL;
  }
  /// @brief Advisory sinks are not supported.
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *pformatetc, DWORD advf,
                                    IAdviseSink *pAdvSink,
                                    DWORD *pdwConnection) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  /// @brief Advisory sinks are not supported.
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD dwConnection) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  /// @brief Advisory sinks are not supported.
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **ppenumAdvise) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
};

/**
 * @brief Synchronously triggers a Win32 OLE drag-and-drop operation for a file.
 * The materialize callback runs lazily only if the drop target actually
 * requests file data.
 * @param win_path Absolute wide-string path to the temporary file path that
 * will be exposed to the shell.
 * @param materialize Callback that creates the file on disk on demand.
 * @return The shell drop effect, or zero if the drag was canceled/failed.
 */
inline DWORD Win32DoDragDrop(const std::wstring &win_path,
                             std::function<bool()> materialize) {
  HRESULT hrOle = OleInitialize(nullptr);
  DWORD effect = 0;
  if (SUCCEEDED(hrOle) || hrOle == RPC_E_CHANGED_MODE) {
    DropSource *src = new DropSource();
    DataObjectDrop *data = new DataObjectDrop(win_path, std::move(materialize));
    DoDragDrop(data, src, DROPEFFECT_COPY, &effect);
    src->Release();
    data->Release();
    if (SUCCEEDED(hrOle)) {
      OleUninitialize();
    }
  }
  return effect;
}

/**
 * @brief Opens a native Windows file picker restricted to `.avv` archives.
 * @param owner Optional owner window handle.
 * @return Absolute path to the selected archive, or an empty string if the
 * dialog was canceled.
 */
inline std::wstring Win32OpenArchiveDialog(HWND owner = nullptr) {
  wchar_t file_buf[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFile = file_buf;
  ofn.nMaxFile = static_cast<DWORD>(std::size(file_buf));
  ofn.lpstrFilter = L"AVV Archives (*.avv)\0*.avv\0All Files (*.*)\0*.*\0";
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
  ofn.lpstrDefExt = L"avv";
  if (!GetOpenFileNameW(&ofn))
    return L"";
  return std::wstring(file_buf);
}

inline std::wstring Win32OpenFolderDialog(HWND owner = nullptr) {
  BROWSEINFOW bi{};
  bi.hwndOwner = owner;
  bi.lpszTitle = L"Select folder to mount";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
  PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
  if (!pidl)
    return L"";
  wchar_t path_buf[MAX_PATH] = {};
  if (!SHGetPathFromIDListW(pidl, path_buf)) {
    CoTaskMemFree(pidl);
    return L"";
  }
  CoTaskMemFree(pidl);
  return std::wstring(path_buf);
}

#endif
