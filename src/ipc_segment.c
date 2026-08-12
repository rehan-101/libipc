/* ==========================================================================
 * ipc_segment.c — POSIX shared-memory segment lifecycle.
 *
 * Target: Ubuntu 24.04, kernel 6.8, gcc 13.3, glibc 2.39, x86-64.
 * ========================================================================== */

/* -std=c11 defines __STRICT_ANSI__, under which glibc hides every POSIX
 * declaration this file uses. The definition must precede all includes:
 * <features.h> latches its configuration on first inclusion, direct or not. */
#define _POSIX_C_SOURCE 200809L

#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Reads as "SHMC1CPI" in a little-endian hexdump, so identity is checkable by
 * eye. 64 bits is the widest naturally-aligned single-copy-atomic store on
 * x86-64, so no reader can observe a torn magic. */
#define IPC_MAGIC       0x49504331434D4853ULL
#define IPC_VERSION     1
#define IPC_CACHE_LINE  64

/* Cross-process ABI, not an implementation detail: every attaching process
 * reads these exact bytes. Changing a field's type or order requires an
 * IPC_VERSION bump.
 *
 * magic is _Atomic because it is the synchronisation variable. The remaining
 * fields are plain: they are written only before the release store and read
 * only after the acquire load, which is what makes plain access safe.
 *
 * ALIGNMENT / LAYOUT (x86-64 SysV ABI):
 *   offset  0: magic       8 bytes, alignment 8
 *   offset  8: version     4
 *   offset 12: hdr_size    4
 *   offset 16: capacity    8, alignment 8 — already aligned, no padding
 *   offset 24: page_size   4
 *   offset 28: cache_line  4
 *   ------------------------ 32 bytes total, struct alignment 8, no tail pad.
 *
 * The uint32_t pair precedes capacity to avoid a 4-byte hole at offset 12. */
typedef struct {
    _Atomic uint64_t magic;      /* written LAST, via release store          */
    uint32_t         version;
    uint32_t         hdr_size;   /* offset where the data region starts      */
    uint64_t         capacity;   /* usable bytes after the header            */
    uint32_t         page_size;  /* creator's page size                      */
    uint32_t         cache_line; /* creator's cache-line size                */
} ipc_header_t;

/* A two-line header would let a write to the first data object invalidate the
 * header's line in every reader. */
_Static_assert(sizeof(ipc_header_t) <= IPC_CACHE_LINE,
               "ipc_header_t must fit within a single cache line");

/* A non-lock-free _Atomic becomes a libatomic call guarding the value with a
 * mutex private to each address space, synchronising nothing across processes.
 * uint64_t is unsigned long on LP64; both spellings are asserted rather than
 * depending on which the typedef resolves to. */
_Static_assert(ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
               "64-bit atomics must be lock-free for cross-process publishing");

/* Private per-process bookkeeping: holds addresses, so it is malloc'd rather
 * than carved out of the segment. */
struct ipc_segment {
    void         *map_base;
    size_t        map_len;    /* exactly what was passed to mmap             */
    ipc_header_t *hdr;
    void         *data;
    uint64_t      capacity;
};

/* ==========================================================================
 * Helpers
 * ========================================================================== */

static ipc_status_t status_from_errno(int e)
{
    switch (e) {
    case EEXIST: return IPC_ERR_EXISTS;
    case ENOENT: return IPC_ERR_NOTFOUND;
    case EINVAL: return IPC_ERR_SIZE;
    case EFBIG:
    case ENOSPC: return IPC_ERR_SIZE;
    default:     return IPC_ERR_SYS;
    }
}

static void set_status(ipc_status_t *st, ipc_status_t v)
{
    if (st) *st = v;
}

/* Power-of-two `align`. Wraps silently on overflow; callers guard first. */
static size_t round_up_pow2(size_t v, size_t align)
{
    return (v + (align - 1)) & ~(align - 1);
}

