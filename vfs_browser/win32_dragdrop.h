/**
 * @file win32_dragdrop.h
 * @brief Native Windows OLE implementation for dragging files out of the
 * browser.
 */
#pragma once

#ifdef _WIN32

#include <shlobj.h>
#include <string>
#include <windows.h>

/**
 * @class DropSource
 * @brief Minimal implementation of IDropSource to handle mouse-state during OLE
 * drag.
 */
// A minimal IDropSource implementation
class DropSource : public IDropSource {
  LONG m_refCount = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (riid == IID_IUnknown || riid == IID_IDropSource) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refCount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = InterlockedDecrement(&m_refCount);
    if (ref == 0)
      delete this;
    return ref;
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL fEscapePressed,
                                              DWORD grfKeyState) override {
    if (fEscapePressed)
      return DRAGDROP_S_CANCEL;
    if (!(grfKeyState & MK_LBUTTON))
      return DRAGDROP_S_DROP;
    return S_OK;
  }
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

public:
  DataObjectDrop(const std::wstring &file) : m_file(file) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&m_refCount);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = InterlockedDecrement(&m_refCount);
    if (ref == 0)
      delete this;
    return ref;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *pformatetcIn,
                                    STGMEDIUM *pmedium) override {
    if (pformatetcIn->cfFormat == CF_HDROP &&
        (pformatetcIn->tymed & TYMED_HGLOBAL)) {
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
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *pformatetc,
                                        STGMEDIUM *pmedium) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *pformatetc) override {
    if (pformatetc->cfFormat == CF_HDROP && (pformatetc->tymed & TYMED_HGLOBAL))
      return S_OK;
    return DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
      FORMATETC *pformatectIn, FORMATETC *pformatetcOut) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *pformatetc, STGMEDIUM *pmedium,
                                    BOOL fRelease) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC **ppenumFormatEtc) override {
    if (dwDirection == DATADIR_GET) {
      FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
      return SHCreateStdEnumFmtEtc(1, &fmt, ppenumFormatEtc);
    }
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *pformatetc, DWORD advf,
                                    IAdviseSink *pAdvSink,
                                    DWORD *pdwConnection) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD dwConnection) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **ppenumAdvise) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
};

/**
 * @brief Synchronously triggers a Win32 OLE drag-and-drop operation for a file.
 * @param win_path Absolute wide-string path to the file on disk to be dragged.
 */
inline void Win32DoDragDrop(const std::wstring &win_path) {
  HRESULT hrOle = OleInitialize(nullptr);
  if (SUCCEEDED(hrOle) || hrOle == RPC_E_CHANGED_MODE) {
    DropSource *src = new DropSource();
    DataObjectDrop *data = new DataObjectDrop(win_path);
    DWORD effect = 0;
    DoDragDrop(data, src, DROPEFFECT_COPY, &effect);
    src->Release();
    data->Release();
    if (SUCCEEDED(hrOle)) {
      OleUninitialize();
    }
  }
}

#endif
