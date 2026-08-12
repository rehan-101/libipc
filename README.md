# libipc

A lock-free, zero-copy shared-memory IPC engine in C.

**Day 1 of 7 — shared-memory substrate complete. Ring buffer lands Day 2.**

Today the library creates, publishes, attaches to, and tears down named
shared-memory segments. The data region is raw bytes. There is no ring buffer,
no message framing, and no shared object yet.

## Why

A message sent through a pipe or a socket costs a syscall and two copies: once
from the sender's buffer into kernel memory, once from kernel memory into the
receiver's buffer. Shared memory removes both from the data path — two
processes map the same physical frames, and a store by one is immediately
visible to the other with no kernel involvement at all. The remaining cost is
coordination, which is what the release/acquire handshake here and the
lock-free ring buffer in Day 2 exist to solve.

This is the standard architecture wherever per-message latency matters:
Chrome's Mojo IPC for browser-to-renderer traffic, Aeron and the LMAX
Disruptor in high-frequency trading, PyTorch's DataLoader passing tensors
between worker processes through `/dev/shm`, and Ray's Plasma object store.

## Build

```bash
make
```

Produces `day1_smoke` in the repo root and objects in `build/`.

Requirements:

- Linux (POSIX shared memory, `/dev/shm` mounted as tmpfs)
- gcc 13+ — C11 with `_Atomic`
- glibc 2.34+ — `shm_open` moved into libc, so no `-lrt` is needed

Tested on Ubuntu 24.04, kernel 6.8, glibc 2.39, gcc 13.3, x86-64.

Other targets:

```bash
make asm     # -S -fverbose-asm into build/, for checking the emitted barriers
make clean
```

## Run the demo

The point of the demo is to show that two processes see the same bytes at
different addresses. Open two terminals.

**Terminal 1** — create a 64 KiB segment and hold it open:

```bash
./day1_smoke create /ipc_demo 65536
```

**Terminal 2** — attach to it, read what terminal 1 wrote, write back:

```bash
./day1_smoke attach /ipc_demo
```

**Terminal 1** — press `Ctrl-C` to print the data region and clean up.

Abbreviated real output. Terminal 1:

```
---------------- CREATOR (pid 204402) ----------------
  map base (header) : 0x7a9078f6f000
  data base         : 0x7a9078f6f040
  mapped bytes      : 69632
  magic             : 0x49504331434d4853
  version           : 1
  hdr_size          : 64
  capacity          : 69568
  page_size         : 4096
  cache_line        : 64
```

Terminal 2:

```
---------------- ATTACHER (pid 204406) ----------------
  map base (header) : 0x726fa3a09000
  data base         : 0x726fa3a09040
  mapped bytes      : 69632
  magic             : 0x49504331434d4853
  version           : 1
  hdr_size          : 64
  capacity          : 69568
  page_size         : 4096
  cache_line        : 64
```

Note the two `map base` values: `0x7a9078f6f000` vs `0x726fa3a09000`.
Different virtual addresses, identical header contents, one physical frame.

The two `map base` values differ — each kernel picked a free range in its own
process's address space, and ASLR randomised the result — while every header
field is identical, because both mappings point at the same physical frames.
The creator's final readback shows a string the attacher wrote, which crossed
between processes with no syscall and no copy:

```
SIGINT received. Reading the data region back:
  data[0..96] = "WRITTEN-BY-ATTACHER pid=204406"
```

Inspect the raw segment while both are running:

```bash
ls -la /dev/shm            # segments are ordinary files on tmpfs
xxd -l 160 /dev/shm/ipc_demo
```

```
-rw------- 1 rehan rehan 69632 /dev/shm/ipc_demo

00000000: 5348 4d43 3143 5049 0100 0000 4000 0000  SHMC1CPI....@...
00000010: c00f 0100 0000 0000 0010 0000 4000 0000  ............@...
...
00000040: 5752 4954 5445 4e2d 4259 2d41 5454 4143  WRITTEN-BY-ATTAC
```

