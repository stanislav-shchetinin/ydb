/*
 * NFS Fault Injection Library (LD_PRELOAD) — step 4.
 *
 * Intercepts the POSIX filesystem calls that ydb/core/wrappers/fs_storage.cpp
 * exercises against the fake "NFS" export and, with probability
 * NFS_FAULT_RATE, returns a randomly chosen NFS-specific errno instead
 * of going to the kernel. Otherwise the call is forwarded unchanged.
 *
 * Intercepted calls (every errno set mirrors a real NFS failure mode):
 *   - stat/lstat      : cosmetic — rewrite st_dev so mount looks like NFS
 *   - open/openat     : ESTALE, EIO, EACCES, ETIMEDOUT, ENOSPC, EDQUOT
 *   - read/pread      : EIO, ESTALE, ETIMEDOUT
 *   - write/pwrite    : EIO, ENOSPC, EDQUOT, ESTALE, ETIMEDOUT
 *   - fsync/fdatasync : EIO, ENOSPC, ESTALE
 *   - ftruncate       : EIO, ENOSPC, EDQUOT, ESTALE
 *   - flock           : EWOULDBLOCK, ENOLCK           (NFSv4 lock contention)
 *   - rename          : ESTALE, EIO, ETIMEDOUT        (lose a rename race)
 *   - unlink          : ESTALE, EIO, EACCES           (.nfsXXXX lingering)
 *   - mkdir           : ESTALE, EIO, EACCES
 *
 * Environment variables:
 *   NFS_FAULT_PATH   — directory prefix to treat as NFS
 *   NFS_FAULT_RATE   — per-call fault probability (0.0 .. 1.0)
 *   NFS_FAULT_LOG    — log verbosity:
 *                        0 (default) — silent
 *                        1           — rewrites / injected faults / passes
 *   NFS_FAULT_LOG_FD — target fd for logs (default: 2 / stderr)
 *
 * Robustness notes:
 *   * No __attribute__((constructor)): early constructors in other .so's
 *     (notably tcmalloc statically linked into ydbd) must run before we
 *     touch dlsym/malloc, otherwise the process segfaults or tcmalloc
 *     CHECK-fails ("bytes_until_sample_ >= 0 (-NN >= 0)") during startup.
 *     Instead we lazily initialise on the first hook call.
 *   * Initialisation is split in two: config_init() (env + pid, allocation-
 *     free, safe at any time) and syms_init() (dlsym'd libc pointers,
 *     only called from hooks reached after ydbd's main() — currently
 *     stat/lstat).
 *   * open/openat go through syscall(SYS_openat, ...) directly. They are
 *     reached extremely early (libc/tcmalloc internals) and must not
 *     pull in dlsym → calloc → tcmalloc.
 *   * t_in_hook guards against reentrancy during library internals
 *     (dlsym / getenv may internally open files or write diagnostics).
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Avoid dragging <linux/magic.h> (pulls kernel headers in some sysroots). */
#ifndef NFS_SUPER_MAGIC
#define NFS_SUPER_MAGIC 0x6969
#endif

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

static volatile int g_config_inited;   /* env-driven config (safe anywhere)   */
static volatile int g_syms_inited;     /* dlsym'd libc pointers (tcmalloc-ok) */
static char     g_fault_path[4096];
static size_t   g_fault_path_len;
static int      g_log_level;
static int      g_log_fd = 2;
static int      g_pid;
static uint32_t g_fault_rate_q;        /* NFS_FAULT_RATE * (1u<<30) */

/* After each injected fault we keep the next COOLDOWN_OPS calls on the
 * same thread fault-free so that the code under test has a retry
 * window. Without this we occasionally make an operation fail forever
 * in a tight retry loop. */
#define COOLDOWN_OPS 32

/* Reentrancy guard: set to 1 while we are inside library internals
 * (init, logging, etc.) so any nested libc call skips our rewrite. */
static __thread int      t_in_hook;
static __thread int      t_cooldown;
static __thread uint32_t t_rng;
static __thread int      t_rng_init;

static int (*o_stat)(const char *, struct stat *);
static int (*o_lstat)(const char *, struct stat *);

