// FolderWatcher backend: inotify, mirroring windows_folder_watch.cpp's
// behavior (recursive tree watch, ~500ms coalescing debounce, clean
// thread-join on stop). inotify has no native recursive/subtree-watch flag
// (unlike ReadDirectoryChangesW's bWatchSubtree), so this walks the tree at
// watch-start and adds one watch per subdirectory, then extends the watch
// set live whenever IN_CREATE reports a new subdirectory.
#include "core/library.h"
#include <sys/inotify.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;

namespace {
constexpr uint32_t kWatchMask =
    IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO |
    IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF;
}

struct FolderWatcher::Impl {
    struct WatchEntry {
        std::string root;
        int         inotifyFd = -1;
        int         stopFd    = -1; // eventfd, written to request stop
        std::thread thread;
        Callback    callback;
        std::unordered_map<int, std::string> wdToPath; // watch descriptor -> absolute path
    };
    std::mutex mu_;
    std::vector<std::unique_ptr<WatchEntry>> entries_;
};

FolderWatcher::FolderWatcher() : impl_(std::make_unique<Impl>()) {}
FolderWatcher::~FolderWatcher() { unwatchAll(); }

namespace {

void addWatchRecursive(int inotifyFd, const std::string& dir,
                        std::unordered_map<int, std::string>& wdToPath) {
    int wd = inotify_add_watch(inotifyFd, dir.c_str(), kWatchMask);
    if (wd < 0) return;
    wdToPath[wd] = dir;

    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        int subWd = inotify_add_watch(inotifyFd, entry.path().c_str(), kWatchMask);
        if (subWd >= 0) wdToPath[subWd] = entry.path().string();
    }
}

} // namespace

void FolderWatcher::watchRoot(const std::string& path, Callback cb) {
    std::lock_guard<std::mutex> lk(impl_->mu_);

    // Don't double-watch
    for (auto& e : impl_->entries_)
        if (e->root == path) return;

    auto entry = std::make_unique<Impl::WatchEntry>();
    entry->root     = path;
    entry->callback = cb;
    entry->inotifyFd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (entry->inotifyFd < 0) {
        printf("[Watcher][ERROR] inotify_init1 failed for: %s\n", path.c_str());
        return;
    }
    entry->stopFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (entry->stopFd < 0) {
        printf("[Watcher][ERROR] eventfd failed for: %s\n", path.c_str());
        close(entry->inotifyFd);
        return;
    }

    addWatchRecursive(entry->inotifyFd, path, entry->wdToPath);
    if (entry->wdToPath.empty()) {
        printf("[Watcher][ERROR] Failed to open directory: %s\n", path.c_str());
        close(entry->inotifyFd);
        close(entry->stopFd);
        return;
    }

    auto* raw = entry.get();
    entry->thread = std::thread([raw]() {
        char buf[64 * 1024] __attribute__((aligned(alignof(inotify_event))));

        while (true) {
            struct pollfd fds[2] = {
                { raw->inotifyFd, POLLIN, 0 },
                { raw->stopFd,    POLLIN, 0 },
            };
            int pr = poll(fds, 2, -1);
            if (pr < 0) break;
            if (fds[1].revents & POLLIN) break; // stop requested

            if (!(fds[0].revents & POLLIN)) continue;

            bool sawEvent = false;
            ssize_t len = read(raw->inotifyFd, buf, sizeof(buf));
            while (len > 0) {
                ssize_t off = 0;
                while (off < len) {
                    auto* ev = reinterpret_cast<inotify_event*>(buf + off);
                    sawEvent = true;

                    if ((ev->mask & IN_CREATE) && (ev->mask & IN_ISDIR)) {
                        // inotify has no recursive-watch flag: a newly created
                        // subdirectory must be explicitly watched or its own
                        // contents' changes would go unseen.
                        auto parentIt = raw->wdToPath.find(ev->wd);
                        if (parentIt != raw->wdToPath.end()) {
                            std::string newDir = parentIt->second + "/" + ev->name;
                            addWatchRecursive(raw->inotifyFd, newDir, raw->wdToPath);
                        }
                    } else if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                        inotify_rm_watch(raw->inotifyFd, ev->wd);
                        raw->wdToPath.erase(ev->wd);
                    }

                    off += sizeof(inotify_event) + ev->len;
                }
                len = read(raw->inotifyFd, buf, sizeof(buf));
            }

            if (!sawEvent) continue;

            // Coalesce: wait 500ms for more changes before notifying, draining
            // anything that arrives in the meantime (same shape as the
            // Windows backend's ReadDirectoryChangesW drain loop).
            while (true) {
                struct pollfd waitFds[2] = {
                    { raw->inotifyFd, POLLIN, 0 },
                    { raw->stopFd,    POLLIN, 0 },
                };
                int wr = poll(waitFds, 2, 500);
                if (wr < 0) break;
                if (waitFds[1].revents & POLLIN) return; // stop requested mid-drain
                if (!(waitFds[0].revents & POLLIN)) break; // 500ms timeout, quiet
                ssize_t drainLen = read(raw->inotifyFd, buf, sizeof(buf));
                (void)drainLen; // just draining; re-loop to re-arm the 500ms wait
            }

            raw->callback(raw->root);
        }
    });

    impl_->entries_.push_back(std::move(entry));
}

void FolderWatcher::unwatchRoot(const std::string& path) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    for (auto it = impl_->entries_.begin(); it != impl_->entries_.end(); ++it) {
        if ((*it)->root == path) {
            uint64_t one = 1;
            [[maybe_unused]] ssize_t w = write((*it)->stopFd, &one, sizeof(one));
            if ((*it)->thread.joinable()) (*it)->thread.join();
            close((*it)->inotifyFd);
            close((*it)->stopFd);
            impl_->entries_.erase(it);
            return;
        }
    }
}

void FolderWatcher::unwatchAll() {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    for (auto& e : impl_->entries_) {
        uint64_t one = 1;
        [[maybe_unused]] ssize_t w = write(e->stopFd, &one, sizeof(one));
        if (e->thread.joinable()) e->thread.join();
        close(e->inotifyFd);
        close(e->stopFd);
    }
    impl_->entries_.clear();
}
