# VirtualBox-Wasm

An experiment to compile Oracle VirtualBox to WebAssembly for preservation and browser-based use.

---

## Goal

Run a minimal VirtualBox environment entirely in the browser via WebAssembly — no native install, no plugins. The target is a headless VM capable of booting a simple OS image using software-only CPU emulation (no VT-x/AMD-V, as hardware virtualization is unavailable in Wasm).

This is a research project. The build will fail many times before it succeeds.

---

## Plan — Iterative Wasm Port

### Why it is hard

VirtualBox is a deeply native application:

- Core VMM uses ring-0 kernel modules (`vboxdrv`) and hardware virtualization (VT-x / AMD-V)
- The build system (kBuild) has no Emscripten target
- Large parts of the codebase use inline assembly, Linux-specific syscalls, and POSIX threading
- The JIT recompiler cannot generate code at runtime in Wasm (no W^X memory)

The strategy is to strip away everything that cannot work in Wasm and build up from the smallest compilable unit toward a running VM.

---

### Phase 0 — Native Linux Build (done)

**Goal**: Prove the source tree builds cleanly.

- [x] Clone VirtualBox source from `github.com/VirtualBox/virtualbox`
- [x] Clone kBuild submodule from `github.com/VirtualBox/kbuild`
- [x] Write `Dockerfile.build` for a reproducible Ubuntu 22.04 build environment
- [x] Successful `kmk` build producing `VBoxSVC`, `VBoxManage`, `VBoxDD.so`, etc.

---

## Phase 1 Research Findings

### VBoxRT Blockers (from source audit)

- 226 `.asm` files with inline x86/x64 assembly — must be replaced with C fallbacks or stubbed
- 166 `r0drv/` kernel-mode files — remove entirely (cannot run in Wasm)
- 20+ files with direct Linux syscalls (`io_submit`, `gettid`, `fork`) — stub or use Emscripten FS
- 19 files using pthreads — stub for single-threaded Wasm or use Emscripten pthreads with SharedArrayBuffer
- ~200 files total need changes out of 1,367 in VBoxRT

### kBuild / Configure Findings

- `wasm` is not a native `KBUILD_TARGET` — bypass by writing `AutoConfig.wasm.kmk` with variable overrides
- `VBoxXClang.kmk` can be adapted into `VBoxXEmscripten.kmk` (`emcc`/`em++`/`emar` replace `clang`/`clang++`/`llvm-ar`)
- Useful configure flags to minimise the build:
  ```
  --build-headless --disable-hardening --disable-docs --disable-additions
  --disable-opengl --disable-sdl --disable-extpack --disable-validationkit
  --disable-dtrace --disable-python --disable-libpam --disable-libcap
  --disable-libasound --disable-libpulse --disable-java --disable-gsoap
  ```

### IEM (Interpreted Execution Manager) — 49 source files total

- 9 core files (`IEMAll.cpp`, `IEMAllMem.cpp`, `IEMAllTlb.cpp`, etc.)
- 40 x86 target files in `target-x86/` (instruction tables, opcode helpers, FPU, exceptions)
- 10 ARM64 files in `target-armv8/` (not needed for x86 emulation)
- `IEMAllN8ve*.cpp` and `IEMAllThrd*.cpp` are JIT/recompiler — must be stubbed for Wasm

### Files Created as Part of Phase 1

| File | Purpose |
|---|---|
| `Dockerfile.wasm` | Emscripten build environment |
| `tools/kBuildTools/VBoxXEmscripten.kmk` | kBuild tool definition for `emcc`/`em++` |
| `AutoConfig.wasm.kmk` | Feature flags and compiler overrides for Wasm build |

---

### Phase 1 — Emscripten Toolchain in Docker

**Goal**: Get Emscripten into the build environment and compile a trivial VirtualBox C file to Wasm.

Steps:
- [x] Add Emscripten SDK (`emsdk`) to a new `Dockerfile.wasm`
- [x] Research VBoxRT incompatibilities (226 asm files, 166 r0drv files, 20 syscall files, 19 pthread files)
- [x] Research configure flags for minimal build
- [x] Create `VBoxXEmscripten.kmk` tool definition
- [x] Create `AutoConfig.wasm.kmk` override
- [ ] First successful `emcc` compilation of a single VirtualBox source file
- [ ] Resolve inline assembly blockers in `VBoxRT/common/`

---

### Phase 2 — VBoxRT (Runtime Library)

**Goal**: Compile `src/VBox/Runtime` to a Wasm static library.

VBoxRT is the foundation everything else depends on. It provides threading, file I/O, memory, string formatting, and timers.

Steps:
- Stub or emulate unsupported APIs (e.g. `RTThreadCreate` → Emscripten pthreads, `RTFileOpen` → Emscripten FS)
- Disable or stub Linux-specific backends (e.g. `r0drv/`, `solaris/`, `darwin/`)
- Compile with `-pthread` for SharedArrayBuffer-backed threading
- Expected failures: dozens of missing syscall wrappers, platform-specific assembly

---

### Phase 3 — IEM (Interpreted Execution Manager)

**Goal**: Compile the software CPU emulator (`src/VBox/VMM/VMMAll/IEM*`) to Wasm.

IEM is VirtualBox's pure-software x86/x64 interpreter. It is the only execution engine that can work in Wasm because it does not generate native code at runtime.

Steps:
- Disable all JIT/recompiler paths (`VBOX_WITH_RECOMPILER=0`, `VBOX_WITH_IEM_RECOMPILER=0`)
- Compile IEM and its instruction tables
- Stub VMM services IEM depends on (CPUM, PGM, EM, etc.) with minimal implementations
- Expected failures: circular dependencies across VMM subsystems