The magic constant is chosen to be readable in a hexdump, so segment identity
is checkable by eye. Because these are ordinary files, a leaked segment is
diagnosable with ordinary file tools and removable with `rm`.

Other modes:

```bash
./day1_smoke sigbus-demo        # deliberately crashes; see below
./day1_smoke destroy /ipc_demo  # shm_unlink a leftover name
```

`sigbus-demo` skips the `ftruncate` that `ipc_create` performs, so the object
stays 0 bytes. `mmap` still succeeds, and the first store dies with `Bus
error (core dumped)` — exit status 135. That is `SIGBUS`, not `SIGSEGV`:
`SIGSEGV` means the address is not mapped, while `SIGBUS` means it is mapped
but the backing store cannot produce a page for that offset.

## API

The full public surface, from [`include/ipc.h`](include/ipc.h). The handle is
opaque; no segment internals are exposed.

```c
ipc_segment_t *ipc_create (const char *name, size_t bytes, ipc_status_t *st);
ipc_segment_t *ipc_attach (const char *name, unsigned timeout_ms, ipc_status_t *st);
void          *ipc_base   (const ipc_segment_t *seg);
size_t         ipc_capacity(const ipc_segment_t *seg);
ipc_status_t   ipc_info   (const ipc_segment_t *seg, ipc_info_t *out);
ipc_status_t   ipc_detach (ipc_segment_t *seg);
ipc_status_t   ipc_destroy(const char *name);
const char    *ipc_strerror(ipc_status_t st);
```

| Function | Description |
|---|---|
| `ipc_create` | Creates and publishes a segment. Exclusive: a taken name yields `IPC_ERR_EXISTS`. `bytes` is a minimum; capacity is rounded up to whole pages. |
| `ipc_attach` | Opens an existing segment, waiting up to `timeout_ms` for the creator to publish the header. |
| `ipc_base` | First byte of the data region, past the header. Valid in the calling process only. |
| `ipc_capacity` | Usable bytes at `ipc_base()`. Always `>=` the `bytes` requested at creation. |
| `ipc_info` | By-value snapshot of the header, so no pointer into shared memory escapes. |
| `ipc_detach` | Unmaps and frees the handle. Accepts `NULL`. |
| `ipc_destroy` | Removes the name. Memory survives until the last detach. |
| `ipc_strerror` | Static description string for a status code. |

Statuses are `IPC_OK`, `IPC_ERR_EXISTS`, `IPC_ERR_NOTFOUND`, `IPC_ERR_SIZE`,
`IPC_ERR_MAGIC`, `IPC_ERR_VERSION`, `IPC_ERR_TIMEOUT`, and `IPC_ERR_SYS`. On
`IPC_ERR_SYS` the underlying `errno` is preserved across the library's internal
cleanup, so it remains inspectable by the caller.

### Usage

Creator:

```c
#include "ipc.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    ipc_status_t st;
    ipc_segment_t *seg = ipc_create("/demo", 4096, &st);
    if (!seg) {
        fprintf(stderr, "create: %s\n", ipc_strerror(st));
        return 1;
    }

    memcpy(ipc_base(seg), "ping", 5);

    /* Hold the mapping open so the peer has something to attach to.
       ipc_destroy below removes the name, so an attach after this
       process exits reports IPC_ERR_NOTFOUND. */
    puts("published; press Enter to destroy");
    getchar();

    ipc_detach(seg);
    ipc_destroy("/demo");
    return 0;
}
```

Attacher, in a separate process:

```c
#include "ipc.h"
#include <stdio.h>

int main(void)
{
    ipc_status_t st;
    ipc_segment_t *seg = ipc_attach("/demo", 1000, &st);
    if (!seg) {
        fprintf(stderr, "attach: %s\n", ipc_strerror(st));
        return 1;
    }

    printf("%s (%zu bytes usable)\n", (char *)ipc_base(seg), ipc_capacity(seg));

    ipc_detach(seg);
    return 0;
}
```

Compile against the library objects:

```bash
gcc -std=c11 -O2 -Iinclude your_program.c build/ipc_segment.o -o your_program
```

