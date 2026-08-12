/* ==========================================================================
 * day1_smoke.c — two-process driver for the shared-memory substrate.
 *
 * Not an assert-driven test. It exists to make two facts observable that a
 * single process cannot demonstrate: the mapped base addresses in two
 * processes differ, and the bytes at those addresses are the same bytes.
 *
 *   Terminal 1:  ./day1_smoke create /ipc_demo 65536
 *   Terminal 2:  ./day1_smoke attach /ipc_demo
 *   Terminal 1:  Ctrl-C   -> prints what terminal 2 wrote
 *
 *   ./day1_smoke sigbus-demo   -> deliberately crashes with SIGBUS
 * ========================================================================== */
#define _POSIX_C_SOURCE 200809L

#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* volatile sig_atomic_t is the only type C guarantees safe to share between a
 * handler and normal code; volatile also stops the wait loop's test from being
 * hoisted, which would make Ctrl-C unable to break it. */
static volatile sig_atomic_t g_stop = 0;

/* Sets a flag and nothing else: stdio is not async-signal-safe, and a printf
 * interrupted mid-update and re-entered here deadlocks or corrupts the heap. */
static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int install_sigint(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: nanosleep should fail with EINTR so the loop re-checks
     * g_stop immediately rather than finishing its nap first. */
    sa.sa_flags = 0;
    return sigaction(SIGINT, &sa, NULL);
}

/* The values to compare across terminals: header fields must match exactly,
 * map base and data base must not. */
static void dump(const char *role, const ipc_segment_t *seg)
{
    ipc_info_t info;
    if (ipc_info(seg, &info) != IPC_OK) {
        fprintf(stderr, "%s: ipc_info failed\n", role);
        return;
    }

    void *data = ipc_base(seg);

    printf("---------------- %s (pid %ld) ----------------\n",
           role, (long)getpid());
    printf("  map base (header) : %p\n", (void *)info.map_base);
    printf("  data base         : %p\n", (void *)data);
    printf("  data - map        : %td bytes\n",
           (ptrdiff_t)((char *)data - (char *)info.map_base));
    printf("  mapped bytes      : %zu\n", info.mapped_bytes);
    printf("  --- header contents (shared, must match across processes) ---\n");
    printf("  magic             : 0x%016" PRIx64 "\n", info.magic);
    printf("  version           : %" PRIu32 "\n", info.version);
    printf("  hdr_size          : %" PRIu32 "\n", info.hdr_size);
    printf("  capacity          : %" PRIu64 "\n", info.capacity);
    printf("  page_size         : %" PRIu32 "\n", info.page_size);
    printf("  cache_line        : %" PRIu32 "\n", info.cache_line);
    printf("  ipc_capacity()    : %zu\n", ipc_capacity(seg));
    printf("------------------------------------------------\n");
    fflush(stdout);
}

/* ========================================================================== */
static int mode_create(const char *name, const char *size_str)
{
    char *end = NULL;
    errno = 0;
    unsigned long long want = strtoull(size_str, &end, 0);
    if (errno != 0 || end == size_str || *end != '\0' || want == 0) {
        fprintf(stderr, "bad size: %s\n", size_str);
        return 2;
    }

    ipc_status_t st;
    ipc_segment_t *seg = ipc_create(name, (size_t)want, &st);
    if (!seg) {
        fprintf(stderr, "ipc_create(%s, %llu) failed: %s\n",
                name, want, ipc_strerror(st));
        if (st == IPC_ERR_EXISTS)
            fprintf(stderr, "  hint: ./day1_smoke destroy %s\n", name);
        return 1;
    }

    printf("created %s (requested %llu bytes)\n", name, want);
    dump("CREATOR", seg);

    /* A recognisable pattern, so the attacher's readback proves it is seeing
     * this process's write rather than plausible-looking garbage. */
    char *data = (char *)ipc_base(seg);
    snprintf(data, ipc_capacity(seg), "HELLO-FROM-CREATOR pid=%ld", (long)getpid());
    printf("creator wrote: \"%s\"\n", data);

    if (install_sigint() != 0) {
        perror("sigaction");
        (void)ipc_detach(seg);
        return 1;
    }

    printf("\nwaiting for SIGINT (Ctrl-C). Now run in another terminal:\n");
    printf("    ./day1_smoke attach %s\n\n", name);
    fflush(stdout);

    /* Holds the mapping open so a second process has something to attach to. */
    while (!g_stop) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200L * 1000 * 1000 };
        (void)nanosleep(&ts, NULL);
    }

    printf("\n\nSIGINT received. Reading the data region back:\n");
    /* Bounded: another process wrote this, and nothing guarantees a NUL
     * anywhere within capacity. */
    int show = (int)(ipc_capacity(seg) < 96 ? ipc_capacity(seg) : 96);
    printf("  data[0..%d] = \"%.*s\"\n", show, show, (const char *)ipc_base(seg));
    dump("CREATOR (after SIGINT)", seg);

    ipc_status_t d = ipc_detach(seg);
    printf("ipc_detach: %s\n", ipc_strerror(d));

    ipc_status_t r = ipc_destroy(name);
    printf("ipc_destroy: %s\n", ipc_strerror(r));
    return 0;
}

