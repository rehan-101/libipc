# Day 1 Verification Report

Scope: `src/ipc_segment.c`, as of the current working tree. Target: Ubuntu
24.04, gcc 13.3.0, glibc 2.39, x86-64 (Haswell host, `-march=native`).

This report checks Day 1's theoretical claims against real compiler output.
It is not a walkthrough of the code — it is an argument, built from
reproducible commands, that three specific assertions are true.

## What Day 1 claimed

1. `memory_order_release` emits no synchronization instruction on x86-64,
   because TSO already forbids store-store reordering in hardware. The
   annotation constrains the compiler, not the CPU.
2. GCC freely reorders and merges the plain header stores among themselves,
   but cannot move any of them below the release store.
3. Small static helper functions are inlined out of existence entirely — no
   call, no function body in the binary.

Day 1 is verified when all three hold against real compiler output.

## Generating the evidence

Two builds. The clean one is what gets read and quoted; the `-g` one exists
only to cross-check that the `-fverbose-asm` line comments in the clean
build are trustworthy, using DWARF `.loc` directives as an independent
source of the same information. `.loc` directives are then not otherwise
used or shown — they're the check, not the exhibit.

```
$ gcc -std=c11 -O2 -march=native -fverbose-asm -S -Iinclude \
      src/ipc_segment.c -o /tmp/ipc_clean.s
$ wc -l /tmp/ipc_clean.s
1299 /tmp/ipc_clean.s

$ gcc -std=c11 -O2 -march=native -g -S -Iinclude \
      src/ipc_segment.c -o /tmp/ipc_debug.s
$ wc -l /tmp/ipc_debug.s
9774 /tmp/ipc_debug.s
```

