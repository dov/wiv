#include "WinDropTarget.h"

#include <spdlog/spdlog.h>

#include <QTimer>

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shellapi.h>

#include <atomic>

namespace givqt
{

namespace
{

FORMATETC hdropFormat()
{
    return FORMATETC{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
}

class FileDropTarget : public IDropTarget
{
public:
    FileDropTarget(HWND hwnd, std::function<void(QStringList)> onDrop) : hwnd_(hwnd), onDrop_(std::move(onDrop))
    {
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
    }
    ~FileDropTarget()
    {
        if (helper_) helper_->Release();
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    // IDropTarget
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
    {
        FORMATETC fmt = hdropFormat();
        accept_ = data->QueryGetData(&fmt) == S_OK;
        update(data, pt, effect, /*enter=*/true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override
    {
        update(nullptr, pt, effect, false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        data_ = nullptr;
        if (helper_) helper_->DragLeave();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
    {
        POINT p{pt.x, pt.y};
        *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        data_ = nullptr;
        if (helper_) helper_->Drop(data, &p, *effect);
        if (!accept_) return S_OK;

        FORMATETC fmt = hdropFormat();
        STGMEDIUM med{};
        if (data->GetData(&fmt, &med) != S_OK) return S_OK;
        QStringList files;
        if (HDROP drop = static_cast<HDROP>(GlobalLock(med.hGlobal)))
        {
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i)
            {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring w(len + 1, L'\0');
                DragQueryFileW(drop, i, w.data(), len + 1);
                w.resize(len);
                files << QString::fromStdWString(w);
            }
            GlobalUnlock(med.hGlobal);
        }
        ReleaseStgMedium(&med);
        if (!files.isEmpty())
            QTimer::singleShot(0, [cb = onDrop_, files]() { cb(files); });
        return S_OK;
    }

private:
    void update(IDataObject* data, POINTL pt, DWORD* effect, bool enter)
    {
        *effect = (accept_ && (*effect & DROPEFFECT_COPY)) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (data) data_ = data;
        // An empty description with no image tells the shell to draw no
        // "Copy to ..." text next to the cursor.
        if (accept_ && data_)
        {
            static const CLIPFORMAT cf = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"DropDescription"));
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPDESCRIPTION)))
            {
                static_cast<DROPDESCRIPTION*>(GlobalLock(h))->type = DROPIMAGE_NOIMAGE;
                GlobalUnlock(h);
                FORMATETC fmt{cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                STGMEDIUM med{};
                med.tymed = TYMED_HGLOBAL;
                med.hGlobal = h;
                if (data_->SetData(&fmt, &med, TRUE) != S_OK) GlobalFree(h);
            }
        }
        POINT p{pt.x, pt.y};
        if (helper_)
        {
            if (enter) helper_->DragEnter(hwnd_, data, &p, *effect);
            else helper_->DragOver(&p, *effect);
        }
    }

    std::atomic<ULONG> refs_{1};
    HWND hwnd_;
    std::function<void(QStringList)> onDrop_;
    IDropTargetHelper* helper_ = nullptr;
    IDataObject* data_ = nullptr; // valid between DragEnter and Drop/DragLeave
    bool accept_ = false;
};

} // namespace

bool installFileDropTarget(QWidget* window, std::function<void(QStringList)> onDrop)
{
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    auto* target = new FileDropTarget(hwnd, std::move(onDrop));
    RevokeDragDrop(hwnd); // in case Qt registered its own
    HRESULT hr = RegisterDragDrop(hwnd, target);
    target->Release(); // RegisterDragDrop holds its own reference
    if (FAILED(hr))
    {
        spdlog::warn("RegisterDragDrop failed: 0x{:08x}", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

} // namespace givqt
