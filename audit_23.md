# Loom Codebase Audit — 2026-11-23

## Executive summary

The repository has a substantial compiler, native runtime, CLI, tests, and VS Code extension. The main build succeeds, the extension test suite passes, and seven of the eight CTest suites pass in this environment. This review found several concrete correctness and memory-safety defects, including a reproduced heap use-after-free, out-of-bounds network reads, Text cloning that truncates embedded NUL bytes, incorrect UTF-8 iteration, and a formatter transformation that can make valid code fail to compile.

The highest-priority work is to define and enforce ownership for `Text`/`String` conversions and to replace implicit C-string network lengths with explicit byte slices. The current APIs blur owned text, borrowed views, and byte buffers; that ambiguity is the common cause behind several defects.

This is a static and targeted dynamic audit, not a proof that every possible compiler/runtime bug has been found. Findings are distinguished below from improvement opportunities and environment-limited checks.

## Scope and method

Reviewed the compiler front end and code generator, runtime C implementation, CLI and formatter, build configuration, test harnesses, documentation, and `vscode-loom` extension. Read repository files and existing changes, built the current checkout, ran CTest and the extension's npm tests, and used temporary examples under `/tmp` for focused reproductions, including AddressSanitizer.

The working tree already had uncommitted edits in compiler, runtime, test, and extension files when this audit began. Those changes were treated as the code under review and were not modified for this report. Only this report was added to the repository.

## Findings

### P1 — Returning a `String` view of local `Text` causes use-after-free

**Evidence:** `src/compiler/sema/Analyzer.h:279-285` permits a `Text` to be cast to `String` as a zero-cost view. `src/compiler/codegen/Generator.h:213-218` emits the backing buffer pointer for that conversion. Function return generation emits scope drops after evaluating the return expression (`src/compiler/codegen/Generator.h:3236-3268`); the cleanup recognition does not treat this cast as ownership transfer. `To_String` also returns the backing buffer (`src/compiler/codegen/Generator.h:1583-1587`) while its result is typed as `String` (`src/compiler/sema/Analyzer.h:1024-1029`).

**Reproduction and impact:** A temporary Loom function created a local `Text`, returned it as `String`, and the caller printed the result. An AddressSanitizer build reported `heap-use-after-free` in the print path; the allocation was released by `loom_text_free` during return cleanup. A caller can read corrupted data or crash. This is a real memory-safety violation, not just an inaccurate diagnostic.

**Recommended fix:** Give borrowed views a distinct, length-carrying type and track their lifetime so they cannot escape the owner, or make the conversion allocate/copy into an owning string with defined cleanup. Apply the same ownership rule to `To_String`, subslices, fields, and task results. Add ASan regressions for return, assignment, nested scopes, and async escape.

### P1 — TCP/UDP send may read 64 bytes past a short string

**Evidence:** `src/compiler/codegen/Generator.h:1739-1757` and `1819-1820` choose a fixed length of 64 for non-`Text` values passed to the send operations. The runtime forwards that length to the OS in `src/runtime/rt_net.c:92-95` and `148-156` without validating the source buffer size.

**Reproduction and impact:** Passing a short string literal such as `"Hello Loom Socket!"` selects the fixed length even though the literal occupies only 20 bytes including its terminator. The native send call can read beyond the literal, potentially crashing or transmitting adjacent process memory. The networking CTest could not exercise this path because local networking is blocked in this environment; the unsafe length selection is visible in the compiler implementation itself.

**Recommended fix:** Make byte sends accept a pointer plus explicit length or a checked byte-slice type. For text, derive length from `Text.Length` or a safe string length; never infer an arbitrary capacity. Reject unsupported argument types during semantic analysis.

### P1 — UDP receive does not reserve or write a terminating byte

**Evidence:** `src/compiler/codegen/Generator.h:1826-1843` allocates a `Text` buffer using `maxLen`. `loom_text_init` allocates exactly the requested capacity (`src/runtime/rt_collections.c:11-23`). `src/runtime/rt_net.c:159-164` receives up to that capacity and stores the received length without appending a NUL byte. Text printing uses C-string formatting (`src/compiler/codegen/Generator.h:2981`).