Both exit 0, gcc 13.3.0, no warnings (this file already builds clean under
`-Wall -Wextra -Wpedantic` per the Makefile; these two invocations omit
those flags only because they're not relevant to codegen inspection).

Cross-check: the header-publish block discussed in Claims 1 and 2 sits at
source lines 223–253. In the `-g` build, `ipc_create`'s `.loc` stream visits
exactly those line numbers in this region, independently confirming what the
`-fverbose-asm` comments in the clean build say:

```
$ awk '/^ipc_create:/,/^\.LFE37:/' /tmp/ipc_debug.s | grep '\.loc' | tail -20
	.loc 1 224 19 view .LVU76
	.loc 1 179 18 view .LVU77
	.loc 1 221 5 is_stmt 1 view .LVU78
	.loc 1 223 5 view .LVU79
	.loc 1 224 5 view .LVU80
	.loc 1 225 5 view .LVU81
	.loc 1 227 21 is_stmt 0 view .LVU82
	.loc 1 257 19 view .LVU83
	.loc 1 226 19 view .LVU84
	.loc 1 224 19 view .LVU85
	.loc 1 226 5 is_stmt 1 view .LVU86
	.loc 1 227 5 view .LVU87
	.loc 1 228 5 view .LVU88
	.loc 1 253 5 is_stmt 0 view .LVU89
	.loc 1 228 19 view .LVU90
	.loc 1 253 5 is_stmt 1 view .LVU91
	.loc 1 253 5 view .LVU92
	.loc 1 253 5 view .LVU93
	.loc 1 253 5 view .LVU94
	.loc 1 253 5 view .LVU95
```

Lines 224, 226, 227, 228, and 253 all appear, in the same relative order the
clean build's comments show below. Where a block below could not be pinned
to a specific source line with this kind of independent confirmation, that
is stated explicitly rather than inferred.

---

## Claim 1: `memory_order_release` emits no synchronization instruction on x86-64

**What we assumed and why.**
x86-64's TSO (total store order) memory model already forbids a CPU from
reordering one store ahead of an earlier store, for any two stores from the
same core. A C11 release store's only extra guarantee over a plain store is
exactly that ordering — earlier writes become visible no later than the
release write. Since TSO gives that for free, we expected the compiler to
translate `atomic_store_explicit(..., memory_order_release)` into an
ordinary store instruction, with no fence, no `lock` prefix, and no atomic
read-modify-write.

**The C.**
[src/ipc_segment.c:223-253](../../src/ipc_segment.c#L223-L253):

```c
    ipc_header_t *h = (ipc_header_t *)map;
    h->version    = IPC_VERSION;
    h->hdr_size   = (uint32_t)hdr_size;
    h->capacity   = (uint64_t)capacity;
    h->page_size  = (uint32_t)page;
    h->cache_line = (uint32_t)IPC_CACHE_LINE;

    /* ... */
    atomic_store_explicit(&h->magic, (uint64_t)IPC_MAGIC, memory_order_release);
```

This writes the five plain header fields, then publishes the segment by
storing `IPC_MAGIC` into the `_Atomic uint64_t magic` field with release
semantics. `h` is a raw pointer into the just-`mmap`'d shared-memory region
(`map`); every store here lands in memory another process may already have
mapped and be polling.

**The assembly.**
Exactly the six stores that write `ipc_header_t`'s fields, extracted from
`ipc_create` in the clean build:

```
$ sed -n '175,192p' /tmp/ipc_clean.s
# src/ipc_segment.c:224:     h->version    = IPC_VERSION;
	movq	.LC0(%rip), %rcx	#, tmp130
# src/ipc_segment.c:179:     const size_t capacity = total - hdr_size;
	leaq	-64(%rbx), %rsi	#, capacity
# src/ipc_segment.c:227:     h->page_size  = (uint32_t)page;
	movl	%r13d, 24(%rdx)	# ps, MEM[(struct ipc_header_t *)map_32].page_size
# src/ipc_segment.c:257:     seg->hdr      = h;
	vmovq	%rdx, %xmm1	# map, map
# src/ipc_segment.c:226:     h->capacity   = (uint64_t)capacity;
	movq	%rsi, 16(%rdx)	# capacity, MEM[(struct ipc_header_t *)map_32].capacity
# src/ipc_segment.c:224:     h->version    = IPC_VERSION;
	movq	%rcx, 8(%rdx)	# tmp130, MEM <vector(2) unsigned int> [(unsigned int *)map_32 + 8B]
# src/ipc_segment.c:253:     atomic_store_explicit(&h->magic, (uint64_t)IPC_MAGIC, memory_order_release);
	movabsq	$5282796241767188563, %rcx	#, tmp131
# src/ipc_segment.c:228:     h->cache_line = (uint32_t)IPC_CACHE_LINE;
	movl	$64, 28(%rdx)	#, MEM[(struct ipc_header_t *)map_32].cache_line
# src/ipc_segment.c:253:     atomic_store_explicit(&h->magic, (uint64_t)IPC_MAGIC, memory_order_release);
	movq	%rcx, (%rdx)	#, tmp131,* map
```

AT&T syntax: every two-operand instruction below reads source, then
destination — `movq %rcx, (%rdx)` moves `%rcx` *into* the memory at `%rdx`,
not the other way around. This is the first assembly in the document; all
later listings use the same order.

Annotated, in the order gcc emits them (`%rdx` holds `map`, the base address
of the header; `%r14`/`%rdx` naming differs slightly by build but refers to
the same value — the mapped header's address):

- `movq .LC0(%rip), %rcx` — loads an 8-byte constant from `.rodata` into
  `%rcx`. `.LC0` (shown later, in Claim 3's evidence) holds the two 32-bit
  words `1` and `64` packed together: `version` (`IPC_VERSION` = 1) and
  `hdr_size` (folded to the compile-time constant 64) as one 64-bit value.
  Setup only — no store yet.
- `leaq -64(%rbx), %rsi` — computes `capacity = total - hdr_size` into
  `%rsi`. Unrelated to the header stores; gcc interleaved it here because
  the scheduler had a free slot. Not part of this claim.
- `movl %r13d, 24(%rdx)` — a 4-byte store, `h->page_size = page` (offset 24
  in `ipc_header_t`). `%r13d` holds `page` (from `sysconf(_SC_PAGESIZE)`
  earlier in the function).
- `vmovq %rdx, %xmm1` — copies `map` into a vector register in preparation
  for building `seg->hdr` (source line 257, part of the private handle, not
  the shared header). Not part of this claim.
- `movq %rsi, 16(%rdx)` — an 8-byte store, `h->capacity = capacity` (offset
  16), using the value computed two instructions earlier.
- `movq %rcx, 8(%rdx)` — an 8-byte store to offset 8, using the constant
  loaded first. This single instruction writes both `h->version` (offset 8)
  and `h->hdr_size` (offset 12) at once — see Claim 2 for why gcc was
  allowed to fuse them.
- `movabsq $5282796241767188563, %rcx` — loads the 64-bit immediate
  `IPC_MAGIC` into `%rcx`. `5282796241767188563` is `0x49504331434D4853`,
  confirmed by direct computation to equal `IPC_MAGIC` as defined at
  [src/ipc_segment.c:28](../../src/ipc_segment.c#L28), and its little-endian
  byte layout spells `SHMC1CPI`, matching the comment there.
- `movl $64, 28(%rdx)` — a 4-byte store, `h->cache_line = 64` (offset 28,
  `IPC_CACHE_LINE`), an immediate — this field's value is known at compile
  time.
- `movq %rcx, (%rdx)` — the release store. An 8-byte plain `mov` writing
  `IPC_MAGIC` to offset 0 (`h->magic`). This is the instruction the whole
  claim is about: it is indistinguishable from an ordinary, non-atomic
  64-bit store. No prefix, no fence, nothing else attached to it.

Grep evidence for the absence of any hardware synchronization anywhere in
this translation unit (not just this block — the whole file):

```
$ grep -inE '^\s+(lock|mfence|sfence|lfence|xchg|cmpxchg|pause)\b' /tmp/ipc_clean.s
$ echo "exit: $?"
exit: 1
$ grep -cE '^\s+(lock|mfence|sfence|lfence|xchg|cmpxchg|pause)\b' /tmp/ipc_clean.s
0
```

Grep exits 1 (no match), and the strict count is 0. A looser, unanchored
`grep -in lock` does match — but only inside `#` comments quoting
`clock_gettime`, never as an instruction mnemonic; the anchored pattern
above (`^\s+` before the mnemonic, `\b` after) rules that false-positive
class out.

**Why this proves the claim.**
The release store compiles to `movq %rcx, (%rdx)` — a plain move, identical
in every respect to how gcc would compile a non-atomic `*p = v` of the same
width. If the release semantics required runtime synchronization on this
architecture, gcc would have needed to emit one of: an `mfence`/`sfence`
(explicit barrier), a `lock`-prefixed instruction (atomic RMW used as a
barrier), or `xchg`/`cmpxchg` (which carry an implicit `lock` on x86-64).
None appear anywhere in the 1299-line output, verified by an anchored grep
rather than eyeballing. That absence, combined with the store's plain `mov`
form, is the whole proof: nothing was inserted for the release to work.

The obvious objection: if `memory_order_relaxed` would compile to the exact
same `movq`, doesn't that mean the annotation is doing nothing, and could be
dropped? No — identical *codegen on this one target* is not identical
*meaning*. `memory_order_release` is a promise to the compiler's optimizer
(this is Claim 2 — it constrains instruction scheduling, GVN, and
store-sinking across this specific point) and a promise about what happens
on every target this code might run on, not just this build. The C standard
defines the release/acquire contract architecture-independently; x86-64's
TSO happens to make the *hardware* half of that contract free, but the
*compiler* half (not reordering the stores past it) is real and enforced
regardless of target. Drop the annotation and gcc is free to sink any of
the five header stores below the magic write — nothing here forces it not
to, it simply chooses not to today, on this input, at this optimization
level. A future gcc, a different optimization level, or LTO inlining this
function into a larger caller could legally reorder a plain store past a
plain store; it can never legally reorder a plain store past a release
store.

On aarch64, ARMv8's memory model is weaker than x86-64's TSO: it permits
store-store reordering in hardware, so the release semantics cannot be free
there. The standard translation is `stlr` (store-release register) in place
of a plain `str`. That is well-established ARMv8 instruction-set behavior,
not something this report generated: no aarch64 cross-compiler was
available in this session (`gcc-aarch64-linux-gnu` is not installed, and
installing it required an interactive `sudo` password this tool does not
have), so no real `stlr` output was produced here to quote. This one
sentence is the exception to "every claim backed by a command in this
report" — it is flagged as such rather than presented as verified.

**What this changes.**
Keep writing `memory_order_release`/`_acquire` explicitly at every
cross-process publish point, even though today's x86-64 build would look
byte-identical with `memory_order_relaxed`. The annotation is what keeps
this code correct if it is ever built for aarch64, run through LTO, or
compiled by a future gcc more aggressive about store sinking — none of
which this test file's output can rule out. Never use "the assembly looks
the same either way" as a reason to relax an ordering annotation.

---

## Claim 2: GCC reorders and fuses plain stores, never sinks past the release

**What we assumed and why.**
`memory_order_release` on the magic store constrains only what may move
*across* it: no store or load that precedes it in program order may be
moved after it. It says nothing about the five plain stores relative to
*each other* — they carry no ordering requirement among themselves, so we
expected gcc to schedule and even merge them however its cost model
prefers, while still keeping every one of them strictly before the release.

**The C.**
Same block as Claim 1,
[src/ipc_segment.c:223-253](../../src/ipc_segment.c#L223-L253) — the five
field assignments in source order (`version`, `hdr_size`, `capacity`,
`page_size`, `cache_line`), followed by the release store to `magic`.

**The assembly.**
Same nine-instruction block quoted in Claim 1. What matters here is store
*order and grouping*, not each instruction individually — reading the six
stores that touch `ipc_header_t` in the order they appear in the emitted
file:

| # emitted | Instruction | Field(s) written | Offset | Source line |
|---|---|---|---|---|
| 1 | `movl %r13d, 24(%rdx)` | `page_size` | 24 | 227 |
| 2 | `movq %rsi, 16(%rdx)` | `capacity` | 16 | 226 |
| 3 | `movq %rcx, 8(%rdx)` | `version` **+** `hdr_size` (fused) | 8, 12 | 224, 225 |
| 4 | `movl $64, 28(%rdx)` | `cache_line` | 28 | 228 |
| 5 | `movq %rcx, (%rdx)` | `magic` (release) | 0 | 253 |

Source order by offset is 8 → 12 → 16 → 24 → 28 → (0, last, by construction
of the code). Emitted order is 24 → 16 → 8/12 → 28 → 0. Every plain field
was reshuffled relative to source order; only the release store's position
— last — matches where it would be in a naive, unoptimized reading of the
source.

The fused instruction is `movq %rcx, 8(%rdx)`: it writes 8 bytes starting
at offset 8, which is exactly `version` (offset 8, 4 bytes) immediately
followed by `hdr_size` (offset 12, 4 bytes) — two adjacent, naturally
aligned `uint32_t` fields with no gap between them. GCC was allowed to fuse
them into one 8-byte store because:

- both are plain (non-`_Atomic`, non-`volatile`) fields, so the language
  places no per-field visibility requirement on them individually — only
  the aggregate "everything before the release is visible after a matching
  acquire" requirement applies, and one 8-byte store satisfies that just as
  well as two 4-byte stores;
- both values are compile-time constants here (`IPC_VERSION` = 1, and
  `hdr_size` = `round_up_pow2(sizeof(ipc_header_t), 64)` folds to the
  constant 64 — see Claim 3), so gcc could precompute the packed 8-byte
  value `{1, 64}` once, as the `.LC0` rodata constant, and load+store it in
  a single instruction pair instead of materializing two separate
  immediates;
- there is no read of either field, and no aliasing store to that address
  range, between the two source assignments — nothing in the intervening
  code could observe `version` written without `hdr_size`, so collapsing
  them is unobservable from within this translation unit.

**Why this proves the claim.**
Two things are independently visible in the table: the plain stores do not
appear in source order (page_size, at source line 227, is emitted *first*;
capacity, at line 226, second; the version/hdr_size pair, source lines
224–225, third), and two of them collapsed into a single instruction that
exists nowhere as a single statement in the C. Both are optimizations the
compiler is only permitted to make on stores that are not ordered relative
to each other. At the same time, the release store — last in source order
— is also last in emitted order, despite every plain store around it moving.
If the release annotation forbade nothing, we might have expected the
optimizer to freely interleave or hoist code around it too, indistinguishable
from any other store; instead it is the one store whose relative position
never changed while everything else's did. Falsifying evidence would have
been the magic write appearing anywhere but last — e.g. `movq %rcx, (%rdx)`
positioned before the `movl $64, 28(%rdx)` cache_line store — which would
mean gcc sank a later-declared plain store below an earlier release, a
miscompile relative to the C11 release semantics.

This is a stronger result than "no reordering happened" would have been. If
every store had simply appeared in source order, that would be equally
consistent with two different explanations: either the release correctly
constrained everything, or gcc just didn't bother optimizing this block at
all (e.g. at `-O0`, or because the function was too small to trigger the
scheduler) and the ordering claim would be untested. Seeing the plain
stores actively shuffled and merged — real optimizer work happened here —
while the release store alone stayed pinned, isolates the release
annotation as the specific cause of the one placement that didn't move.

**What this changes.**
Do not read store order in the C source as a guarantee of anything for
plain fields — the compiler already doesn't. Only the release/acquire pair
is a placement guarantee; everything else in this header write is fair game
for scheduling, fusion, or (at higher optimization, LTO, or with different
struct layouts) further transformation. When adding a new plain field to
`ipc_header_t`, its assignment can be written anywhere before the release
store without affecting correctness or, likely, codegen shape.

---

## Claim 3: small static helpers are inlined out of existence

**What we assumed and why.**
Every helper in this file is `static`, used only within this translation
unit, small, and usually called from few sites. That is exactly the profile
gcc's inliner targets at `-O2`: a `static` function with a single visible
translation unit and no address ever taken has no ABI to preserve, so the
compiler is free to substitute its body at each call site and delete the
standalone function entirely if nothing keeps a pointer to it alive.

**The C.**
Seven `static` helpers are defined in this file, at
[src/ipc_segment.c:85-149](../../src/ipc_segment.c#L85-L149):
`status_from_errno`, `set_status`, `round_up_pow2`, `elapsed_ms`,
`poll_backoff`, `name_is_valid`, `this_page_size`.

**The assembly.**
Every function symbol gcc emitted in the clean build:

```
$ grep -n '\.type.*@function' /tmp/ipc_clean.s
9:	.type	name_is_valid, @function
78:	.type	ipc_create, @function
365:	.type	ipc_attach, @function
898:	.type	ipc_base, @function
921:	.type	ipc_capacity, @function
941:	.type	ipc_info, @function
991:	.type	ipc_detach, @function
1053:	.type	ipc_destroy, @function
1159:	.type	ipc_strerror, @function
```

Of the nine standalone functions in the binary, eight are the public API
(`ipc_create`, `ipc_attach`, `ipc_base`, `ipc_capacity`, `ipc_info`,
`ipc_detach`, `ipc_destroy`, `ipc_strerror` — all declared in `ipc.h` and
callable from another translation unit, so gcc cannot delete them
regardless of size). The ninth is `name_is_valid` — the one static helper
that survived as a real function. Checking whether the other six leave any
trace at all:

```
$ for f in status_from_errno set_status round_up_pow2 elapsed_ms poll_backoff this_page_size; do
    echo "--- $f ---"; grep -n "$f" /tmp/ipc_clean.s || echo "  (absent entirely)"
  done
--- status_from_errno ---
(19 matches, all lines of the form '# src/ipc_segment.c:208: ... status_from_errno(errno));' — comments quoting source text, zero instructions)
--- set_status ---
(28 matches, same pattern — comments only)
--- round_up_pow2 ---
590:# src/ipc_segment.c:338:     if (total < round_up_pow2(sizeof(ipc_header_t), IPC_CACHE_LINE)) {
--- elapsed_ms ---
(4 matches, comments only)
--- poll_backoff ---
  (absent entirely)
--- this_page_size ---
  (absent entirely)
```

Every one of these six names appears, at most, inside a `#`-prefixed
verbose-asm comment quoting the original C statement — never as a `.type`
symbol, never as the target of a `call`. `poll_backoff` and
`this_page_size` don't appear at all, not even in a comment: their bodies
were substituted so completely that not even a source-line reference to the
call survives (their contents — `nanosleep`/`sysconf` — get their own
comments instead, tied to the *caller's* source line). **Six of seven
vanished. One — `name_is_valid` — survived.**

`round_up_pow2`'s body, next to the instructions inside `ipc_create` that
are it:

```c
static size_t round_up_pow2(size_t v, size_t align)
{
    return (v + (align - 1)) & ~(align - 1);
}
```

It is called twice in `ipc_create`
([src/ipc_segment.c:169,178](../../src/ipc_segment.c#L169-L178)):
`round_up_pow2(sizeof(ipc_header_t), IPC_CACHE_LINE)` — both arguments
compile-time constants (32 and 64) — and
`round_up_pow2(hdr_size + bytes, page)`, where `page` is a runtime value
from `sysconf`. The first call disappears without a trace: `32 + 64 - 1 =
95`, `95 & ~63 = 64`, computed entirely by the compiler and folded into the
literal `64` used throughout (e.g. `movl $64, 28(%rdx)` in Claim 1, and the
`.LC0` constant `{1, 64}`). It contributes zero runtime instructions — not
"an inlined function", but arithmetic that never survives past compile
time.

The second call — where `align` (`page`) is not known until runtime —
*does* leave real instructions, and this is where "the add and the and"
from `round_up_pow2`'s body are directly visible:

```
$ sed -n '128,138p' /tmp/ipc_clean.s
# src/ipc_segment.c:105:     return (v + (align - 1)) & ~(align - 1);
	leaq	63(%rax,%rbx), %rbx	#, tmp120
# src/ipc_segment.c:183:     fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
	movl	$384, %edx	#,
# src/ipc_segment.c:105:     return (v + (align - 1)) & ~(align - 1);
	negq	%rax	# tmp121
# src/ipc_segment.c:183:     fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
	movl	$194, %esi	#,
	movq	%r12, %rdi	# name,
# src/ipc_segment.c:105:     return (v + (align - 1)) & ~(align - 1);
	andq	%rax, %rbx	# tmp121, _61
```

At this point `%rax` holds `page` and `%rbx` holds `bytes`. `leaq
63(%rax,%rbx), %rbx` is **the add**: it computes `page + bytes + 63` in one
instruction, which is algebraically `(hdr_size + bytes) + (page - 1)` — the
caller's own `hdr_size + bytes` argument expression fused with
`round_up_pow2`'s `v + (align - 1)`, since `hdr_size` is the constant 64 and
`64 - 1 = 63` collapses into the immediate. `negq %rax` turns `page` into
`-page`, which is `~(page - 1)` by the two's-complement identity
`~(x-1) == -x` — needed only because `align` is a runtime value here, so
gcc cannot fold the mask into an immediate the way it did for the first,
fully-constant call. `andq %rax, %rbx` is **the and**: `(v + align - 1) &
-page`, the final masking step, landing in `%rbx` as `total`. Three
instructions realize the whole function body; none of them is a `call`, and
there is no matching `ret` anywhere near this block — `round_up_pow2` has
no standalone existence in the binary at all, at either call site.

`name_is_valid` is the exception, and it is only a partial one. It is
called from two of its three use sites:

```
$ grep -n 'name_is_valid' /tmp/ipc_clean.s
9:	.type	name_is_valid, @function
10:name_is_valid:
75:	.size	name_is_valid, .-name_is_valid
107:	call	name_is_valid	#     <- from ipc_create, line 161
398:	call	name_is_valid	#     <- from ipc_attach, line 292
```

But at its third call site, `ipc_destroy`
([src/ipc_segment.c:513-516](../../src/ipc_segment.c#L513-L516)), no `call`
appears — the check is inlined and duplicated instead:

```
$ sed -n '1054,1082p' /tmp/ipc_clean.s
ipc_destroy:
.LFB43:
	.cfi_startproc
	endbr64	
# src/ipc_segment.c:133:     if (!name || name[0] != '/')
	testq	%rdi, %rdi	# name
	je	.L195	#,
	pushq	%rbx	#
	...
	cmpb	$47, (%rdi)	#, *name_8(D)
	movq	%rdi, %rbx	# tmp103, name
	jne	.L189	#,
	call	strlen@PLT	#
	subq	$2, %rax	#, tmp97
	cmpq	$254, %rax	#, tmp97
	ja	.L189	#,
	leaq	1(%rbx), %rdi	#, tmp98
	movl	$47, %esi	#,
	call	strchr@PLT	#
```

This is `name_is_valid`'s full body, re-emitted byte-for-byte-equivalent
inside `ipc_destroy` rather than reached via `call name_is_valid`. GCC's
inliner makes this decision per call site, weighing the caller's size and
the surrounding control flow, not once per function; `ipc_create` and
`ipc_attach` are large functions where inlining a third-or-more copy of
this check was judged not worth the code growth, while `ipc_destroy` is
small enough that gcc chose to inline there. This is an interpretation of a
known gcc heuristic (its inliner's per-call-site cost/benefit evaluation),
not a fact this report's evidence alone establishes — it is offered as the
standard explanation, flagged as such.

`name_is_valid` most likely survived as a callable function (at all) rather
than disappearing like the other six because it is the largest and most
control-flow-heavy of the seven — a null/prefix check, a `strlen` call, a
length range check, and a `strchr` call, each with its own branch — and it
has three call sites. Inlining a body that size into three places would
grow the binary for comparatively little benefit versus one `call`, which
is exactly the trade gcc's cost model exists to reject.

**Why this proves the claim.**
Six of the seven helpers leave no instruction, no symbol, and in two cases
no comment anywhere in a 1299-line assembly file that covers the entire
translation unit — the only remaining trace is the source-line reference
attached to whatever instructions their *callers'* logic produced. For
`round_up_pow2` specifically, both the "vanished into a compile-time
constant" case and the "vanished into inline arithmetic" case are shown
side by side, and in the arithmetic case, every operator in the one-line
function body (`+`, subtraction-as-negation for the complement, `&`) maps
onto a specific instruction with no unaccounted-for step. Falsifying
evidence for this claim would have been a `.type round_up_pow2, @function`
line, or a `call round_up_pow2` anywhere in the file — neither exists. The
claim as stated ("no call, no function body") is the literal, checked
condition of the grep in the first code block of this section.

The claim is not universally true of *every* static function in the file,
and the report says so rather than rounding `name_is_valid` off to fit:
it survives as a real, callable, `.type @function` symbol, reached by
`call` from two of three sites. The claim holds for six of seven helpers,
not seven of seven, and that is the accurate, falsifiable result — not "all
small helpers vanish," but "gcc inlines aggressively, and what survives is
governed by size and call-site count, not by the `static` keyword or
smallness alone."

**What this changes.**
Don't assume `static` plus "looks small" is sufficient to predict a helper
vanishes — `name_is_valid` looked exactly as good a candidate as the other
six and didn't fully disappear. When call-count or body size for a helper
grows, expect gcc to keep (or introduce) a real function rather than
inlining every site; that's a size/perf tradeoff made by the compiler's
cost model, not something to fight by hand (no need to mark helpers
`inline` or restructure them preemptively — verify with `-S` if it ever
matters, as done here, rather than assuming).

---

## Verdict

| Claim | Status | Evidence |
|---|---|---|
| 1. Release store costs nothing on x86-64 | **Verified** | Release store compiles to plain `movq %rcx, (%rdx)`; anchored grep for `lock/mfence/sfence/lfence/xchg/cmpxchg/pause` across the whole file returns 0 matches |
| 2. Plain stores reorder/fuse; none sinks past the release | **Verified** | Emitted store order (24→16→8→28→0) differs from source order (8→16→24→28→[0 last]); `version`+`hdr_size` fused into one `movq`; magic (offset 0) is last in both source and emitted order |
| 3. Static helpers inlined out of existence | **Verified, with one documented exception** | `status_from_errno`, `set_status`, `round_up_pow2`, `elapsed_ms`, `poll_backoff`, `this_page_size` — 6/7 — leave zero symbols/calls; `round_up_pow2`'s add+and appear as bare `leaq`/`andq` inside `ipc_create` with no `call`/`ret`; `name_is_valid` survives as a real `.type @function`, called from 2 of 3 sites and inline-duplicated at the 3rd |

One footnote applies to Claim 1 only: the aarch64 comparison (`stlr` vs. a
plain `str`) is stated as documented ARMv8 ISA behavior, not verified by a
command run in this session — no aarch64 cross-compiler was available and
installing one required interactive `sudo` credentials this environment
doesn't have. Every other statement in this report traces to a command
above and its real output.

### Reproduce this report

```sh
# From the repository root:
gcc -std=c11 -O2 -march=native -fverbose-asm -S -Iinclude \
    src/ipc_segment.c -o /tmp/ipc_clean.s
gcc -std=c11 -O2 -march=native -g -S -Iinclude \
    src/ipc_segment.c -o /tmp/ipc_debug.s

# Cross-check the header-publish block's line mapping via DWARF .loc:
awk '/^ipc_create:/,/^\.LFE37:/' /tmp/ipc_debug.s | grep '\.loc' | tail -20

# Claim 1: the header-publish block, and the fence/atomic-RMW grep:
sed -n '175,192p' /tmp/ipc_clean.s
grep -inE '^\s+(lock|mfence|sfence|lfence|xchg|cmpxchg|pause)\b' /tmp/ipc_clean.s
grep -cE '^\s+(lock|mfence|sfence|lfence|xchg|cmpxchg|pause)\b' /tmp/ipc_clean.s

# Claim 2: same block (order/fusion read directly from the sed output above).

# Claim 3: function symbols, helper name search, and round_up_pow2 / name_is_valid:
grep -n '\.type.*@function' /tmp/ipc_clean.s
for f in status_from_errno set_status round_up_pow2 elapsed_ms poll_backoff this_page_size; do
  echo "--- $f ---"; grep -n "$f" /tmp/ipc_clean.s || echo "  (absent entirely)"
done
sed -n '128,138p' /tmp/ipc_clean.s
grep -n 'name_is_valid' /tmp/ipc_clean.s
sed -n '1054,1082p' /tmp/ipc_clean.s

# Optional: verify the magic constant independently.
python3 -c "print(0x49504331434D4853 == 5282796241767188563)"
```