/* MONOTONIC, not REALTIME: a backwards NTP step would stretch a timeout. */
static long long elapsed_ms(const struct timespec *start)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0x7fffffffLL;   /* clock unusable: report elapsed, give up    */

    long long sec  = (long long)now.tv_sec  - (long long)start->tv_sec;
    long long nsec = (long long)now.tv_nsec - (long long)start->tv_nsec;
    return sec * 1000LL + nsec / 1000000LL;
}

/* Yields rather than spins: the awaited peer must be scheduled, and a tight
 * spin can delay that. Callers re-check the deadline, so EINTR needs none. */
static void poll_backoff(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 };
    (void)nanosleep(&ts, NULL);
}

/* shm_open names are a flat namespace: glibc maps "/foo" to /dev/shm/foo, so an
 * interior '/' would name a directory nothing creates. Rejected here rather
 * than surfacing later as an opaque ENOENT. */
static int name_is_valid(const char *name)
{
    if (!name || name[0] != '/')
        return 0;
    size_t len = strlen(name);
    if (len < 2 || len > NAME_MAX + 1)
        return 0;
    if (strchr(name + 1, '/') != NULL)
        return 0;
    return 1;
}

/* getpagesize() is hidden without _DEFAULT_SOURCE. Reads the auxv, not a
 * syscall. */
static size_t this_page_size(void)
{
    long ps = sysconf(_SC_PAGESIZE);
    return (ps > 0) ? (size_t)ps : 4096u;
}

/* ==========================================================================
 * ipc_create
 * ========================================================================== */
ipc_segment_t *ipc_create(const char *name, size_t bytes, ipc_status_t *st)
{
    int            fd             = -1;
    void          *map            = MAP_FAILED;
    ipc_segment_t *seg            = NULL;
    int            unlink_on_fail = 0;

    if (!name_is_valid(name)) { set_status(st, IPC_ERR_SYS);  return NULL; }
    if (bytes == 0)           { set_status(st, IPC_ERR_SIZE); return NULL; }

    const size_t page = this_page_size();

    /* Data starts on a cache line so the first object cannot share the
     * header's. Total rounds to a page — the kernel's allocation granularity —
     * so the mmap length matches the object size and every page stays backed. */
    const size_t hdr_size = round_up_pow2(sizeof(ipc_header_t), IPC_CACHE_LINE);

    /* Guards round_up_pow2's (page - 1): a wrap would size the object below
     * the capacity reported to the caller. */
    if (bytes > SIZE_MAX - hdr_size - page) {
        set_status(st, IPC_ERR_SIZE);
        return NULL;
    }

    const size_t total    = round_up_pow2(hdr_size + bytes, page);
    const size_t capacity = total - hdr_size;   /* >= bytes, never less      */

    /* O_EXCL makes test-and-create atomic, so exactly one of two racing
     * creators initialises the header. */
    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        /* unlink_on_fail stays 0: on EEXIST the name belongs to another
         * process, and unlinking would destroy a live segment. */
        set_status(st, status_from_errno(errno));
        return NULL;
    }
    unlink_on_fail = 1;

    /* ftruncate must precede mmap. mmap never validates its length against the
     * inode's i_size — it only builds a VMA — so mapping a zero-length object
     * succeeds and faults on first touch instead. That fault is SIGBUS, not
     * SIGSEGV: the address is mapped, but the backing store has no page for
     * the offset, and the check is lazy, per page. Sizing first makes every
     * page in the mapping legal. (tests/day1_smoke.c sigbus-demo reproduces
     * the inverted ordering deliberately.) */
    if (ftruncate(fd, (off_t)total) != 0) {
        set_status(st, status_from_errno(errno));
        goto fail;
    }

    /* MAP_SHARED, not MAP_PRIVATE: the latter is copy-on-write, so writes
     * would go to a private copy — invisible to peers, correct-looking here. */
    map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        set_status(st, status_from_errno(errno));
        goto fail;
    }

    /* Closing does not unmap: mmap holds its own reference in vma->vm_file,
     * outliving the fd. The inode survives while either a /dev/shm link or any
     * mapping references it. A tmpfs fd has no writeback, so close cannot
     * meaningfully fail and the descriptor is released regardless. */
    (void)close(fd);
    fd = -1;

    seg = (ipc_segment_t *)malloc(sizeof(*seg));
    if (!seg) { set_status(st, IPC_ERR_SYS); goto fail; }
    memset(seg, 0, sizeof(*seg));

    ipc_header_t *h = (ipc_header_t *)map;
    h->version    = IPC_VERSION;
    h->hdr_size   = (uint32_t)hdr_size;
    h->capacity   = (uint64_t)capacity;
    h->page_size  = (uint32_t)page;
    h->cache_line = (uint32_t)IPC_CACHE_LINE;

    /* The release store publishes the header. It forbids the compiler from
     * sinking any of the field stores above past this point, and pairs with
     * the attacher's acquire load to establish happens-before: an attacher
     * that observes IPC_MAGIC is guaranteed to see every preceding write.
     *
     * On x86-64 this costs nothing at runtime. TSO already forbids store-store
     * reordering, so no fence is emitted; the annotation constrains gcc alone.
     * Omitting it passes every test here and corrupts data on aarch64, where
     * the release compiles to a real stlr.
     *
     * VERIFIED — actual -O2 output from `make asm` (gcc 13.3, -march=native),
     * lightly trimmed:
     *
     *     movq    %rax, 16(%r14)   # .capacity
     *     movl    $64,  28(%r14)   # .cache_line
     *     movq    %rdx, 8(%r14)    # <vector(2) unsigned int> version+hdr_size
     *     movabsq $5282796241767188563, %rdx
     *     movq    %rdx, (%r14)     # <-- the release store. Plain mov.
     *
     * The publish is one unfenced movq; the file contains no mfence, lock or
     * xchg. gcc did reorder the plain stores among themselves and fused two of
     * them into a single 8-byte store — freedom the release does not remove —
     * but none was allowed to sink below the magic. */
    atomic_store_explicit(&h->magic, (uint64_t)IPC_MAGIC, memory_order_release);

    seg->map_base = map;
    seg->map_len  = total;
    seg->hdr      = h;
    seg->data     = (void *)((char *)map + hdr_size);
    seg->capacity = (uint64_t)capacity;

    set_status(st, IPC_OK);
    return seg;