## Design notes

### Offsets, never pointers

Two processes map the same physical frames at different virtual addresses, as
the demo output shows. An address written into the segment by one process
names unrelated memory in another — frequently *mapped* memory, which corrupts
silently instead of faulting, and moves between runs because of ASLR.
Everything stored in the segment is therefore an offset, resolved against each
process's own base. The header's `hdr_size` is the first instance; the ring
buffer's indices in Day 2 will be the next.

### ftruncate before mmap

`mmap` does not validate its length against the object's size. It builds a VMA
and returns; the size check is deferred to the page-fault handler and applied
lazily, per page. Mapping past the end of the object therefore succeeds and
fails later with `SIGBUS` on first touch, which is a crash rather than an
error return and cannot be recovered from in a library. Sizing the object with
`ftruncate` first makes every page in the mapping legal.

### Publication via release/acquire

All header fields are written first, then the magic constant is published with
a release store. Attachers poll it with an acquire load and trust nothing until
it appears. On x86-64's TSO memory model both compile to plain `mov`
instructions with no fence — the barrier constrains the compiler's freedom to
reorder, not the CPU's. Verified `-O2` output from `make asm`:

```asm
movq    %rax, 16(%r14)   # .capacity
movl    $64,  28(%r14)   # .cache_line
movq    %rdx, 8(%r14)    # <vector(2) unsigned int> version+hdr_size
movabsq $5282796241767188563, %rdx
movq    %rdx, (%r14)     # <-- the release store. Plain mov.
```

gcc reordered the plain stores among themselves and fused two of them into a
single 8-byte store, which the annotation permits. None was allowed to sink
below the magic, which is the guarantee that matters. Omitting the annotation
passes every test on x86-64 and corrupts data on aarch64, where the release
compiles to a real `stlr`.

### Untrusted header

A segment name proves nothing about its contents: any process running as the
same user can create one. `ipc_attach` treats the header as hostile input. It
takes the size from the kernel via `fstat` rather than from the segment,
probe-maps a single page, polls for the magic, then validates version,
`page_size`, `cache_line`, and `hdr_size`, and checks that `capacity`
reconciles with the kernel's size. Only then does it map the full segment. A
rejected segment costs one 4 KiB mapping instead of a commitment to whatever
length the header claimed.

## Project layout

```
include/ipc.h          public API; opaque handle, no internals exposed
src/ipc_segment.c      segment lifecycle over shm_open + mmap
tests/day1_smoke.c     two-process driver and the SIGBUS demonstration
Makefile               all, clean, asm
```

## Roadmap

| Day | Feature | Status |
|---|---|---|
| 1 | Shared-memory segment lifecycle | Complete |
| 2 | SPSC ring buffer | Pending |
| 3 | Zero-copy variable-length records | Pending |
| 4 | MPSC producers via CAS | Pending |
| 5 | Crash recovery and futex waits | Pending |
| 6 | Packaging as `libipc.so`, Python and Rust bindings | Pending |
| 7 | RDTSC benchmarks and chaos tests | Pending |

## Known limitations

- **x86-64 only so far.** The memory-order annotations are correct by the C11
  model, but the barriers have not been reviewed or tested on aarch64, where
  release/acquire emit real instructions rather than compiling away.
- **`ipc_attach` does not wait for a segment that does not exist.** A missing
  name returns `IPC_ERR_NOTFOUND` immediately; `timeout_ms` covers only the
  window between the creator's `shm_open` and its release store.
  Attach-before-create requires a caller-side retry loop. This is deliberate —
  a mistyped name should not cost the full timeout — but it is a sharp edge.
- **No ring buffer.** The data region is raw bytes with no framing, no
  producer/consumer protocol, and no synchronisation beyond the initial
  publication handshake. Concurrent writers will corrupt each other.
- **Not packaged as a shared library.** Consumers link `build/ipc_segment.o`
  directly. `libipc.so` with a version script arrives on Day 6.

## License

MIT. See [LICENSE](LICENSE).