**Impact:** A full-size datagram fills the allocation with no terminator; a shorter datagram leaves the remainder uninitialized. Any consumer that treats the buffer as a C string can read beyond received data and, at the full-size boundary, beyond the allocation. This can expose unrelated memory or crash.

**Recommended fix:** Separate binary datagrams from text. For text, reserve `maxLen + 1`, receive at most `maxLen`, and append the terminator. Check addition overflow and preserve the exact byte length. Keep binary handling length-based throughout.

### P2 — TCP receive has a one-byte capacity overflow

**Evidence:** `src/compiler/codegen/Generator.h:1764-1773` creates a `Text` with capacity `maxLen`. `src/runtime/rt_net.c:98-107` receives `max_len` bytes when `max_len == 1` and then writes `buf[recvd] = '\0'`.

**Impact:** If one byte is received, the terminator is written to `buf[1]`, past a one-byte allocation. For larger capacities the implementation reserves one byte by receiving at most `max_len - 1`; the one-byte special case breaks that invariant.

**Recommended fix:** Define whether `max_len` includes the terminator, enforce a minimum capacity, and ensure every receive writes only within allocated storage. Add boundary tests for capacities 0, 1, 2, and full buffers.

### P2 — `loom_text_clone` truncates content at embedded NUL

**Evidence:** `src/runtime/rt_collections.c:139-145` clones with `loom_text_append_str(dest, src->data)`. That helper computes its length using `strlen` (`src/runtime/rt_collections.c:48-62`) instead of the source `Text.length`.

**Reproduction and impact:** A temporary C runtime test built a Text containing bytes `a`, NUL, `b` using the character append operation. The source length was 3, but its clone length was 1. Cloning binary-capable Text silently changes its value and can invalidate lengths and later processing.

**Recommended fix:** Copy exactly `src->length` bytes, set the destination length, and append a terminator after the copied data. Include embedded-NUL clone/equality/append tests.

### P2 — Text subslices allocate memory with no visible owner or release

**Evidence:** Text subslicing is typed as `String` (`src/compiler/sema/Analyzer.h:442-443`) and calls `loom_text_subslice` (`src/runtime/rt_collections.c:100-118`), which allocates and returns a new buffer. The generated String value has no corresponding ownership cleanup in the paths reviewed.

**Impact:** Repeated subslicing, especially in loops, can leak one heap allocation per operation. The same ambiguous `String` ownership model also complicates safe return and assignment.

**Recommended fix:** Return a borrowed `(pointer, length)` slice when valid, or define `String` as owning and add deterministic cleanup/transfer semantics. Add repeated-subslice leak checks under LeakSanitizer.

### P2 — Iterating `Text` decodes UTF-8 as individual bytes

**Evidence:** The analyzer describes a Text loop variable as `Character` (`src/compiler/sema/Analyzer.h:1997-1999`), but code generation loads one byte and zero-extends it (`src/compiler/codegen/Generator.h:3586-3591`).

**Reproduction and impact:** A temporary Loom loop over the UTF-8 text `"é"` emitted two characters (the UTF-8 bytes interpreted separately) rather than one Unicode scalar. Non-ASCII text therefore produces wrong iteration results and can create invalid character values.

**Recommended fix:** Decode UTF-8 into Unicode scalar values with validation and a clear policy for malformed sequences, or define iteration explicitly as bytes and use an unsigned byte type rather than `Character`. Add tests for ASCII, 2/3/4-byte scalars, malformed UTF-8, and embedded NUL.

### P2 — Formatter splits multi-character operators and changes program meaning

**Evidence:** `src/driver.cpp:2334-2359` tokenizes punctuation one character at a time; `src/driver.cpp:2399-2409` inserts spaces between those pieces.

**Reproduction and impact:** Running `loom fmt` on a compact expression using `>=` produced `> =`. The formatted file then failed `loomc --check` with an expression parse error. The same tokenizer pattern can corrupt other multi-character operators such as `==`, `!=`, `<=`, `+=`, `:=`, `..`, and shifts.