/* ========================================================================== */
static int mode_attach(const char *name)
{
    ipc_status_t st;
    /* This timeout covers only the window between the creator's shm_open and
     * its release store. Starting this process first yields IPC_ERR_NOTFOUND
     * immediately — attach-before-create is a caller-side retry loop, not a
     * longer timeout. */
    ipc_segment_t *seg = ipc_attach(name, 2000, &st);
    if (!seg) {
        fprintf(stderr, "ipc_attach(%s) failed: %s\n", name, ipc_strerror(st));
        return 1;
    }

    printf("attached to %s\n", name);
    dump("ATTACHER", seg);

    printf("reading what the creator left: \"%.64s\"\n",
           (const char *)ipc_base(seg));

    /* Offset 64 keeps the creator's string intact so both are visible at once
     * in a hexdump of /dev/shm. */
    const size_t off = 64;
    if (ipc_capacity(seg) > off + 64) {
        char *slot = (char *)ipc_base(seg) + off;
        snprintf(slot, 64, "HELLO-FROM-ATTACHER pid=%ld", (long)getpid());
        printf("attacher wrote at offset %zu: \"%s\"\n", off, slot);
        printf("  (that store went straight to the shared physical frame -\n"
               "   no syscall, no copy. Ctrl-C the creator to see it.)\n");
    }

    /* Overwrites the head too, so the creator's readback unambiguously shows
     * attacher-written bytes. */
    snprintf((char *)ipc_base(seg), 64, "WRITTEN-BY-ATTACHER pid=%ld", (long)getpid());

    ipc_status_t d = ipc_detach(seg);
    printf("ipc_detach: %s\n", ipc_strerror(d));
    return 0;
}

/* ==========================================================================
 * Reproduces the failure ipc_create's ftruncate-before-mmap ordering prevents:
 * the object stays 0 bytes, mmap succeeds anyway because it never consults
 * i_size, and the first store faults. SIGBUS, not SIGSEGV — the address is
 * mapped, but no page can back that offset. Expect "Bus error (core dumped)".
 * ========================================================================== */
static int mode_sigbus_demo(void)
{
    const char *name = "/ipc_sigbus_demo";

    (void)shm_unlink(name);   /* leftover from a previous run; ENOENT is fine */

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open");
        return 1;
    }

    /* Unlinked immediately while the fd still holds the inode alive, so the
     * name cannot leak when this process dies in a few microseconds. */
    (void)shm_unlink(name);

    /* No ftruncate. That is the bug being demonstrated. */

    long ps = sysconf(_SC_PAGESIZE);
    size_t page = (ps > 0) ? (size_t)ps : 4096u;

    void *p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        (void)close(fd);
        return 1;
    }

    printf("sigbus-demo:\n");
    printf("  shm_open ok, ftruncate SKIPPED (object size is 0)\n");
    printf("  mmap(%zu bytes) SUCCEEDED and returned %p\n", page, p);
    printf("  <- note that mmap did not complain. The size check is lazy,\n");
    printf("     deferred to the page-fault handler, per page.\n");
    printf("  now storing one byte at %p ...\n", p);

    /* Required: stdout is block-buffered when redirected, and the process is
     * about to die from a signal without running atexit handlers. */
    fflush(stdout);

    *(volatile char *)p = 0x42;

    printf("  ...no SIGBUS?! The object had a nonzero size after all.\n");
    (void)munmap(p, page);
    (void)close(fd);
    return 0;
}

/* ========================================================================== */
static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage:\n"
        "  %s create <name> <bytes>   create a segment, hold it until SIGINT\n"
        "  %s attach <name>           attach, print, read and write\n"
        "  %s destroy <name>          shm_unlink the name\n"
        "  %s sigbus-demo             deliberately crash with SIGBUS\n"
        "\n"
        "names must start with '/' and contain no other '/', e.g. /ipc_demo\n",
        argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 2; }

    if (strcmp(argv[1], "create") == 0) {
        if (argc != 4) { usage(argv[0]); return 2; }
        return mode_create(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "attach") == 0) {
        if (argc != 3) { usage(argv[0]); return 2; }
        return mode_attach(argv[2]);
    }
    if (strcmp(argv[1], "destroy") == 0) {
        if (argc != 3) { usage(argv[0]); return 2; }
        ipc_status_t r = ipc_destroy(argv[2]);
        printf("ipc_destroy(%s): %s\n", argv[2], ipc_strerror(r));
        return r == IPC_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "sigbus-demo") == 0) {
        if (argc != 2) { usage(argv[0]); return 2; }
        return mode_sigbus_demo();
    }

    usage(argv[0]);
    return 2;
}