fail: {
    /* Reverse acquisition order, leaving no name behind: a name whose magic
     * never appears fails every later create with EEXIST and stalls every
     * attach, and outlives the process. The cleanup calls clobber errno, which
     * ipc_strerror(IPC_ERR_SYS) directs callers to inspect. */
    const int saved_errno = errno;

    if (map != MAP_FAILED) (void)munmap(map, total);
    if (fd >= 0)           (void)close(fd);
    if (unlink_on_fail)    (void)shm_unlink(name);
    free(seg);

    errno = saved_errno;
    return NULL;
}
}

/* ==========================================================================
 * ipc_attach
 * ========================================================================== */
ipc_segment_t *ipc_attach(const char *name, unsigned timeout_ms, ipc_status_t *st)
{
    int             fd    = -1;
    void           *probe = MAP_FAILED;
    void           *map   = MAP_FAILED;
    ipc_segment_t  *seg   = NULL;
    struct timespec start;

    if (!name_is_valid(name)) { set_status(st, IPC_ERR_SYS); return NULL; }

    const size_t page = this_page_size();

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        set_status(st, IPC_ERR_SYS);
        return NULL;
    }

    /* No O_CREAT: a missing name is reported, not conjured into an empty
     * segment awaiting a magic nobody will write. */
    fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        set_status(st, status_from_errno(errno));
        return NULL;
    }

    /* The header is untrusted until the magic verifies: shm_open succeeding
     * proves only that a name exists. Size therefore comes from the kernel via
     * fstat before anything in the segment is read — O_CREAT publishes the
     * directory entry before the creator's ftruncate, so i_size is legitimately
     * observable as 0, and probing that would SIGBUS rather than return. fstat,
     * not stat, so the check binds to the opened inode. */
    struct stat sb;
    size_t total = 0;

    for (;;) {
        if (fstat(fd, &sb) != 0) {
            set_status(st, status_from_errno(errno));
            goto fail;
        }

        /* Whole pages, at least one; the lower bound also rejects a negative
         * st_size before the cast to size_t inflates it. */
        if (sb.st_size >= (off_t)page && ((size_t)sb.st_size % page) == 0) {
            total = (size_t)sb.st_size;
            break;
        }

        if (elapsed_ms(&start) >= (long long)timeout_ms) {
            set_status(st, IPC_ERR_TIMEOUT);
            goto fail;
        }
        poll_backoff();
    }

    if (total < round_up_pow2(sizeof(ipc_header_t), IPC_CACHE_LINE)) {
        set_status(st, IPC_ERR_SIZE);
        goto fail;
    }

    /* One page first, because the full length would have to come from the
     * not-yet-validated header. Validation, not optimisation: a rejected
     * segment costs one 4 KB mapping instead of whatever length it claimed. */
    probe = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (probe == MAP_FAILED) {
        set_status(st, status_from_errno(errno));
        goto fail;
    }

    ipc_header_t *ph = (ipc_header_t *)probe;

    /* Pairs with the creator's release store. Forbids hoisting the field reads
     * below above it, where they could observe pre-initialisation zeroes, and
     * forces a real load each iteration — a plain load would be hoisted out of
     * the loop and never observe the publish. Free on x86-64 TSO. */
    for (;;) {
        uint64_t m = atomic_load_explicit(&ph->magic, memory_order_acquire);
        if (m == (uint64_t)IPC_MAGIC)
            break;

        /* Zero is the legitimate not-yet-published state (ftruncate
         * zero-fills). Any other value is foreign; waiting cannot help. */
        if (m != 0) {
            set_status(st, IPC_ERR_MAGIC);
            goto fail;
        }

        if (elapsed_ms(&start) >= (long long)timeout_ms) {
            set_status(st, IPC_ERR_TIMEOUT);
            goto fail;
        }
        poll_backoff();
    }

    if (ph->version != IPC_VERSION)       { set_status(st, IPC_ERR_VERSION); goto fail; }
    if (ph->page_size != (uint32_t)page)  { set_status(st, IPC_ERR_SIZE);    goto fail; }
    if (ph->cache_line != (uint32_t)IPC_CACHE_LINE)
                                          { set_status(st, IPC_ERR_SIZE);    goto fail; }

    /* Bounded by the probe mapping, cache-line aligned as create guarantees. */
    if (ph->hdr_size < sizeof(ipc_header_t) ||
        ph->hdr_size > page ||
        (ph->hdr_size % IPC_CACHE_LINE) != 0) {
        set_status(st, IPC_ERR_SIZE);
        goto fail;
    }

    /* The kernel's size is authoritative: a disagreeing capacity would put the
     * data region's end past the mapping's. */
    if (ph->capacity != (uint64_t)(total - ph->hdr_size)) {
        set_status(st, IPC_ERR_SIZE);
        goto fail;
    }

    const size_t   hdr_size = (size_t)ph->hdr_size;
    const uint64_t capacity = ph->capacity;

    if (munmap(probe, page) != 0) {
        set_status(st, status_from_errno(errno));
        probe = MAP_FAILED;   /* already gone, or partially so: do not retry  */
        goto fail;
    }
    probe = MAP_FAILED;

    map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        set_status(st, status_from_errno(errno));
        goto fail;
    }

    (void)close(fd);
    fd = -1;

    seg = (ipc_segment_t *)malloc(sizeof(*seg));
    if (!seg) { set_status(st, IPC_ERR_SYS); goto fail; }
    memset(seg, 0, sizeof(*seg));

    /* This base almost certainly differs from the creator's: each kernel
     * searches its own process's address space, and ASLR randomises the result.
     * Only the physical frames are shared, through PTEs in separate page tables.
     *
     * Hence the invariant the design rests on: nothing stored in the segment
     * may be a pointer. An address written by one process names unrelated —
     * often mapped — memory in another, corrupting silently rather than
     * faulting. Shared values are offsets, resolved against each map_base. */
    seg->map_base = map;
    seg->map_len  = total;
    seg->hdr      = (ipc_header_t *)map;
    seg->data     = (void *)((char *)map + hdr_size);
    seg->capacity = capacity;

    set_status(st, IPC_OK);
    return seg;

