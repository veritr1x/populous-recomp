// os.h - the platform layer: what the runtime, the DirectX shims, the mod
// foundation and the shared host code need from the operating system. One
// POSIX implementation (macOS, Linux) and one Win32 implementation; nothing
// above this header includes a platform header, nothing here knows the guest.
//
// Every function is plain C with C linkage so C plugins and C++ hosts share
// it. Errors are reported the C way: -1 or NULL, errno where POSIX sets it.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Threads. A created thread is an opaque handle; identity is a 64-bit id that
// compares with ==, because the scheduler asks "is the caller this thread"
// far more often than it joins anything.
// ---------------------------------------------------------------------------
typedef struct OsThread OsThread;
typedef uint64_t OsThreadId;
// `stack_bytes` 0 means the platform default. NULL when the thread could not
// be created; nothing has started in that case.
OsThread *os_thread_create(void *(*fn)(void *), void *arg, size_t stack_bytes);
void os_thread_join(OsThread *t);   // waits, then frees the handle
void os_thread_detach(OsThread *t); // frees the handle; the thread runs on
void os_thread_exit(void);          // ends the calling thread; never returns
OsThreadId os_thread_self(void);
OsThreadId os_thread_id_of(const OsThread *t);

// ---------------------------------------------------------------------------
// Virtual memory: a zero-filled, readable, writable, page-aligned region.
// ---------------------------------------------------------------------------
void *os_vm_reserve(size_t bytes); // NULL on failure
void os_vm_release(void *p, size_t bytes);

// ---------------------------------------------------------------------------
// Plugins.
// ---------------------------------------------------------------------------
void *os_dlopen(const char *path);
void *os_dlopen_noload(const char *path); // a handle only if already loaded
void *os_dlsym(void *handle, const char *name);
int os_dlclose(void *handle);          // 0 on success
const char *os_dlerror(void);          // text for the last failure
const char *os_plugin_extension(void); // ".dylib", ".so" or ".dll"

// ---------------------------------------------------------------------------
// Paths. UTF-8 everywhere; the Win32 implementation converts.
// ---------------------------------------------------------------------------
typedef struct OsStat {
    uint64_t size;
    int64_t atime, mtime, ctime; // seconds since the epoch
    int is_dir, is_regular, is_symlink, is_readonly;
} OsStat;
int os_stat(const char *path, OsStat *out);      // follows symlinks; 0 or -1
int os_lstat(const char *path, OsStat *out);     // does not follow
int os_mkdir(const char *path);                  // 0, or -1 (an existing directory is -1, as mkdir)
int os_rename(const char *from, const char *to); // replaces an existing destination file
int os_unlink(const char *path);
int os_rmdir(const char *path);       // the directory must be empty
int os_getcwd(char *buf, size_t cap); // 0 or -1
int os_chdir(const char *path);
// Calls `fn` for every entry except "." and "..", in directory order. A
// nonzero return from `fn` stops the walk. -1 when the directory cannot be
// opened, 0 otherwise.
typedef int (*OsListDirFn)(const char *name, void *user);
int os_listdir(const char *dir, OsListDirFn fn, void *user);
// The trailing XXXXXX of `template_path` is replaced in place; returns an open
// read-write descriptor for the new file, or -1.
int os_mkstemp(char *template_path);

// ---------------------------------------------------------------------------
// Descriptors. Binary mode always; created files are mode 0644.
// ---------------------------------------------------------------------------
enum {
    OS_O_RDONLY = 0,
    OS_O_WRONLY = 1,
    OS_O_RDWR = 2,
    OS_O_CREAT = 0x40,
    OS_O_EXCL = 0x80,
    OS_O_TRUNC = 0x200
};
enum { OS_SEEK_SET = 0, OS_SEEK_CUR = 1, OS_SEEK_END = 2 };
int os_fd_open(const char *path, int flags);
int64_t os_fd_read(int fd, void *buf, size_t n);        // bytes read, 0 at EOF, -1 on error
int64_t os_fd_write(int fd, const void *buf, size_t n); // bytes written, -1 on error
int64_t os_fd_seek(int fd, int64_t off, int whence);    // new offset or -1
int os_fd_close(int fd);
int os_fd_dup(int fd);
int os_fd_fsync(int fd);
int os_fd_truncate(int fd, int64_t length);
int os_fd_stat(int fd, OsStat *out);
// A FILE over an open descriptor, which then owns it.
void *os_fdopen(int fd, const char *mode);

// ---------------------------------------------------------------------------
// Process.
// ---------------------------------------------------------------------------
int os_exe_path(char *buf, size_t cap); // 0 or -1; NUL-terminated
// Report a fatal signal inside guest code. `what` is "SIGSEGV", "SIGBUS" or
// "an abort from the runtime". Returns 1 when handlers were installed and 0
// where the platform has no equivalent yet.
typedef void (*OsFaultFn)(const char *what);
int os_install_fault_handlers(OsFaultFn fn);
// For use inside a fault handler: a raw write to the error stream and an
// immediate process exit, neither of which touches stdio or runs destructors.
void os_write_stderr_raw(const char *s, size_t n);
void os_exit_immediately(int code);

// ---------------------------------------------------------------------------
// Time.
// ---------------------------------------------------------------------------
uint64_t os_monotonic_ns(void); // never goes backwards; arbitrary origin
uint64_t os_wall_time_us(void); // microseconds since the Unix epoch
void os_sleep_us(uint64_t us);

// ---------------------------------------------------------------------------
// Strings.
// ---------------------------------------------------------------------------
int os_strcasecmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