/* Note: we deliberately do NOT dlsym open/openat. ydbd's libc and
 * tcmalloc call open() extremely early — before tcmalloc has finished
 * initialising its per-thread sampler. Going through dlsym would
 * trigger a malloc on that path (dlsym internally allocates via
 * calloc), which tcmalloc then CHECK-fails on with
 *   "bytes_until_sample_ >= 0 (-NN >= 0)"
 * because the thread-local accounting is not yet live.
 *
 * Instead we always forward open/openat straight to the kernel via
 * syscall(SYS_openat, ...). The only downside is losing POSIX's libc
 * wrapper (thread cancellation point, errno setting via __errno_location),
 * which is irrelevant for our fault-injection use case. */

/* ------------------------------------------------------------------ */
/*  Raw syscall fallbacks                                              */
/* ------------------------------------------------------------------ */

static ssize_t raw_write(int fd, const void *buf, size_t n) {
    return syscall(SYS_write, fd, buf, n);
}

static int raw_newfstatat(int dirfd, const char *path,
                          struct stat *buf, int flags) {
    return (int)syscall(SYS_newfstatat, dirfd, path, buf, flags);
}

static int raw_openat(int dirfd, const char *path, int flags, mode_t mode) {
    return (int)syscall(SYS_openat, dirfd, path, flags, mode);
}

static ssize_t raw_read(int fd, void *buf, size_t n) {
    return syscall(SYS_read, fd, buf, n);
}

static ssize_t raw_pread(int fd, void *buf, size_t n, off_t off) {
    return syscall(SYS_pread64, fd, buf, n, off);
}

static ssize_t raw_writev(int fd, const void *buf, size_t n) {
    return syscall(SYS_write, fd, buf, n);
}

static ssize_t raw_pwrite(int fd, const void *buf, size_t n, off_t off) {
    return syscall(SYS_pwrite64, fd, buf, n, off);
}

static int raw_fsync(int fd) {
    return (int)syscall(SYS_fsync, fd);
}

static int raw_fdatasync(int fd) {
    return (int)syscall(SYS_fdatasync, fd);
}

static int raw_ftruncate(int fd, off_t len) {
    return (int)syscall(SYS_ftruncate, fd, len);
}

static int raw_flock(int fd, int op) {
    return (int)syscall(SYS_flock, fd, op);
}

static int raw_renameat(const char *oldp, const char *newp) {
    return (int)syscall(SYS_renameat, AT_FDCWD, oldp, AT_FDCWD, newp);
}

static int raw_unlinkat(const char *path, int flags) {
    return (int)syscall(SYS_unlinkat, AT_FDCWD, path, flags);
}

static int raw_mkdirat(const char *path, mode_t mode) {
    return (int)syscall(SYS_mkdirat, AT_FDCWD, path, mode);
}

static ssize_t raw_readlinkat(int dirfd, const char *path,
                              char *buf, size_t bufsize) {
    return syscall(SYS_readlinkat, dirfd, path, buf, bufsize);
}

/* ------------------------------------------------------------------ */
/*  Initialisation                                                     */
/*                                                                     */
/*  Split into two parts intentionally:                                */
/*                                                                     */
/*    config_init() — reads environ + getpid(). No dlsym, no malloc.   */
/*        Safe to call arbitrarily early (including from the open()    */
/*        hook that runs before tcmalloc is fully up).                 */
/*                                                                     */
/*    syms_init()   — dlsym's the RTLD_NEXT libc pointers we want to   */
/*        use for pass-through. dlsym internally calls calloc(), which */
/*        on ydbd goes through tcmalloc; tcmalloc must have finished   */
/*        its per-thread sampler setup before we get here, or it       */
/*        CHECK-fails with "bytes_until_sample_ >= 0". Only hooks      */
/*        that are known to be reached *after* ydbd's main() (stat,    */
/*        lstat — ydbd does not call them during early constructors)   */
/*        may invoke this.                                             */
/* ------------------------------------------------------------------ */

static void config_init(void) {
    if (__atomic_load_n(&g_config_inited, __ATOMIC_ACQUIRE)) {
        return;
    }

    int save = t_in_hook;
    t_in_hook = 1;

    const char *p = getenv("NFS_FAULT_PATH");
    if (p && p[0]) {
        size_t n = strlen(p);
        if (n < sizeof(g_fault_path)) {
            memcpy(g_fault_path, p, n + 1);
            g_fault_path_len = n;
        }
    }

    const char *r = getenv("NFS_FAULT_RATE");
    if (r) {
        /* We quantise the rate to a 30-bit fixed-point integer so the
         * hot path only needs an integer compare against an xorshift
         * output — no strtod, no double comparisons, no locale. */
        double d = strtod(r, NULL);
        if (d < 0.0) d = 0.0;
        if (d > 1.0) d = 1.0;
        g_fault_rate_q = (uint32_t)(d * (double)(1u << 30));
    }

    const char *lv = getenv("NFS_FAULT_LOG");
    if (lv) {
        int v = atoi(lv);
        g_log_level = v > 0 ? v : 0;
    }

    const char *lfd = getenv("NFS_FAULT_LOG_FD");
    if (lfd) {
        int v = atoi(lfd);
        if (v > 0) g_log_fd = v;
    }

    g_pid = (int)getpid();

    __atomic_store_n(&g_config_inited, 1, __ATOMIC_RELEASE);
    t_in_hook = save;
}

