// FolderWatcher backend: ReadDirectoryChangesW, moved verbatim out of
// library.cpp during the core/ folder reorg (was previously the only reason
// library.cpp needed <windows.h>). Behavior unchanged: recursive tree watch,
// 500ms coalescing debounce, clean thread-join on stop.
#include "core/library.h"
#include <windows.h>
#include <mutex>
#include <thread>
#include <cstdio>

struct FolderWatcher::Impl {
    struct WatchEntry {
        std::string  root;
        HANDLE       dirHandle = INVALID_HANDLE_VALUE;
        HANDLE       stopEvent = nullptr;
        std::thread  thread;
        Callback     callback;
    };
    std::mutex mu_;
    std::vector<std::unique_ptr<WatchEntry>> entries_;
};

FolderWatcher::FolderWatcher() : impl_(std::make_unique<Impl>()) {}
FolderWatcher::~FolderWatcher() { unwatchAll(); }

void FolderWatcher::watchRoot(const std::string& path, Callback cb) {
    std::lock_guard<std::mutex> lk(impl_->mu_);

    // Don't double-watch
    for (auto& e : impl_->entries_)
        if (e->root == path) return;

    auto entry = std::make_unique<Impl::WatchEntry>();
    entry->root     = path;
    entry->callback = cb;
    entry->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    int wl3 = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wl3, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wl3);
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
    entry->dirHandle = CreateFileW(
        wpath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (entry->dirHandle == INVALID_HANDLE_VALUE) {
        printf("[Watcher][ERROR] Failed to open directory: %s\n", path.c_str());
        CloseHandle(entry->stopEvent);
        return;
    }

    auto* raw = entry.get();
    entry->thread = std::thread([raw]() {
        alignas(DWORD) char buf[4096];
        OVERLAPPED ovl = {};
        ovl.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        while (true) {
            ResetEvent(ovl.hEvent);
            DWORD bytesReturned = 0;
            BOOL ok = ReadDirectoryChangesW(
                raw->dirHandle, buf, sizeof(buf), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                &bytesReturned, &ovl, nullptr);

            if (!ok) break;

            HANDLE handles[] = { ovl.hEvent, raw->stopEvent };
            DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

            if (wait == WAIT_OBJECT_0 + 1) break; // stop requested
            if (wait != WAIT_OBJECT_0) break;

            GetOverlappedResult(raw->dirHandle, &ovl, &bytesReturned, FALSE);

            // Coalesce: wait 500ms for more changes before notifying
            Sleep(500);

            // Drain any additional changes that accumulated
            while (true) {
                ResetEvent(ovl.hEvent);
                ReadDirectoryChangesW(raw->dirHandle, buf, sizeof(buf), TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                    &bytesReturned, &ovl, nullptr);
                DWORD drain = WaitForSingleObject(ovl.hEvent, 100);
                if (drain == WAIT_TIMEOUT) {
                    CancelIo(raw->dirHandle);
                    break;
                }
                GetOverlappedResult(raw->dirHandle, &ovl, &bytesReturned, FALSE);
            }

            raw->callback(raw->root);
        }

        CloseHandle(ovl.hEvent);
    });

    impl_->entries_.push_back(std::move(entry));
}

void FolderWatcher::unwatchRoot(const std::string& path) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    for (auto it = impl_->entries_.begin(); it != impl_->entries_.end(); ++it) {
        if ((*it)->root == path) {
            SetEvent((*it)->stopEvent);
            if ((*it)->thread.joinable()) (*it)->thread.join();
            CloseHandle((*it)->dirHandle);
            CloseHandle((*it)->stopEvent);
            impl_->entries_.erase(it);
            return;
        }
    }
}

void FolderWatcher::unwatchAll() {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    for (auto& e : impl_->entries_) {
        SetEvent(e->stopEvent);
        if (e->thread.joinable()) e->thread.join();
        CloseHandle(e->dirHandle);
        CloseHandle(e->stopEvent);
    }
    impl_->entries_.clear();
}
