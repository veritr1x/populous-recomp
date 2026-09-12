// os_posix.cpp - the platform layer on macOS and Linux. The only file above
// third_party/ that may include a POSIX or Mach header besides os_win32.cpp.
#include "os.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

struct OsThread {
    pthread_t handle;
};

extern "C" {

OsThread *os_thread_create(void *(*fn)(void *), void *arg, size_t stack_bytes) {
    OsThread *t = (OsThread *)calloc(1, sizeof *t);
    if (!t)
        return nullptr;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_bytes)
        pthread_attr_setstacksize(&attr, stack_bytes);
    int rc = pthread_create(&t->handle, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return nullptr;
    }
    return t;
}
void os_thread_join(OsThread *t) {
    pthread_join(t->handle, nullptr);
    free(t);
}
void os_thread_detach(OsThread *t) {
    pthread_detach(t->handle);
    free(t);
}
void os_thread_exit(void) {
    pthread_exit(nullptr);
}
OsThreadId os_thread_self(void) {
    return (OsThreadId)(uintptr_t)pthread_self();
}
OsThreadId os_thread_id_of(const OsThread *t) {
    return (OsThreadId)(uintptr_t)t->handle;
}

void *os_vm_reserve(size_t bytes) {
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
void os_vm_release(void *p, size_t bytes) {
    munmap(p, bytes);
}

void *os_dlopen(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
void *os_dlopen_noload(const char *path) {
    return dlopen(path, RTLD_NOLOAD | RTLD_LOCAL);
}
void *os_dlsym(void *handle, const char *name) {
    return dlsym(handle, name);
}
int os_dlclose(void *handle) {
    return dlclose(handle);
}
const char *os_dlerror(void) {
    const char *e = dlerror();
    return e ? e : "unknown dynamic loader error";
}
const char *os_plugin_extension(void) {
#ifdef __APPLE__
    return ".dylib";
#else
    return ".so";
#endif
}

static void fill_stat(const struct stat &st, OsStat *out) {
    out->size = (uint64_t)st.st_size;
    out->atime = (int64_t)st.st_atime;
    out->mtime = (int64_t)st.st_mtime;
    out->ctime = (int64_t)st.st_ctime;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->is_regular = S_ISREG(st.st_mode) ? 1 : 0;
    out->is_symlink = S_ISLNK(st.st_mode) ? 1 : 0;
    out->is_readonly = (st.st_mode & S_IWUSR) ? 0 : 1;
}
int os_stat(const char *path, OsStat *out) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
int os_lstat(const char *path, OsStat *out) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
int os_mkdir(const char *path) {
    return mkdir(path, 0755);
}
int os_rename(const char *from, const char *to) {
    return rename(from, to);
}
int os_unlink(const char *path) {
    return unlink(path);
}
int os_getcwd(char *buf, size_t cap) {
    return getcwd(buf, cap) ? 0 : -1;
}
int os_chdir(const char *path) {
    return chdir(path);
}
int os_listdir(const char *dir, OsListDirFn fn, void *user) {
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    while (struct dirent *e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (fn(e->d_name, user) != 0)
            break;
    }
    closedir(d);
    return 0;
}
int os_mkstemp(char *template_path) {
    return mkstemp(template_path);
}

static int native_flags(int flags) {
    int f = 0;
    switch (flags & 3) {
    case OS_O_WRONLY:
        f = O_WRONLY;
        break;
    case OS_O_RDWR:
        f = O_RDWR;
        break;
    default:
        f = O_RDONLY;
        break;
    }
    if (flags & OS_O_CREAT)
        f |= O_CREAT;
    if (flags & OS_O_EXCL)
        f |= O_EXCL;
    if (flags & OS_O_TRUNC)
        f |= O_TRUNC;
    return f;
}
int os_fd_open(const char *path, int flags) {
    return open(path, native_flags(flags), 0644);
}
int64_t os_fd_read(int fd, void *buf, size_t n) {
    return (int64_t)read(fd, buf, n);
}
int64_t os_fd_write(int fd, const void *buf, size_t n) {
    return (int64_t)write(fd, buf, n);
}
int64_t os_fd_seek(int fd, int64_t off, int whence) {
    return (int64_t)lseek(fd, (off_t)off, whence);
}
int os_fd_close(int fd) {
    return close(fd);
}
int os_fd_dup(int fd) {
    return dup(fd);
}
int os_fd_fsync(int fd) {
    return fsync(fd);
}
int os_fd_truncate(int fd, int64_t length) {
    return ftruncate(fd, (off_t)length);
}
int os_fd_stat(int fd, OsStat *out) {
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
void *os_fdopen(int fd, const char *mode) {
    return fdopen(fd, mode);
}

int os_exe_path(char *buf, size_t cap) {
#ifdef __APPLE__
    uint32_t size = (uint32_t)cap;
    return _NSGetExecutablePath(buf, &size) == 0 ? 0 : -1;
#else
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return 0;
#endif
}

static OsFaultFn g_fault_fn = nullptr;
static void fault_trampoline(int sig) {
    const char *what = sig == SIGSEGV   ? "SIGSEGV"
                       : sig == SIGBUS  ? "SIGBUS"
                       : sig == SIGABRT ? "an abort from the runtime"
                                        : "a fatal signal";
    g_fault_fn(what);
}
int os_install_fault_handlers(OsFaultFn fn) {
    g_fault_fn = fn;
    signal(SIGSEGV, fault_trampoline);
    signal(SIGBUS, fault_trampoline);
    signal(SIGABRT, fault_trampoline);
    return 1;
}

uint64_t os_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t os_wall_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}
void os_sleep_us(uint64_t us) {
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000ull);
    ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
    nanosleep(&ts, nullptr);
}

int os_strcasecmp(const char *a, const char *b) {
    return strcasecmp(a, b);
}

} // extern "C"