static void syms_init(void) {
    if (__atomic_load_n(&g_syms_inited, __ATOMIC_ACQUIRE)) {
        return;
    }

    int save = t_in_hook;
    t_in_hook = 1;

    o_stat   = (int (*)(const char *, struct stat *)) dlsym(RTLD_NEXT, "stat");
    o_lstat  = (int (*)(const char *, struct stat *)) dlsym(RTLD_NEXT, "lstat");

    __atomic_store_n(&g_syms_inited, 1, __ATOMIC_RELEASE);
    t_in_hook = save;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void log_line(const char *fmt, ...) {
    char buf[512];
    int save = t_in_hook;
    t_in_hook = 1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
        (void)raw_write(g_log_fd, buf, (size_t)n);
    }
    t_in_hook = save;
}

static int path_in_nfs(const char *path) {
    return g_fault_path_len > 0
        && path != NULL
        && strncmp(path, g_fault_path, g_fault_path_len) == 0;
}

/* Resolve fd -> path via /proc/self/fd/N and check whether the backing
 * file lives inside the configured NFS prefix. The caller may pass a
 * NULL out_buf to skip the copy; otherwise the resolved path is left
 * there for use in log lines.
 *
 * All work here is done through direct syscalls (readlinkat) to avoid
 * dragging libc — the hooks that call this (read/write/fsync/...) are
 * invoked from tcmalloc-sensitive code paths.
 *
 * Intended calling pattern: check it only *after* the fault RNG has
 * already decided we want to inject. That keeps the common case —
 * pass-through at zero fault rate — a single syscall with no extra
 * cost. */
static int fd_in_nfs(int fd, char *out_buf, size_t out_size) {
    if (g_fault_path_len == 0 || fd < 0) return 0;

    char link[64];
    char resolved[4096];

    int save = t_in_hook;
    t_in_hook = 1;
    int n_link = snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    t_in_hook = save;

    if (n_link <= 0 || n_link >= (int)sizeof(link)) return 0;

    ssize_t n = raw_readlinkat(AT_FDCWD, link, resolved, sizeof(resolved) - 1);
    if (n <= 0) return 0;
    resolved[n] = '\0';

    if (!path_in_nfs(resolved)) return 0;

    if (out_buf && out_size > 0) {
        size_t copy = (size_t)n < out_size - 1 ? (size_t)n : out_size - 1;
        memcpy(out_buf, resolved, copy);
        out_buf[copy] = '\0';
    }
    return 1;
}

/* -- RNG ----------------------------------------------------------- */

/* 32-bit xorshift. Not cryptographic — just enough mixing to make the
 * fault distribution uncorrelated across threads. Per-thread state,
 * seeded with (wall ns ^ tid-ish ^ stack addr) on first use. No heap
 * allocations anywhere in this path, safe to call from any hook. */
static uint32_t rng_u32(void) {
    if (!t_rng_init) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint32_t seed = (uint32_t)ts.tv_nsec
                      ^ (uint32_t)ts.tv_sec
                      ^ (uint32_t)(uintptr_t)&t_rng
                      ^ (uint32_t)g_pid;
        if (seed == 0) seed = 0x9E3779B9u;
        t_rng = seed;
        t_rng_init = 1;
    }
    uint32_t x = t_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t_rng = x;
    return x;
}

/* -- Fault picker -------------------------------------------------- */

/* Returns a non-zero errno to inject, or 0 to pass the call through to
 * the kernel. Honours the per-thread cooldown counter. */
static int pick_fault(const int *errs, int n_errs) {
    if (g_fault_rate_q == 0) return 0;
    if (t_cooldown > 0) { --t_cooldown; return 0; }

    /* Compare the top 30 bits of the RNG output against the quantised
     * rate: P(fault) == g_fault_rate_q / (1<<30). */
    uint32_t r = rng_u32() >> 2;                  /* 30 bits */
    if (r >= g_fault_rate_q) return 0;

    int idx = (int)(rng_u32() % (uint32_t)n_errs);
    t_cooldown = COOLDOWN_OPS;
    return errs[idx];
}