**Recommended fix:** Reuse the compiler's real lexer/token stream, preserving operator tokens and source constructs, rather than building a punctuation splitter. Add formatting round-trip tests asserting formatted output still parses and repeated formatting is idempotent.

### P2 — Tabs shift diagnostic columns away from editor positions

**Evidence:** `src/compiler/lexer/Lexer.h:57-58` advances the source column to visual tab stops. LSP character offsets count a tab as one source character (and use UTF-16 code units), so diagnostics after a tab can be reported too far to the right.

**Impact:** Diagnostics, highlights, and navigation may underline the wrong text in VS Code for tab-indented files. Non-ASCII characters can also expose byte-versus-editor-column mismatches if source offsets are converted without UTF-16-aware mapping.

**Recommended fix:** Track byte offsets and editor-facing positions separately. Convert byte offsets to LSP UTF-16 line/character coordinates at the protocol boundary. Add tests with tabs, multibyte UTF-8, surrogate-pair characters, and CRLF.

### P2 — Diagnostic code assignment is based on message substrings

**Evidence:** `src/compiler/codegen/Generator.h:4173-4180` assigns `E1001` to any diagnostic text containing `expected ` before it checks later semantic categories.

**Impact:** A semantic message such as “expected Signed_32, found Unsigned_32” can be labeled as a parser error. Consumers relying on stable diagnostic codes may show the wrong category or attach the wrong action.

**Recommended fix:** Store a structured diagnostic identifier at each diagnostic creation site and propagate it through compiler/LSP output. Avoid inferring category from human-readable prose.

## Test and build results

- **Build:** `cmake --build .build --parallel 4` completed successfully. It emitted warnings about unused code/variables in `src/driver.cpp`, duplicate LLVM libraries, and LLVM libraries built for a newer macOS deployment target than this build target.
- **CTest:** 7 of 8 suites passed. `modern_features_suite` failed at its native networking assertion. Direct reproduction showed TCP/UDP socket creation/bind returning failure; a Python localhost bind in the same environment raised `PermissionError: Operation not permitted`. This indicates the host execution environment blocks local networking, so this run does not establish a Loom networking defect. The test should detect and report a restricted environment distinctly rather than treating it as a product regression.
- **VS Code extension:** `npm test` in `vscode-loom` passed all 5 tests.
- **AddressSanitizer:** Confirmed the `Text`-to-`String` return use-after-free described above.
- **Runtime micro-reproduction:** Confirmed embedded-NUL clone truncation (`source length=3`, `clone length=1`).
- **Formatter reproduction:** Confirmed formatting changes `>=` into `> =` and makes the result fail parsing.
- **Unicode reproduction:** Confirmed iteration over `é` yields two byte-derived characters.

## Improvement and optimization opportunities

### Runtime and language safety

1. Specify ownership and borrowing for every pointer-like value. Define whether `String` owns bytes, borrows bytes, or is merely a C-string ABI type. Encode the distinction in types and enforce escape/lifetime rules in semantic analysis.
2. Adopt length-carrying slices for text and binary data. C strings should be an explicit FFI boundary, not the default representation for arbitrary text or network payloads.
3. Audit all allocation-size calculations for overflow (`count * element_size`, `length + terminator`, capacity growth) and centralize checked allocation helpers.
4. Define panic/trap behavior and make runtime APIs consistently report errors rather than mixing silent no-ops, sentinel values, and process aborts.
5. Document thread/task handle ownership, cancellation, joining, channel close semantics, and synchronization requirements. Add race-detector and sanitizer coverage; shared-handle operations need a clear synchronization contract.

### Compiler architecture and diagnostics

1. Keep structured diagnostics (code, severity, source span, related spans) as first-class compiler data. Render text only at CLI/LSP boundaries.
2. Centralize source spans in byte offsets and provide tested UTF-8/UTF-16 mapping helpers for editor clients.
3. Make formatter and language-server features consume the compiler lexer/parser/semantic model. This avoids drift between separate implementations.
4. Add fuzz targets for lexer, parser recovery, formatter round-tripping, constant evaluation, type conversions, and malformed source. Seed with real `.loom` files and minimize crashing inputs.
5. Build a compiler regression corpus covering integer boundaries, overflow, signedness, casts, pointer arithmetic, arrays/slices, nested scopes, generics/modules, tasks, and malformed input. Run under ASan, UBSan, and where practical MSan/LeakSanitizer.