fail: {
    /* No shm_unlink on any path here: this process did not create the name. */
    const int saved_errno = errno;

    if (probe != MAP_FAILED) (void)munmap(probe, page);
    if (map   != MAP_FAILED) (void)munmap(map, total);
    if (fd >= 0)             (void)close(fd);
    free(seg);

    errno = saved_errno;
    return NULL;
}
}

/* ==========================================================================
 * Accessors
 * ========================================================================== */

void *ipc_base(const ipc_segment_t *seg)
{
    return seg ? seg->data : NULL;
}

size_t ipc_capacity(const ipc_segment_t *seg)
{
    return seg ? (size_t)seg->capacity : 0;
}

ipc_status_t ipc_info(const ipc_segment_t *seg, ipc_info_t *out)
{
    if (!seg || !out)
        return IPC_ERR_SYS;

    /* Re-acquires rather than reusing attach-time values: the snapshot is
     * current, and the plain reads stay ordered after the load. */
    out->magic        = atomic_load_explicit(&seg->hdr->magic, memory_order_acquire);
    out->version      = seg->hdr->version;
    out->hdr_size     = seg->hdr->hdr_size;
    out->capacity     = seg->hdr->capacity;
    out->page_size    = seg->hdr->page_size;
    out->cache_line   = seg->hdr->cache_line;
    out->mapped_bytes = seg->map_len;
    out->map_base     = seg->map_base;
    return IPC_OK;
}

