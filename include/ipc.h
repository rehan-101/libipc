/* ==========================================================================
 * ipc.h — public interface of libipc.
 *
 * One creator, N attachers. No ring buffer and no locking: the only
 * synchronisation is a release/acquire handshake on the segment header's
 * magic, which is what makes a concurrently-attaching reader safe.
 * ========================================================================== */

#ifndef IPC_H
#define IPC_H

#include <stddef.h>
#include <stdint.h>   /* fixed-width types: these values cross a process
                       * boundary, where `unsigned long` is not a contract */

#ifdef __cplusplus
extern "C" {
#endif

/* Incomplete by design. Exposing the definition would let callers embed it,
 * turning any change to internal bookkeeping into an ABI break. */
typedef struct ipc_segment ipc_segment_t;

/* Returned instead of errno, which is thread-local global state any
 * intervening call may clobber, and too generic to distinguish "wrong layout
 * version" from "bad argument". IPC_ERR_SYS is the escape hatch: errno is
 * preserved across cleanup on the failing paths, so it remains inspectable. */
typedef enum {
    IPC_OK = 0,
    IPC_ERR_EXISTS,     /* create: name already taken                        */
    IPC_ERR_NOTFOUND,   /* attach/destroy: no such name                      */
    IPC_ERR_SIZE,       /* zero, overflowing, or self-inconsistent size      */
    IPC_ERR_MAGIC,      /* mapped, but not a libipc segment                  */
    IPC_ERR_VERSION,    /* a libipc segment of an unsupported layout         */
    IPC_ERR_TIMEOUT,    /* attach: magic never published within the deadline */
    IPC_ERR_SYS         /* see errno                                         */
} ipc_status_t;

/* Header snapshot, taken by value so no pointer into shared memory — mutable
 * by other processes — escapes to the caller. */
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t hdr_size;      /* offset from map_base to the data region       */
    uint64_t capacity;
    uint32_t page_size;     /* of the creating process                       */
    uint32_t cache_line;    /* of the creating process                       */
    size_t   mapped_bytes;  /* this process's mapping length                 */
    void    *map_base;      /* first byte of the mapping = the header        */
} ipc_info_t;

/* `name` must be a POSIX shared-memory name: a leading '/', no other '/', at
 * most NAME_MAX characters after it.
 *
 * `bytes` is a minimum. Actual capacity is rounded up so header plus data
 * occupy whole pages; ipc_capacity() reports the real figure and is always
 * >= bytes. Creation is exclusive, so two racing creators cannot both
 * initialise the same segment; the loser gets IPC_ERR_EXISTS.
 *
 * `st` may be NULL. Returns NULL on failure. */
ipc_segment_t *ipc_create(const char *name, size_t bytes, ipc_status_t *st);

/* A name existing does not mean the segment is ready: shm_open publishes it
 * before the creator has sized or written the header. `timeout_ms` bounds the
 * wait for that window only — a name that does not exist fails immediately
 * with IPC_ERR_NOTFOUND rather than blocking. 0 polls exactly once. */
ipc_segment_t *ipc_attach(const char *name, unsigned timeout_ms, ipc_status_t *st);

/* Start of the data region, past the header. Valid in the calling process
 * only. NULL for a NULL handle. */
void *ipc_base(const ipc_segment_t *seg);

/* Usable bytes at ipc_base(). 0 for a NULL handle. */
size_t ipc_capacity(const ipc_segment_t *seg);

/* IPC_ERR_SYS if either argument is NULL. */
ipc_status_t ipc_info(const ipc_segment_t *seg, ipc_info_t *out);

/* Drops this process's mapping and frees the handle; NULL is accepted so
 * cleanup paths need no guard. Pointers from ipc_base() dangle afterwards. */
ipc_status_t ipc_detach(ipc_segment_t *seg);

/* Removes the name. Memory and existing mappings survive until the last
 * detach — unlink() semantics. */
ipc_status_t ipc_destroy(const char *name);

/* Static storage, never NULL. */
const char *ipc_strerror(ipc_status_t st);

#ifdef __cplusplus
}
#endif

#endif /* IPC_H */