/* NFS-specific errors that can legitimately surface from open():
 *   ESTALE     — stale NFS file handle
 *   EIO        — generic network / I/O error
 *   EACCES     — stale auth cache, squashed uid
 *   ETIMEDOUT  — NFS server not responding
 *   ENOSPC     — only if the client asked to create the file
 *   EDQUOT     — user quota exceeded on the NFS export
 * Clients are expected to retry most of these (with back-off) except
 * EACCES, which is terminal — that's what we want to exercise in the
 * retry / failure paths of the code under test. */
static const int F_OPEN[]        = { ESTALE, EIO, EACCES, ETIMEDOUT };
static const int F_OPEN_CREATE[] = { ESTALE, EIO, EACCES, ETIMEDOUT, ENOSPC, EDQUOT };
static const int F_READ[]        = { EIO, ESTALE, ETIMEDOUT };
static const int F_WRITE[]       = { EIO, ENOSPC, EDQUOT, ESTALE, ETIMEDOUT };
static const int F_FSYNC[]       = { EIO, ENOSPC, ESTALE };
static const int F_FTRUNC[]      = { EIO, ENOSPC, EDQUOT, ESTALE };
static const int F_FLOCK[]       = { EWOULDBLOCK, ENOLCK };
static const int F_RENAME[]      = { ESTALE, EIO, ETIMEDOUT };
static const int F_UNLINK[]      = { ESTALE, EIO, EACCES };
static const int F_MKDIR[]       = { ESTALE, EIO, EACCES };

#define NELEMS(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const char *errno_name(int err) {
    switch (err) {
        case ESTALE:     return "ESTALE";
        case EIO:        return "EIO";
        case EACCES:     return "EACCES";
        case ETIMEDOUT:  return "ETIMEDOUT";
        case ENOSPC:     return "ENOSPC";
        case EDQUOT:     return "EDQUOT";
        case EWOULDBLOCK:return "EWOULDBLOCK";
        case ENOLCK:     return "ENOLCK";
        default:         return "E?";
    }
}

/* Rewrite st_dev so the struct stat looks like it came from NFS.
 *
 * Real NFS mounts get a synthetic device id from the kernel (major 0,
 * minor encoding the mount sequence), but the visible effect is that
 * the device id differs from the underlying block device. We encode
 * NFS_SUPER_MAGIC in the low bits so anyone who inspects st_dev can
 * recognise it as "NFS-ish" without accidentally colliding with a
 * real local device. */
static void mark_as_nfs(struct stat *buf) {
    buf->st_dev = (dev_t)NFS_SUPER_MAGIC;
}

/* ------------------------------------------------------------------ */
/*  Interceptors: stat / lstat                                         */
/* ------------------------------------------------------------------ */

int stat(const char *path, struct stat *buf) {
    config_init();
    syms_init();

    int rc;
    if (o_stat) {
        rc = o_stat(path, buf);
    } else {
        rc = raw_newfstatat(AT_FDCWD, path, buf, 0);
    }

    if (rc == 0 && !t_in_hook && path_in_nfs(path)) {
        mark_as_nfs(buf);
        if (g_log_level >= 1) {
            log_line("[nfs_fault][pid=%d] stat(%s) -> NFS dev=0x%x\n",
                     g_pid, path, (unsigned)buf->st_dev);
        }
    }
    return rc;
}