---

### Phase 4 — Minimal Device Layer

**Goal**: Bring up enough device emulation to boot from a disk image.

Target device set (minimal):
- PIT (timer), PIC (interrupts), RTC
- PS/2 keyboard
- VGA (text mode only, framebuffer mapped to a canvas)
- IDE/ATA controller + disk image via Emscripten FS or `fetch()`

Steps:
- Compile `src/VBox/Devices` selectively, including only the above
- Stub the HGCM/shared folders/USB/network subsystems
- Wire a virtual disk image loaded from a URL via `fetch()` into the IDE controller

---

### Phase 5 — Headless VM Bootstrap

**Goal**: Link everything into a single `.wasm` binary and boot a VM.

Steps:
- Write a minimal C `main()` that initialises VBoxRT, IEM, and the device layer without XPCOM/COM
- Link with Emscripten, produce `virtualbox.js` + `virtualbox.wasm`
- Boot a tiny OS image (e.g. a minimal DOS or Linux with `CONFIG_NOHZ=n`)
- Wire the VGA framebuffer output to an HTML5 `<canvas>`
- Wire keyboard events from the browser into the PS/2 emulator

---

### Phase 6 — Qt 6 Manager GUI for WebAssembly

**Goal**: Compile the real VirtualBox Manager Qt GUI (`src/VBox/Frontends/VirtualBox/`) to WebAssembly — pixel-perfect, no HTML clone.

#### Why Qt for WebAssembly

Qt 6.5+ officially supports `wasm-emscripten` as a first-class platform target. Qt renders all widgets into an HTML5 `<canvas>` element via its own OpenGL ES → WebGL backend. This means the real `VirtualBox.cpp` / `UIMainWindow.cpp` source compiles unchanged — no reimplementation needed.

#### Threading

Qt Wasm with multi-threading requires `SharedArrayBuffer`, which in turn requires the browser to serve the page with:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

GitHub Pages supports these headers — no extra server config needed.

#### XPCOM / IPC replacement strategy

The VirtualBox Manager communicates with `VBoxSVC` via XPCOM IPC (inter-process COM). In a Wasm single-process environment there is no second process, so:

- XPCOM IPC is replaced with direct in-process C++ API calls to the VM engine
- `VBoxSVC` is linked into the same `.wasm` binary as the GUI
- A thin shim layer (`wasm-api/`) wraps the internal API in XPCOM-compatible interfaces so the GUI source needs minimal changes

Steps:
- [ ] Build Qt 6 for Wasm from source using `Dockerfile.qt-wasm`
- [ ] Wire `src/VBox/Frontends/VirtualBox/` CMake build against Qt/Wasm sysroot
- [ ] Stub / remove XPCOM IPC, replace with in-process shim
- [ ] Produce `VirtualBoxManager.js` + `VirtualBoxManager.wasm`
- [ ] Host on GitHub Pages with correct COOP/COEP headers

---

### Phase 7 — Browser Frontend Integration

**Goal**: A complete browser experience — VM engine + GUI in one page.

- Link VM engine Wasm (Phase 5) with Manager GUI Wasm (Phase 6) into one binary
- VGA framebuffer mapped to the Qt window's `<canvas>`
- Drag-and-drop or URL-based disk image loading
- Basic on-screen controls (power, reset, snapshot)
- GitHub Pages deployment

---

## Build Files

| File | Purpose |
|---|---|
| [Dockerfile.build](Dockerfile.build) | Native Linux build (Ubuntu 22.04, gcc) — Phase 0 |
| [Dockerfile.wasm](Dockerfile.wasm) | Wasm VM engine build (Emscripten) — Phase 1+ |
| [Dockerfile.qt-wasm](Dockerfile.qt-wasm) | Qt 6 for Wasm build + VirtualBox Manager GUI — Phase 6 |
| `AutoConfig.wasm.kmk` | kBuild overrides for Emscripten compiler |
| `tools/kBuildTools/VBoxXEmscripten.kmk` | kBuild tool definition for emcc/em++ |

---

## How to Build

### Native (Phase 0 — working)

```bash
docker build -f Dockerfile.build -t virtualbox-build .
```

Output lands in `/build/vbox/out/linux.amd64/release/bin/` inside the image.

### Wasm VM engine (Phase 1+ — in progress)

```bash
docker build -f Dockerfile.wasm -t virtualbox-wasm .
```

### Qt 6 for Wasm + Manager GUI (Phase 6 — in progress)

```bash
# Stage 1 builds Qt 6 from source (~30-60 min, cached after first run)
# Stage 2 compiles the VirtualBox Manager GUI against the Qt/Wasm sysroot
docker build -f Dockerfile.qt-wasm -t virtualbox-qt-wasm .

# Extract build artifacts
docker create --name vbox-qt-out virtualbox-qt-wasm
docker cp vbox-qt-out:/output ./wasm-output
docker rm vbox-qt-out
```

Output: `wasm-output/VirtualBoxManager.{js,wasm,html}`

---

## References

- [Emscripten documentation](https://emscripten.org/docs/)
- [Qt for WebAssembly](https://doc.qt.io/qt-6/wasm.html)
- [VirtualBox build instructions](https://www.virtualbox.org/wiki/Build_instructions)
- [IEM — VirtualBox's software CPU interpreter](https://www.virtualbox.org/wiki/IEM)
- [v86](https://github.com/copy/v86) — prior art: x86 emulator in JavaScript/Wasm
- [JSLinux](https://bellard.org/jslinux/) — prior art: QEMU compiled to Wasm