### Performance and binary-size measurement

1. Establish a reproducible benchmark suite with pinned compiler versions, optimization flags, CPU, and warm-up/repetition rules. Compare equivalent algorithms and I/O behavior in Loom, C, and Rust.
2. Record throughput/latency, allocations, peak RSS, code size, and compile time. Report variance and avoid single-run speed claims.
3. Inspect generated LLVM IR and native assembly for representative loops, bounds checks, integer arithmetic, calls, and allocations. Add optimization remarks and compare debug/release builds.
4. Measure binary size both stripped and unstripped, with static/dynamic runtime choices stated. Current build warnings about duplicate LLVM libraries and deployment target mismatch should be resolved before release-size or portability claims.
5. Treat optimizations as evidence-driven: profile allocator pressure, unnecessary Text copies, runtime call overhead, and codegen quality before introducing complexity.

### Portability and developer experience

1. Make CMake select LLVM backends based on the configured target instead of linking all listed architectures unconditionally; verify Linux, macOS, and supported Windows targets in CI.
2. Align the macOS deployment target with the LLVM toolchain actually used to link release builds.
3. Make networking behavior explicit: hostname/IPv6 support, partial TCP sends, EINTR, invalid-address errors, and sandboxed test environments.
4. Remove or resolve the unused/dead-code warnings in `src/driver.cpp`; keep warning-clean builds as a quality gate.
5. For the extension, maintain the language server as a compiler client with versioned semantic output. Continue smoke-testing packaged VSIX behavior separately from source-tree tests.

## Recommended priority

1. Fix Text/String ownership and escaping first; add sanitizer regressions.
2. Fix send/receive bounds and distinguish binary payloads from text.
3. Fix `Text` clone and subslice ownership, then specify embedded-NUL semantics consistently.
4. Fix UTF-8 iteration and source-position conversion.
5. Replace formatter punctuation splitting with compiler tokenization and add round-trip checks.
6. Stabilize diagnostic IDs and improve environment-aware network tests.
7. Establish cross-language performance, memory, and binary-size benchmarks before making optimization claims.

## Follow-up fixes applied

The audit follow-up changed the checkout after the findings above were recorded:

- `loom_text_clone` now copies the recorded byte length, including embedded NUL bytes.
- TCP and UDP send code derives String byte lengths with `strlen` and rejects unsupported data types instead of reading an arbitrary 64 bytes.
- TCP/UDP receive paths now allocate one extra terminator byte; UDP receive appends a NUL, and TCP receives the requested payload length into the expanded buffer.
- `Loop Character in Text` now decodes UTF-8 scalars, advances by scalar width, and replaces malformed sequences with U+FFFD.
- The formatter keeps common multi-character operators as single tokens. Regression coverage checks `>=` formatting and non-ASCII Text iteration.
- Lexer tab columns now count a tab as one source character, matching editor/LSP character offsets.
- Returning a direct Text-to-String or `To_String(TextVariable)` view skips destruction of the source Text buffer, preventing the confirmed immediate use-after-free.

**Remaining ownership limitation:** The last fix transfers the Text allocation to the returned raw `String` pointer, but Loom does not yet have an owning String representation or a release operation for that allocation. Repeated calls that return such views can therefore leak memory. Text subslices also still allocate a raw String without deterministic reclamation. A proper fix needs explicit owned versus borrowed string/slice types (or equivalent lifetime and cleanup semantics); treating this as solved would be misleading.

**Follow-up verification:** The project rebuilt successfully; the focused Unicode/formatter regression tests passed; the embedded-NUL clone reproduction now reports source and clone lengths of 3; the prior Text return reproduction prints correctly under AddressSanitizer with leak detection disabled. All seven CTest suites excluding the host-restricted networking suite passed, and the VS Code extension's five tests passed. The full modern feature suite still fails at its localhost networking assertion because this environment denies local socket operations; this is the same restriction identified during the audit.
