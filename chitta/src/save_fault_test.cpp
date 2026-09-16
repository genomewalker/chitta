// Test-only LD_PRELOAD seam: ENOSPC on snapshot temporary-file writes in one
// explicitly armed scratch store. Never linked into a release executable.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <cstdio>
#include <csignal>
#include <sys/syscall.h>
#include <unistd.h>
extern "C" ssize_t write(int fd, const void* buf, size_t size) {
    using Write = ssize_t (*)(int, const void*, size_t);
    static auto real_write = reinterpret_cast<Write>(dlsym(RTLD_NEXT, "write"));
    const char* arm = std::getenv("CHITTA_CHAOS_ENOSPC_ARM");
    const char* root = std::getenv("CHITTA_CHAOS_STORE");
    const char* pause = std::getenv("CHITTA_CHAOS_PAUSE_ARM");
    if (root && ((arm && access(arm, F_OK) == 0) || (pause && access(pause, F_OK) == 0))) {
        char link[64], path[PATH_MAX];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        auto n = readlink(link, path, sizeof(path)-1);
        if (n > 0) {
            path[n] = 0;
            auto len = std::strlen(root);
            if (std::strncmp(path, root, len) == 0 && path[len] == '/' && std::strstr(path, ".tmp")) {
                if (pause && access(pause, F_OK) == 0) {
                    unlink(pause);
                    raise(SIGSTOP); // Parent kills while a snapshot write is pending.
                    return real_write(fd, buf, size);
                }
                char marker[PATH_MAX];
                std::snprintf(marker, sizeof(marker), "%s.hit", arm);
                int hit = open(marker, O_WRONLY | O_CREAT, 0600);
                if (hit >= 0) close(hit);
                errno = ENOSPC;
                return -1;
            }
        }
    }
    return real_write(fd, buf, size);
}