int lstat(const char *path, struct stat *buf) {
    config_init();
    syms_init();

    int rc;
    if (o_lstat) {
        rc = o_lstat(path, buf);
    } else {
        rc = raw_newfstatat(AT_FDCWD, path, buf, AT_SYMLINK_NOFOLLOW);
    }

    if (rc == 0 && !t_in_hook && path_in_nfs(path)) {
        mark_as_nfs(buf);
        if (g_log_level >= 1) {
            log_line("[nfs_fault][pid=%d] lstat(%s) -> NFS dev=0x%x\n",
                     g_pid, path, (unsigned)buf->st_dev);
        }
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Interceptors: open / openat                                        */
/*                                                                     */
/*  No fault injection yet — we only observe and forward. The purpose  */
/*  is to verify that intercepting the open() family is safe on ydbd's */
/*  startup path before we start returning synthetic errors.           */
/* ------------------------------------------------------------------ */

/* open/openat only call config_init() (env + pid), never syms_init().
 * That keeps this path malloc-free even when invoked before tcmalloc
 * has finished bringing its per-thread sampler online, while still
 * enabling logging as soon as the first open() fires.
 *
 * The kernel call itself goes through syscall(SYS_openat, ...), so we
 * never need an RTLD_NEXT pointer for open. */

/* Shared fault logic used by both open and openat. Returns:
 *   >0  — errno to inject (caller must set errno and return -1);
 *    0  — pass through to the kernel.
 * Also emits a log line at verbosity >=1 for whichever decision was made. */
static int open_decide_fault(const char *op, const char *path, int flags) {
    if (t_in_hook || !path_in_nfs(path)) {
        return 0;
    }

    const int *tbl = (flags & O_CREAT) ? F_OPEN_CREATE : F_OPEN;
    int n          = (flags & O_CREAT) ? NELEMS(F_OPEN_CREATE) : NELEMS(F_OPEN);

    int e = pick_fault(tbl, n);
    if (e && g_log_level >= 1) {
        log_line("[nfs_fault][pid=%d] %s(%s, 0x%x) -> FAULT %s(%d)\n",
                 g_pid, op, path, (unsigned)flags, errno_name(e), e);
    }
    return e;
}

int open(const char *path, int flags, ...) {
    /* Per POSIX, the mode argument is required iff O_CREAT or O_TMPFILE
     * is set. Using O_TMPFILE constant unconditionally would pull a
     * _GNU_SOURCE dependency; testing O_CREAT is sufficient in practice
     * since O_TMPFILE encodes O_DIRECTORY|O_CREAT on Linux. */
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    config_init();

    int e = open_decide_fault("open", path, flags);
    if (e) { errno = e; return -1; }

    int rc = raw_openat(AT_FDCWD, path, flags, mode);
    int saved_errno = (rc < 0) ? errno : 0;

    if (!t_in_hook && g_log_level >= 1 && path_in_nfs(path)) {
        if (rc >= 0) {
            log_line("[nfs_fault][pid=%d] open(%s, 0x%x, 0%o) -> fd=%d\n",
                     g_pid, path, (unsigned)flags, (unsigned)mode, rc);
        } else {
            log_line("[nfs_fault][pid=%d] open(%s, 0x%x, 0%o) -> errno=%d (real)\n",
                     g_pid, path, (unsigned)flags, (unsigned)mode, saved_errno);
        }
    }

    if (rc < 0) errno = saved_errno;
    return rc;
}

int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    config_init();

    /* Only inject for absolute paths: relative paths would need
     * resolving against dirfd, which is more than we need here.
     * Pass-through is always safe. */
    int do_inject = path[0] == '/';

    if (do_inject) {
        int e = open_decide_fault("openat", path, flags);
        if (e) { errno = e; return -1; }
    }

    int rc = raw_openat(dirfd, path, flags, mode);
    int saved_errno = (rc < 0) ? errno : 0;

    if (do_inject && !t_in_hook && g_log_level >= 1 && path_in_nfs(path)) {
        if (rc >= 0) {
            log_line("[nfs_fault][pid=%d] openat(%s, 0x%x, 0%o) -> fd=%d\n",
                     g_pid, path, (unsigned)flags, (unsigned)mode, rc);
        } else {
            log_line("[nfs_fault][pid=%d] openat(%s, 0x%x, 0%o) -> errno=%d (real)\n",
                     g_pid, path, (unsigned)flags, (unsigned)mode, saved_errno);
        }
    }

    if (rc < 0) errno = saved_errno;
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Interceptors: fd-based I/O (read, write, fsync, ftruncate, flock)  */
/*                                                                     */
/*  All of these first run a cheap RNG filter — if it says "no fault", */
/*  we skip the (more expensive) readlink of /proc/self/fd/N entirely. */
/*  Only when the RNG has already selected this call for injection do  */
/*  we verify that the fd's backing file actually lives inside the     */
/*  configured NFS prefix. This keeps per-call overhead close to a     */
/*  single branch for the common zero-rate / non-NFS case.             */
/* ------------------------------------------------------------------ */

/* Shared fault pick for fd-based hooks: RNG first, fd resolution only
 * on an RNG hit. Logs the fault line if the injection fires. */
static int fd_decide_fault(const char *op, int fd,
                           const int *tbl, int n_tbl) {
    if (t_in_hook) return 0;
    if (g_fault_rate_q == 0) return 0;
    if (t_cooldown > 0) { --t_cooldown; return 0; }

    uint32_t r = rng_u32() >> 2;
    if (r >= g_fault_rate_q) return 0;

    char path[4096];
    if (!fd_in_nfs(fd, path, sizeof(path))) return 0;

    int idx = (int)(rng_u32() % (uint32_t)n_tbl);
    int e = tbl[idx];
    t_cooldown = COOLDOWN_OPS;

    if (g_log_level >= 1) {
        log_line("[nfs_fault][pid=%d] %s(fd=%d, %s) -> FAULT %s(%d)\n",
                 g_pid, op, fd, path, errno_name(e), e);
    }
    return e;
}

ssize_t read(int fd, void *buf, size_t count) {
    config_init();
    int e = fd_decide_fault("read", fd, F_READ, NELEMS(F_READ));
    if (e) { errno = e; return -1; }
    return raw_read(fd, buf, count);
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    config_init();
    int e = fd_decide_fault("pread", fd, F_READ, NELEMS(F_READ));
    if (e) { errno = e; return -1; }
    return raw_pread(fd, buf, count, offset);
}

ssize_t write(int fd, const void *buf, size_t count) {
    config_init();
    int e = fd_decide_fault("write", fd, F_WRITE, NELEMS(F_WRITE));
    if (e) { errno = e; return -1; }
    return raw_writev(fd, buf, count);
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    config_init();
    int e = fd_decide_fault("pwrite", fd, F_WRITE, NELEMS(F_WRITE));
    if (e) { errno = e; return -1; }
    return raw_pwrite(fd, buf, count, offset);
}

int fsync(int fd) {
    config_init();
    int e = fd_decide_fault("fsync", fd, F_FSYNC, NELEMS(F_FSYNC));
    if (e) { errno = e; return -1; }
    return raw_fsync(fd);
}

int fdatasync(int fd) {
    config_init();
    int e = fd_decide_fault("fdatasync", fd, F_FSYNC, NELEMS(F_FSYNC));
    if (e) { errno = e; return -1; }
    return raw_fdatasync(fd);
}

int ftruncate(int fd, off_t length) {
    config_init();
    int e = fd_decide_fault("ftruncate", fd, F_FTRUNC, NELEMS(F_FTRUNC));
    if (e) { errno = e; return -1; }
    return raw_ftruncate(fd, length);
}

int flock(int fd, int operation) {
    config_init();
    int e = fd_decide_fault("flock", fd, F_FLOCK, NELEMS(F_FLOCK));
    if (e) { errno = e; return -1; }
    return raw_flock(fd, operation);
}

/* ------------------------------------------------------------------ */
/*  Interceptors: path-based ops (rename, unlink, mkdir)               */
/* ------------------------------------------------------------------ */

/* Shared fault pick for path-based hooks. `target` is the string put
 * into the log line (for rename it's "oldp -> newp"). */
static int path_decide_fault(const char *op, const char *target,
                             int in_nfs,
                             const int *tbl, int n_tbl) {
    if (t_in_hook || !in_nfs) return 0;
    int e = pick_fault(tbl, n_tbl);
    if (e && g_log_level >= 1) {
        log_line("[nfs_fault][pid=%d] %s(%s) -> FAULT %s(%d)\n",
                 g_pid, op, target, errno_name(e), e);
    }
    return e;
}

int rename(const char *oldpath, const char *newpath) {
    config_init();
    int in_nfs = path_in_nfs(oldpath) || path_in_nfs(newpath);

    if (in_nfs) {
        char target[4096];
        snprintf(target, sizeof(target), "%s -> %s",
                 oldpath ? oldpath : "(null)",
                 newpath ? newpath : "(null)");
        int e = path_decide_fault("rename", target, 1,
                                  F_RENAME, NELEMS(F_RENAME));
        if (e) { errno = e; return -1; }
    }

    return raw_renameat(oldpath, newpath);
}

int unlink(const char *path) {
    config_init();
    int e = path_decide_fault("unlink", path, path_in_nfs(path),
                              F_UNLINK, NELEMS(F_UNLINK));
    if (e) { errno = e; return -1; }
    return raw_unlinkat(path, 0);
}

int mkdir(const char *path, mode_t mode) {
    config_init();
    int e = path_decide_fault("mkdir", path, path_in_nfs(path),
                              F_MKDIR, NELEMS(F_MKDIR));
    if (e) { errno = e; return -1; }
    return raw_mkdirat(path, mode);
}