/* ==========================================================================
 * ipc_detach
 * ========================================================================== */
ipc_status_t ipc_detach(ipc_segment_t *seg)
{
    if (!seg)
        return IPC_OK;

    /* map_len must be exactly what mmap received — a shorter length splits the
     * VMA and strands the remainder — so it is held here rather than recomputed
     * from the shared, mutable header. Unmapping drops this view only; the
     * pages belong to the inode's page cache and any peer keeps them. */
    if (munmap(seg->map_base, seg->map_len) != 0) {
        /* Mapping still live: the handle stays valid for a retry, since
         * freeing it would leak the address space. */
        return status_from_errno(errno);
    }

    /* Scrubbed so a use-after-free faults on NULL rather than through a stale
     * pointer into whatever malloc returns next. */
    seg->map_base = NULL;
    seg->hdr      = NULL;
    seg->data     = NULL;
    free(seg);
    return IPC_OK;
}

/* ==========================================================================
 * ipc_destroy
 * ========================================================================== */
ipc_status_t ipc_destroy(const char *name)
{
    if (!name_is_valid(name))
        return IPC_ERR_SYS;

    /* Removes the directory entry only — unlink() semantics, /dev/shm being
     * tmpfs. The inode and its pages persist until the link count and every
     * open reference, mmap included, reach zero: attached peers are unaffected
     * and unnotified, new attachers get ENOENT, and memory is reclaimed at the
     * last munmap rather than here. Skipping the call leaks name and pages past
     * process exit. ENOENT is surfaced rather than ignored — the caller named a
     * segment that was not there. */
    if (shm_unlink(name) != 0)
        return status_from_errno(errno);

    return IPC_OK;
}

/* ========================================================================== */
const char *ipc_strerror(ipc_status_t st)
{
    switch (st) {
    case IPC_OK:           return "success";
    case IPC_ERR_EXISTS:   return "segment already exists";
    case IPC_ERR_NOTFOUND: return "segment not found";
    case IPC_ERR_SIZE:     return "invalid or inconsistent segment size";
    case IPC_ERR_MAGIC:    return "bad magic: not an libipc segment";
    case IPC_ERR_VERSION:  return "unsupported segment version";
    case IPC_ERR_TIMEOUT:  return "timed out waiting for segment to be published";
    case IPC_ERR_SYS:      return "system error (see errno)";
    default:               return "unknown status";
    }
}
