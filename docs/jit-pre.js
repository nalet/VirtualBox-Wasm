// jit-pre.js — x86 fast interpreter for VirtualBox/Wasm
// Embedded via --pre-js. Runs in all threads (main + workers).
// Called from EM_JS hook in IEM execution loop.
// V8/SpiderMonkey JIT-compiles this interpreter into fast native code.
'use strict';

globalThis.VBoxJIT = (function() {

// ── CPUMCTX register offsets (from cpumctx-x86-amd64.h) ──
const R_AX=0,R_CX=8,R_DX=0x10,R_BX=0x18,R_SP=0x20,R_BP=0x28,R_SI=0x30,R_DI=0x38;
const R_IP=0x140, R_FLAGS=0x148;
// Segment registers: each 24 bytes (sel[2],pad[2],validSel[2],flags[2],base[8],limit[4],attr[4])
const S_ES=0x80,S_CS=0x98,S_SS=0xB0,S_DS=0xC8,S_FS=0xE0,S_GS=0xF8;
const SEG_BASE=8, SEG_LIMIT=16, SEG_ATTR=20, SEG_SEL=0, SEG_VALIDSEL=4, SEG_FLAGS=6;
// CR0-CR4
const R_CR0=0x160;
const R_CR2=0x168;
const R_CR3=0x170;
const R_CR4=0x178;
const X86DESCATTR_D = 0x4000; // bit 14 of segment descriptor Attr.u

// ── Lazy flags ──
const OP_NONE=0,OP_ADD=1,OP_SUB=2,OP_AND=3,OP_OR=4,OP_XOR=5,OP_INC=6,OP_DEC=7,OP_SHL=8,OP_SHR=9,OP_SAR=10,OP_ROL=11,OP_ROR=12,OP_EXPLICIT=13;
let lazyOp=OP_EXPLICIT, lazyRes=0, lazyOp1=0, lazyOp2=0, lazySize=16, lazyCF=0;
// OP_EXPLICIT: all flags stored in lazyExplicitFlags (from loadFlags/SAHF/POPF).
// OP_NONE:     ZF/SF from lazyRes/lazySize, CF from lazyCF, OF/PF/AF also from lazyRes.
let lazyExplicitFlags=0x02; // bit 1 always set

// Parity lookup (even parity = 1)
const parityTable = new Uint8Array(256);
for (let i = 0; i < 256; i++) {
  let b = i, p = 0;
  for (let j = 0; j < 8; j++) { p ^= (b & 1); b >>= 1; }
  parityTable[i] = p ? 0 : 1; // PF=1 means even parity
}

const SIZE_MASK = [0, 0xFF, 0xFFFF, 0, 0xFFFFFFFF];
const SIZE_SIGN = [0, 0x80, 0x8000, 0, 0x80000000];

function getCF() {
  if (lazyOp === OP_EXPLICIT) return (lazyExplicitFlags >> 0) & 1;
  switch (lazyOp) {
    case OP_ADD: return (lazyRes & SIZE_MASK[lazySize]) < (lazyOp1 & SIZE_MASK[lazySize]) ? 1 : 0;
    case OP_SUB: return (lazyOp1 & SIZE_MASK[lazySize]) < (lazyOp2 & SIZE_MASK[lazySize]) ? 1 : 0;
    case OP_AND: case OP_OR: case OP_XOR: return 0;
    default: return lazyCF; // OP_NONE and shift ops use lazyCF
  }
}

function getZF() {
  if (lazyOp === OP_EXPLICIT) return (lazyExplicitFlags >> 6) & 1;
  return ((lazyRes & SIZE_MASK[lazySize]) === 0) ? 1 : 0;
}

function getSF() {
  if (lazyOp === OP_EXPLICIT) return (lazyExplicitFlags >> 7) & 1;
  return ((lazyRes & SIZE_SIGN[lazySize]) !== 0) ? 1 : 0;
}

function getOF() {
  if (lazyOp === OP_EXPLICIT) return (lazyExplicitFlags >> 11) & 1;
  const m = SIZE_MASK[lazySize], s = SIZE_SIGN[lazySize];
  switch (lazyOp) {
    case OP_ADD: return ((~(lazyOp1 ^ lazyOp2) & (lazyOp1 ^ lazyRes)) & s) ? 1 : 0;
    case OP_SUB: return (((lazyOp1 ^ lazyOp2) & (lazyOp1 ^ lazyRes)) & s) ? 1 : 0;
    case OP_AND: case OP_OR: case OP_XOR: return 0;
    case OP_INC: return ((lazyRes & m) === (s & m)) ? 1 : 0;
    case OP_DEC: return ((lazyRes & m) === ((s - 1) & m)) ? 1 : 0;
    default: return 0;
  }
}

function getPF() {
  if (lazyOp === OP_EXPLICIT) return (lazyExplicitFlags >> 2) & 1;
  return parityTable[lazyRes & 0xFF];
}

function getAF() {
  if (lazyOp === OP_EXPLICIT) return (lazyExplicitFlags >> 4) & 1;
  if (lazyOp === OP_ADD || lazyOp === OP_SUB || lazyOp === OP_INC || lazyOp === OP_DEC)
    return ((lazyOp1 ^ lazyOp2 ^ lazyRes) & 0x10) ? 1 : 0;
  return 0;
}

function flagsToWord() {
  if (lazyOp === OP_EXPLICIT) return lazyExplicitFlags | 0x02;
  return (getCF()) | (getPF() << 2) | (getAF() << 4) | (getZF() << 6) |
         (getSF() << 7) | (getOF() << 11) | 0x02;
}

function loadFlags(val) {
  lazyOp = OP_EXPLICIT;
  lazyCF = val & 1; // keep lazyCF in sync for instructions that read it directly
  lazyExplicitFlags = val | 0x02; // store all bits explicitly
}

function setFlagsArith(op, res, op1, op2, size) {
  lazyOp = op; lazyRes = res; lazyOp1 = op1; lazyOp2 = op2; lazySize = size;
}

// ── Memory access helpers ──
let mem8, dv;
let cpuPtr = 0, ramBase = 0;

// ROM overlay buffer (set by C++ via wasmJitSetRomBuffer)
let romBufBase = 0;   // offset in Wasm linear memory
let romBufSize = 0;   // size in bytes (256KB)
let romGCPhysStart = 0; // guest physical start (0xC0000)
let romGCPhysEnd = 0;   // guest physical end (0x100000)

// High RAM (>= 0x100000) — set by C++ via wasmJitSetHighRAM
// PGM allocates high RAM in a separate range from low RAM (0-0x9FFFF).
// highRamPtr is the Wasm memory offset of GCPhys 0x100000.
let highRamPtr = 0;    // Wasm memory offset for GCPhys 0x100000
let highRamSize = 0;   // size in bytes (e.g. 31MB for 32MB guest)
let highRamEnd = 0;    // GCPhys end = 0x100000 + highRamSize

function init(memory) {
  mem8 = new Uint8Array(memory.buffer);
  dv = new DataView(memory.buffer);
}

function refreshViews() {
  if (mem8.buffer !== wasmMemory.buffer) {
    mem8 = new Uint8Array(wasmMemory.buffer);
    dv = new DataView(wasmMemory.buffer);
  }
}

// Called from C++ after PGMPhysRead copies ROM content
function setRomBuffer(bufPtr, bufSize, gcPhysStart) {
  romBufBase = bufPtr;
  romBufSize = bufSize;
  romGCPhysStart = gcPhysStart;
  romGCPhysEnd = gcPhysStart + bufSize;
  console.log('[JIT] ROM buffer set: base=0x' + bufPtr.toString(16) +
    ' size=' + (bufSize/1024) + 'KB range=0x' + gcPhysStart.toString(16) +
    '-0x' + romGCPhysEnd.toString(16));
}

// Called from C++ after PGMPhysGCPhys2CCPtr maps high RAM
function setHighRAM(ptr, size) {
  highRamPtr = ptr;
  highRamSize = size;
  highRamEnd = 0x100000 + size;
  console.log('[JIT] High RAM set: ptr=0x' + ptr.toString(16) +
    ' size=' + (size >> 20) + 'MB range=0x100000-0x' + highRamEnd.toString(16));
}

// ── Gzip/DEFLATE decompressor for fast kernel boot ──
// Decompresses bzImage kernel payload in JavaScript, skipping the 20-minute
// IEM instruction-by-instruction decompression of the kernel's startup_64 code.

function jsGunzip(input) {
  if (input[0] !== 0x1f || input[1] !== 0x8b) return null; // not gzip
  if (input[2] !== 8) return null; // must be deflate method
  const flags = input[3];
  let pos = 10;
  if (flags & 4) { const xlen = input[pos] | (input[pos+1] << 8); pos += 2 + xlen; }
  if (flags & 8) { while (input[pos++] !== 0); }
  if (flags & 16) { while (input[pos++] !== 0); }
  if (flags & 2) pos += 2;
  return jsInflate(input, pos);
}

function jsInflate(data, pos) {
  // Use Wasm heap for output to avoid JS ArrayBuffer allocation failures.
  // The Wasm heap is already 1GB+ so 40MB allocation always succeeds.
  const INITIAL_SIZE = 40 * 1024 * 1024; // 40MB — enough for most kernels
  let outBuf;
  try {
    // Try Wasm-backed allocation first (always succeeds in worker thread)
    if (typeof wasmMemory !== 'undefined' && wasmMemory.buffer) {
      outBuf = new Uint8Array(wasmMemory.buffer, wasmMemory.buffer.byteLength - INITIAL_SIZE, INITIAL_SIZE);
      // This uses the END of Wasm memory as scratch space.
      // It's safe because the Wasm heap grows from the front.
    } else {
      outBuf = new Uint8Array(INITIAL_SIZE);
    }
  } catch(e) {
    // Fallback to small JS allocation
    outBuf = new Uint8Array(4 * 1024 * 1024);
  }
  let outPos = 0;
  let bitBuf = 0, bitCnt = 0;
  const usingWasmMem = outBuf.buffer === (typeof wasmMemory !== 'undefined' ? wasmMemory.buffer : null);

  function bits(n) {
    while (bitCnt < n) { bitBuf |= data[pos++] << bitCnt; bitCnt += 8; }
    const v = bitBuf & ((1 << n) - 1); bitBuf >>>= n; bitCnt -= n; return v;
  }

  function grow(need) {
    if (usingWasmMem) return; // Wasm buffer is pre-allocated large enough
    while (outPos + need > outBuf.length) {
      const b = new Uint8Array(outBuf.length * 2); b.set(outBuf); outBuf = b;
    }
  }

  function buildTree(lens, n) {
    const cnt = new Uint16Array(16), offs = new Uint16Array(16);
    for (let i = 0; i < n; i++) cnt[lens[i]]++;
    for (let i = 1; i < 16; i++) offs[i] = offs[i-1] + cnt[i-1];
    const syms = new Uint16Array(n);
    for (let i = 0; i < n; i++) if (lens[i]) syms[offs[lens[i]]++] = i;
    return { cnt, syms };
  }

  function decode(t) {
    let code = 0, first = 0, idx = 0;
    for (let len = 1; len <= 15; len++) {
      code |= bits(1);
      const c = t.cnt[len];
      if (code < first + c) return t.syms[idx + (code - first)];
      idx += c; first = (first + c) << 1; code <<= 1;
    }
    return -1;
  }

  const lenBase = [3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258];
  const lenXtra = [0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0];
  const dstBase = [1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577];
  const dstXtra = [0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13];

  let fixLit, fixDst;
  function buildFixed() {
    const l = new Uint8Array(288);
    for (let i = 0; i < 144; i++) l[i] = 8;
    for (let i = 144; i < 256; i++) l[i] = 9;
    for (let i = 256; i < 280; i++) l[i] = 7;
    for (let i = 280; i < 288; i++) l[i] = 8;
    fixLit = buildTree(l, 288);
    const d = new Uint8Array(30); for (let i = 0; i < 30; i++) d[i] = 5;
    fixDst = buildTree(d, 30);
  }

  let bfinal = 0;
  while (!bfinal) {
    bfinal = bits(1);
    const btype = bits(2);
    if (btype === 0) {
      bitBuf = 0; bitCnt = 0;
      const len = data[pos] | (data[pos+1] << 8); pos += 4;
      grow(len);
      for (let i = 0; i < len; i++) outBuf[outPos++] = data[pos++];
    } else {
      let lt, dt;
      if (btype === 1) {
        if (!fixLit) buildFixed();
        lt = fixLit; dt = fixDst;
      } else {
        const hlit = bits(5) + 257, hdist = bits(5) + 1, hclen = bits(4) + 4;
        const clOrd = [16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15];
        const cl = new Uint8Array(19);
        for (let i = 0; i < hclen; i++) cl[clOrd[i]] = bits(3);
        const clT = buildTree(cl, 19);
        const all = new Uint8Array(hlit + hdist);
        let ai = 0;
        while (ai < hlit + hdist) {
          const s = decode(clT);
          if (s < 16) { all[ai++] = s; }
          else if (s === 16) { const r = bits(2) + 3, p = all[ai-1]; for (let j = 0; j < r; j++) all[ai++] = p; }
          else if (s === 17) { ai += bits(3) + 3; }
          else { ai += bits(7) + 11; }
        }
        lt = buildTree(all.subarray(0, hlit), hlit);
        dt = buildTree(all.subarray(hlit), hdist);
      }
      for (;;) {
        const s = decode(lt);
        if (s < 256) { grow(1); outBuf[outPos++] = s; }
        else if (s === 256) break;
        else {
          const li = s - 257, length = lenBase[li] + bits(lenXtra[li]);
          const di = decode(dt), dist = dstBase[di] + bits(dstXtra[di]);
          grow(length);
          for (let j = 0; j < length; j++) outBuf[outPos + j] = outBuf[outPos - dist + j];
          outPos += length;
        }
      }
    }
  }
  return outBuf.subarray(0, outPos);
}

// Parse ELF64 binary, return entry point and PT_LOAD segments
function parseELF64(data) {
  if (data[0] !== 0x7f || data[1] !== 0x45 || data[2] !== 0x4c || data[3] !== 0x46) return null;
  if (data[4] !== 2) return null; // must be ELFCLASS64
  const edv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const entry = edv.getBigUint64(24, true);
  const phoff = Number(edv.getBigUint64(32, true));
  const phentsz = edv.getUint16(54, true);
  const phnum = edv.getUint16(56, true);
  const segs = [];
  for (let i = 0; i < phnum; i++) {
    const o = phoff + i * phentsz;
    if (edv.getUint32(o, true) !== 1) continue; // PT_LOAD only
    segs.push({
      offset: Number(edv.getBigUint64(o + 8, true)),
      vaddr: edv.getBigUint64(o + 16, true),
      paddr: edv.getBigUint64(o + 24, true),
      filesz: Number(edv.getBigUint64(o + 32, true)),
      memsz: Number(edv.getBigUint64(o + 40, true))
    });
  }
  return { entry, segs };
}

// Build x86-64 4-level page tables in guest RAM for kernel fast boot.
// Maps identity (GPA=GVA) + kernel high half (0xFFFFFFFF80000000+GPA).
// Uses 2MB pages for efficiency. Returns CR3 value (GPA of PML4).
function buildPageTables64(totalRAM) {
  const PT_GPA = 0x800000; // place at guest 8MB
  const pt = highRamPtr + (PT_GPA - 0x100000); // wasm offset
  const pdv = new DataView(mem8.buffer, 0);
  // Clear 32KB for page tables
  mem8.fill(0, pt, pt + 0x8000);

  const P = 1, W = 2, PS = 0x80; // Present, Writable, PageSize(2MB)
  const pml4 = pt;
  const pdptLo = pt + 0x1000;
  const pdptHi = pt + 0x2000;
  const pdLo = pt + 0x3000;
  const pdHi = pt + 0x4000;

  // PML4[0] → PDPT-low (identity)
  pdv.setBigUint64(pml4, BigInt(PT_GPA + 0x1000) | BigInt(P|W), true);
  // PML4[511] → PDPT-high (kernel at 0xFFFFFFFF80000000)
  pdv.setBigUint64(pml4 + 511*8, BigInt(PT_GPA + 0x2000) | BigInt(P|W), true);

  // PDPT-low[0] → PD-low
  pdv.setBigUint64(pdptLo, BigInt(PT_GPA + 0x3000) | BigInt(P|W), true);
  // PDPT-high[510] → PD-high (0xFFFFFFFF80000000 = PML4[511]:PDPT[510])
  pdv.setBigUint64(pdptHi + 510*8, BigInt(PT_GPA + 0x4000) | BigInt(P|W), true);

  // PD-low: identity-map first 1GB using 2MB pages
  const nPages = Math.min(512, Math.ceil(totalRAM / 0x200000));
  for (let i = 0; i < nPages; i++)
    pdv.setBigUint64(pdLo + i*8, BigInt(i * 0x200000) | BigInt(P|W|PS), true);

  // PD-high: same mapping for kernel virtual addresses
  for (let i = 0; i < nPages; i++)
    pdv.setBigUint64(pdHi + i*8, BigInt(i * 0x200000) | BigInt(P|W|PS), true);

  return PT_GPA;
}

// MMIO fault flag: set when a guest memory access goes to an MMIO address
// (outside Wasm linear memory). The main loop checks this flag and bails
// to IEM so the instruction is re-executed via the PGM MMIO handler.
let mmioFault = false;

// A20 gate mask: when A20 is disabled, bit 20 of physical addresses is masked
// to zero (addresses wrap at 1 MB). When A20 is enabled, all bits pass through.
// This must match PGM's A20 masking to avoid the JIT seeing different memory
// contents than IEM.  Set via globalThis.VBoxJIT._a20 (from wasmJitSetA20).
let a20Mask = 0xFFFFFFFF; // default: A20 enabled (all bits pass)

// Resolve guest physical address to Wasm memory offset.
// Returns the offset, or sets mmioFault and returns -1 for unmapped addresses.
// Memory map: low RAM (0-0x9FFFF), VGA MMIO (0xA0000-0xBFFFF) → bail,
// ROM (0xC0000-0xFFFFF) → romBuf, high RAM (0x100000+) → highRamPtr.
function resolvePhys(addr) {
  if (addr < 0xA0000) return ramBase + addr;
  if (addr >= 0x100000) {
    if (highRamPtr && addr < highRamEnd) return highRamPtr + (addr - 0x100000);
    mmioFault = true; return -1;
  }
  if (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd)
    return romBufBase + (addr - romGCPhysStart);
  mmioFault = true; return -1;
}

function guestRb(addr) {
  const off = resolvePhys(addr);
  if (off < 0) return 0xFF;
  return mem8[off];
}

function guestRw(addr) {
  const off = resolvePhys(addr);
  if (off < 0) return 0xFFFF;
  return dv.getUint16(off, true);
}

function guestRd(addr) {
  const off = resolvePhys(addr);
  if (off < 0) return 0xFFFFFFFF;
  return dv.getUint32(off, true);
}
function guestWb(addr, v) {
  const off = resolvePhysW(addr);
  if (off < 0) return;
  mem8[off] = v;
}

// Read CPU register (64-bit, return as Number — safe for 32-bit values)
function rr64(off) { return Number(dv.getBigUint64(cpuPtr + off, true)); }
function wr64(off, v) { dv.setBigUint64(cpuPtr + off, BigInt(v) & 0xFFFFFFFFFFFFFFFFn, true); }
function rr32(off) { return dv.getUint32(cpuPtr + off, true); }
function wr32(off, v) { dv.setUint32(cpuPtr + off, v >>> 0, true); }
function rr16(off) { return dv.getUint16(cpuPtr + off, true); }
function wr16(off, v) { dv.setUint16(cpuPtr + off, v & 0xFFFF, true); }
function rr8(off) { return dv.getUint8(cpuPtr + off); }
function wr8(off, v) { dv.setUint8(cpuPtr + off, v & 0xFF); }

// Write dword to guest physical memory via mem8 (little-endian)
function writeDword(addr, v) {
  mem8[addr] = v & 0xFF;
  mem8[addr + 1] = (v >>> 8) & 0xFF;
  mem8[addr + 2] = (v >>> 16) & 0xFF;
  mem8[addr + 3] = (v >>> 24) & 0xFF;
}

// Segment base (cached)
function segBase(segOff) { return Number(dv.getBigUint64(cpuPtr + segOff + SEG_BASE, true)); }

// Guest physical memory read/write (ROM-aware for reads, paging-aware)
// A20 masking is applied after address translation (linear -> physical),
// but NOT to ROM range addresses — ROM sits on the chipset bus above the A20 gate.
// In non-paging mode the linear address IS the physical address.
function rb(addr) {
  if (_pagingOn) {
    addr = translateLinear(addr >>> 0);
    if (addr < 0) { mmioFault = true; return 0xFF; }
  }
  // ROM sits on the chipset bus above the A20 gate — check ROM BEFORE A20 masking
  if (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd)
    return mem8[romBufBase + (addr - romGCPhysStart)];
  addr = (addr & a20Mask) >>> 0;
  return guestRb(addr);
}
function rw(addr) {
  if (_pagingOn) {
    addr = translateLinear(addr >>> 0);
    if (addr < 0) { mmioFault = true; return 0xFFFF; }
  }
  // ROM sits on the chipset bus above the A20 gate — check ROM BEFORE A20 masking
  if (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd) {
    const off = romBufBase + (addr - romGCPhysStart);
    return dv.getUint16(off, true);
  }
  addr = (addr & a20Mask) >>> 0;
  return guestRw(addr);
}
function rd(addr) {
  if (_pagingOn) {
    addr = translateLinear(addr >>> 0);
    if (addr < 0) { mmioFault = true; return 0xFFFFFFFF; }
  }
  // ROM sits on the chipset bus above the A20 gate — check ROM BEFORE A20 masking
  if (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd) {
    const off = romBufBase + (addr - romGCPhysStart);
    return dv.getUint32(off, true);
  }
  addr = (addr & a20Mask) >>> 0;
  return guestRd(addr);
}
// Resolve guest physical address for writes (no ROM — ROM is read-only)
function resolvePhysW(addr) {
  if (addr < 0xA0000) return ramBase + addr;
  if (addr >= 0x100000) {
    if (highRamPtr && addr < highRamEnd) return highRamPtr + (addr - 0x100000);
    mmioFault = true; return -1;
  }
  // 0xA0000-0xFFFFF: VGA MMIO and ROM area — bail to IEM
  mmioFault = true; return -1;
}

// Resolve a contiguous range [lo, hi) to Wasm offset of lo (read, includes ROM)
function resolveRange(lo, hi) {
  if (lo >= 0 && hi <= 0xA0000) return ramBase + lo;
  if (lo >= 0x100000 && hi <= highRamEnd && highRamPtr) return highRamPtr + (lo - 0x100000);
  if (romBufSize > 0 && lo >= romGCPhysStart && hi <= romGCPhysEnd) return romBufBase + (lo - romGCPhysStart);
  return -1;
}
// Resolve a contiguous range [lo, hi) to Wasm offset of lo (write, excludes ROM)
function resolveRangeW(lo, hi) {
  if (lo >= 0 && hi <= 0xA0000) return ramBase + lo;
  if (lo >= 0x100000 && hi <= highRamEnd && highRamPtr) return highRamPtr + (lo - 0x100000);
  return -1;
}

function wb(addr, v) {
  if (_pagingOn) {
    addr = translateLinear(addr >>> 0);
    if (addr < 0) { mmioFault = true; return; }
  }
  addr = (addr & a20Mask) >>> 0;
  const off = resolvePhysW(addr);
  if (off < 0) return;
  mem8[off] = v;
}
function ww(addr, v) {
  if (_pagingOn) {
    addr = translateLinear(addr >>> 0);
    if (addr < 0) { mmioFault = true; return; }
  }
  addr = (addr & a20Mask) >>> 0;
  const off = resolvePhysW(addr);
  if (off < 0) return;
  dv.setUint16(off, v & 0xFFFF, true);
}
function wd(addr, v) {
  if (_pagingOn) {
    addr = translateLinear(addr >>> 0);
    if (addr < 0) { mmioFault = true; return; }
  }
  addr = (addr & a20Mask) >>> 0;
  const off = resolvePhysW(addr);
  if (off < 0) return;
  dv.setUint32(off, v >>> 0, true);
}

// ── GPR access by index ──
const GPR_OFFS = [R_AX,R_CX,R_DX,R_BX,R_SP,R_BP,R_SI,R_DI];
function gr16(idx) { return rr16(GPR_OFFS[idx]); }
function sr16(idx, v) { wr16(GPR_OFFS[idx], v); }
function gr32(idx) { return rr32(GPR_OFFS[idx]); }
function sr32(idx, v) { wr32(GPR_OFFS[idx], v); }
// 8-bit: 0-3 = AL,CL,DL,BL; 4-7 = AH,CH,DH,BH
function gr8(idx) {
  if (idx < 4) return rr8(GPR_OFFS[idx]);
  return rr8(GPR_OFFS[idx - 4] + 1); // high byte
}
function sr8(idx, v) {
  if (idx < 4) wr8(GPR_OFFS[idx], v);
  else wr8(GPR_OFFS[idx - 4] + 1, v);
}

// ── Segment register access by index ──
const SEG_OFFS = [S_ES, S_CS, S_SS, S_DS, S_FS, S_GS, 0, 0]; // SREG encoding: 0=ES,1=CS,2=SS,3=DS,4=FS,5=GS

// ── Port I/O callback (set by C++ side) ──
let portInFn = null, portOutFn = null;
// ── I/O diagnostic: track IN/OUT per IP for stall detection ──
const ioDiagCounts = new Map();
// ── ATA I/O aggregate counter for BSY polling diagnostics ──
let ataStatusPolls = 0;      // total IN to 0x1F7/0x177
let ataStatusPollStart = 0;  // timestamp of first poll in current burst
let ataStatusLastReport = 0; // timestamp of last aggregate log
// ── BIOS debug port capture (ports 0x402/0x403/0x504) ──
let biosDebugBuf = '';
function biosDebugChar(c) {
  if (c === '\n' || c === '\r') {
    if (biosDebugBuf.length > 0) {
      console.log('[BIOS] ' + biosDebugBuf);
      biosDebugBuf = '';
    }
  } else {
    biosDebugBuf += c;
    if (biosDebugBuf.length >= 200) {
      console.log('[BIOS] ' + biosDebugBuf);
      biosDebugBuf = '';
    }
  }
}
// ── POST code port 0x80 tracking ──
let lastPost80 = -1;
let postCodes80 = [];  // log of POST codes

function portIn(port, size) {
  if (portInFn) return portInFn(port, size);
  return 0xFF; // floating bus
}
function portOut(port, size, val) {
  if (portOutFn) portOutFn(port, size, val);
}

// ── ModR/M decoding (16-bit addressing) ──
// Returns { ea: effective address (physical), disp: bytes consumed }
function decodeModRM16(modrm, code, codeOff, dsBase, ssBase) {
  const mod = (modrm >> 6) & 3;
  const rm = modrm & 7;

  if (mod === 3) return { ea: -1, reg: rm, len: 0 }; // register operand

  let ea = 0, len = 0;
  switch (rm) {
    case 0: ea = (gr16(3) + gr16(6)) & 0xFFFF; break; // BX+SI
    case 1: ea = (gr16(3) + gr16(7)) & 0xFFFF; break; // BX+DI
    case 2: ea = (gr16(5) + gr16(6)) & 0xFFFF; break; // BP+SI
    case 3: ea = (gr16(5) + gr16(7)) & 0xFFFF; break; // BP+DI
    case 4: ea = gr16(6); break; // SI
    case 5: ea = gr16(7); break; // DI
    case 6:
      if (mod === 0) {
        ea = code[codeOff] | (code[codeOff+1] << 8);
        len = 2;
      } else {
        ea = gr16(5); // BP
      }
      break;
    case 7: ea = gr16(3); break; // BX
  }

  // Default segment: SS for BP-based, DS for others
  let base = (rm === 2 || rm === 3 || (rm === 6 && mod !== 0)) ? ssBase : dsBase;

  if (mod === 1) {
    let d = code[codeOff + len];
    if (d > 127) d -= 256; // sign extend
    ea = (ea + d) & 0xFFFF;
    len += 1;
  } else if (mod === 2) {
    ea = (ea + (code[codeOff + len] | (code[codeOff + len + 1] << 8))) & 0xFFFF;
    len += 2;
  }

  return { ea: base + ea, len: len };
}

// ── ModR/M decoding (32-bit addressing) ──
function decodeModRM32(modrm, code, codeOff, dsBase, ssBase) {
  const mod = (modrm >> 6) & 3;
  const rm = modrm & 7;

  if (mod === 3) return { ea: -1, reg: rm, len: 0 };

  let ea = 0, len = 0;
  let base = dsBase;

  if (rm === 4) {
    // SIB byte
    const sib = code[codeOff]; len = 1;
    const scale = (sib >> 6) & 3;
    const index = (sib >> 3) & 7;
    const sibBase = sib & 7;

    if (sibBase === 5 && mod === 0) {
      ea = code[codeOff+1] | (code[codeOff+2]<<8) | (code[codeOff+3]<<16) | (code[codeOff+4]<<24);
      len = 5;
    } else {
      ea = gr32(sibBase);
      if (sibBase === 4 || sibBase === 5) base = ssBase;
    }
    if (index !== 4) {
      ea = (ea + (gr32(index) << scale)) >>> 0;
    }
  } else if (rm === 5 && mod === 0) {
    ea = code[codeOff] | (code[codeOff+1]<<8) | (code[codeOff+2]<<16) | (code[codeOff+3]<<24);
    len = 4;
  } else {
    ea = gr32(rm);
    if (rm === 4 || rm === 5) base = ssBase;
  }

  if (mod === 1) {
    let d = code[codeOff + len]; if (d > 127) d -= 256;
    ea = (ea + d) >>> 0;
    len += 1;
  } else if (mod === 2) {
    ea = (ea + (code[codeOff+len] | (code[codeOff+len+1]<<8) | (code[codeOff+len+2]<<16) | (code[codeOff+len+3]<<24))) >>> 0;
    len += 4;
  }

  return { ea: (base + ea) >>> 0, len: len };
}

// ── ALU operations ──
function alu8(op, a, b) {
  let r;
  switch (op) {
    case 0: r = a + b; setFlagsArith(OP_ADD,r,a,b,1); lazyCF = (r > 0xFF) ? 1 : 0; return r & 0xFF;
    case 1: r = a | b; setFlagsArith(OP_OR,r,a,b,1); return r & 0xFF;
    case 2: r = a + b + getCF(); setFlagsArith(OP_ADD,r,a,b+getCF(),1); lazyCF = (r > 0xFF) ? 1 : 0; return r & 0xFF;
    case 3: r = a - b - getCF(); setFlagsArith(OP_SUB,r,a,b+getCF(),1); lazyCF = (r < 0) ? 1 : 0; return ((r & 0xFF) + 256) & 0xFF;
    case 4: r = a & b; setFlagsArith(OP_AND,r,a,b,1); return r & 0xFF;
    case 5: r = a - b; setFlagsArith(OP_SUB,r,a,b,1); lazyCF = (a < b) ? 1 : 0; return ((r & 0xFF) + 256) & 0xFF;
    case 6: r = a ^ b; setFlagsArith(OP_XOR,r,a,b,1); return r & 0xFF;
    case 7: r = a - b; setFlagsArith(OP_SUB,r,a,b,1); lazyCF = (a < b) ? 1 : 0; return a; // CMP: don't store
    default: return a;
  }
}

function alu16(op, a, b) {
  a &= 0xFFFF; b &= 0xFFFF;
  let r;
  switch (op) {
    case 0: r = a + b; setFlagsArith(OP_ADD,r,a,b,2); lazyCF = (r > 0xFFFF) ? 1 : 0; return r & 0xFFFF;
    case 1: r = a | b; setFlagsArith(OP_OR,r,a,b,2); return r & 0xFFFF;
    case 2: { const c = getCF(); r = a + b + c; setFlagsArith(OP_ADD,r,a,b+c,2); lazyCF = (r > 0xFFFF) ? 1 : 0; return r & 0xFFFF; }
    case 3: { const c = getCF(); r = a - b - c; setFlagsArith(OP_SUB,r,a,b+c,2); lazyCF = (r < 0) ? 1 : 0; return r & 0xFFFF; }
    case 4: r = a & b; setFlagsArith(OP_AND,r,a,b,2); return r;
    case 5: r = a - b; setFlagsArith(OP_SUB,r,a,b,2); lazyCF = (a < b) ? 1 : 0; return r & 0xFFFF;
    case 6: r = a ^ b; setFlagsArith(OP_XOR,r,a,b,2); return r;
    case 7: r = a - b; setFlagsArith(OP_SUB,r,a,b,2); lazyCF = (a < b) ? 1 : 0; return a;
    default: return a;
  }
}

function alu32(op, a, b) {
  a = a >>> 0; b = b >>> 0;
  let r;
  switch (op) {
    case 0: r = (a + b) >>> 0; setFlagsArith(OP_ADD,r,a,b,4); lazyCF = (r < a) ? 1 : 0; return r;
    case 1: r = (a | b) >>> 0; setFlagsArith(OP_OR,r,a,b,4); return r;
    case 2: { const c = getCF(); r = (a + b + c) >>> 0; setFlagsArith(OP_ADD,r,a,b+c,4); lazyCF = (c ? r <= a : r < a) ? 1 : 0; return r; }
    case 3: { const c = getCF(); r = (a - b - c) >>> 0; setFlagsArith(OP_SUB,r,a,b+c,4); lazyCF = (c ? a <= b : a < b) ? 1 : 0; return r; }
    case 4: r = (a & b) >>> 0; setFlagsArith(OP_AND,r,a,b,4); return r;
    case 5: r = (a - b) >>> 0; setFlagsArith(OP_SUB,r,a,b,4); lazyCF = (a < b) ? 1 : 0; return r;
    case 6: r = (a ^ b) >>> 0; setFlagsArith(OP_XOR,r,a,b,4); return r;
    case 7: r = (a - b) >>> 0; setFlagsArith(OP_SUB,r,a,b,4); lazyCF = (a < b) ? 1 : 0; return a;
    default: return a;
  }
}

// ── Condition code testing ──
function testCC(cc) {
  switch (cc) {
    case 0x0: return getOF();                       // O
    case 0x1: return !getOF();                      // NO
    case 0x2: return getCF();                       // B/C
    case 0x3: return !getCF();                      // AE/NC
    case 0x4: return getZF();                       // E/Z
    case 0x5: return !getZF();                      // NE/NZ
    case 0x6: return getCF() || getZF();            // BE
    case 0x7: return !getCF() && !getZF();          // A
    case 0x8: return getSF();                       // S
    case 0x9: return !getSF();                      // NS
    case 0xA: return getPF();                       // P
    case 0xB: return !getPF();                      // NP
    case 0xC: return getSF() !== getOF();           // L
    case 0xD: return getSF() === getOF();           // GE
    case 0xE: return getZF() || (getSF() !== getOF()); // LE
    case 0xF: return !getZF() && (getSF() === getOF()); // G
  }
  return false;
}

// ── Stack operations ──
let _ssBig = false; // SS.B=1 means use ESP (32-bit), SS.B=0 means use SP (16-bit)

function push16(v, ssBase) {
  if (_ssBig) {
    let esp = (gr32(4) - 2) >>> 0;
    ww(ssBase + esp, v);
    if (!mmioFault) sr32(4, esp);
  } else {
    let sp = (gr16(4) - 2) & 0xFFFF;
    ww(ssBase + sp, v);
    if (!mmioFault) sr16(4, sp);
  }
}
function pop16(ssBase) {
  if (_ssBig) {
    const esp = gr32(4);
    const v = rw(ssBase + esp);
    if (!mmioFault) sr32(4, (esp + 2) >>> 0);
    return v;
  } else {
    const sp = gr16(4);
    const v = rw(ssBase + sp);
    if (!mmioFault) sr16(4, (sp + 2) & 0xFFFF);
    return v;
  }
}
function push32(v, ssBase) {
  if (_ssBig) {
    let esp = (gr32(4) - 4) >>> 0;
    wd(ssBase + esp, v);
    if (!mmioFault) sr32(4, esp);
  } else {
    let sp = (gr16(4) - 4) & 0xFFFF;
    wd(ssBase + sp, v);
    if (!mmioFault) sr16(4, sp);
  }
}
function pop32(ssBase) {
  if (_ssBig) {
    const esp = gr32(4);
    const v = rd(ssBase + esp);
    if (!mmioFault) sr32(4, (esp + 4) >>> 0);
    return v;
  } else {
    const sp = gr16(4);
    const v = rd(ssBase + sp);
    if (!mmioFault) sr16(4, (sp + 4) & 0xFFFF);
    return v;
  }
}

// ── 32-bit paging support ──
let _pagingOn = false;

// ── Direct boot flag — set after kernel staging, used for CPUID faking ──
let _directBootDone = false;

// Direct-mapped TLB: 1024 entries for fast virtual-to-physical lookup
const TLB_SIZE = 1024;
const TLB_MASK = TLB_SIZE - 1;
const tlbTags = new Int32Array(TLB_SIZE).fill(-1);
const tlbPhys = new Uint32Array(TLB_SIZE);
let lastCR3 = -1;

function tlbFlush() { tlbTags.fill(-1); }

// 32-bit non-PAE page table walk
function translateLinear32(linearAddr) {
  // TLB check
  const idx = (linearAddr >>> 12) & TLB_MASK;
  const tag = linearAddr & 0xFFFFF000;
  if (tlbTags[idx] === (tag | 0)) {
    return (tlbPhys[idx] | (linearAddr & 0xFFF)) >>> 0;
  }

  const cr3 = rr32(R_CR3) & 0xFFFFF000;
  const cr4 = rr32(R_CR4);
  const pse = !!(cr4 & 0x10);

  // PDE: bits [31:22]
  const pdeAddr = cr3 + ((linearAddr >>> 22) << 2);
  const pdeOff = resolvePhys(pdeAddr);
  if (pdeOff < 0) { mmioFault = false; return -1; }
  const pde = dv.getUint32(pdeOff, true);
  if (!(pde & 1)) return -1; // not present

  // 4MB page (PSE)?
  if (pse && (pde & 0x80)) {
    const physBase = pde & 0xFFC00000;
    const phys = (physBase | (linearAddr & 0x3FFFFF)) >>> 0;
    tlbTags[idx] = tag | 0;
    tlbPhys[idx] = phys & 0xFFFFF000;
    return phys;
  }

  // PTE: bits [21:12]
  const ptBase = pde & 0xFFFFF000;
  const pteAddr = ptBase + (((linearAddr >>> 12) & 0x3FF) << 2);
  const pteOff = resolvePhys(pteAddr);
  if (pteOff < 0) { mmioFault = false; return -1; }
  const pte = dv.getUint32(pteOff, true);
  if (!(pte & 1)) return -1; // not present

  const phys = ((pte & 0xFFFFF000) | (linearAddr & 0xFFF)) >>> 0;
  tlbTags[idx] = tag | 0;
  tlbPhys[idx] = phys & 0xFFFFF000;
  return phys;
}

// PAE page table walk (for ISOLINUX/Linux)
function translateLinearPAE(linearAddr) {
  const idx = (linearAddr >>> 12) & TLB_MASK;
  const tag = linearAddr & 0xFFFFF000;
  if (tlbTags[idx] === (tag | 0)) {
    return (tlbPhys[idx] | (linearAddr & 0xFFF)) >>> 0;
  }

  // PDPTE: read from CPUMCTX.aPaePdpes (offset 0x240, 4 entries x 8 bytes)
  const pdpteIdx = (linearAddr >>> 30) & 3;
  const pdpteLo = dv.getUint32(cpuPtr + 0x240 + pdpteIdx * 8, true);
  if (!(pdpteLo & 1)) return -1;

  // PDE: bits [29:21]
  const pdBase = pdpteLo & 0xFFFFF000;
  const pdeIdx = (linearAddr >>> 21) & 0x1FF;
  const pdeOff = resolvePhys(pdBase + pdeIdx * 8);
  if (pdeOff < 0) { mmioFault = false; return -1; }
  const pdeLo = dv.getUint32(pdeOff, true);
  if (!(pdeLo & 1)) return -1;

  // 2MB large page?
  if (pdeLo & 0x80) {
    const phys = ((pdeLo & 0xFFE00000) | (linearAddr & 0x1FFFFF)) >>> 0;
    tlbTags[idx] = tag | 0;
    tlbPhys[idx] = phys & 0xFFFFF000;
    return phys;
  }

  // PTE: bits [20:12]
  const ptBase = pdeLo & 0xFFFFF000;
  const pteIdx = (linearAddr >>> 12) & 0x1FF;
  const pteOff = resolvePhys(ptBase + pteIdx * 8);
  if (pteOff < 0) { mmioFault = false; return -1; }
  const pteLo = dv.getUint32(pteOff, true);
  if (!(pteLo & 1)) return -1;

  const phys = ((pteLo & 0xFFFFF000) | (linearAddr & 0xFFF)) >>> 0;
  tlbTags[idx] = tag | 0;
  tlbPhys[idx] = phys & 0xFFFFF000;
  return phys;
}

// Unified translate: picks 32-bit or PAE based on CR4.PAE
function translateLinear(linearAddr) {
  const cr4 = rr32(R_CR4);
  if (cr4 & 0x20) return translateLinearPAE(linearAddr);
  return translateLinear32(linearAddr);
}

// ═══════════════════════════════════════════════════════
// MAIN INTERPRETER LOOP
// ═══════════════════════════════════════════════════════
//
// Returns: number of instructions executed (>0), or 0 for fallback needed
//
// cpuP:    pointer to CPUMCTX in Wasm linear memory
// ramB:    pointer to guest RAM base in Wasm linear memory
// maxInsn: max instructions to execute before returning
//
// A20 state is read from globalThis.VBoxJIT._a20 (set by wasmJitSetA20 EM_JS
// before each call).  This avoids adding a 4th parameter to the EM_JS
// signature, which caused an invoke_ijiij trampoline mismatch in Wasm64.
//
function execBlock(cpuP, ramB, maxInsn) {
  cpuPtr = cpuP;
  ramBase = ramB;
  // Set A20 mask: when disabled, bit 20 is forced to 0 (address wrap at 1 MB)
  const fA20 = globalThis.VBoxJIT._a20;
  a20Mask = fA20 ? 0xFFFFFFFF : ~(1 << 20);
  refreshViews();

  // Load frequently-used state
  let flags = rr32(R_FLAGS);

  // CR0: check PE and PG (read before segment bases so we can fix up real-mode CS)
  const cr0 = rr32(R_CR0);
  const protMode = !!(cr0 & 1);        // CR0.PE
  const pagingOn = !!(cr0 & 0x80000000); // CR0.PG
  _pagingOn = pagingOn;
  const realMode = !protMode;

  // Use CPUMCTX hidden base for CS — it's authoritative in all modes.
  // In normal real mode, hidden base == selector<<4.
  // In "unreal mode" (PM→RM without far JMP), hidden base retains PM value
  // (e.g. CS=0x0020, hidden base=0xF0000). IEM keeps it accurate.
  let csBase = segBase(S_CS);
  let dsBase = segBase(S_DS);
  let ssBase = segBase(S_SS);
  let esBase = segBase(S_ES);

  // Flush TLB on CR3 change
  if (pagingOn) {
    const currentCR3 = rr32(R_CR3);
    if (currentCR3 !== lastCR3) { tlbFlush(); lastCR3 = currentCR3; }
  }

  // CS descriptor D bit: determines default operand/address size in protected mode
  const csAttr = rr32(S_CS + SEG_ATTR);
  const csDefBig = protMode && !!(csAttr & X86DESCATTR_D);
  // Bail on 32-bit PM — the JIT is for real-mode BIOS code. After the 32-bit
  // boot protocol enters the kernel in PM, IEM handles all code correctly.
  if (csDefBig) return 0;
  const ipMask = csDefBig ? 0xFFFFFFFF : 0xFFFF;

  // SS.B for stack operations (ESP vs SP)
  const ssAttr = rr32(S_SS + SEG_ATTR);
  _ssBig = protMode && !!(ssAttr & X86DESCATTR_D);

  let ip = csDefBig ? rr32(R_IP) : rr16(R_IP);

  // Trace kernel setup code (CS=0x9020) for debugging direct boot
  if (!execBlock._kernelTraceCount) execBlock._kernelTraceCount = 0;
  const csSel = rr16(S_CS);

  // Debug: log once when CS=0x9000 and IP is in the ISOLINUX stuck range
  if (!execBlock._cs9diag && csSel === 0x9000 && ip >= 0x1a00 && ip <= 0x1c00) {
    execBlock._cs9diag = true;
    console.log('[JIT-CS9-DIAG] csSel=0x' + csSel.toString(16) +
      ' csBase=0x' + csBase.toString(16) + ' ip=0x' + ip.toString(16) +
      ' cr0=0x' + cr0.toString(16) + ' protMode=' + protMode +
      ' csDefBig=' + csDefBig + ' flags=0x' + flags.toString(16) +
      ' _directBootDone=' + !!execBlock._directBootDone);
  }
  if (csSel === 0x9020 && execBlock._kernelTraceCount < 20) {
    execBlock._kernelTraceCount++;
    const c0 = mem8[ramBase + csBase + ip];
    const c1 = mem8[ramBase + csBase + ip + 1];
    const c2 = mem8[ramBase + csBase + ip + 2];
    const c3 = mem8[ramBase + csBase + ip + 3];
    console.log('[JIT-KERNEL] #' + execBlock._kernelTraceCount +
      ' CS=' + csSel.toString(16) + ' IP=' + ip.toString(16) +
      ' FL=' + flags.toString(16) + ' code=' +
      c0.toString(16).padStart(2,'0') + c1.toString(16).padStart(2,'0') +
      c2.toString(16).padStart(2,'0') + c3.toString(16).padStart(2,'0') +
      ' DS=' + rr16(S_DS).toString(16) + ' dsBase=' + dsBase.toString(16) +
      ' ssBase=' + ssBase.toString(16) + ' SP=' + rr16(R_SP).toString(16));
  }

  // Bail immediately if Trap Flag is set — IEM must handle #DB exceptions
  if (flags & 0x100) return 0;

  // Protected mode without paging: bail for non-flat segments.
  // Flat PM (base=0, limit>=FFFFFFFF for CS/DS/SS) can be JIT'd directly.
  // Non-flat PM (segment-based addressing) needs IEM for GDT lookups.
  if (protMode && !pagingOn) {
    const csLim = rr32(S_CS + SEG_LIMIT);
    const dsLim = rr32(S_DS + SEG_LIMIT);
    const ssLim = rr32(S_SS + SEG_LIMIT);
    if (csBase !== 0 || dsBase !== 0 || ssBase !== 0 ||
        csLim < 0xFFFF0000 || dsLim < 0xFFFF0000 || ssLim < 0xFFFF0000) {
      return 0;
    }
    // Flat PM: fall through to JIT execution
  }

  // Protected mode: the JIT handles flat-model PM (base=0, limit=FFFFFFFF)
  // directly. Per-instruction bails handle segment-changing instructions
  // (MOV Sreg, RETF, INT, IRET, etc.) that need GDT/IDT lookup.

  // Initialize lazy flags from current RFLAGS
  loadFlags(flags);
  lazySize = csDefBig ? 4 : 2; // default operand size

  // Linear PC for diagnostics and ROM checks
  const linearPC = csBase + ip;

  // Self-modifying code detection: bail if a write overlaps the code being
  // executed. In real mode, track the 64K code segment. In PM flat model,
  // track just the current 4KB code page (full segment is 0-4GB).
  const csLimit = rr32(S_CS + SEG_LIMIT);
  const codeSegIsFlat = csDefBig && csBase === 0 && csLimit >= 0xFFFFFFFF;
  let codeSegStart, codeSegEnd;
  if (codeSegIsFlat) {
    // Will be updated per-instruction from codePhys below
    codeSegStart = 0; codeSegEnd = 0;
  } else {
    codeSegStart = csBase;
    codeSegEnd = csDefBig ? (csBase + Math.min(csLimit + 1, 0x100000000)) : (csBase + 0x10000);
  }

  // Helper to write IP back to CPUMCTX with correct width
  function wrIP(val) {
    if (csDefBig) wr32(R_IP, val & 0xFFFFFFFF);
    else wr16(R_IP, val & 0xFFFF);
  }

  let executed = 0;
  let lastBailOp = -1; // track the opcode that caused early exit
  const ramSize = mem8.length - ramBase; // available RAM

  // ── Direct Boot Recovery ──
  // After ISOLINUX loads the kernel to 0x100000, it may get stuck in a shuffle
  // loop or I/O loop. Detect this by counting execBlock calls while the kernel
  // is present but boot hasn't progressed.
  if (!execBlock._directBootDone && !protMode && csBase < 0xF0000) {
    if (!execBlock._dbCallCount) execBlock._dbCallCount = 0;
    execBlock._dbCallCount++;

    // Track whether we've ever been in a bootloader segment (not BIOS).
    // BIOS uses CS=F000 (filtered above), CS=0000, CS=C000 (VGA BIOS).
    // ISOLINUX uses CS=07C0, 9000, etc. — mid-range segments.
    if (!execBlock._dbSeenBootloader && csSel !== 0 && csSel !== 0xF000 &&
        csSel !== 0xC000 && csSel !== 0xC800 && csBase < 0xF0000 && csBase >= 0x600) {
      execBlock._dbSeenBootloader = true;
      console.log('[JIT-BOOT] Bootloader segment detected: CS=0x' + csSel.toString(16) +
        ' csBase=0x' + csBase.toString(16) + ' at call ' + execBlock._dbCallCount);
    }

    // Check every 50000 calls (~200M insns at 4096/call)
    if (execBlock._dbCallCount % 50000 === 0) {
      // Check if kernel code is present at 0x100000 — but only if we've been
      // in a bootloader segment (avoids false positive from BIOS memory test)
      const kBase = highRamPtr;
      const hasKernel = execBlock._dbSeenBootloader && kBase && (
        mem8[kBase] !== 0 || mem8[kBase+1] !== 0 || mem8[kBase+2] !== 0 || mem8[kBase+3] !== 0);
      if (hasKernel && !execBlock._dbKernelSeen) {
        execBlock._dbKernelSeen = execBlock._dbCallCount;
        console.log('[JIT-BOOT] Kernel detected at 0x100000 after ' +
          execBlock._dbCallCount + ' calls (bootloader at CS=0x' +
          csSel.toString(16) + '). Waiting for bootloader to finish...');
      }
      // If kernel was seen 100K+ calls ago (~400M insns) and we're still in
      // real mode, bootloader is stuck. Normal boot needs ~42M insns total.
      if (execBlock._dbKernelSeen &&
          (execBlock._dbCallCount - execBlock._dbKernelSeen) >= 100000) {
        console.log('[JIT-BOOT] Bootloader stuck! Kernel loaded but still in real mode after ' +
          execBlock._dbCallCount + ' calls (kernel first seen at ' + execBlock._dbKernelSeen + ')');
        console.log('[JIT-BOOT] CS=0x' + csSel.toString(16) + ' csBase=0x' + csBase.toString(16) +
          ' IP=0x' + ip.toString(16));
        // Fall through to direct boot below
      } else {
        // Not ready yet — don't fall through
        execBlock._dbCallCount; // no-op, just skip
      }
    }
    // Trigger conditions: either stuck timeout OR original zero-code detection
    const triggerStuck = execBlock._dbKernelSeen &&
      (execBlock._dbCallCount - execBlock._dbKernelSeen) >= 100000;
    const testAddr = ramBase + csBase + ip;
    let zeroCount = 0;
    for (let t = 0; t < 16; t++) if (mem8[testAddr + t] === 0) zeroCount++;
    const triggerZeros = (zeroCount >= 12 && ip >= 0x1a00 && ip <= 0x1c00);
    if (triggerStuck || triggerZeros) {
      console.log('[JIT-BOOT] ISOLINUX shuffle corrupted code at CS=0x' +
        csSel.toString(16) + ' (phys 0x' + csBase.toString(16) + ') IP=0x' + ip.toString(16));

      // Scan multiple locations for HdrS magic — PGM/mem8 may diverge after bcopy32
      const hdrCandidates = [csBase, 0x10000, 0x90000, 0x7C00, 0x20000, 0x80000];
      let hdrPhys = -1;
      for (let ci = 0; ci < hdrCandidates.length; ci++) {
        const cand = hdrCandidates[ci];
        if (cand >= 0xA0000) continue; // only low RAM accessible via ramBase
        const off = ramBase + cand;
        if (mem8[off + 0x202] === 0x48 && mem8[off + 0x203] === 0x64 &&
            mem8[off + 0x204] === 0x72 && mem8[off + 0x205] === 0x53) {
          hdrPhys = cand;
          console.log('[JIT-BOOT] Found HdrS at phys 0x' + cand.toString(16));
          break;
        }
      }

      if (hdrPhys < 0) {
        // HdrS not found in mem8[] — PGM has diverged. Use hardcoded FossaPup64 values.
        console.log('[JIT-BOOT] HdrS not found in mem8 — using hardcoded boot params');
        // Diagnostics: dump what mem8 sees at candidate locations
        for (let ci = 0; ci < 3; ci++) {
          const cand = [csBase, 0x10000, 0x90000][ci];
          if (cand >= 0xA0000) continue;
          let d = '';
          for (let b = 0; b < 16; b++)
            d += mem8[ramBase + cand + 0x200 + b].toString(16).padStart(2, '0') + ' ';
          console.log('[JIT-BOOT-DIAG] phys 0x' + cand.toString(16) + '+0x200: ' + d);
        }
      }

      // Build boot_params at 0x90000 — either from found header or hardcoded
      const setupBase = ramBase + 0x90000;
      if (hdrPhys >= 0 && hdrPhys !== 0x90000) {
        // Copy setup code from found location to 0x90000
        const srcBase = ramBase + hdrPhys;
        const srcSects = mem8[srcBase + 0x1F1] || 32;
        const cpSize = (srcSects + 1) * 512;
        console.log('[JIT-BOOT] Copying setup from 0x' + hdrPhys.toString(16) +
          ' (' + cpSize + ' bytes)');
        for (let i = 0; i < cpSize; i++) mem8[setupBase + i] = mem8[srcBase + i];
      } else if (hdrPhys < 0) {
        // No setup header found anywhere. Jump DIRECTLY to protected-mode
        // kernel at 0x100000, bypassing real-mode setup entirely.
        console.log('[JIT-BOOT] No HdrS found — direct PM entry to kernel at 0x100000');
        // Diagnostics: dump what mem8 sees at candidate locations
        for (let ci = 0; ci < 4; ci++) {
          const cand = [csBase, 0x10000, 0x90000, 0x7C00][ci];
          if (cand >= 0xA0000) continue;
          let d = '';
          for (let b = 0; b < 16; b++)
            d += mem8[ramBase + cand + 0x200 + b].toString(16).padStart(2, '0') + ' ';
          console.log('[JIT-BOOT-DIAG] phys 0x' + cand.toString(16) + '+0x200: ' + d);
        }

        // Write minimal boot_params at 0x90000 for the kernel
        for (let i = 0; i < 0x1000; i++) mem8[setupBase + i] = 0;
        // e820 memory map: entries at offset 0xD00 (boot_params.e820_table)
        // Entry format: 20 bytes each (addr:8, size:8, type:4)
        const e820Base = 0xD00;
        // Entry 0: 0x0 - 0x9FC00 usable
        writeDword(setupBase + e820Base + 0, 0);
        writeDword(setupBase + e820Base + 4, 0);
        writeDword(setupBase + e820Base + 8, 0x9FC00);
        writeDword(setupBase + e820Base + 12, 0);
        writeDword(setupBase + e820Base + 16, 1);
        // Entry 1: 0x100000 - RAM_TOP usable
        writeDword(setupBase + e820Base + 20, 0x100000);
        writeDword(setupBase + e820Base + 24, 0);
        const ramTop = highRamEnd ? (0x100000 + (highRamEnd - 0x100000)) : 0x2000000;
        writeDword(setupBase + e820Base + 28, ramTop - 0x100000);
        writeDword(setupBase + e820Base + 32, 0);
        writeDword(setupBase + e820Base + 36, 1);
        mem8[setupBase + 0x1E8] = 2; // e820_entries count
        // HdrS signature for kernel validation
        mem8[setupBase + 0x202] = 0x48;
        mem8[setupBase + 0x203] = 0x64;
        mem8[setupBase + 0x204] = 0x72;
        mem8[setupBase + 0x205] = 0x53;
        // Protocol 2.13
        mem8[setupBase + 0x206] = 0x0D;
        mem8[setupBase + 0x207] = 0x02;
        // loadflags: LOADED_HIGH
        mem8[setupBase + 0x211] = 0x01;
        // type_of_loader
        mem8[setupBase + 0x210] = 0xFF;
        // code32_start
        writeDword(setupBase + 0x214, 0x100000);
        // Command line
        const cmdline = 'loglevel=3 vga=791 console=ttyS0,115200 idle=halt notsc clocksource=jiffies acpi=off nopti nospectre_v1 nospectre_v2 pci=lastbus=0 mitigations=off notrace lpj=100';
        for (let i = 0; i < cmdline.length; i++)
          mem8[ramBase + 0x99000 + i] = cmdline.charCodeAt(i);
        mem8[ramBase + 0x99000 + cmdline.length] = 0;
        writeDword(setupBase + 0x228, 0x99000);

        // Write GDT at 0x1000 — Linux boot protocol: __BOOT_CS=0x10, __BOOT_DS=0x18
        const gdtBase = ramBase + 0x1000;
        for (let i = 0; i < 32; i++) mem8[gdtBase + i] = 0; // 4 entries
        // Entry 2 (0x10): __BOOT_CS — flat code32
        mem8[gdtBase+16]=0xFF; mem8[gdtBase+17]=0xFF; mem8[gdtBase+18]=0; mem8[gdtBase+19]=0;
        mem8[gdtBase+20]=0; mem8[gdtBase+21]=0x9A; mem8[gdtBase+22]=0xCF; mem8[gdtBase+23]=0;
        // Entry 3 (0x18): __BOOT_DS — flat data32
        mem8[gdtBase+24]=0xFF; mem8[gdtBase+25]=0xFF; mem8[gdtBase+26]=0; mem8[gdtBase+27]=0;
        mem8[gdtBase+28]=0; mem8[gdtBase+29]=0x92; mem8[gdtBase+30]=0xCF; mem8[gdtBase+31]=0;

        // Set GDTR (4 entries = 32 bytes, limit = 31)
        wr64(R_GDTR_BASE, 0x1000);
        wr16(R_GDTR_LIMIT, 31);

        // Set CR0: PE=1, PG=0
        wr32(R_CR0, (cr0 | 1) & ~0x80000000);

        // CS = 0x10 (__BOOT_CS)
        wr16(S_CS + SEG_SEL, 0x10);
        wr64(S_CS + SEG_BASE, 0);
        wr32(S_CS + SEG_LIMIT, 0xFFFFFFFF);
        wr32(S_CS + SEG_ATTR, 0xC09B);

        // DS/ES/SS/FS/GS = 0x18 (__BOOT_DS)
        const dataAttr = 0xC093;
        for (const seg of [S_DS, S_ES, S_SS, S_FS, S_GS]) {
          wr16(seg + SEG_SEL, 0x18);
          wr64(seg + SEG_BASE, 0);
          wr32(seg + SEG_LIMIT, 0xFFFFFFFF);
          wr32(seg + SEG_ATTR, dataAttr);
        }

        // EIP = 0x100000 (startup_32 entry)
        wr32(R_IP, 0x100000);
        // ESI = boot_params address (0x90000)
        sr32(6, 0x90000); // ESI = reg index 6
        // Clear other registers
        sr32(0, 0); sr32(1, 0); sr32(2, 0); sr32(3, 0);
        sr32(5, 0); sr32(7, 0); // EBP=0, EDI=0
        sr32(4, 0x90000); // ESP = some safe stack
        // Disable interrupts
        wr32(R_FLAGS, 2); // just the reserved bit

        execBlock._directBootDone = true;
        console.log('[JIT-BOOT] Direct PM boot: EIP=0x100000 ESI=0x90000 CR0=0x' +
          ((cr0 | 1) & ~0x80000000).toString(16));
        console.log('[JIT-BOOT] Jumping directly to startup_32!');
        return 0;
      }

      // If we have a header source, copy boot params (ramdisk/cmdline info)
      // but ALWAYS do direct PM boot — real-mode setup triple-faults because
      // ISOLINUX corrupted the IVT during its shuffle.
      if (hdrPhys >= 0) {
        const srcBase = (hdrPhys === 0x90000) ? setupBase : ramBase + hdrPhys;
        // Copy key boot_params fields from ISOLINUX's loaded header
        // to our boot_params at 0x90000
        if (hdrPhys !== 0x90000) {
          // Copy the entire header (up to 4K) to preserve all fields
          const srcSects = mem8[srcBase + 0x1F1] || 32;
          const cpSize = Math.min((srcSects + 1) * 512, 0x1000);
          for (let i = 0; i < cpSize; i++) mem8[setupBase + i] = mem8[srcBase + i];
          console.log('[JIT-BOOT] Copied header from 0x' + hdrPhys.toString(16) +
            ' (' + cpSize + ' bytes)');
        }
        // Copy command line to safe location
        const cmdPtr = mem8[srcBase + 0x228] | (mem8[srcBase + 0x229] << 8) |
          (mem8[srcBase + 0x22A] << 16) | (mem8[srcBase + 0x22B] << 24);
        let cmdLen = 0;
        if (cmdPtr > 0 && cmdPtr < 0xA0000) {
          for (let i = 0; i < 256; i++) {
            const ch = mem8[ramBase + cmdPtr + i];
            mem8[ramBase + 0x99000 + i] = ch;
            if (ch === 0) { cmdLen = i; break; }
          }
          console.log('[JIT-BOOT] cmdline @0x' + cmdPtr.toString(16) + ' (' + cmdLen + ' bytes)');
        }
        // Append serial console options to command line
        const serialOpts = ' console=ttyS0,115200 loglevel=3 idle=halt notsc clocksource=jiffies acpi=off nopti nospectre_v1 nospectre_v2 pci=lastbus=0 mitigations=off notrace lpj=100';
        for (let i = 0; i < serialOpts.length; i++)
          mem8[ramBase + 0x99000 + cmdLen + i] = serialOpts.charCodeAt(i);
        mem8[ramBase + 0x99000 + cmdLen + serialOpts.length] = 0;
        cmdLen += serialOpts.length;
        console.log('[JIT-BOOT] appended serial console, total cmdline=' + cmdLen);
        // Set cmdline pointer in boot_params
        writeDword(setupBase + 0x228, 0x99000);
      }

      // Read ramdisk info from boot_params (whether from header or constructed)
      const rdImg = mem8[setupBase+0x218]|(mem8[setupBase+0x219]<<8)|
        (mem8[setupBase+0x21A]<<16)|(mem8[setupBase+0x21B]<<24);
      const rdSz = mem8[setupBase+0x21C]|(mem8[setupBase+0x21D]<<8)|
        (mem8[setupBase+0x21E]<<16)|(mem8[setupBase+0x21F]<<24);
      const code32 = mem8[setupBase+0x214]|(mem8[setupBase+0x215]<<8)|
        (mem8[setupBase+0x216]<<16)|(mem8[setupBase+0x217]<<24);
      const code32addr = code32 || 0x100000;

      // Ensure loadflags has LOADED_HIGH
      mem8[setupBase + 0x211] |= 0x01;
      // Set type_of_loader if not set
      if (!mem8[setupBase + 0x210]) mem8[setupBase + 0x210] = 0xFF;

      console.log('[JIT-BOOT] Direct PM boot to startup_32 at 0x' + code32addr.toString(16));
      console.log('[JIT-BOOT] ramdisk=0x' + rdImg.toString(16) + ' size=0x' + rdSz.toString(16));

      // ── Try fast 64-bit boot (JS decompression) ──
      const setupProto = mem8[setupBase + 0x206] | (mem8[setupBase + 0x207] << 8);
      if (setupProto >= 0x208 && highRamPtr) {
        const payOff = mem8[setupBase+0x248]|(mem8[setupBase+0x249]<<8)|
          (mem8[setupBase+0x24A]<<16)|(mem8[setupBase+0x24B]<<24);
        const payLen = mem8[setupBase+0x24C]|(mem8[setupBase+0x24D]<<8)|
          (mem8[setupBase+0x24E]<<16)|(mem8[setupBase+0x24F]<<24);
        if (payOff > 0 && payLen > 0) {
          // Payload is at guest GPA code32addr + payOff
          const payStart = highRamPtr + (code32addr - 0x100000) + payOff;
          console.log('[FAST-BOOT] proto=0x' + setupProto.toString(16) +
            ' payload_offset=0x' + payOff.toString(16) + ' payload_length=' + payLen +
            ' magic=0x' + mem8[payStart].toString(16).padStart(2,'0') +
            mem8[payStart+1].toString(16).padStart(2,'0'));

          const t0 = performance.now();
          const comp = mem8.subarray(payStart, payStart + payLen);
          const vmlinux = jsGunzip(comp);
          if (vmlinux) {
            const dt = (performance.now() - t0) | 0;
            console.log('[FAST-BOOT] Decompressed: ' + vmlinux.length + ' bytes (' +
              (vmlinux.length >> 20) + 'MB) in ' + dt + 'ms');
            const elf = parseELF64(vmlinux);
            if (elf) {
              console.log('[FAST-BOOT] ELF entry=0x' + elf.entry.toString(16) +
                ' segments=' + elf.segs.length);
              const TOTAL_RAM = 0x100000 + highRamSize;
              for (let si = 0; si < elf.segs.length; si++) {
                const seg = elf.segs[si];
                const pa = Number(seg.paddr);
                console.log('[FAST-BOOT] seg[' + si + '] paddr=0x' + pa.toString(16) +
                  ' vaddr=0x' + seg.vaddr.toString(16) +
                  ' filesz=' + seg.filesz + ' memsz=' + seg.memsz);
                if (pa >= 0x100000 && pa + seg.memsz <= TOTAL_RAM) {
                  const dst = highRamPtr + (pa - 0x100000);
                  if (seg.filesz > 0)
                    mem8.set(vmlinux.subarray(seg.offset, seg.offset + seg.filesz), dst);
                  if (seg.memsz > seg.filesz)
                    mem8.fill(0, dst + seg.filesz, dst + seg.memsz);
                }
              }
              const cr3val = buildPageTables64(TOTAL_RAM);
              // Write boot_params copy to 0x10000 (boot_params GPA for kernel)
              for (let i = 0; i < 4096; i++) mem8[ramBase + 0x10000 + i] = mem8[setupBase + i];
              // Write 64-bit metadata at 0x7200
              dv.setUint32(ramBase + 0x7200, 0x42343644, true); // "D64B"
              dv.setUint32(ramBase + 0x7204, cr3val, true);
              dv.setBigUint64(ramBase + 0x7208, elf.entry, true);
              dv.setUint32(ramBase + 0x7210, 0x10000, true); // boot_params GPA
              // Write GDT at 0x7300
              dv.setBigUint64(ramBase + 0x7300, 0n, true);
              dv.setBigUint64(ramBase + 0x7308, 0x00CF9A000000FFFFn, true);
              dv.setBigUint64(ramBase + 0x7310, 0x00AF9B000000FFFFn, true);
              dv.setBigUint64(ramBase + 0x7318, 0x00CF93000000FFFFn, true);
              wr32(R_CR2, 0xD64B0001);
              execBlock._directBootDone = true;
              _directBootDone = true;
              console.log('[FAST-BOOT] 64-bit kernel ready! entry=0x' +
                elf.entry.toString(16) + ' CR3=0x' + cr3val.toString(16));
              return 0;
            }
          } else {
            console.log('[FAST-BOOT] Not gzip or decompress failed');
          }
        }
      }

      // ── Fallback: 32-bit PM boot (slow, IEM decompresses) ──
      // Write GDT at 0x1000 in guest RAM
      // Linux boot protocol requires __BOOT_CS=0x10, __BOOT_DS=0x18
      const gdtB = ramBase + 0x1000;
      for (let i = 0; i < 32; i++) mem8[gdtB + i] = 0; // clear 4 entries
      // Entry 0 (0x00): null descriptor — already zeroed
      // Entry 1 (0x08): unused (keep null for safety)
      // Entry 2 (0x10): __BOOT_CS — flat 32-bit code
      mem8[gdtB+16]=0xFF; mem8[gdtB+17]=0xFF; mem8[gdtB+18]=0; mem8[gdtB+19]=0;
      mem8[gdtB+20]=0; mem8[gdtB+21]=0x9A; mem8[gdtB+22]=0xCF; mem8[gdtB+23]=0;
      // Entry 3 (0x18): __BOOT_DS — flat 32-bit data
      mem8[gdtB+24]=0xFF; mem8[gdtB+25]=0xFF; mem8[gdtB+26]=0; mem8[gdtB+27]=0;
      mem8[gdtB+28]=0; mem8[gdtB+29]=0x92; mem8[gdtB+30]=0xCF; mem8[gdtB+31]=0;

      // Set GDTR (4 entries = 32 bytes, limit = 31)
      wr64(R_GDTR_BASE, 0x1000);
      wr16(R_GDTR_LIMIT, 31);
      // CR0: PE=1, PG=0
      wr32(R_CR0, (cr0 | 1) & ~0x80000000);
      // CS = 0x10 (__BOOT_CS, flat code32)
      wr16(S_CS + SEG_SEL, 0x10);
      wr64(S_CS + SEG_BASE, 0);
      wr32(S_CS + SEG_LIMIT, 0xFFFFFFFF);
      wr32(S_CS + SEG_ATTR, 0xC09B);
      // DS/ES/SS/FS/GS = 0x18 (__BOOT_DS, flat data32)
      const dA = 0xC093;
      for (const sg of [S_DS, S_ES, S_SS, S_FS, S_GS]) {
        wr16(sg + SEG_SEL, 0x18);
        wr64(sg + SEG_BASE, 0);
        wr32(sg + SEG_LIMIT, 0xFFFFFFFF);
        wr32(sg + SEG_ATTR, dA);
      }
      // EIP = startup_32
      wr32(R_IP, code32addr);
      // ESI = boot_params
      sr32(6, 0x90000);
      sr32(0, 0); sr32(1, 0); sr32(2, 0); sr32(3, 0);
      sr32(5, 0); sr32(7, 0);
      sr32(4, 0x90000); // ESP
      // EFLAGS: just reserved bit, no IF
      wr32(R_FLAGS, 2);

      execBlock._directBootDone = true;
      _directBootDone = true;
      console.log('[JIT-BOOT] PM state set. Jumping to startup_32!');
      return 0;
    }
  }

  // Pre-read a chunk of code for fast access
  let codeLinear = csBase + ip;
  let codePhys;
  // Check if address is in accessible range.
  // ROM (0xC0000-0xFFFFF) is in a separate ROM buffer.
  // VGA MMIO (0xA0000-0xBFFFF): PGM dispatches to VGA device, but for Wasm
  // we also allow the flat RAM buffer here since bootloaders (ISOLINUX)
  // relocate code to this range. PGM's write-through may or may not populate
  // the flat buffer — if not, the JIT will execute stale data and bail quickly
  // on decode failure, falling back to IEM.
  const addrAccessible = (addr) => {
    if (addr >= 0 && addr + 16 <= 0xA0000) return true;  // low RAM
    if (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd) return true;
    if (highRamPtr && addr >= 0x100000 && addr + 16 <= highRamEnd) return true;
    return false;
  };
  const inRomRange = (addr) => romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd;
  if (pagingOn) {
    codePhys = translateLinear(codeLinear >>> 0);
    if (codePhys < 0) {
      if (execBlock._bailDiagCount === undefined) execBlock._bailDiagCount = 0;
      if (execBlock._bailDiagCount++ < 10)
        console.log('[JIT-BAIL] translateLinear failed: codeLinear=0x' +
          (codeLinear >>> 0).toString(16) + ' CR3=0x' + rr32(R_CR3).toString(16) +
          ' CR0=0x' + rr32(R_CR0).toString(16) + ' CR4=0x' + rr32(R_CR4).toString(16));
      return 0;
    }
  } else {
    codePhys = codeLinear;
  }
  // ROM is above the A20 gate on the chipset bus — skip A20 masking for ROM range
  if (!inRomRange(codePhys))
    codePhys = (codePhys & a20Mask) >>> 0;
  if (!addrAccessible(codePhys)) {
    if (execBlock._addrDiagCount === undefined) execBlock._addrDiagCount = 0;
    if (execBlock._addrDiagCount++ < 10)
      console.log('[JIT-BAIL] addrAccessible failed: codePhys=0x' +
        codePhys.toString(16) + ' codeLinear=0x' + (codeLinear>>>0).toString(16) +
        ' csBase=0x' + csBase.toString(16) + ' ip=0x' + ip.toString(16) +
        ' CS=' + rr16(S_CS).toString(16).padStart(4,'0') +
        ' CR0=0x' + rr32(R_CR0).toString(16) +
        ' pagingOn=' + pagingOn + ' romBufSize=' + romBufSize +
        ' ramSize=' + ramSize + ' a20=' + (fA20?'on':'off'));
    return 0;
  }

  // Bail periodically to let IEM deliver hardware interrupts (PIT timer, etc.)
  // Without this, the JIT blocks interrupt delivery for the entire batch,
  // causing BIOS POST to stall waiting for timer ticks.
  const interruptCheckInterval = 8192;

  for (let iter = 0; iter < maxInsn; iter++) {
    mmioFault = false; // reset MMIO fault flag for each instruction attempt
    // Periodic bail for interrupt delivery
    if (executed > 0 && (executed & (interruptCheckInterval - 1)) === 0) break;

    codeLinear = csBase + ip;
    if (pagingOn) {
      codePhys = translateLinear(codeLinear >>> 0);
      if (codePhys < 0) { executed = executed || 0; break; } // bail on code page fault
    } else {
      codePhys = codeLinear;
    }
    // ROM is above the A20 gate on the chipset bus — skip A20 masking for ROM range
    if (!inRomRange(codePhys))
      codePhys = (codePhys & a20Mask) >>> 0;
    if (codePhys < 0 || (!addrAccessible(codePhys))) {
      break;
    }

    // Update self-modifying code range for flat PM (track current 4KB page)
    if (codeSegIsFlat) {
      codeSegStart = codePhys & 0xFFFFF000;
      codeSegEnd = codeSegStart + 0x1000;
    }

    // Near page boundary: instruction might span two pages — bail to IEM
    if (pagingOn && (codePhys & 0xFFF) > 0xFF0) {
      executed = executed || 0;
      break;
    }

    // Read up to 15 bytes of instruction (ROM-aware)
    const c0 = guestRb(codePhys);

    // Stale ROM detection: if the first code byte AND the next 3 bytes are
    // all zero in a ROM page, the ROM buffer is likely stale (BIOS hasn't
    // decompressed this page yet). Bail to IEM which reads from PGM directly.
    if (c0 === 0 && inRomRange(codePhys)) {
      const c1 = guestRb(codePhys + 1), c2 = guestRb(codePhys + 2), c3 = guestRb(codePhys + 3);
      if (c1 === 0 && c2 === 0 && c3 === 0) {
        break; // bail — stale ROM page
      }
    }

    // ── Prefix handling ──
    let segOverride = -1; // -1 = default
    let opSizeOverride = false;
    let addrSizeOverride = false;
    let repPrefix = 0; // 0=none, 0xF2=REPNE, 0xF3=REP/REPE
    let pos = 0; // bytes consumed for prefixes

    let b = c0;
    let scanning = true;
    while (scanning && pos < 4) {
      switch (b) {
        case 0x26: segOverride = S_ES; break;
        case 0x2E: segOverride = S_CS; break;
        case 0x36: segOverride = S_SS; break;
        case 0x3E: segOverride = S_DS; break;
        case 0x64: segOverride = S_FS; break;
        case 0x65: segOverride = S_GS; break;
        case 0x66: opSizeOverride = true; break;
        case 0x67: addrSizeOverride = true; break;
        case 0xF0: break; // LOCK prefix — consumed, no special behavior in JIT
        case 0xF2: repPrefix = 0xF2; break;
        case 0xF3: repPrefix = 0xF3; break;
        default: scanning = false; continue;
      }
      pos++;
      b = guestRb(codePhys + pos);
    }

    // Effective segment bases
    const effDS = segOverride >= 0 ? segBase(segOverride) : dsBase;
    const effSS = ssBase; // stack segment rarely overridden
    // operand size: inverted by 0x66 prefix
    const opSize = csDefBig ? (opSizeOverride ? 2 : 4) : (opSizeOverride ? 4 : 2);
    // address size: inverted by 0x67 prefix
    const addrSize = csDefBig ? (addrSizeOverride ? 2 : 4) : (addrSizeOverride ? 4 : 2);

    // Code bytes after prefixes — resolve physical address for code fetch
    const inROM = (codePhys >= romGCPhysStart && codePhys < romGCPhysEnd);
    const inHighRAM = (!inROM && codePhys >= 0x100000);
    const ci = inROM ? (romBufBase + (codePhys - romGCPhysStart) + pos)
             : inHighRAM ? (highRamPtr + (codePhys - 0x100000) + pos)
             : (ramBase + codePhys + pos);
    let ilen = pos; // instruction length accumulator

    // ── Opcode dispatch ──
    switch (b) {

    // ──── NOP ────
    case 0x90:
      ilen += 1;
      break;

    // ──── MOV r8, r/m8 (0x8A) ────
    case 0x8A: {
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      if ((modrm >> 6) === 3) {
        sr8(reg, gr8(modrm & 7));
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        sr8(reg, rb(m.ea));
      }
      break;
    }

    // ──── MOV r/m8, r8 (0x88) ────
    case 0x88: {
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      if ((modrm >> 6) === 3) {
        sr8(modrm & 7, gr8(reg));
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        wb(m.ea, gr8(reg));
      }
      break;
    }

    // ──── MOV r16/32, r/m16/32 (0x8B) ────
    case 0x8B: {
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      if ((modrm >> 6) === 3) {
        if (opSize === 2) sr16(reg, gr16(modrm & 7));
        else sr32(reg, gr32(modrm & 7));
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        if (opSize === 2) sr16(reg, rw(m.ea));
        else sr32(reg, rd(m.ea));
      }
      break;
    }

    // ──── MOV r/m16/32, r16/32 (0x89) ────
    case 0x89: {
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      if ((modrm >> 6) === 3) {
        if (opSize === 2) sr16(modrm & 7, gr16(reg));
        else sr32(modrm & 7, gr32(reg));
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        if (opSize === 2) ww(m.ea, gr16(reg));
        else wd(m.ea, gr32(reg));
      }
      break;
    }

    // ──── MOV r/m8, imm8 (0xC6) ────
    case 0xC6: {
      const modrm = mem8[ci+1]; ilen += 2;
      if ((modrm >> 6) === 3) {
        sr8(modrm & 7, mem8[ci+2]); ilen += 1;
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        wb(m.ea, mem8[ci+2+m.len]); ilen += 1;
      }
      break;
    }

    // ──── MOV r/m16/32, imm16/32 (0xC7) ────
    case 0xC7: {
      const modrm = mem8[ci+1]; ilen += 2;
      if ((modrm >> 6) === 3) {
        if (opSize === 2) { sr16(modrm & 7, mem8[ci+2] | (mem8[ci+3] << 8)); ilen += 2; }
        else { sr32(modrm & 7, mem8[ci+2]|(mem8[ci+3]<<8)|(mem8[ci+4]<<16)|(mem8[ci+5]<<24)); ilen += 4; }
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        const imOff = ci + 2 + m.len;
        if (opSize === 2) { ww(m.ea, mem8[imOff] | (mem8[imOff+1] << 8)); ilen += 2; }
        else { wd(m.ea, mem8[imOff]|(mem8[imOff+1]<<8)|(mem8[imOff+2]<<16)|(mem8[imOff+3]<<24)); ilen += 4; }
      }
      break;
    }

    // ──── MOV r16/32, imm (0xB8-0xBF) ────
    case 0xB8:case 0xB9:case 0xBA:case 0xBB:case 0xBC:case 0xBD:case 0xBE:case 0xBF: {
      const reg = b - 0xB8;
      if (opSize === 2) {
        sr16(reg, mem8[ci+1] | (mem8[ci+2] << 8));
        ilen += 3;
      } else {
        sr32(reg, mem8[ci+1]|(mem8[ci+2]<<8)|(mem8[ci+3]<<16)|(mem8[ci+4]<<24));
        ilen += 5;
      }
      break;
    }

    // ──── MOV r8, imm8 (0xB0-0xB7) ────
    case 0xB0:case 0xB1:case 0xB2:case 0xB3:case 0xB4:case 0xB5:case 0xB6:case 0xB7:
      sr8(b - 0xB0, mem8[ci+1]);
      ilen += 2;
      break;

    // ──── MOV AL, moffs8 (0xA0) ────
    case 0xA0: {
      const addr = addrSize === 2 ? (mem8[ci+1] | (mem8[ci+2] << 8)) :
        (mem8[ci+1]|(mem8[ci+2]<<8)|(mem8[ci+3]<<16)|(mem8[ci+4]<<24));
      sr8(0, rb(effDS + addr));
      ilen += 1 + addrSize;
      break;
    }
    // ──── MOV moffs8, AL (0xA2) ────
    case 0xA2: {
      const addr = addrSize === 2 ? (mem8[ci+1] | (mem8[ci+2] << 8)) :
        (mem8[ci+1]|(mem8[ci+2]<<8)|(mem8[ci+3]<<16)|(mem8[ci+4]<<24));
      wb(effDS + addr, gr8(0));
      ilen += 1 + addrSize;
      break;
    }
    // ──── MOV AX, moffs16 (0xA1) ────
    case 0xA1: {
      const addr = addrSize === 2 ? (mem8[ci+1] | (mem8[ci+2] << 8)) :
        (mem8[ci+1]|(mem8[ci+2]<<8)|(mem8[ci+3]<<16)|(mem8[ci+4]<<24));
      if (opSize === 2) sr16(0, rw(effDS + addr));
      else sr32(0, rd(effDS + addr));
      ilen += 1 + addrSize;
      break;
    }
    // ──── MOV moffs16, AX (0xA3) ────
    case 0xA3: {
      const addr = addrSize === 2 ? (mem8[ci+1] | (mem8[ci+2] << 8)) :
        (mem8[ci+1]|(mem8[ci+2]<<8)|(mem8[ci+3]<<16)|(mem8[ci+4]<<24));
      if (opSize === 2) ww(effDS + addr, gr16(0));
      else wd(effDS + addr, gr32(0));
      ilen += 1 + addrSize;
      break;
    }

    // ──── MOV Sreg, r/m16 (0x8E) ────
    case 0x8E: {
      const modrm = mem8[ci+1]; ilen += 2;
      const sreg = (modrm >> 3) & 7;
      let val;
      if ((modrm >> 6) === 3) {
        val = gr16(modrm & 7);
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        val = rw(m.ea);
      }
      // In protected mode, MOV Sreg requires GDT lookup — let IEM handle it.
      if (!realMode) { lastBailOp = 0x8E; iter = maxInsn; break; }
      // Real mode: base = sel << 4
      const sOff = SEG_OFFS[sreg];
      if (!sOff && sreg !== 0) { ip = (ip + ilen) & ipMask; break; } // invalid sreg
      wr16(sOff + SEG_SEL, val);
      wr64(sOff + SEG_BASE, val << 4);
      if (sreg === 3) dsBase = val << 4;
      else if (sreg === 2) ssBase = val << 4;
      else if (sreg === 0) esBase = val << 4;
      break;
    }

    // ──── MOV r/m16, Sreg (0x8C) ────
    case 0x8C: {
      const modrm = mem8[ci+1]; ilen += 2;
      const sreg = (modrm >> 3) & 7;
      const sOff = SEG_OFFS[sreg];
      const val = sOff ? rr16(sOff + SEG_SEL) : (sreg === 0 ? rr16(S_ES + SEG_SEL) : 0);
      if ((modrm >> 6) === 3) {
        sr16(modrm & 7, val);
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        ww(m.ea, val);
      }
      break;
    }

    // ──── ALU r/m8, r8 (0x00,0x08,0x10,0x18,0x20,0x28,0x30,0x38) ────
    case 0x00:case 0x08:case 0x10:case 0x18:case 0x20:case 0x28:case 0x30:case 0x38: {
      const op = b >> 3;
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      const rv = gr8(reg);
      if ((modrm >> 6) === 3) {
        const rm = modrm & 7;
        const res = alu8(op, gr8(rm), rv);
        if (op !== 7) sr8(rm, res); // CMP doesn't store
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        const res = alu8(op, rb(m.ea), rv);
        if (op !== 7) wb(m.ea, res);
      }
      break;
    }

    // ──── ALU r8, r/m8 (0x02,0x0A,0x12,0x1A,0x22,0x2A,0x32,0x3A) ────
    case 0x02:case 0x0A:case 0x12:case 0x1A:case 0x22:case 0x2A:case 0x32:case 0x3A: {
      const op = (b - 2) >> 3;
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      let val;
      if ((modrm >> 6) === 3) {
        val = gr8(modrm & 7);
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        val = rb(m.ea);
      }
      const res = alu8(op, gr8(reg), val);
      if (op !== 7) sr8(reg, res);
      break;
    }

    // ──── ALU r/m16/32, r16/32 (0x01,0x09,0x11,0x19,0x21,0x29,0x31,0x39) ────
    case 0x01:case 0x09:case 0x11:case 0x19:case 0x21:case 0x29:case 0x31:case 0x39: {
      const op = (b - 1) >> 3;
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      if ((modrm >> 6) === 3) {
        const rm = modrm & 7;
        if (opSize === 2) {
          const res = alu16(op, gr16(rm), gr16(reg));
          if (op !== 7) sr16(rm, res);
        } else {
          const res = alu32(op, gr32(rm), gr32(reg));
          if (op !== 7) sr32(rm, res);
        }
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        if (opSize === 2) {
          const res = alu16(op, rw(m.ea), gr16(reg));
          if (op !== 7) ww(m.ea, res);
        } else {
          const res = alu32(op, rd(m.ea), gr32(reg));
          if (op !== 7) wd(m.ea, res);
        }
      }
      break;
    }

    // ──── ALU r16/32, r/m16/32 (0x03,0x0B,0x13,0x1B,0x23,0x2B,0x33,0x3B) ────
    case 0x03:case 0x0B:case 0x13:case 0x1B:case 0x23:case 0x2B:case 0x33:case 0x3B: {
      const op = (b - 3) >> 3;
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      let val;
      if ((modrm >> 6) === 3) {
        val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        val = opSize === 2 ? rw(m.ea) : rd(m.ea);
      }
      if (opSize === 2) {
        const res = alu16(op, gr16(reg), val);
        if (op !== 7) sr16(reg, res);
      } else {
        const res = alu32(op, gr32(reg), val);
        if (op !== 7) sr32(reg, res);
      }
      break;
    }

    // ──── ALU AL, imm8 (0x04,0x0C,0x14,0x1C,0x24,0x2C,0x34,0x3C) ────
    case 0x04:case 0x0C:case 0x14:case 0x1C:case 0x24:case 0x2C:case 0x34:case 0x3C: {
      const op = (b - 4) >> 3;
      const imm = mem8[ci+1]; ilen += 2;
      const res = alu8(op, gr8(0), imm);
      if (op !== 7) sr8(0, res);
      break;
    }

    // ──── ALU AX, imm16/32 (0x05,0x0D,0x15,0x1D,0x25,0x2D,0x35,0x3D) ────
    case 0x05:case 0x0D:case 0x15:case 0x1D:case 0x25:case 0x2D:case 0x35:case 0x3D: {
      const op = (b - 5) >> 3;
      ilen += 1;
      if (opSize === 2) {
        const imm = mem8[ci+1] | (mem8[ci+2] << 8); ilen += 2;
        const res = alu16(op, gr16(0), imm);
        if (op !== 7) sr16(0, res);
      } else {
        const imm = mem8[ci+1]|(mem8[ci+2]<<8)|(mem8[ci+3]<<16)|(mem8[ci+4]<<24); ilen += 4;
        const res = alu32(op, gr32(0), imm);
        if (op !== 7) sr32(0, res);
      }
      break;
    }

    // ──── ALU r/m8, imm8 (0x80) ────
    case 0x80:
    case 0x82: { // 0x82 is undocumented alias for 0x80
      const modrm = mem8[ci+1]; ilen += 2;
      const op = (modrm >> 3) & 7;
      if ((modrm >> 6) === 3) {
        const imm = mem8[ci+2]; ilen += 1;
        const res = alu8(op, gr8(modrm & 7), imm);
        if (op !== 7) sr8(modrm & 7, res);
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        const imm = mem8[ci+2+m.len]; ilen += 1;
        const res = alu8(op, rb(m.ea), imm);
        if (op !== 7) wb(m.ea, res);
      }
      break;
    }

    // ──── ALU r/m16/32, imm16/32 (0x81) ────
    case 0x81: {
      const modrm = mem8[ci+1]; ilen += 2;
      const op = (modrm >> 3) & 7;
      if ((modrm >> 6) === 3) {
        const rm = modrm & 7;
        if (opSize === 2) {
          const imm = mem8[ci+2] | (mem8[ci+3] << 8); ilen += 2;
          const res = alu16(op, gr16(rm), imm);
          if (op !== 7) sr16(rm, res);
        } else {
          const imm = mem8[ci+2]|(mem8[ci+3]<<8)|(mem8[ci+4]<<16)|(mem8[ci+5]<<24); ilen += 4;
          const res = alu32(op, gr32(rm), imm);
          if (op !== 7) sr32(rm, res);
        }
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        const imOff = ci + 2 + m.len;
        if (opSize === 2) {
          const imm = mem8[imOff] | (mem8[imOff+1] << 8); ilen += 2;
          const res = alu16(op, rw(m.ea), imm);
          if (op !== 7) ww(m.ea, res);
        } else {
          const imm = mem8[imOff]|(mem8[imOff+1]<<8)|(mem8[imOff+2]<<16)|(mem8[imOff+3]<<24); ilen += 4;
          const res = alu32(op, rd(m.ea), imm);
          if (op !== 7) wd(m.ea, res);
        }
      }
      break;
    }

    // ──── ALU r/m16/32, imm8 sign-extended (0x83) ────
    case 0x83: {
      const modrm = mem8[ci+1]; ilen += 2;
      const op = (modrm >> 3) & 7;
      let imm = mem8[ci+2]; ilen += 1;
      // But wait — imm is after modrm+displacement, not at ci+2 for memory operands
      // Need to handle this correctly
      if ((modrm >> 6) === 3) {
        // imm is at ci+2
        if (imm > 127) imm = opSize === 2 ? (imm | 0xFF00) : ((imm | 0xFFFFFF00) >>> 0);
        if (opSize === 2) {
          const res = alu16(op, gr16(modrm & 7), imm & 0xFFFF);
          if (op !== 7) sr16(modrm & 7, res);
        } else {
          const res = alu32(op, gr32(modrm & 7), imm);
          if (op !== 7) sr32(modrm & 7, res);
        }
      } else {
        ilen -= 1; // undo imm consumption, recalculate after displacement
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        imm = mem8[ci + 2 + m.len]; ilen += 1;
        if (imm > 127) imm = opSize === 2 ? (imm | 0xFF00) : ((imm | 0xFFFFFF00) >>> 0);
        if (opSize === 2) {
          const res = alu16(op, rw(m.ea), imm & 0xFFFF);
          if (op !== 7) ww(m.ea, res);
        } else {
          const res = alu32(op, rd(m.ea), imm);
          if (op !== 7) wd(m.ea, res);
        }
      }
      break;
    }

    // ──── TEST r/m8, r8 (0x84) ────
    case 0x84: {
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      let val;
      if ((modrm >> 6) === 3) val = gr8(modrm & 7);
      else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        val = rb(m.ea);
      }
      alu8(4, val, gr8(reg)); // AND but don't store
      break;
    }

    // ──── TEST r/m16/32, r16/32 (0x85) ────
    case 0x85: {
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      let val;
      if ((modrm >> 6) === 3) val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
      else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        val = opSize === 2 ? rw(m.ea) : rd(m.ea);
      }
      if (opSize === 2) alu16(4, val, gr16(reg));
      else alu32(4, val, gr32(reg));
      break;
    }

    // ──── TEST AL, imm8 (0xA8) ────
    case 0xA8:
      alu8(4, gr8(0), mem8[ci+1]);
      ilen += 2;
      break;

    // ──── TEST AX, imm16/32 (0xA9) ────
    case 0xA9:
      ilen += 1;
      if (opSize === 2) { alu16(4, gr16(0), mem8[ci+1]|(mem8[ci+2]<<8)); ilen += 2; }
      else { alu32(4, gr32(0), mem8[ci+1]|(mem8[ci+2]<<8)|(mem8[ci+3]<<16)|(mem8[ci+4]<<24)); ilen += 4; }
      break;

    // ──── INC r16/32 (0x40-0x47) ────
    case 0x40:case 0x41:case 0x42:case 0x43:case 0x44:case 0x45:case 0x46:case 0x47: {
      const reg = b - 0x40;
      const oldCF = getCF();
      if (opSize === 2) {
        const v = (gr16(reg) + 1) & 0xFFFF;
        sr16(reg, v);
        setFlagsArith(OP_INC, v, v-1, 1, 2);
      } else {
        const v = (gr32(reg) + 1) >>> 0;
        sr32(reg, v);
        setFlagsArith(OP_INC, v, v-1, 1, 4);
      }
      lazyCF = oldCF;
      ilen += 1;
      break;
    }

    // ──── DEC r16/32 (0x48-0x4F) ────
    case 0x48:case 0x49:case 0x4A:case 0x4B:case 0x4C:case 0x4D:case 0x4E:case 0x4F: {
      const reg = b - 0x48;
      const oldCF = getCF();
      if (opSize === 2) {
        const v = (gr16(reg) - 1) & 0xFFFF;
        sr16(reg, v);
        setFlagsArith(OP_DEC, v, v+1, 1, 2);
      } else {
        const v = (gr32(reg) - 1) >>> 0;
        sr32(reg, v);
        setFlagsArith(OP_DEC, v, v+1, 1, 4);
      }
      lazyCF = oldCF;
      ilen += 1;
      break;
    }

    // ──── PUSH r16/32 (0x50-0x57) ────
    case 0x50:case 0x51:case 0x52:case 0x53:case 0x54:case 0x55:case 0x56:case 0x57:
      if (opSize === 2) push16(gr16(b - 0x50), ssBase);
      else push32(gr32(b - 0x50), ssBase);
      ilen += 1;
      break;

    // ──── POP r16/32 (0x58-0x5F) ────
    case 0x58:case 0x59:case 0x5A:case 0x5B:case 0x5C:case 0x5D:case 0x5E:case 0x5F:
      if (opSize === 2) sr16(b - 0x58, pop16(ssBase));
      else sr32(b - 0x58, pop32(ssBase));
      ilen += 1;
      break;

    // ──── PUSH imm16/32 (0x68) ────
    case 0x68:
      ilen += 1;
      if (opSize === 2) {
        push16(mem8[ci+1] | (mem8[ci+2] << 8), ssBase);
        ilen += 2;
      } else {
        push32(mem8[ci+1]|(mem8[ci+2]<<8)|(mem8[ci+3]<<16)|(mem8[ci+4]<<24), ssBase);
        ilen += 4;
      }
      break;

    // ──── PUSH imm8 sign-extended (0x6A) ────
    case 0x6A: {
      let v = mem8[ci+1];
      if (v > 127) v = opSize === 2 ? (v | 0xFF00) : ((v | 0xFFFFFF00) >>> 0);
      if (opSize === 2) push16(v & 0xFFFF, ssBase);
      else push32(v, ssBase);
      ilen += 2;
      break;
    }

    // ──── IMUL r, r/m, imm16/32 (0x69) ────
    case 0x69: {
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      let val;
      if ((modrm >> 6) === 3) { val = opSize===2 ? gr16(modrm&7) : gr32(modrm&7); }
      else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS) : decodeModRM32(modrm, mem8, ci+2, effDS, effSS); ilen += m.len; val = opSize===2 ? rw(m.ea) : rd(m.ea); }
      if (opSize === 2) {
        let imm = mem8[ci+ilen] | (mem8[ci+ilen+1] << 8); ilen += 2;
        if (imm > 0x7FFF) imm -= 0x10000;
        const sval = (val << 16) >> 16;
        const result = sval * imm;
        sr16(reg, result & 0xFFFF);
        lazyCF = (result !== ((result << 16) >> 16)) ? 1 : 0;
      } else {
        let imm = mem8[ci+ilen]|(mem8[ci+ilen+1]<<8)|(mem8[ci+ilen+2]<<16)|(mem8[ci+ilen+3]<<24); ilen += 4;
        const result = Math.imul(val, imm);
        sr32(reg, result >>> 0);
        const big = BigInt(val | 0) * BigInt(imm | 0);
        lazyCF = (big !== BigInt(result | 0)) ? 1 : 0;
      }
      lazyOp = OP_EXPLICIT;
      lazyExplicitFlags = lazyCF ? (0x801 | 0x02) : 0x02;
      break;
    }

    // ──── IMUL r, r/m, imm8 (0x6B) ────
    case 0x6B: {
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      let val;
      if ((modrm >> 6) === 3) { val = opSize===2 ? gr16(modrm&7) : gr32(modrm&7); }
      else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS) : decodeModRM32(modrm, mem8, ci+2, effDS, effSS); ilen += m.len; val = opSize===2 ? rw(m.ea) : rd(m.ea); }
      let imm = mem8[ci+ilen]; ilen += 1;
      if (imm > 127) imm -= 256; // sign-extend
      if (opSize === 2) {
        const sval = (val << 16) >> 16;
        const result = sval * imm;
        sr16(reg, result & 0xFFFF);
        lazyCF = (result !== ((result << 16) >> 16)) ? 1 : 0;
      } else {
        const result = Math.imul(val, imm);
        sr32(reg, result >>> 0);
        const big = BigInt(val | 0) * BigInt(imm);
        lazyCF = (big !== BigInt(result | 0)) ? 1 : 0;
      }
      lazyOp = OP_EXPLICIT;
      lazyExplicitFlags = lazyCF ? (0x801 | 0x02) : 0x02;
      break;
    }

    // ──── PUSHF (0x9C) ────
    case 0x9C: {
      const f = flagsToWord() | (flags & 0xFFFFF700); // preserve TF/IF/DF/upper bits
      if (opSize === 2) push16(f & 0xFFFF, ssBase);
      else push32(f, ssBase);
      ilen += 1;
      break;
    }

    // ──── POPF (0x9D) ────
    case 0x9D: {
      let f;
      if (opSize === 2) f = pop16(ssBase);
      else f = pop32(ssBase);
      flags = f;
      loadFlags(f);
      ilen += 1;
      // Bail if TF was set by POPF — IEM must handle #DB on next instruction
      if (flags & 0x100) { ip = (ip + ilen) & ipMask; wrIP(ip); wr32(R_FLAGS, (flags & 0xFFFFF700) | flagsToWord()); executed++; iter = maxInsn; ilen = 0; continue; }
      break;
    }

    // ──── XCHG r16/32, AX (0x91-0x97) ────
    case 0x91:case 0x92:case 0x93:case 0x94:case 0x95:case 0x96:case 0x97: {
      const reg = b - 0x90;
      if (opSize === 2) {
        const t = gr16(0); sr16(0, gr16(reg)); sr16(reg, t);
      } else {
        const t = gr32(0); sr32(0, gr32(reg)); sr32(reg, t);
      }
      ilen += 1;
      break;
    }

    // ──── XCHG r/m8, r8 (0x86) ────
    case 0x86: {
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      if ((modrm >> 6) === 3) {
        const t = gr8(reg); sr8(reg, gr8(modrm & 7)); sr8(modrm & 7, t);
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        // Read and write must both succeed atomically; write first, check mmioFault
        const memVal = rb(m.ea);
        const regVal = gr8(reg);
        wb(m.ea, regVal);
        if (!mmioFault) { sr8(reg, memVal); }
      }
      break;
    }

    // ──── XCHG r/m16/32, r16/32 (0x87) ────
    case 0x87: {
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      if ((modrm >> 6) === 3) {
        if (opSize === 2) { const t = gr16(reg); sr16(reg, gr16(modrm&7)); sr16(modrm&7, t); }
        else { const t = gr32(reg); sr32(reg, gr32(modrm&7)); sr32(modrm&7, t); }
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        if (opSize === 2) {
          const memVal = rw(m.ea); const regVal = gr16(reg);
          ww(m.ea, regVal); if (!mmioFault) sr16(reg, memVal);
        } else {
          const memVal = rd(m.ea); const regVal = gr32(reg);
          wd(m.ea, regVal); if (!mmioFault) sr32(reg, memVal);
        }
      }
      break;
    }

    // ──── LEA r16/32, m (0x8D) ────
    case 0x8D: {
      const modrm = mem8[ci+1]; ilen += 2;
      const reg = (modrm >> 3) & 7;
      // LEA computes effective address but doesn't add segment base
      const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, 0, 0) // base=0 to get raw offset
                               : decodeModRM32(modrm, mem8, ci+2, 0, 0);
      ilen += m.len;
      if (opSize === 2) sr16(reg, m.ea & 0xFFFF);
      else sr32(reg, m.ea);
      break;
    }

    // ──── JMP rel8 (0xEB) ────
    case 0xEB: {
      let rel = mem8[ci+1];
      if (rel > 127) rel -= 256;
      ip = (ip + 2 + pos + rel) & ipMask;
      ilen = 0; // ip already set
      executed++;
      // Store state and continue from new IP
      wrIP(ip);
      continue; // skip ip update at bottom
    }

    // ──── JMP rel16/32 (0xE9) ────
    case 0xE9: {
      let rel;
      if (opSize === 2) {
        rel = mem8[ci+1] | (mem8[ci+2] << 8);
        if (rel > 0x7FFF) rel -= 0x10000;
        ip = (ip + 3 + pos + rel) & ipMask;
      } else {
        rel = mem8[ci+1]|(mem8[ci+2]<<8)|(mem8[ci+3]<<16)|(mem8[ci+4]<<24);
        ip = (ip + 5 + pos + rel) & ipMask;
      }
      ilen = 0;
      executed++;
      wrIP(ip);
      continue;
    }

    // ──── Jcc rel8 (0x70-0x7F) ────
    case 0x70:case 0x71:case 0x72:case 0x73:case 0x74:case 0x75:case 0x76:case 0x77:
    case 0x78:case 0x79:case 0x7A:case 0x7B:case 0x7C:case 0x7D:case 0x7E:case 0x7F: {
      let rel = mem8[ci+1];
      if (rel > 127) rel -= 256;
      ilen += 2;
      if (testCC(b - 0x70)) {
        ip = (ip + ilen + rel) & ipMask;
        ilen = 0;
        executed++;
        wrIP(ip);
        continue;
      }
      break;
    }

    // ──── LOOP/LOOPcc (0xE0-0xE2) ────
    case 0xE2: { // LOOP rel8
      let rel = mem8[ci+1]; if (rel > 127) rel -= 256;
      ilen += 2;
      let cx;
      if (addrSize === 4) { cx = (gr32(1) - 1) >>> 0; sr32(1, cx); } else { cx = (gr16(1) - 1) & 0xFFFF; sr16(1, cx); }
      if (cx !== 0) {
        ip = (ip + ilen + rel) & ipMask;
        ilen = 0; executed++; wrIP(ip); continue;
      }
      break;
    }
    case 0xE1: { // LOOPE rel8
      let rel = mem8[ci+1]; if (rel > 127) rel -= 256;
      ilen += 2;
      let cx;
      if (addrSize === 4) { cx = (gr32(1) - 1) >>> 0; sr32(1, cx); } else { cx = (gr16(1) - 1) & 0xFFFF; sr16(1, cx); }
      if (cx !== 0 && getZF()) {
        ip = (ip + ilen + rel) & ipMask;
        ilen = 0; executed++; wrIP(ip); continue;
      }
      break;
    }
    case 0xE0: { // LOOPNE rel8
      let rel = mem8[ci+1]; if (rel > 127) rel -= 256;
      ilen += 2;
      let cx;
      if (addrSize === 4) { cx = (gr32(1) - 1) >>> 0; sr32(1, cx); } else { cx = (gr16(1) - 1) & 0xFFFF; sr16(1, cx); }
      if (cx !== 0 && !getZF()) {
        ip = (ip + ilen + rel) & ipMask;
        ilen = 0; executed++; wrIP(ip); continue;
      }
      break;
    }

    // ──── JCXZ rel8 (0xE3) — jump if CX (or ECX) zero ────
    case 0xE3: {
      let rel = mem8[ci+1]; if (rel > 127) rel -= 256;
      ilen += 2;
      const cx = addrSize === 2 ? gr16(1) : gr32(1);
      if (cx === 0) {
        ip = (ip + ilen + rel) & ipMask;
        ilen = 0; executed++; wrIP(ip); continue;
      }
      break;
    }

    // ──── ENTER imm16, imm8 (0xC8) ────
    case 0xC8: {
      const frameSize = mem8[ci+1] | (mem8[ci+2] << 8);
      const level = mem8[ci+3] & 0x1F;
      // Level > 0: copy outer frames (rare in BIOS, bail before touching state)
      if (level > 0) { lastBailOp = b; iter = maxInsn; break; }
      ilen += 4;
      if (opSize === 2) {
        push16(gr16(5), ssBase); // push BP
        const framePtr = _ssBig ? gr32(4) : gr16(4); // SP after push = new BP
        sr16(5, framePtr & 0xFFFF);
        if (_ssBig) sr32(4, (gr32(4) - frameSize) >>> 0);
        else sr16(4, (gr16(4) - frameSize) & 0xFFFF);
      } else {
        push32(gr32(5), ssBase); // push EBP
        const framePtr = _ssBig ? gr32(4) : gr16(4);
        sr32(5, framePtr);
        if (_ssBig) sr32(4, (gr32(4) - frameSize) >>> 0);
        else sr16(4, (gr16(4) - frameSize) & 0xFFFF);
      }
      break;
    }

    // ──── CALL rel16/32 (0xE8) ────
    case 0xE8: {
      let rel;
      if (opSize === 2) {
        rel = mem8[ci+1] | (mem8[ci+2] << 8);
        if (rel > 0x7FFF) rel -= 0x10000;
        ilen += 3;
        push16((ip + ilen) & 0xFFFF, ssBase);
        ip = (ip + ilen + rel) & ipMask;
      } else {
        rel = mem8[ci+1]|(mem8[ci+2]<<8)|(mem8[ci+3]<<16)|(mem8[ci+4]<<24);
        ilen += 5;
        push32((ip + ilen) & 0xFFFFFFFF, ssBase);
        ip = (ip + ilen + rel) & ipMask;
      }
      ilen = 0; executed++; wrIP(ip); continue;
    }

    // ──── RET near (0xC3) ────
    case 0xC3:
      if (opSize === 2) ip = pop16(ssBase);
      else ip = pop32(ssBase) & ipMask;
      ilen = 0; executed++; wrIP(ip); continue;

    // ──── RET near imm16 (0xC2) ────
    case 0xC2: {
      const imm = mem8[ci+1] | (mem8[ci+2] << 8);
      if (opSize === 2) {
        ip = pop16(ssBase);
        if (_ssBig) sr32(4, (gr32(4) + imm) >>> 0);
        else sr16(4, (gr16(4) + imm) & 0xFFFF);
      } else {
        ip = pop32(ssBase) & ipMask;
        if (_ssBig) sr32(4, (gr32(4) + imm) >>> 0);
        else sr16(4, (gr16(4) + imm) & 0xFFFF);
      }
      ilen = 0; executed++; wrIP(ip); continue;
    }

    // ──── CLI (0xFA), STI (0xFB) ────
    case 0xFA: flags &= ~0x200; ilen += 1; break;
    case 0xFB: {
      const wasIF0 = !(flags & 0x200);
      flags |= 0x200;
      ilen += 1;
      // Don't bail for kernel setup code (CS=0x1020) — the timer cascade
      // prevents the kernel from ever getting past STI.  Let the kernel
      // continue executing in JIT until it hits something we can't handle
      // (protected mode, unsupported INT, etc.).
      const csSel_sti = rr16(S_CS);
      if (wasIF0 && executed > 0 && globalThis.VBoxJIT._irqPending
          && csSel_sti !== 0x1020) {
        // Transitioning IF from 0→1: bail to IEM so pending interrupts
        // (PIT timer, keyboard IRQ) can be delivered.  On real x86, one
        // instruction after STI executes before interrupts are serviced,
        // but IEM handles this correctly via its "inhibit interrupts
        // after STI" logic.  Without this bail the JIT can spin through
        // thousands of CLI/STI polling loops (e.g. BIOS INT 16h keyboard
        // wait) without ever letting the EM deliver pending IRQs.
        ip = (ip + ilen) & ipMask;
        executed++;
        wrIP(ip);
        ilen = 0;
        iter = maxInsn; // force exit from main loop
      }
      break;
    }

    // ──── CLD (0xFC), STD (0xFD) ────
    case 0xFC: flags &= ~0x400; ilen += 1; break;
    case 0xFD: flags |= 0x400; ilen += 1; break;

    // ──── CLC (0xF8), STC (0xF9), CMC (0xF5) ────
    case 0xF8: lazyCF = 0; lazyOp = OP_NONE; ilen += 1; break;
    case 0xF9: lazyCF = 1; lazyOp = OP_NONE; ilen += 1; break;
    case 0xF5: lazyCF = getCF() ? 0 : 1; lazyOp = OP_NONE; ilen += 1; break;

    // ──── CBW / CWDE (0x98) ────
    case 0x98:
      if (opSize === 2) {
        let al = gr8(0); if (al > 127) al |= 0xFF00;
        sr16(0, al & 0xFFFF);
      } else {
        let ax = gr16(0); if (ax > 0x7FFF) ax |= 0xFFFF0000;
        sr32(0, ax >>> 0);
      }
      ilen += 1;
      break;

    // ──── CWD / CDQ (0x99) ────
    case 0x99:
      if (opSize === 2) {
        sr16(2, (gr16(0) & 0x8000) ? 0xFFFF : 0); // DX
      } else {
        sr32(2, (gr32(0) & 0x80000000) ? 0xFFFFFFFF : 0);
      }
      ilen += 1;
      break;

    // ──── MOVZX r16/32, r/m8 (0x0F 0xB6) — handled in 0x0F block ────
    // ──── MOVSX r16/32, r/m8 (0x0F 0xBE) — handled in 0x0F block ────

    // ──── IN/OUT: bail to IEM for proper I/O port handling ────
    // IN/OUT must go through VBox's I/O port infrastructure so devices
    // (keyboard controller, PIT, PIC, VGA, IDE) respond correctly.
    // Without this, portIn returns 0xFF causing infinite polling loops.
    // EXCEPTION: debug-only ports (0x80, 0x402, 0x403, 0x504) are handled
    // locally to capture BIOS POST codes and panic messages without IEM overhead.
    case 0xE4: case 0xE5: case 0xEC: case 0xED:  // IN
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: { // OUT
      const isImm = (b === 0xE4||b===0xE5||b===0xE6||b===0xE7);
      const portNum = isImm ? mem8[ci+1] : gr16(2);
      const isOut = (b===0xE6||b===0xE7||b===0xEE||b===0xEF);

      // Handle debug-only ports locally (no VBox device response needed)
      if (isOut && (portNum === 0x80 || portNum === 0x402 || portNum === 0x403 || portNum === 0x504)) {
        const val = gr8(0); // AL for byte OUT
        if (portNum === 0x80) {
          // POST diagnostic code
          if (val !== lastPost80) {
            postCodes80.push(val);
            lastPost80 = val;
            if (postCodes80.length <= 200 || postCodes80.length % 100 === 0)
              console.log('[POST80] code=0x' + val.toString(16).padStart(2,'0') +
                ' @' + (csBase>>>4).toString(16) + ':' + ip.toString(16) +
                ' seq=' + postCodes80.length);
          }
        } else {
          // BIOS debug message (port 0x402/0x403/0x504)
          biosDebugChar(String.fromCharCode(val));
        }
        ilen += isImm ? 2 : 1;
        break; // handle locally, don't bail
      }

      // ATA status register polling aggregate tracking
      const isAtaStatus = !isOut && (portNum === 0x1F7 || portNum === 0x177);
      if (isAtaStatus) {
        ataStatusPolls++;
        if (!ataStatusPollStart) ataStatusPollStart = Date.now();
        const now = Date.now();
        if (now - ataStatusLastReport > 3000 && ataStatusPolls > 0) {
          const elapsed = now - ataStatusPollStart;
          console.log('[ATA-POLL] ' + ataStatusPolls + ' status polls in ' +
            elapsed + 'ms @' + (csBase>>>4).toString(16) + ':' + ip.toString(16) +
            ' port=0x' + portNum.toString(16));
          ataStatusLastReport = now;
        }
      }

      // Log VGA attribute controller port 0x3C0-0x3DF with higher limit
      const isVgaPort = (portNum >= 0x3C0 && portNum <= 0x3DF);
      // Log the port being accessed (first 30 per IP to diagnose stalls; 200 for VGA ports)
      const portDiagKey = (isVgaPort ? 0x10000 : 0) | portNum;
      if (!ioDiagCounts.has(portDiagKey)) ioDiagCounts.set(portDiagKey, 0);
      const cnt = ioDiagCounts.get(portDiagKey) + 1;
      ioDiagCounts.set(portDiagKey, cnt);
      const logLimit = isVgaPort ? 3 : (isAtaStatus ? 2 : 3); /* reduced from 200/5/30 */
      if (cnt <= logLimit || cnt % 100000 === 0) {
        const dir = isOut ? 'OUT' : 'IN';
        const logTag = isVgaPort ? '[VGA-IO]' : (isAtaStatus ? '[ATA-IO]' : '[JIT-IO]');
        console.log(logTag + ' @' + (csBase>>>4).toString(16) + ':' + ip.toString(16) +
          ' ' + dir + ' port=0x' + portNum.toString(16) +
          ' DX=0x' + gr16(2).toString(16) + ' AX=0x' + gr16(0).toString(16) +
          ' #' + cnt);
      }
      lastBailOp = b; iter = maxInsn;
      break;
    }

    // ──── REP/REPNE + string ops ────
    case 0xAA: case 0xAB: case 0xAC: case 0xAD:
    case 0xAE: case 0xAF: case 0xA4: case 0xA5:
    case 0xA6: case 0xA7: case 0x6C: case 0x6D:
    case 0x6E: case 0x6F: {
      // String operations — address size selects SI/DI/CX width
      const dir = (flags & 0x400) ? -1 : 1; // DF flag
      ilen += 1;
      const a32 = addrSize === 4;
      const aMask = a32 ? 0xFFFFFFFF : 0xFFFF;
      const grDI = () => a32 ? gr32(7) : gr16(7);
      const grSI = () => a32 ? gr32(6) : gr16(6);
      const grCX = () => a32 ? gr32(1) : gr16(1);
      const srDI = (v) => { if (a32) sr32(7, v >>> 0); else sr16(7, v & 0xFFFF); };
      const srSI = (v) => { if (a32) sr32(6, v >>> 0); else sr16(6, v & 0xFFFF); };
      const srCX = (v) => { if (a32) sr32(1, v >>> 0); else sr16(1, v & 0xFFFF); };

      if (repPrefix && (b === 0xA4 || b === 0xA5 || b === 0xAA || b === 0xAB ||
                         b === 0xAC || b === 0xAD || b === 0x6C || b === 0x6D ||
                         b === 0x6E || b === 0x6F)) {
        // REP prefix — repeat CX/ECX times
        let cx = grCX();
        if (cx === 0) break;

        const srcSeg = segOverride >= 0 ? segBase(segOverride) : dsBase;

        switch (b) {
          case 0xAA: { // STOSB — optimized bulk fill
            let di = grDI();
            const val = gr8(0);
            // Chunk limit: process at most 65536 iterations to allow timer interrupts
            const maxChunk = 65536;
            const origCx = cx;
            if (cx > maxChunk) cx = maxChunk;
            const byteCount = cx;
            let addr = esBase + di;
            // Apply A20 mask — compute range for both forward and backward
            let addrLo = dir === 1 ? addr : addr - byteCount + 1;
            let addrHi = dir === 1 ? addr + byteCount : addr + 1;
            // When A20 is disabled and range crosses 1MB, it wraps (non-contiguous) — bail
            if (a20Mask !== 0xFFFFFFFF && addrHi > 0x100000) {
              lastBailOp = b; iter = maxInsn; break;
            }
            {
              const wOff = resolveRangeW(addrLo, addrHi);
              if (wOff >= 0) {
                if (!inRomRange(codePhys) && addrHi > codeSegStart && addrLo < codeSegEnd) {
                  ilen = 0; iter = maxInsn; break;
                }
                mem8.fill(val, wOff, wOff + (addrHi - addrLo));
                di = (di + dir * byteCount) & aMask;
                cx = origCx - byteCount;
              } else {
                lastBailOp = b; iter = maxInsn; break;
              }
            }
            srDI(di); srCX(cx);
            if (cx > 0) { ilen = 0; iter = maxInsn; } // more to do — yield for timers
            break;
          }
          case 0xAB: { // STOSW/STOSD — optimized bulk fill
            let di = grDI();
            const sz = opSize; // 2 or 4
            const v = sz === 2 ? gr16(0) : gr32(0);
            // Chunk limit: process at most 65536 iterations to allow timer interrupts
            const maxChunkAB = 65536;
            const origCxAB = cx;
            if (cx > maxChunkAB) cx = maxChunkAB;
            const totalBytes = cx * sz;
            let addr = esBase + di;
            let addrLo = dir === 1 ? addr : addr - totalBytes + sz;
            let addrHi = dir === 1 ? addr + totalBytes : addr + sz;
            // When A20 is disabled and range crosses 1MB, it wraps (non-contiguous) — bail
            if (a20Mask !== 0xFFFFFFFF && addrHi > 0x100000) {
              lastBailOp = b; iter = maxInsn; break;
            }
            {
              const wOff = resolveRangeW(addrLo, addrHi);
              if (wOff >= 0) {
                if (!inRomRange(codePhys) && addrHi > codeSegStart && addrLo < codeSegEnd) {
                  ilen = 0; iter = maxInsn; break;
                }
                if (v === 0) {
                  mem8.fill(0, wOff, wOff + totalBytes);
                } else {
                  if (sz === 2) dv.setUint16(wOff, v, true);
                  else dv.setUint32(wOff, v >>> 0, true);
                  let filled = sz;
                  while (filled < totalBytes) {
                    const chunk = Math.min(filled, totalBytes - filled);
                    mem8.copyWithin(wOff + filled, wOff, wOff + chunk);
                    filled += chunk;
                  }
                }
                di = (di + dir * totalBytes) & aMask;
                cx = origCxAB - cx;
              } else {
                lastBailOp = b; iter = maxInsn; break;
              }
            }
            srDI(di); srCX(cx);
            if (cx > 0) { ilen = 0; iter = maxInsn; } // more to do — yield for timers
            break;
          }
          case 0xA4: { // MOVSB — optimized bulk copy
            let si = grSI(), di = grDI();
            const srcAddr = srcSeg + si;
            const dstAddr = esBase + di;
            // Chunk limit: process at most 65536 iterations to allow timer interrupts
            const maxChunkA4 = 65536;
            const origCxA4 = cx;
            if (cx > maxChunkA4) cx = maxChunkA4;
            const byteCount = cx;
            // Compute address ranges for both forward and backward
            const srcLo = dir === 1 ? srcAddr : srcAddr - byteCount + 1;
            const srcHi = dir === 1 ? srcAddr + byteCount : srcAddr + 1;
            const dstLo = dir === 1 ? dstAddr : dstAddr - byteCount + 1;
            const dstHi = dir === 1 ? dstAddr + byteCount : dstAddr + 1;
            // When A20 is disabled and either range crosses 1MB, bail
            if (a20Mask !== 0xFFFFFFFF && (srcHi > 0x100000 || dstHi > 0x100000)) {
              lastBailOp = b; iter = maxInsn; break;
            }
            {
              const srcOff = resolveRange(srcLo, srcHi);
              const dstOff = resolveRangeW(dstLo, dstHi);
              if (srcOff >= 0 && dstOff >= 0) {
                if (!inRomRange(codePhys) && dstHi > codeSegStart && dstLo < codeSegEnd) {
                  ilen = 0; iter = maxInsn; break;
                }
                // copyWithin works even across different base offsets in same buffer
                mem8.copyWithin(dstOff, srcOff, srcOff + byteCount);
                si = (si + dir * byteCount) & aMask;
                di = (di + dir * byteCount) & aMask;
                cx = origCxA4 - byteCount;
              } else {
                lastBailOp = b; iter = maxInsn; break;
              }
            }
            srSI(si); srDI(di); srCX(cx);
            if (cx > 0) { ilen = 0; iter = maxInsn; } // more to do — yield for timers
            break;
          }
          case 0xA5: { // MOVSW/MOVSD — optimized bulk copy
            let si = grSI(), di = grDI();
            const sz5 = opSize; // 2 or 4
            // Chunk limit: process at most 65536 iterations to allow timer interrupts
            const maxChunkA5 = 65536;
            const origCxA5 = cx;
            if (cx > maxChunkA5) cx = maxChunkA5;
            const totalBytes5 = cx * sz5;
            const srcAddr5 = srcSeg + si;
            const dstAddr5 = esBase + di;
            const srcLo5 = dir === 1 ? srcAddr5 : srcAddr5 - totalBytes5 + sz5;
            const srcHi5 = dir === 1 ? srcAddr5 + totalBytes5 : srcAddr5 + sz5;
            const dstLo5 = dir === 1 ? dstAddr5 : dstAddr5 - totalBytes5 + sz5;
            const dstHi5 = dir === 1 ? dstAddr5 + totalBytes5 : dstAddr5 + sz5;
            // When A20 is disabled and either range crosses 1MB, bail
            if (a20Mask !== 0xFFFFFFFF && (srcHi5 > 0x100000 || dstHi5 > 0x100000)) {
              lastBailOp = b; iter = maxInsn; break;
            }
            {
              const srcOff5 = resolveRange(srcLo5, srcHi5);
              const dstOff5 = resolveRangeW(dstLo5, dstHi5);
              if (srcOff5 >= 0 && dstOff5 >= 0) {
                if (!inRomRange(codePhys) && dstHi5 > codeSegStart && dstLo5 < codeSegEnd) {
                  ilen = 0; iter = maxInsn; break;
                }
                mem8.copyWithin(dstOff5, srcOff5, srcOff5 + totalBytes5);
                si = (si + dir * totalBytes5) & aMask;
                di = (di + dir * totalBytes5) & aMask;
                cx = origCxA5 - cx;
              } else {
                lastBailOp = b; iter = maxInsn; break;
              }
            }
            srSI(si); srDI(di); srCX(cx);
            if (cx > 0) { ilen = 0; iter = maxInsn; } // more to do — yield for timers
            break;
          }
          case 0xAC: { // LODSB
            let si = grSI();
            while (cx > 0) {
              sr8(0, rb(srcSeg + si));
              si = (si + dir) & aMask; cx--;
            }
            srSI(si); srCX(0);
            break;
          }
          case 0xAD: { // LODSW/LODSD
            let si = grSI();
            while (cx > 0) {
              if (opSize === 2) sr16(0, rw(srcSeg + si));
              else sr32(0, rd(srcSeg + si));
              si = (si + dir * opSize) & aMask; cx--;
            }
            srSI(si); srCX(0);
            break;
          }
          default:
            // INSB/INSW/OUTSB/OUTSW — fall back to IEM
            lastBailOp = b; iter = maxInsn; // force exit
            break;
        }
      } else if (repPrefix && (b === 0xAE || b === 0xAF)) {
        // REPE/REPNE SCAS
        let cx = grCX(), di = grDI();
        const isRepNE = (repPrefix === 0xF2);
        if (b === 0xAE) { // SCASB
          const al = gr8(0);
          while (cx > 0) {
            const v = rb(esBase + di);
            di = (di + dir) & aMask; cx--;
            alu8(7, al, v); // CMP
            if (isRepNE ? getZF() : !getZF()) break;
          }
        } else { // SCASW/SCASD
          if (opSize === 2) {
            const ax = gr16(0);
            while (cx > 0) {
              const v = rw(esBase + di);
              di = (di + dir * 2) & aMask; cx--;
              alu16(7, ax, v);
              if (isRepNE ? getZF() : !getZF()) break;
            }
          } else {
            const eax = gr32(0);
            while (cx > 0) {
              const v = rd(esBase + di);
              di = (di + dir * 4) & aMask; cx--;
              alu32(7, eax, v);
              if (isRepNE ? getZF() : !getZF()) break;
            }
          }
        }
        srDI(di); srCX(cx);
      } else if (repPrefix && (b === 0xA6 || b === 0xA7)) {
        // REPE/REPNE CMPS
        let cx = grCX(), si = grSI(), di = grDI();
        const isRepNE = (repPrefix === 0xF2);
        const srcSeg = segOverride >= 0 ? segBase(segOverride) : dsBase;
        if (b === 0xA6) { // CMPSB
          while (cx > 0) {
            const a = rb(srcSeg + si), bv = rb(esBase + di);
            si = (si + dir) & aMask; di = (di + dir) & aMask; cx--;
            alu8(7, a, bv);
            if (isRepNE ? getZF() : !getZF()) break;
          }
        } else { // CMPSW/CMPSD
          const sz = opSize;
          while (cx > 0) {
            const a = sz === 2 ? rw(srcSeg + si) : rd(srcSeg + si);
            const bv = sz === 2 ? rw(esBase + di) : rd(esBase + di);
            si = (si + dir * sz) & aMask; di = (di + dir * sz) & aMask; cx--;
            if (sz === 2) alu16(7, a, bv); else alu32(7, a, bv);
            if (isRepNE ? getZF() : !getZF()) break;
          }
        }
        srSI(si); srDI(di); srCX(cx);
      } else {
        // Single string op (no REP prefix)
        // IMPORTANT: Do NOT modify DI/SI/CX if the memory write triggered an
        // MMIO fault — IEM will re-execute the entire instruction from scratch.
        switch (b) {
          case 0xAA: { const di = grDI(); wb(esBase + di, gr8(0)); if (!mmioFault) srDI((di+dir)&aMask); break; }
          case 0xAB: {
            const di = grDI();
            if (opSize===2) { ww(esBase+di, gr16(0)); if (!mmioFault) srDI((di+dir*2)&aMask); }
            else { wd(esBase+di, gr32(0)); if (!mmioFault) srDI((di+dir*4)&aMask); }
            break;
          }
          case 0xA4: {
            const srcSeg2 = segOverride >= 0 ? segBase(segOverride) : dsBase;
            const si = grSI(), di = grDI();
            wb(esBase+di, rb(srcSeg2+si));
            if (!mmioFault) { srSI((si+dir)&aMask); srDI((di+dir)&aMask); }
            break;
          }
          case 0xA5: {
            const srcSeg2 = segOverride >= 0 ? segBase(segOverride) : dsBase;
            const si = grSI(), di = grDI();
            if (opSize===2) ww(esBase+di, rw(srcSeg2+si));
            else wd(esBase+di, rd(srcSeg2+si));
            if (!mmioFault) { srSI((si+dir*opSize)&aMask); srDI((di+dir*opSize)&aMask); }
            break;
          }
          case 0xAC: {
            const srcSeg2 = segOverride >= 0 ? segBase(segOverride) : dsBase;
            const si = grSI();
            sr8(0, rb(srcSeg2+si)); srSI((si+dir)&aMask); break;
          }
          case 0xAD: {
            const srcSeg2 = segOverride >= 0 ? segBase(segOverride) : dsBase;
            const si = grSI();
            if (opSize===2) sr16(0, rw(srcSeg2+si));
            else sr32(0, rd(srcSeg2+si));
            srSI((si+dir*opSize)&aMask); break;
          }
          case 0xAE: { const di = grDI(); alu8(7, gr8(0), rb(esBase+di)); srDI((di+dir)&aMask); break; }
          case 0xAF: {
            const di = grDI();
            if (opSize===2) alu16(7, gr16(0), rw(esBase+di));
            else alu32(7, gr32(0), rd(esBase+di));
            srDI((di+dir*opSize)&aMask); break;
          }
          default:
            lastBailOp = b; iter = maxInsn; break;
        }
      }
      break;
    }

    // ──── SAHF (0x9E), LAHF (0x9F) ────
    case 0x9E: { // SAHF: load AH into FLAGS[7:0] (SF:ZF:0:AF:0:PF:1:CF)
      const ah = gr8(4); // AH
      lazyOp = OP_EXPLICIT;
      lazyExplicitFlags = (ah | 0x02) & 0xFF; // preserve reserved bit 1
      lazyCF = ah & 1; // keep lazyCF in sync
      ilen += 1;
      break;
    }
    case 0x9F: { // LAHF: store flags low 8 into AH
      sr8(4, flagsToWord() & 0xFF);
      ilen += 1;
      break;
    }

    // ──── NOT / NEG (0xF6, 0xF7) ────
    case 0xF6: {
      const modrm = mem8[ci+1]; ilen += 2;
      const op = (modrm >> 3) & 7;
      if (op === 2) { // NOT r/m8
        if ((modrm >> 6) === 3) sr8(modrm & 7, ~gr8(modrm & 7) & 0xFF);
        else {
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                   : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
          ilen += m.len; wb(m.ea, ~rb(m.ea) & 0xFF);
        }
      } else if (op === 3) { // NEG r/m8
        let val;
        if ((modrm >> 6) === 3) {
          val = gr8(modrm & 7);
          const r = (-val) & 0xFF;
          sr8(modrm & 7, r);
          setFlagsArith(OP_SUB, r, 0, val, 1); lazyCF = val !== 0 ? 1 : 0;
        } else {
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                   : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
          ilen += m.len;
          val = rb(m.ea); const r = (-val) & 0xFF; wb(m.ea, r);
          setFlagsArith(OP_SUB, r, 0, val, 1); lazyCF = val !== 0 ? 1 : 0;
        }
      } else if (op === 0) { // TEST r/m8, imm8
        let val;
        if ((modrm >> 6) === 3) {
          val = gr8(modrm & 7);
          alu8(4, val, mem8[ci+2]); ilen += 1;
        } else {
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                   : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
          ilen += m.len;
          val = rb(m.ea);
          alu8(4, val, mem8[ci+2+m.len]); ilen += 1;
        }
      } else if (op === 4) { // MUL r/m8 — AX = AL * r/m8
        let val;
        if ((modrm >> 6) === 3) { val = gr8(modrm & 7); }
        else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS) : decodeModRM32(modrm, mem8, ci+2, effDS, effSS); ilen += m.len; val = rb(m.ea); }
        const result = (gr8(0) & 0xFF) * (val & 0xFF);
        sr16(0, result & 0xFFFF); // AX
        lazyCF = (result & 0xFF00) ? 1 : 0;
        lazyOp = OP_EXPLICIT; lazyExplicitFlags = lazyCF ? (0x801 | 0x02) : 0x02;
      } else if (op === 5) { // IMUL r/m8 — AX = AL * r/m8 (signed)
        let val;
        if ((modrm >> 6) === 3) { val = gr8(modrm & 7); }
        else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS) : decodeModRM32(modrm, mem8, ci+2, effDS, effSS); ilen += m.len; val = rb(m.ea); }
        const a = (gr8(0) << 24) >> 24; // sign-extend AL
        const b2 = (val << 24) >> 24;
        const result = a * b2;
        sr16(0, result & 0xFFFF);
        lazyCF = ((result & 0xFFFF) !== ((result << 24) >> 24) & 0xFFFF) ? 1 : 0;
        lazyOp = OP_EXPLICIT; lazyExplicitFlags = lazyCF ? (0x801 | 0x02) : 0x02;
      } else if (op === 6) { // DIV r/m8 — AL = AX / r/m8, AH = AX % r/m8
        let val;
        if ((modrm >> 6) === 3) { val = gr8(modrm & 7); }
        else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS) : decodeModRM32(modrm, mem8, ci+2, effDS, effSS); ilen += m.len; val = rb(m.ea); }
        if (val === 0) { lastBailOp = b; iter = maxInsn; break; } // #DE
        const ax = gr16(0);
        const quot = (ax / val) >>> 0;
        if (quot > 0xFF) { lastBailOp = b; iter = maxInsn; break; } // #DE
        const rem = ax % val;
        sr8(0, quot & 0xFF); sr8(4, rem & 0xFF); // AL=quot, AH=rem
      } else if (op === 7) { // IDIV r/m8
        let val;
        if ((modrm >> 6) === 3) { val = gr8(modrm & 7); }
        else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS) : decodeModRM32(modrm, mem8, ci+2, effDS, effSS); ilen += m.len; val = rb(m.ea); }
        const divisor = (val << 24) >> 24;
        if (divisor === 0) { lastBailOp = b; iter = maxInsn; break; }
        const ax = (gr16(0) << 16) >> 16; // sign-extend AX
        const quot = (ax / divisor) | 0;
        if (quot > 127 || quot < -128) { lastBailOp = b; iter = maxInsn; break; }
        const rem = (ax % divisor) | 0;
        sr8(0, quot & 0xFF); sr8(4, rem & 0xFF);
      } else {
        lastBailOp = b; iter = maxInsn; break;
      }
      break;
    }

    case 0xF7: {
      const modrm = mem8[ci+1]; ilen += 2;
      const op = (modrm >> 3) & 7;
      if (op === 2) { // NOT r/m16/32
        if ((modrm >> 6) === 3) {
          if (opSize===2) sr16(modrm&7, ~gr16(modrm&7) & 0xFFFF);
          else sr32(modrm&7, ~gr32(modrm&7) >>> 0);
        } else {
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                   : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
          ilen += m.len;
          if (opSize===2) ww(m.ea, ~rw(m.ea) & 0xFFFF);
          else wd(m.ea, ~rd(m.ea) >>> 0);
        }
      } else if (op === 3) { // NEG r/m16/32
        if ((modrm >> 6) === 3) {
          if (opSize===2) {
            const v = gr16(modrm&7), r = (-v) & 0xFFFF;
            sr16(modrm&7, r); setFlagsArith(OP_SUB,r,0,v,2); lazyCF = v?1:0;
          } else {
            const v = gr32(modrm&7), r = (-v) >>> 0;
            sr32(modrm&7, r); setFlagsArith(OP_SUB,r,0,v,4); lazyCF = v?1:0;
          }
        } else {
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                   : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
          ilen += m.len;
          if (opSize===2) {
            const v = rw(m.ea), r = (-v) & 0xFFFF;
            ww(m.ea, r); setFlagsArith(OP_SUB,r,0,v,2); lazyCF = v?1:0;
          } else {
            const v = rd(m.ea), r = (-v) >>> 0;
            wd(m.ea, r); setFlagsArith(OP_SUB,r,0,v,4); lazyCF = v?1:0;
          }
        }
      } else if (op === 0) { // TEST r/m16/32, imm
        if ((modrm >> 6) === 3) {
          if (opSize===2) { alu16(4, gr16(modrm&7), mem8[ci+2]|(mem8[ci+3]<<8)); ilen += 2; }
          else { alu32(4, gr32(modrm&7), mem8[ci+2]|(mem8[ci+3]<<8)|(mem8[ci+4]<<16)|(mem8[ci+5]<<24)); ilen += 4; }
        } else {
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                   : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
          ilen += m.len;
          const off = ci+2+m.len;
          if (opSize===2) { alu16(4, rw(m.ea), mem8[off]|(mem8[off+1]<<8)); ilen += 2; }
          else { alu32(4, rd(m.ea), mem8[off]|(mem8[off+1]<<8)|(mem8[off+2]<<16)|(mem8[off+3]<<24)); ilen += 4; }
        }
      } else if (op === 4) { // MUL r/m16/32
        let val;
        if ((modrm >> 6) === 3) { val = opSize===2 ? gr16(modrm&7) : gr32(modrm&7); }
        else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS) : decodeModRM32(modrm, mem8, ci+2, effDS, effSS); ilen += m.len; val = opSize===2 ? rw(m.ea) : rd(m.ea); }
        if (opSize === 2) {
          const result = (gr16(0) & 0xFFFF) * (val & 0xFFFF);
          sr16(0, result & 0xFFFF); // AX
          sr16(2, (result >>> 16) & 0xFFFF); // DX
          lazyCF = (result & 0xFFFF0000) ? 1 : 0;
        } else {
          // 32-bit MUL: EDX:EAX = EAX * r/m32
          const a = gr32(0) >>> 0, b2 = val >>> 0;
          const result = BigInt(a) * BigInt(b2);
          sr32(0, Number(result & 0xFFFFFFFFn)); // EAX
          sr32(2, Number((result >> 32n) & 0xFFFFFFFFn)); // EDX
          lazyCF = (result >> 32n) ? 1 : 0;
        }
        lazyOp = OP_EXPLICIT; lazyExplicitFlags = lazyCF ? (0x801 | 0x02) : 0x02;
      } else if (op === 5) { // IMUL r/m16/32
        let val;
        if ((modrm >> 6) === 3) { val = opSize===2 ? gr16(modrm&7) : gr32(modrm&7); }
        else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS) : decodeModRM32(modrm, mem8, ci+2, effDS, effSS); ilen += m.len; val = opSize===2 ? rw(m.ea) : rd(m.ea); }
        if (opSize === 2) {
          const a = (gr16(0) << 16) >> 16, b2 = (val << 16) >> 16;
          const result = a * b2;
          sr16(0, result & 0xFFFF);
          sr16(2, (result >> 16) & 0xFFFF);
          lazyCF = (result !== ((result << 16) >> 16)) ? 1 : 0;
        } else {
          const a = gr32(0) | 0, b2 = val | 0;
          const result = BigInt(a) * BigInt(b2);
          sr32(0, Number(result & 0xFFFFFFFFn));
          sr32(2, Number((result >> 32n) & 0xFFFFFFFFn));
          lazyCF = (result !== BigInt(Number(result & 0xFFFFFFFFn) | 0)) ? 1 : 0;
        }
        lazyOp = OP_EXPLICIT; lazyExplicitFlags = lazyCF ? (0x801 | 0x02) : 0x02;
      } else if (op === 6) { // DIV r/m16/32
        let val;
        if ((modrm >> 6) === 3) { val = opSize===2 ? gr16(modrm&7) : gr32(modrm&7); }
        else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS) : decodeModRM32(modrm, mem8, ci+2, effDS, effSS); ilen += m.len; val = opSize===2 ? rw(m.ea) : rd(m.ea); }
        if (val === 0) { lastBailOp = b; iter = maxInsn; break; }
        if (opSize === 2) {
          const dividend = ((gr16(2) & 0xFFFF) << 16) | (gr16(0) & 0xFFFF);
          const quot = (dividend / val) >>> 0;
          if (quot > 0xFFFF) { lastBailOp = b; iter = maxInsn; break; }
          sr16(0, quot & 0xFFFF);
          sr16(2, (dividend % val) & 0xFFFF);
        } else {
          const dividend = (BigInt(gr32(2) >>> 0) << 32n) | BigInt(gr32(0) >>> 0);
          const divisor = BigInt(val >>> 0);
          const quot = dividend / divisor;
          if (quot > 0xFFFFFFFFn) { lastBailOp = b; iter = maxInsn; break; }
          sr32(0, Number(quot & 0xFFFFFFFFn));
          sr32(2, Number((dividend % divisor) & 0xFFFFFFFFn));
        }
      } else if (op === 7) { // IDIV r/m16/32
        let val;
        if ((modrm >> 6) === 3) { val = opSize===2 ? gr16(modrm&7) : gr32(modrm&7); }
        else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS) : decodeModRM32(modrm, mem8, ci+2, effDS, effSS); ilen += m.len; val = opSize===2 ? rw(m.ea) : rd(m.ea); }
        if (opSize === 2) {
          const divisor = (val << 16) >> 16;
          if (divisor === 0) { lastBailOp = b; iter = maxInsn; break; }
          const dividend = ((gr16(2) << 16) | (gr16(0) & 0xFFFF));
          const quot = (dividend / divisor) | 0;
          if (quot > 32767 || quot < -32768) { lastBailOp = b; iter = maxInsn; break; }
          sr16(0, quot & 0xFFFF);
          sr16(2, (dividend % divisor) & 0xFFFF);
        } else {
          const divisor = val | 0;
          if (divisor === 0) { lastBailOp = b; iter = maxInsn; break; }
          const dividend = (BigInt(gr32(2) | 0) << 32n) | BigInt(gr32(0) >>> 0);
          const quot = dividend / BigInt(divisor);
          if (quot > 0x7FFFFFFFn || quot < -0x80000000n) { lastBailOp = b; iter = maxInsn; break; }
          sr32(0, Number(quot & 0xFFFFFFFFn));
          sr32(2, Number((dividend % BigInt(divisor)) & 0xFFFFFFFFn));
        }
      } else {
        lastBailOp = b; iter = maxInsn; break;
      }
      break;
    }

    // ──── SHL/SHR/SAR/ROL/ROR (0xD0, 0xD1, 0xD2, 0xD3, 0xC0, 0xC1) ────
    case 0xD0: case 0xD1: case 0xC0: case 0xC1: case 0xD2: case 0xD3: {
      const modrm = mem8[ci+1]; ilen += 2;
      const shOp = (modrm >> 3) & 7;
      const isWord = (b & 1); // 0=byte, 1=word/dword
      const sz = isWord ? opSize : 1;
      let count;
      if (b === 0xD0 || b === 0xD1) count = 1;
      else if (b === 0xC0 || b === 0xC1) { count = mem8[ci+2] & 0x1F; ilen += 1; }
      else count = gr8(1) & 0x1F; // CL

      // Handle shift only for SHL(4), SHR(5), SAR(7)
      if (count === 0) break;

      let val;
      let isMem = (modrm >> 6) !== 3;
      let mea = 0, mlen = 0;
      if (isMem) {
        // Need to recalculate ilen for memory operand before count byte
        if (b === 0xC0 || b === 0xC1) ilen -= 1; // undo count byte
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        mea = m.ea; mlen = m.len;
        ilen += mlen;
        if (b === 0xC0 || b === 0xC1) {
          count = mem8[ci + 2 + mlen] & 0x1F;
          ilen += 1;
        }
        if (sz === 1) val = rb(mea);
        else if (sz === 2) val = rw(mea);
        else val = rd(mea);
      } else {
        const rm = modrm & 7;
        if (sz === 1) val = gr8(rm);
        else if (sz === 2) val = gr16(rm);
        else val = gr32(rm);
      }

      if (count === 0) break;

      let res;
      const mask = SIZE_MASK[sz];
      switch (shOp) {
        case 4: // SHL
          lazyCF = ((val >> (sz * 8 - count)) & 1);
          res = (val << count) & mask;
          setFlagsArith(OP_SHL, res, val, count, sz);
          break;
        case 5: // SHR
          lazyCF = ((val >> (count - 1)) & 1);
          res = (val >>> count) & mask;
          setFlagsArith(OP_SHR, res, val, count, sz);
          break;
        case 7: { // SAR
          lazyCF = ((val >> (count - 1)) & 1);
          let sv = val;
          if (sv & SIZE_SIGN[sz]) sv |= ~mask; // sign extend
          res = (sv >> count) & mask;
          setFlagsArith(OP_SAR, res, val, count, sz);
          break;
        }
        case 0: { // ROL
          const bc = sz * 8;
          res = ((val << (count % bc)) | (val >>> (bc - count % bc))) & mask;
          lazyCF = res & 1;
          lazyOp = OP_NONE;
          break;
        }
        case 1: { // ROR
          const bc = sz * 8;
          res = ((val >>> (count % bc)) | (val << (bc - count % bc))) & mask;
          lazyCF = (res >>> (bc - 1)) & 1;
          lazyOp = OP_NONE;
          break;
        }
        case 2: { // RCL — rotate left through CF
          const bc = sz * 8;
          const cf = getCF();
          const cnt = count % (bc + 1); // effective count mod (bits+1)
          if (cnt === 0) break;
          // Concatenate CF:val as (bc+1)-bit value, rotate left by cnt
          // New CF = bit (bc - cnt) of original val (or original CF if cnt == bc)
          const newCF = cnt < bc ? ((val >> (bc - cnt)) & 1) : cf;
          // Result: top (cnt-1) bits come from low bits of val, bottom (bc-cnt) bits
          // come from original val shifted left, + original CF shifted in at bit (bc-cnt)
          if (cnt === 1) {
            res = ((val << 1) | cf) & mask;
          } else {
            // General case: bits [bc-1 : bc-cnt+1] = val[cnt-2:0], bit[bc-cnt] = cf, bits[bc-cnt-1:0] = val[bc-1:cnt]
            res = (((val << cnt) | (cf << (cnt - 1)) | (val >>> (bc - cnt + 1))) & mask) >>> 0;
          }
          lazyCF = newCF;
          lazyOp = OP_NONE;
          break;
        }
        case 3: { // RCR — rotate right through CF
          const bc = sz * 8;
          const cf = getCF();
          const cnt = count % (bc + 1);
          if (cnt === 0) break;
          const newCF = cnt === 1 ? (val & 1) : ((val >> (cnt - 1)) & 1);
          if (cnt === 1) {
            res = ((cf << (bc - 1)) | (val >>> 1)) & mask;
          } else {
            res = (((val >>> cnt) | (cf << (bc - cnt)) | (val << (bc - cnt + 1))) & mask) >>> 0;
          }
          lazyCF = newCF;
          lazyOp = OP_NONE;
          break;
        }
        default:
          lastBailOp = b; iter = maxInsn; break; // unknown shift op
      }

      if (isMem) {
        if (sz === 1) wb(mea, res);
        else if (sz === 2) ww(mea, res);
        else wd(mea, res);
      } else {
        const rm = modrm & 7;
        if (sz === 1) sr8(rm, res);
        else if (sz === 2) sr16(rm, res);
        else sr32(rm, res);
      }
      break;
    }

    // ──── INC/DEC r/m8 (0xFE) ────
    case 0xFE: {
      const modrm = mem8[ci+1]; ilen += 2;
      const op = (modrm >> 3) & 7;
      if (op > 1) { lastBailOp = b; iter = maxInsn; break; }
      const oldCF = getCF();
      if ((modrm >> 6) === 3) {
        const rm = modrm & 7;
        let v = gr8(rm);
        if (op === 0) { v = (v+1)&0xFF; setFlagsArith(OP_INC,v,v-1,1,1); }
        else { v = (v-1)&0xFF; setFlagsArith(OP_DEC,v,v+1,1,1); }
        sr8(rm, v);
      } else {
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        let v = rb(m.ea);
        if (op === 0) { v = (v+1)&0xFF; setFlagsArith(OP_INC,v,v-1,1,1); }
        else { v = (v-1)&0xFF; setFlagsArith(OP_DEC,v,v+1,1,1); }
        wb(m.ea, v);
      }
      lazyCF = oldCF;
      break;
    }

    // ──── INC/DEC/CALL/JMP/PUSH r/m16/32 (0xFF) ────
    case 0xFF: {
      const modrm = mem8[ci+1]; ilen += 2;
      const op = (modrm >> 3) & 7;

      if (op === 0 || op === 1) { // INC / DEC
        const oldCF = getCF();
        if ((modrm >> 6) === 3) {
          const rm = modrm & 7;
          if (opSize === 2) {
            let v = gr16(rm);
            if (op===0) { v=(v+1)&0xFFFF; setFlagsArith(OP_INC,v,v-1,1,2); }
            else { v=(v-1)&0xFFFF; setFlagsArith(OP_DEC,v,v+1,1,2); }
            sr16(rm, v);
          } else {
            let v = gr32(rm);
            if (op===0) { v=(v+1)>>>0; setFlagsArith(OP_INC,v,v-1,1,4); }
            else { v=(v-1)>>>0; setFlagsArith(OP_DEC,v,v+1,1,4); }
            sr32(rm, v);
          }
        } else {
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                   : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
          ilen += m.len;
          if (opSize === 2) {
            let v = rw(m.ea);
            if (op===0) { v=(v+1)&0xFFFF; setFlagsArith(OP_INC,v,v-1,1,2); }
            else { v=(v-1)&0xFFFF; setFlagsArith(OP_DEC,v,v+1,1,2); }
            ww(m.ea, v);
          } else {
            let v = rd(m.ea);
            if (op===0) { v=(v+1)>>>0; setFlagsArith(OP_INC,v,v-1,1,4); }
            else { v=(v-1)>>>0; setFlagsArith(OP_DEC,v,v+1,1,4); }
            wd(m.ea, v);
          }
        }
        lazyCF = oldCF;
      } else if (op === 2) { // CALL r/m16/32 (indirect)
        let target;
        if ((modrm >> 6) === 3) {
          target = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
        } else {
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                   : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
          ilen += m.len;
          target = opSize === 2 ? rw(m.ea) : rd(m.ea);
        }
        if (opSize === 2) push16((ip + ilen) & 0xFFFF, ssBase);
        else push32((ip + ilen) & 0xFFFFFFFF, ssBase);
        ip = target & ipMask;
        ilen = 0; executed++; wrIP(ip); continue;
      } else if (op === 4) { // JMP r/m16/32 (indirect)
        let target;
        if ((modrm >> 6) === 3) {
          target = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
        } else {
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                   : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
          ilen += m.len;
          target = opSize === 2 ? rw(m.ea) : rd(m.ea);
        }
        ip = target & ipMask;
        ilen = 0; executed++; wrIP(ip); continue;
      } else if (op === 6) { // PUSH r/m16/32
        let val;
        if ((modrm >> 6) === 3) {
          val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
        } else {
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                   : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
          ilen += m.len;
          val = opSize === 2 ? rw(m.ea) : rd(m.ea);
        }
        if (opSize === 2) push16(val, ssBase);
        else push32(val, ssBase);
      } else if (op === 3 || op === 5) { // CALL FAR / JMP FAR m16:16 (indirect)
        if (!realMode) { lastBailOp = 0xFF00 | op; iter = maxInsn; break; }
        // Only memory form (modrm /3 and /5 can't be register for far ops)
        if ((modrm >> 6) === 3) { lastBailOp = 0xFF00 | op; iter = maxInsn; break; }
        const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS)
                                 : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
        ilen += m.len;
        // Read target [offset16, cs16] from memory
        const newIP2 = rw(m.ea);
        const newCS2 = rw(m.ea + 2);
        if (op === 3) { // CALL FAR
          push16(csBase >>> 4, ssBase);
          push16((ip + ilen) & 0xFFFF, ssBase);
        }
        ip = newIP2 & ipMask;
        csBase = newCS2 << 4;
        wr16(S_CS + SEG_SEL, newCS2);
        wr64(S_CS + SEG_BASE, csBase);
        ilen = 0; executed++; wrIP(ip); continue;
      } else {
        // Undefined /7 — fallback
        lastBailOp = 0xFF00 | op; iter = maxInsn; break;
      }
      break;
    }

    // ──── 0x0F two-byte opcodes ────
    case 0x0F: {
      const b2 = mem8[ci+1]; ilen += 2;
      switch (b2) {
        // CMOVcc r16/32, r/m16/32 (0x0F 0x40-0x4F)
        case 0x40:case 0x41:case 0x42:case 0x43:case 0x44:case 0x45:case 0x46:case 0x47:
        case 0x48:case 0x49:case 0x4A:case 0x4B:case 0x4C:case 0x4D:case 0x4E:case 0x4F: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          if (testCC(b2 - 0x40)) {
            let val;
            if ((modrm >> 6) === 3) { val = opSize===2 ? gr16(modrm&7) : gr32(modrm&7); }
            else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+3, effDS, effSS)
                                       : decodeModRM32(modrm, mem8, ci+3, effDS, effSS);
              ilen += m.len;
              val = opSize===2 ? rw(m.ea) : rd(m.ea);
            }
            if (opSize===2) sr16(reg, val); else sr32(reg, val);
          } else {
            // Condition false: skip operand decode but still advance ilen
            if ((modrm >> 6) !== 3) {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+3, effDS, effSS)
                                       : decodeModRM32(modrm, mem8, ci+3, effDS, effSS);
              ilen += m.len;
            }
          }
          break;
        }

        // Jcc rel16/32 (0x0F 0x80-0x8F)
        case 0x80:case 0x81:case 0x82:case 0x83:case 0x84:case 0x85:case 0x86:case 0x87:
        case 0x88:case 0x89:case 0x8A:case 0x8B:case 0x8C:case 0x8D:case 0x8E:case 0x8F: {
          let rel;
          if (opSize === 2) {
            rel = mem8[ci+2] | (mem8[ci+3] << 8);
            if (rel > 0x7FFF) rel -= 0x10000;
            ilen += 2;
          } else {
            rel = mem8[ci+2]|(mem8[ci+3]<<8)|(mem8[ci+4]<<16)|(mem8[ci+5]<<24);
            ilen += 4;
          }
          if (testCC(b2 - 0x80)) {
            ip = (ip + ilen + rel) & ipMask;
            ilen = 0; executed++; wrIP(ip); continue;
          }
          break;
        }

        // CMPXCHG r/m8, r8 (0x0F 0xB0)
        case 0xB0: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          let dst, mea8 = -1;
          if ((modrm >> 6) === 3) dst = gr8(modrm&7);
          else { const m = addrSize===2 ? decodeModRM16(modrm,mem8,ci+3,effDS,effSS) : decodeModRM32(modrm,mem8,ci+3,effDS,effSS); ilen += m.len; mea8 = m.ea; dst = rb(m.ea); }
          const al = gr8(0);
          setFlagsArith(OP_SUB, (al-dst)&0xFF, al, dst, 1);
          if (al === dst) {
            if ((modrm>>6)===3) sr8(modrm&7, gr8(reg));
            else wb(mea8, gr8(reg));
          } else sr8(0, dst);
          break;
        }

        // CMPXCHG r/m16/32, r16/32 (0x0F 0xB1)
        case 0xB1: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          let dst;
          let mea2 = -1;
          if ((modrm >> 6) === 3) dst = opSize===2 ? gr16(modrm&7) : gr32(modrm&7);
          else { const m = addrSize===2 ? decodeModRM16(modrm,mem8,ci+3,effDS,effSS) : decodeModRM32(modrm,mem8,ci+3,effDS,effSS); ilen += m.len; mea2 = m.ea; dst = opSize===2 ? rw(m.ea) : rd(m.ea); }
          const ax = opSize===2 ? gr16(0) : gr32(0);
          if (opSize===2) setFlagsArith(OP_SUB, (ax-dst)&0xFFFF, ax, dst, 2);
          else setFlagsArith(OP_SUB, (ax-dst)>>>0, ax, dst, 4);
          if (ax === dst) {
            const src = opSize===2 ? gr16(reg) : gr32(reg);
            if ((modrm>>6)===3) { if (opSize===2) sr16(modrm&7, src); else sr32(modrm&7, src); }
            else { if (opSize===2) ww(mea2, src); else wd(mea2, src); }
          } else { if (opSize===2) sr16(0, dst); else sr32(0, dst); }
          break;
        }

        // MOVZX r16/32, r/m8 (0x0F 0xB6)
        case 0xB6: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) val = gr8(modrm & 7);
          else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+3, effDS, effSS)
                                     : decodeModRM32(modrm, mem8, ci+3, effDS, effSS);
            ilen += m.len;
            val = rb(m.ea);
          }
          if (opSize === 2) sr16(reg, val);
          else sr32(reg, val);
          break;
        }

        // MOVZX r16/32, r/m16 (0x0F 0xB7)
        case 0xB7: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) val = gr16(modrm & 7);
          else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+3, effDS, effSS)
                                     : decodeModRM32(modrm, mem8, ci+3, effDS, effSS);
            ilen += m.len;
            val = rw(m.ea);
          }
          sr32(reg, val); // zero-extend to 32
          break;
        }

        // MOVSX r16/32, r/m8 (0x0F 0xBE)
        case 0xBE: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) val = gr8(modrm & 7);
          else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+3, effDS, effSS)
                                     : decodeModRM32(modrm, mem8, ci+3, effDS, effSS);
            ilen += m.len;
            val = rb(m.ea);
          }
          if (val > 127) val |= (opSize === 2 ? 0xFF00 : 0xFFFFFF00);
          if (opSize === 2) sr16(reg, val & 0xFFFF);
          else sr32(reg, val >>> 0);
          break;
        }

        // MOVSX r32, r/m16 (0x0F 0xBF)
        case 0xBF: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) val = gr16(modrm & 7);
          else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+3, effDS, effSS)
                                     : decodeModRM32(modrm, mem8, ci+3, effDS, effSS);
            ilen += m.len;
            val = rw(m.ea);
          }
          if (val > 0x7FFF) val |= 0xFFFF0000;
          sr32(reg, val >>> 0);
          break;
        }

        // SETcc r/m8 (0x0F 0x90-0x9F)
        case 0x90:case 0x91:case 0x92:case 0x93:case 0x94:case 0x95:case 0x96:case 0x97:
        case 0x98:case 0x99:case 0x9A:case 0x9B:case 0x9C:case 0x9D:case 0x9E:case 0x9F: {
          const modrm = mem8[ci+2]; ilen += 1;
          const val = testCC(b2 - 0x90) ? 1 : 0;
          if ((modrm >> 6) === 3) sr8(modrm & 7, val);
          else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+3, effDS, effSS)
                                     : decodeModRM32(modrm, mem8, ci+3, effDS, effSS);
            ilen += m.len;
            wb(m.ea, val);
          }
          break;
        }

        // IMUL r16/32, r/m16/32 (0x0F 0xAF)
        case 0xAF: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) { val = opSize===2 ? gr16(modrm&7) : gr32(modrm&7); }
          else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+3, effDS, effSS) : decodeModRM32(modrm, mem8, ci+3, effDS, effSS); ilen += m.len; val = opSize===2 ? rw(m.ea) : rd(m.ea); }
          if (opSize === 2) {
            const a = (gr16(reg) << 16) >> 16, b2 = (val << 16) >> 16;
            const result = a * b2;
            sr16(reg, result & 0xFFFF);
            lazyCF = (result !== ((result << 16) >> 16)) ? 1 : 0;
          } else {
            const a32 = gr32(reg) | 0;
            const result = Math.imul(a32, val);
            sr32(reg, result >>> 0);
            // OF/CF set if result doesn't fit in 32 bits (requires 64-bit product)
            const big = BigInt(a32) * BigInt(val | 0);
            lazyCF = (big !== BigInt(result | 0)) ? 1 : 0;
          }
          // OF=CF=overflow; ZF/SF/PF/AF undefined. Use OP_EXPLICIT for correct OF.
          lazyOp = OP_EXPLICIT;
          lazyExplicitFlags = lazyCF ? (0x801 | 0x02) : 0x02; // CF(0) and OF(11) = overflow
          break;
        }

        // BSF r16/32, r/m16/32 (0x0F 0xBC)
        case 0xBC: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) { val = opSize===2 ? gr16(modrm&7) : gr32(modrm&7); }
          else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+3, effDS, effSS) : decodeModRM32(modrm, mem8, ci+3, effDS, effSS); ilen += m.len; val = opSize===2 ? rw(m.ea) : rd(m.ea); }
          if (val === 0) { lazyOp = OP_NONE; lazyRes = 0; lazySize = opSize; } // ZF=1
          else {
            let bit = 0;
            while (!(val & (1 << bit))) bit++;
            if (opSize===2) sr16(reg, bit); else sr32(reg, bit);
            lazyOp = OP_NONE; lazyRes = 1; lazySize = opSize; // ZF=0
          }
          break;
        }

        // BSR r16/32, r/m16/32 (0x0F 0xBD)
        case 0xBD: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) { val = opSize===2 ? gr16(modrm&7) : gr32(modrm&7); }
          else { const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+3, effDS, effSS) : decodeModRM32(modrm, mem8, ci+3, effDS, effSS); ilen += m.len; val = opSize===2 ? rw(m.ea) : rd(m.ea); }
          if (val === 0) { lazyOp = OP_NONE; lazyRes = 0; lazySize = opSize; }
          else {
            let bit = opSize === 2 ? 15 : 31;
            while (!(val & (1 << bit))) bit--;
            if (opSize===2) sr16(reg, bit); else sr32(reg, bit);
            lazyOp = OP_NONE; lazyRes = 1; lazySize = opSize;
          }
          break;
        }

        // BT/BTS/BTR/BTC r/m16/32, r16/32 (0x0F 0xA3/0xAB/0xB3/0xBB)
        case 0xA3: case 0xAB: case 0xB3: case 0xBB: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          const bitIdx = (opSize===2 ? gr16(reg) : gr32(reg)) & (opSize===2 ? 15 : 31);
          let val;
          if ((modrm >> 6) === 3) {
            val = opSize===2 ? gr16(modrm&7) : gr32(modrm&7);
            lazyCF = (val >> bitIdx) & 1;
            if (b2 === 0xAB) val |= (1 << bitIdx);       // BTS
            else if (b2 === 0xB3) val &= ~(1 << bitIdx);  // BTR
            else if (b2 === 0xBB) val ^= (1 << bitIdx);   // BTC
            if (b2 !== 0xA3) { if (opSize===2) sr16(modrm&7, val & 0xFFFF); else sr32(modrm&7, val >>> 0); }
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+3, effDS, effSS)
                                     : decodeModRM32(modrm, mem8, ci+3, effDS, effSS);
            ilen += m.len;
            val = opSize===2 ? rw(m.ea) : rd(m.ea);
            lazyCF = (val >> bitIdx) & 1;
            if (b2 === 0xAB) val |= (1 << bitIdx);
            else if (b2 === 0xB3) val &= ~(1 << bitIdx);
            else if (b2 === 0xBB) val ^= (1 << bitIdx);
            if (b2 !== 0xA3) { if (opSize===2) ww(m.ea, val & 0xFFFF); else wd(m.ea, val >>> 0); }
          }
          lazyOp = OP_NONE;
          break;
        }

        // BT/BTS/BTR/BTC r/m, imm8 (0x0F 0xBA)
        case 0xBA: {
          const modrm = mem8[ci+2]; ilen += 1;
          const btOp = (modrm >> 3) & 7;
          if (btOp < 4) { lastBailOp = 0x0F00 | b2; iter = maxInsn; break; }
          let val, bitIdx;
          if ((modrm >> 6) === 3) {
            val = opSize===2 ? gr16(modrm&7) : gr32(modrm&7);
            bitIdx = mem8[ci+3] & (opSize===2 ? 15 : 31); ilen += 1;
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+3, effDS, effSS) : decodeModRM32(modrm, mem8, ci+3, effDS, effSS);
            ilen += m.len;
            val = opSize===2 ? rw(m.ea) : rd(m.ea);
            bitIdx = mem8[ci+3+m.len] & (opSize===2 ? 15 : 31); ilen += 1;
          }
          lazyCF = (val >> bitIdx) & 1; lazyOp = OP_NONE;
          if (btOp === 5) val |= (1 << bitIdx); // BTS
          else if (btOp === 6) val &= ~(1 << bitIdx); // BTR
          else if (btOp === 7) val ^= (1 << bitIdx); // BTC
          // btOp === 4 is BT (no modification)
          if (btOp !== 4) {
            if ((modrm >> 6) === 3) { if (opSize===2) sr16(modrm&7, val & 0xFFFF); else sr32(modrm&7, val >>> 0); }
            // memory case already handled above
          }
          break;
        }

        // XADD r/m8, r8 (0x0F 0xC0)
        case 0xC0: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          let dst, meaXA = -1;
          if ((modrm >> 6) === 3) dst = gr8(modrm&7);
          else { const m = addrSize===2 ? decodeModRM16(modrm,mem8,ci+3,effDS,effSS) : decodeModRM32(modrm,mem8,ci+3,effDS,effSS); ilen += m.len; meaXA = m.ea; dst = rb(m.ea); }
          const src = gr8(reg);
          const sum = (dst + src) & 0xFF;
          setFlagsArith(OP_ADD, sum, dst, src, 1);
          sr8(reg, dst); // old dst → reg
          if ((modrm>>6)===3) sr8(modrm&7, sum); else wb(meaXA, sum);
          break;
        }

        // XADD r/m16/32, r16/32 (0x0F 0xC1)
        case 0xC1: {
          const modrm = mem8[ci+2]; ilen += 1;
          const reg = (modrm >> 3) & 7;
          let dst, mea3 = -1;
          if ((modrm >> 6) === 3) dst = opSize===2 ? gr16(modrm&7) : gr32(modrm&7);
          else { const m = addrSize===2 ? decodeModRM16(modrm,mem8,ci+3,effDS,effSS) : decodeModRM32(modrm,mem8,ci+3,effDS,effSS); ilen += m.len; mea3 = m.ea; dst = opSize===2 ? rw(m.ea) : rd(m.ea); }
          const src = opSize===2 ? gr16(reg) : gr32(reg);
          const sum = opSize===2 ? (dst + src) & 0xFFFF : (dst + src) >>> 0;
          setFlagsArith(OP_ADD, sum, dst, src, opSize);
          if (opSize===2) sr16(reg, dst); else sr32(reg, dst);
          if ((modrm>>6)===3) { if (opSize===2) sr16(modrm&7, sum); else sr32(modrm&7, sum); }
          else { if (opSize===2) ww(mea3, sum); else wd(mea3, sum); }
          break;
        }

        // PUSH FS (0x0F 0xA0) / POP FS (0x0F 0xA1) / PUSH GS (0x0F 0xA8) / POP GS (0x0F 0xA9)
        // In 32-bit PM (opSize=4), selector is zero-extended to 32 bits
        case 0xA0: { const s=rr16(S_FS+SEG_SEL); if(opSize===4) push32(s,ssBase); else push16(s,ssBase); break; }
        case 0xA1: {
          if (!realMode) { lastBailOp = 0x0F00 | b2; iter = maxInsn; break; }
          const s = pop16(ssBase); wr16(S_FS + SEG_SEL, s); wr64(S_FS + SEG_BASE, s << 4); break;
        }
        case 0xA8: { const s=rr16(S_GS+SEG_SEL); if(opSize===4) push32(s,ssBase); else push16(s,ssBase); break; }
        case 0xA9: {
          if (!realMode) { lastBailOp = 0x0F00 | b2; iter = maxInsn; break; }
          const s = pop16(ssBase); wr16(S_GS + SEG_SEL, s); wr64(S_GS + SEG_BASE, s << 4); break;
        }

        // ──── CPUID (0x0F 0xA2) ────
        case 0xA2: {
          // CS-based LM injection: the kernel setup code runs at CS >= 0x1000
          // (typically CS=0x1020). BIOS runs at CS=F000, ISOLINUX at CS < 0x1000.
          // Inject LM/NX/SYSCALL only for the kernel, not for BIOS/ISOLINUX.
          const cpuidCS = rr16(S_CS + SEG_SEL);
          const leaf = rr32(R_AX) >>> 0;
          if (cpuidCS >= 0x1000 && cpuidCS < 0xF000 &&
              (leaf === 0x80000000 || leaf === 0x80000001)) {
            if (leaf === 0x80000000) {
              wr32(R_AX, 0x80000008);
              wr32(R_BX, 0); wr32(R_CX, 0); wr32(R_DX, 0);
            } else {
              // leaf 0x80000001: inject LM, NX, SYSCALL, RDTSCP
              wr32(R_AX, 0x00000000);
              wr32(R_BX, 0x00000000);
              wr32(R_CX, 0x00000001); // LAHF/SAHF
              wr32(R_DX, (1 << 29) | (1 << 20) | (1 << 11) | (1 << 27));
              // Set CR2 magic so CPUMGetGuestCpuId also injects LM in PM
              if (!_directBootDone) {
                _directBootDone = true;
                wr32(R_CR2, 0xC0DEBA5E);
                console.log('[CPUID] LM injected for kernel at CS=0x' +
                  cpuidCS.toString(16) + ' — CR2 magic set');
              }
            }
          } else {
            // BIOS/ISOLINUX or non-extended leaf: bail to IEM
            lastBailOp = 0x0F00 | b2; iter = maxInsn; break;
          }
          break;
        }

        default:
          // Unsupported 0x0F opcode — fallback
          lastBailOp = 0x0F00 | b2; iter = maxInsn;
          break;
      }
      break;
    }

    // ──── JMP far (0xEA) ────
    case 0xEA: {
      if (!realMode) { lastBailOp = b; iter = maxInsn; break; } // protected mode needs GDT lookup
      if (opSize === 2) {
        const newIP = mem8[ci+1] | (mem8[ci+2] << 8);
        const newCS = mem8[ci+3] | (mem8[ci+4] << 8);
        ilen += 5;
        wr16(S_CS + SEG_SEL, newCS);
        csBase = newCS << 4; wr64(S_CS + SEG_BASE, csBase);
        ip = newIP;
        ilen = 0; executed++; wrIP(ip); continue;
      } else {
        lastBailOp = b; iter = maxInsn; break; // 32-bit far jump — complex
      }
    }

    // ──── CALL far (0x9A) ────
    case 0x9A: {
      if (!realMode) { lastBailOp = b; iter = maxInsn; break; } // protected mode needs GDT lookup
      if (opSize === 2) {
        const newIP = mem8[ci+1] | (mem8[ci+2] << 8);
        const newCS = mem8[ci+3] | (mem8[ci+4] << 8);
        ilen += 5;
        push16(rr16(S_CS + SEG_SEL), ssBase); // push CS
        push16((ip + ilen) & 0xFFFF, ssBase); // push IP
        wr16(S_CS + SEG_SEL, newCS);
        csBase = newCS << 4; wr64(S_CS + SEG_BASE, csBase);
        ip = newIP;
        ilen = 0; executed++; wrIP(ip); continue;
      } else {
        lastBailOp = b; iter = maxInsn; break;
      }
    }

    // ──── RETF imm16 (0xCA) — far return, pop N extra bytes ────
    case 0xCA: {
      if (!realMode) { lastBailOp = b; iter = maxInsn; break; }
      const retfImm = mem8[ci+1] | (mem8[ci+2] << 8);
      const retfIP = pop16(ssBase);
      const retfCS = pop16(ssBase);
      csBase = retfCS << 4;
      wr16(S_CS + SEG_SEL, retfCS); wr64(S_CS + SEG_BASE, csBase);
      // Pop retfImm extra bytes from stack
      const sp = gr16(4);
      sr16(4, (sp + retfImm) & 0xFFFF);
      ip = retfIP;
      ilen = 0; executed++; wrIP(ip); continue;
    }

    // ──── RETF (0xCB) ────
    case 0xCB: {
      // In protected mode, RETF requires GDT lookup for CS — bail to IEM.
      if (!realMode) { lastBailOp = b; iter = maxInsn; break; }
      if (opSize === 2) {
        const newIP = pop16(ssBase);
        const newCS = pop16(ssBase);
        csBase = newCS << 4;
        wr16(S_CS + SEG_SEL, newCS); wr64(S_CS + SEG_BASE, csBase);
        ip = newIP;
        ilen = 0; executed++; wrIP(ip); continue;
      } else {
        lastBailOp = b; iter = maxInsn; break;
      }
    }

    // ──── PUSHA (0x60) ────
    case 0x60: {
      const sp0 = gr16(4);
      if (opSize === 2) {
        push16(gr16(0), ssBase); push16(gr16(1), ssBase); push16(gr16(2), ssBase); push16(gr16(3), ssBase);
        push16(sp0, ssBase); push16(gr16(5), ssBase); push16(gr16(6), ssBase); push16(gr16(7), ssBase);
      } else {
        const esp0 = gr32(4);
        push32(gr32(0), ssBase); push32(gr32(1), ssBase); push32(gr32(2), ssBase); push32(gr32(3), ssBase);
        push32(esp0, ssBase); push32(gr32(5), ssBase); push32(gr32(6), ssBase); push32(gr32(7), ssBase);
      }
      ilen += 1;
      break;
    }

    // ──── POPA (0x61) ────
    case 0x61:
      if (opSize === 2) {
        sr16(7, pop16(ssBase)); sr16(6, pop16(ssBase)); sr16(5, pop16(ssBase));
        pop16(ssBase); // skip SP
        sr16(3, pop16(ssBase)); sr16(2, pop16(ssBase)); sr16(1, pop16(ssBase)); sr16(0, pop16(ssBase));
      } else {
        sr32(7, pop32(ssBase)); sr32(6, pop32(ssBase)); sr32(5, pop32(ssBase));
        pop32(ssBase);
        sr32(3, pop32(ssBase)); sr32(2, pop32(ssBase)); sr32(1, pop32(ssBase)); sr32(0, pop32(ssBase));
      }
      ilen += 1;
      break;

    // ──── PUSH ES/CS/SS/DS (0x06,0x0E,0x16,0x1E) ────
    // In 32-bit PM (opSize=4), selector is zero-extended to 32 bits
    case 0x06: { const s=rr16(S_ES+SEG_SEL); if(opSize===4) push32(s,ssBase); else push16(s,ssBase); ilen+=1; break; }
    case 0x0E: { const s=rr16(S_CS+SEG_SEL); if(opSize===4) push32(s,ssBase); else push16(s,ssBase); ilen+=1; break; }
    case 0x16: { const s=rr16(S_SS+SEG_SEL); if(opSize===4) push32(s,ssBase); else push16(s,ssBase); ilen+=1; break; }
    case 0x1E: { const s=rr16(S_DS+SEG_SEL); if(opSize===4) push32(s,ssBase); else push16(s,ssBase); ilen+=1; break; }

    // ──── POP ES/SS/DS (0x07,0x17,0x1F) ────
    case 0x07: {
      if (!realMode) { lastBailOp = b; iter = maxInsn; break; }
      const v = pop16(ssBase);
      wr16(S_ES + SEG_SEL, v); wr64(S_ES + SEG_BASE, v << 4); esBase = v << 4;
      ilen += 1;
      break;
    }
    case 0x17: {
      if (!realMode) { lastBailOp = b; iter = maxInsn; break; }
      const v = pop16(ssBase);
      wr16(S_SS + SEG_SEL, v); wr64(S_SS + SEG_BASE, v << 4); ssBase = v << 4;
      ilen += 1;
      break;
    }
    case 0x1F: {
      if (!realMode) { lastBailOp = b; iter = maxInsn; break; }
      const v = pop16(ssBase);
      wr16(S_DS + SEG_SEL, v); wr64(S_DS + SEG_BASE, v << 4); dsBase = v << 4;
      ilen += 1;
      break;
    }

    // ──── LES r, m (0xC4) ────
    case 0xC4: {
      if (!realMode) { lastBailOp = b; iter = maxInsn; break; }
      const modrm = mem8[ci+1]; ilen += 2;
      if ((modrm >> 6) === 3) { lastBailOp = b; iter = maxInsn; break; } // must be memory
      const reg = (modrm >> 3) & 7;
      const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS) : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
      ilen += m.len;
      if (opSize === 2) { sr16(reg, rw(m.ea)); } else { sr32(reg, rd(m.ea)); }
      const seg = rw(m.ea + opSize);
      wr16(S_ES + SEG_SEL, seg); wr64(S_ES + SEG_BASE, seg << 4); esBase = seg << 4;
      break;
    }

    // ──── LDS r, m (0xC5) ────
    case 0xC5: {
      if (!realMode) { lastBailOp = b; iter = maxInsn; break; }
      const modrm = mem8[ci+1]; ilen += 2;
      if ((modrm >> 6) === 3) { lastBailOp = b; iter = maxInsn; break; }
      const reg = (modrm >> 3) & 7;
      const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci+2, effDS, effSS) : decodeModRM32(modrm, mem8, ci+2, effDS, effSS);
      ilen += m.len;
      if (opSize === 2) { sr16(reg, rw(m.ea)); } else { sr32(reg, rd(m.ea)); }
      const seg = rw(m.ea + opSize);
      wr16(S_DS + SEG_SEL, seg); wr64(S_DS + SEG_BASE, seg << 4); dsBase = seg << 4;
      break;
    }

    // ──── LEAVE (0xC9) ────
    case 0xC9:
      if (_ssBig) sr32(4, gr32(5)); // ESP = EBP
      else sr16(4, gr16(5)); // SP = BP
      if (opSize === 2) sr16(5, pop16(ssBase)); // BP = pop
      else sr32(5, pop32(ssBase));
      ilen += 1;
      break;

    // ──── XLAT (0xD7) — AL = [DS:BX+AL] ────
    case 0xD7: {
      const seg = segOverride >= 0 ? segBase(segOverride) : dsBase;
      const base = addrSize === 4 ? gr32(3) : gr16(3);
      const addr = (seg + ((base + gr8(0)) & (addrSize === 4 ? 0xFFFFFFFF : 0xFFFF)));
      sr8(0, rb(addr));
      ilen += 1;
      break;
    }

    // ──── AAA (0x37) — ASCII adjust after addition ────
    case 0x37: {
      let al = gr8(0), ah = gr8(4);
      if ((al & 0xF) > 9 || getAF()) {
        al = (al + 6) & 0xFF; ah = (ah + 1) & 0xFF;
        sr8(4, ah); lazyCF = 1; lazyOp = OP_NONE;
      } else { lazyCF = 0; lazyOp = OP_NONE; }
      sr8(0, al & 0x0F);
      ilen += 1; break;
    }

    // ──── AAS (0x3F) — ASCII adjust after subtraction ────
    case 0x3F: {
      let al = gr8(0), ah = gr8(4);
      if ((al & 0xF) > 9 || getAF()) {
        al = (al - 6) & 0xFF; ah = (ah - 1) & 0xFF;
        sr8(4, ah); lazyCF = 1; lazyOp = OP_NONE;
      } else { lazyCF = 0; lazyOp = OP_NONE; }
      sr8(0, al & 0x0F);
      ilen += 1; break;
    }

    // ──── DAA (0x27) — Decimal adjust after addition ────
    case 0x27: {
      let al = gr8(0), cf = getCF(), af = getAF();
      let newCF = 0;
      if ((al & 0xF) > 9 || af) { al += 6; newCF = cf || (al > 0xFF ? 1 : 0); al &= 0xFF; af = 1; } else af = 0;
      if (al > 0x99 || cf) { al = (al + 0x60) & 0xFF; newCF = 1; }
      sr8(0, al); lazyCF = newCF; lazyOp = OP_NONE; lazyRes = al; lazySize = 1;
      ilen += 1; break;
    }

    // ──── DAS (0x2F) — Decimal adjust after subtraction ────
    case 0x2F: {
      let al = gr8(0), cf = getCF(), af = getAF();
      let newCF = 0;
      if ((al & 0xF) > 9 || af) { al -= 6; newCF = cf || (al < 0 ? 1 : 0); al &= 0xFF; af = 1; } else af = 0;
      if (al > 0x99 || cf) { al = (al - 0x60) & 0xFF; newCF = 1; }
      sr8(0, al); lazyCF = newCF; lazyOp = OP_NONE; lazyRes = al; lazySize = 1;
      ilen += 1; break;
    }

    // ──── AAM (0xD4) — ASCII adjust after multiply ────
    case 0xD4: {
      const base = mem8[ci+1] || 10; // usually 0x0A
      if (base === 0) { lastBailOp = b; iter = maxInsn; break; }
      const al = gr8(0);
      sr8(4, (al / base) & 0xFF); sr8(0, (al % base) & 0xFF);
      lazyOp = OP_NONE; lazyRes = gr8(0); lazySize = 1; lazyCF = 0;
      ilen += 2; break;
    }

    // ──── AAD (0xD5) — ASCII adjust before division ────
    case 0xD5: {
      const base = mem8[ci+1] || 10;
      const al = ((gr8(4) * base) + gr8(0)) & 0xFF;
      sr8(0, al); sr8(4, 0);
      lazyOp = OP_NONE; lazyRes = al; lazySize = 1; lazyCF = 0;
      ilen += 2; break;
    }

    // ──── INT n (0xCD imm8) — software interrupt ────
    case 0xCD: {
      if (!realMode) { lastBailOp = b; iter = maxInsn; break; } // protected mode INT needs IDT
      const intNum = mem8[ci+1];
      // INT 15h: bail to IEM — the BIOS e820 handler uses .386 pushad/call/popad
      // in a USE16 segment which needs exact stack frame alignment.  Let IEM handle it.
      if (intNum === 0x15) { lastBailOp = b; iter = maxInsn; break; }
      // Log all INT calls to trace ISOLINUX boot sequence
      if (intNum !== 0x1c && intNum !== 0x08) { // skip timer INTs from count
      if (!execBlock._intLog) execBlock._intLog = { count: 0 };
      const cntInt = ++execBlock._intLog.count;
      if (cntInt <= 2000) {
        const ahInt = (gr16(0) >> 8) & 0xFF;
        const alInt = gr16(0) & 0xFF;
        console.log('[INT] INT 0x' + intNum.toString(16).padStart(2,'0') +
          ' AH=0x' + ahInt.toString(16).padStart(2,'0') +
          ' AL=0x' + alInt.toString(16).padStart(2,'0') +
          ' @' + (csBase>>>4).toString(16) + ':' + ip.toString(16) +
          ' #' + cntInt);
      }
      } // end timer filter
      // INT 10h (Video BIOS): Handle natively — let the JIT execute the VGA
      // BIOS handler.  The JIT will bail on VGA memory writes (mmioFault) or
      // I/O port accesses (AH=02h cursor update writes to 0x3D4/0x3D5).
      // AH=03h (Get Cursor) runs entirely in the JIT with no bail.
      // This avoids a costly JIT→IEM transition for each character write.
      // Log INT 13h calls (disk I/O) to trace ISOLINUX boot failures
      if (intNum === 0x13) {
        if (!execBlock._int13Log) execBlock._int13Log = { count: 0 };
        const cnt13 = ++execBlock._int13Log.count;
        const ah13 = (gr16(0) >> 8) & 0xFF;
        const al13 = gr16(0) & 0xFF;
        const dl13 = gr16(2) & 0xFF; // drive
        if (cnt13 <= 200)
          console.log('[INT13] AH=0x' + ah13.toString(16).padStart(2,'0') +
            ' AL=0x' + al13.toString(16).padStart(2,'0') +
            ' DL=0x' + dl13.toString(16).padStart(2,'0') +
            ' CX=0x' + gr16(1).toString(16).padStart(4,'0') +
            ' BX=0x' + gr16(3).toString(16).padStart(4,'0') +
            ' ES=0x' + rr16(S_ES + SEG_SEL).toString(16).padStart(4,'0') +
            ' @' + (csBase>>>4).toString(16) + ':' + ip.toString(16) +
            ' #' + cnt13);
      }
      // Materialize FLAGS: arithmetic bits from lazy, IF/DF/IOPL from stored flags
      const arithFlags = flagsToWord();
      const pushFlags = (flags & ~0x8D5) | (arithFlags & 0x8D5);
      // Push FLAGS, CS, IP (return address = after INT instruction)
      const retIP = (ip + pos + 2) & 0xFFFF;
      push16(pushFlags, ssBase);
      push16(rr16(S_CS + SEG_SEL), ssBase);
      push16(retIP, ssBase);
      // Clear IF and TF
      flags = pushFlags & ~0x0300; // IF=0, TF=0
      loadFlags(flags);
      // Read IVT entry: [IP:CS] at intNum*4
      const ivtAddr = intNum * 4;
      const newIP = rw(ivtAddr);
      const newCS = rw(ivtAddr + 2);
      csBase = newCS << 4;
      wr16(S_CS + SEG_SEL, newCS);
      wr64(S_CS + SEG_BASE, csBase);
      ip = newIP;
      ilen = 0; executed++;
      wrIP(ip);
      wr32(R_FLAGS, flags);
      continue;
    }

    // ──── INT3 (0xCC) — breakpoint ────
    case 0xCC: {
      if (!realMode) { lastBailOp = b; iter = maxInsn; break; }
      const arithF3 = flagsToWord();
      const pushF3 = (flags & ~0x8D5) | (arithF3 & 0x8D5);
      const retIP3 = (ip + pos + 1) & 0xFFFF;
      push16(pushF3, ssBase);
      push16(rr16(S_CS + SEG_SEL), ssBase);
      push16(retIP3, ssBase);
      flags = pushF3 & ~0x0300;
      loadFlags(flags);
      const newIP3 = rw(3 * 4);
      const newCS3 = rw(3 * 4 + 2);
      csBase = newCS3 << 4;
      wr16(S_CS + SEG_SEL, newCS3);
      wr64(S_CS + SEG_BASE, csBase);
      ip = newIP3;
      ilen = 0; executed++;
      wrIP(ip);
      wr32(R_FLAGS, flags);
      continue;
    }

    // ──── IRET (0xCF) — return from interrupt ────
    case 0xCF: {
      if (!realMode) { lastBailOp = b; iter = maxInsn; break; }
      const iretIP = pop16(ssBase);
      const iretCS = pop16(ssBase);
      const iretFlags = pop16(ssBase);
      csBase = iretCS << 4;
      wr16(S_CS + SEG_SEL, iretCS);
      wr64(S_CS + SEG_BASE, csBase);
      ip = iretIP;
      // Restore full flags
      flags = (iretFlags & 0xFFFF) | 2; // bit 1 always set
      loadFlags(flags);
      // Bail if TF was set by IRET — IEM must handle #DB on next instruction
      if (flags & 0x100) { wrIP(ip); wr32(R_FLAGS, (flags & 0xFFFFF700) | flagsToWord()); executed++; iter = maxInsn; ilen = 0; continue; }
      // Log ALL IRETss during boot phase to debug cascade
      if (!execBlock._iretAllCnt) execBlock._iretAllCnt = 0;
      if (++execBlock._iretAllCnt <= 100)
        console.log('[IRET] →' + iretCS.toString(16) + ':' + iretIP.toString(16) +
          ' FL=' + iretFlags.toString(16) + ' SP=' + gr16(4).toString(16) +
          ' #' + execBlock._iretAllCnt);
      // Log IRET returns to kernel setup segment
      if ((iretCS === 0x1020 || iretCS === 0x1000) && !(flags & 0x100)) {
        if (!execBlock._kernelIretCnt) execBlock._kernelIretCnt = 0;
        if (++execBlock._kernelIretCnt <= 50)
          console.log('[IRET→KRNL] CS=' + iretCS.toString(16) + ':' + iretIP.toString(16) +
            ' FL=' + iretFlags.toString(16) + ' SP=' + gr16(4).toString(16) +
            ' #' + execBlock._kernelIretCnt);
      }
      // Log IRET returns to ISOLINUX segment (CS=0003) to trace INT 13h results
      if (iretCS === 0x0003) {
        if (!execBlock._iretLog) execBlock._iretLog = { count: 0 };
        const cntIret = ++execBlock._iretLog.count;
        const cf = iretFlags & 1; // CF = error flag for INT 13h
        const ah_ret = (gr16(0) >> 8) & 0xFF;
        if (cntIret <= 200)
          console.log('[IRET→ISOL] CS:IP=' + iretCS.toString(16) + ':' + iretIP.toString(16).padStart(4,'0') +
            ' CF=' + cf + ' AH=0x' + ah_ret.toString(16).padStart(2,'0') +
            ' FL=0x' + iretFlags.toString(16).padStart(4,'0') +
            ' #' + cntIret);
      }
      ilen = 0; executed++;
      wrIP(ip);
      wr32(R_FLAGS, flags);
      continue;
    }

    // ──── HLT (0xF4) — halt processor ────
    case 0xF4:
      // Always bail to IEM for HLT, regardless of IF state.
      // IF=1: IEM enters halt state, wakes on next interrupt (PIT, keyboard).
      // IF=0: IEM enters halt state; only NMI/RESET can wake the CPU.
      //       This is the correct behavior for "CLI; HLT" (system halt).
      //       Previously we skipped HLT when IF=0 as a BIOS POST workaround,
      //       but the VBox BIOS never executes CLI+HLT during normal POST.
      //       Skipping it caused bootloaders (e.g. ISOLINUX) that do CLI+HLT
      //       on fatal errors to continue executing garbage code and hang.
      {
        const hltCS = rr16(S_CS + SEG_SEL);
        const hltIF = !!(flags & 0x200);
        // Log first few HLTs with stack trace to diagnose stuck loops
        if (!execBlock._hltLog) execBlock._hltLog = { count: 0 };
        const hltCnt = ++execBlock._hltLog.count;
        if (hltCnt <= 5 || (hltCnt % 10000 === 0)) {
          // Read return addresses from stack
          const sp = gr16(4); // SP
          const ssBase2 = Number(rr64(S_SS + SEG_BASE));
          let stackDump = '';
          for (let si = 0; si < 12; si += 2) {
            const w = rw(ssBase2 + ((sp + si) & 0xFFFF));
            stackDump += w.toString(16).padStart(4,'0') + ' ';
          }
          console.log('[HLT] CS:IP=' + hltCS.toString(16) + ':' + ip.toString(16) +
            ' IF=' + (hltIF?1:0) + ' SP=0x' + sp.toString(16) +
            ' stack=[' + stackDump.trim() + '] #' + hltCnt);
        }
        // Dump VGA text buffer to see error message on screen
        if (hltCS <= 0x10 && !hltIF) {
          try {
            // VGA text buffer at 0xB8000, 80x25, 2 bytes per char (char+attr)
            let vgaText = '';
            for (let row = 0; row < 25; row++) {
              let line = '';
              for (let col = 0; col < 80; col++) {
                const addr = 0xB8000 + (row * 80 + col) * 2;
                const ch = mem8[addr] || 0x20;
                line += String.fromCharCode(ch < 0x20 ? 0x20 : ch);
              }
              const trimmed = line.trimEnd();
              if (trimmed.length > 0) vgaText += row + ': ' + trimmed + '\n';
            }
            console.log('[VGA-TEXT]\n' + vgaText);
          } catch(e) { console.log('[VGA-TEXT] err: ' + e.message); }
          // Dump 32 bytes of code around the crash point
          const codeBase = (hltCS << 4) + ip;
          let codeDump = '';
          for (let i = -16; i < 16; i++) {
            codeDump += mem8[codeBase + i].toString(16).padStart(2, '0') + ' ';
          }
          console.log('[CODE-DUMP] around ' + codeBase.toString(16) + ': ' + codeDump);
        }

        // ── Direct Kernel Boot ──
        // When the BIOS is stuck in its ATA retry/halt loop after boot failure,
        // bypass ISOLINUX by loading the pre-staged kernel directly into guest RAM.
        // Trigger: 30K+ HLTs at BIOS halt_forever (f000:709c) + KRNL magic present.
        // BIOS POST is already complete by this point (ISOLINUX has run and failed).
        const hltSP = gr16(4);
        if (hltCnt >= 10 && hltCS === 0xF000 &&
            !execBlock._directBootDone) {
          const md = ramBase + 0x500;
          if (mem8[md+12] === 0x4B && mem8[md+13] === 0x52 &&
              mem8[md+14] === 0x4E && mem8[md+15] === 0x4C) { // "KRNL" magic
            execBlock._directBootDone = true;

            // Read staging metadata (written by main thread)
            const stageBase = (mem8[md] | (mem8[md+1]<<8) | (mem8[md+2]<<16) | (mem8[md+3]<<24)) >>> 0;
            const vmlinuzLen = (mem8[md+4] | (mem8[md+5]<<8) | (mem8[md+6]<<16) | (mem8[md+7]<<24)) >>> 0;
            const initrdLen = (mem8[md+8] | (mem8[md+9]<<8) | (mem8[md+10]<<16) | (mem8[md+11]<<24)) >>> 0;

            console.log('[DIRECT-BOOT] Triggered at HLT #' + hltCnt +
              ' SP=0x' + hltSP.toString(16) +
              ' stage=0x' + stageBase.toString(16) +
              ' vmlinuz=' + vmlinuzLen + ' initrd=' + initrdLen);

            // Parse bzImage header
            const setup_sects = mem8[stageBase + 0x1F1] || 4;
            const hdrSig = String.fromCharCode(
              mem8[stageBase+0x202], mem8[stageBase+0x203],
              mem8[stageBase+0x204], mem8[stageBase+0x205]);
            const protoVer = mem8[stageBase+0x206] | (mem8[stageBase+0x207]<<8);

            // Read init_size and pref_address from setup header
            const initSz = dv.getUint32(stageBase + 0x260, true);
            const prefAddr = dv.getUint32(stageBase + 0x258, true);
            console.log('[DIRECT-BOOT] header=' + hdrSig + ' proto=0x' +
              protoVer.toString(16) + ' setup_sects=' + setup_sects +
              ' init_size=0x' + initSz.toString(16) +
              ' pref_addr=0x' + prefAddr.toString(16));

            if (hdrSig === 'HdrS' && highRamPtr) {
              const setupSize = (setup_sects + 1) * 512;
              const pmKernelOff = setupSize;
              const pmKernelSize = vmlinuzLen - pmKernelOff;

              // Guest physical addresses
              const SETUP_GPA = 0x10000;
              const KERNEL_GPA = 0x100000;
              const CMDLINE_GPA = 0x20000;
              // Place initrd at end of RAM, page-aligned
              const INITRD_GPA = ((0x100000 + highRamSize - initrdLen) & ~0xFFF) >>> 0;

              console.log('[DIRECT-BOOT] Copying: setup=' + setupSize +
                ' @0x' + SETUP_GPA.toString(16) +
                ' kernel=' + pmKernelSize + ' @0x' + KERNEL_GPA.toString(16) +
                ' initrd=' + initrdLen + ' @0x' + INITRD_GPA.toString(16));

              // Copy setup code to guest 0x10000 (conventional memory)
              mem8.set(
                mem8.subarray(stageBase, stageBase + setupSize),
                ramBase + SETUP_GPA);

              // Copy protected-mode kernel to guest 0x100000 (high RAM)
              mem8.set(
                mem8.subarray(stageBase + pmKernelOff, stageBase + vmlinuzLen),
                highRamPtr);

              // Copy initrd to end of RAM
              mem8.set(
                mem8.subarray(stageBase + vmlinuzLen, stageBase + vmlinuzLen + initrdLen),
                highRamPtr + (INITRD_GPA - 0x100000));

              // Write kernel command line at guest 0x20000
              const cmdline = 'loglevel=3 vga=791 console=ttyS0,115200 idle=halt notsc clocksource=jiffies acpi=off nopti nospectre_v1 nospectre_v2 pci=lastbus=0 mitigations=off notrace lpj=100\0';
              for (let ci = 0; ci < cmdline.length; ci++)
                mem8[ramBase + CMDLINE_GPA + ci] = cmdline.charCodeAt(ci);

              // Set boot params in setup header
              const bp = ramBase + SETUP_GPA;
              mem8[bp + 0x210] = 0xFF; // type_of_loader
              mem8[bp + 0x211] |= 0x81; // loadflags: LOADED_HIGH + CAN_USE_HEAP
              mem8[bp + 0x1FA] = 0xFF; mem8[bp + 0x1FB] = 0xFF; // vid_mode: normal
              mem8[bp + 0x224] = 0x00; mem8[bp + 0x225] = 0xFE; // heap_end_ptr
              dv.setUint32(ramBase + SETUP_GPA + 0x228, CMDLINE_GPA, true); // cmd_line_ptr
              dv.setUint32(ramBase + SETUP_GPA + 0x218, INITRD_GPA, true); // ramdisk_image
              dv.setUint32(ramBase + SETUP_GPA + 0x21C, initrdLen, true);  // ramdisk_size

              // ── e820 memory map ──
              // boot_params->e820_entries at offset 0x1E8
              // boot_params->e820_table at offset 0x2D0 (20 bytes per entry)
              const TOTAL_RAM = 0x100000 + highRamSize; // e.g. 512MB + 1MB low
              const e820Off = bp + 0x2D0;
              let e820idx = 0;
              // Entry 0: 0x000000-0x09FFFF usable (640KB conventional)
              dv.setUint32(e820Off + 0, 0x00000, true);  dv.setUint32(e820Off + 4, 0, true);
              dv.setUint32(e820Off + 8, 0xA0000, true);  dv.setUint32(e820Off + 12, 0, true);
              dv.setUint32(e820Off + 16, 1, true); // type=1 (usable)
              e820idx++;
              // Entry 1: 0x100000-end_of_ram usable
              const e1 = e820Off + 20;
              dv.setUint32(e1 + 0, 0x100000, true);  dv.setUint32(e1 + 4, 0, true);
              dv.setUint32(e1 + 8, (TOTAL_RAM - 0x100000) >>> 0, true);
              dv.setUint32(e1 + 12, ((TOTAL_RAM - 0x100000) > 0xFFFFFFFF) ? 1 : 0, true);
              dv.setUint32(e1 + 16, 1, true); // type=1 (usable)
              e820idx++;
              // Entry 2: 0x0F0000-0x0FFFFF reserved (BIOS area)
              const e2 = e820Off + 40;
              dv.setUint32(e2 + 0, 0xF0000, true);  dv.setUint32(e2 + 4, 0, true);
              dv.setUint32(e2 + 8, 0x10000, true);  dv.setUint32(e2 + 12, 0, true);
              dv.setUint32(e2 + 16, 2, true); // type=2 (reserved)
              e820idx++;
              mem8[bp + 0x1E8] = e820idx;

              // ── ext_mem_k (offset 0x02) — extended memory above 1MB in KB ──
              const extMemK = ((TOTAL_RAM - 0x100000) >>> 10);
              dv.setUint16(bp + 0x02, Math.min(extMemK, 0xFFFF), true);
              // alt_mem_k (offset 0x1E0) — same but 32-bit
              dv.setUint32(bp + 0x1E0, extMemK, true);

              // ── screen_info (offset 0x00) — basic VGA text mode ──
              mem8[bp + 0x06] = 3;     // orig_video_mode = 3 (80x25 text)
              mem8[bp + 0x07] = 80;    // orig_video_cols
              mem8[bp + 0x0E] = 25;    // orig_video_lines
              mem8[bp + 0x0F] = 0;     // orig_video_isVGA = 0 (standard VGA)
              dv.setUint16(bp + 0x10, 16, true); // orig_video_points (char height)

              console.log('[DIRECT-BOOT] e820: ' + e820idx + ' entries, RAM=' +
                (TOTAL_RAM >> 20) + 'MB');

              // ── Fast kernel decompression (skip 20-min IEM decompressor) ──
              // Try to decompress the bzImage payload in JavaScript and jump
              // directly to the decompressed kernel in 64-bit mode.
              let fastBootDone = false;
              if (protoVer >= 0x208) {
                const payloadOff = dv.getUint32(stageBase + 0x248, true);
                const payloadLen = dv.getUint32(stageBase + 0x24C, true);
                if (payloadOff > 0 && payloadLen > 0) {
                  const payloadStart = stageBase + pmKernelOff + payloadOff;
                  console.log('[FAST-BOOT] payload_offset=0x' + payloadOff.toString(16) +
                    ' payload_length=' + payloadLen +
                    ' magic=0x' + mem8[payloadStart].toString(16).padStart(2,'0') +
                    mem8[payloadStart+1].toString(16).padStart(2,'0'));

                  const t0 = performance.now();
                  const compressed = mem8.subarray(payloadStart, payloadStart + payloadLen);
                  const vmlinux = jsGunzip(compressed);
                  if (vmlinux) {
                    const dt = (performance.now() - t0) | 0;
                    console.log('[FAST-BOOT] Decompressed kernel: ' + vmlinux.length +
                      ' bytes (' + (vmlinux.length >> 20) + 'MB) in ' + dt + 'ms');

                    const elf = parseELF64(vmlinux);
                    if (elf) {
                      console.log('[FAST-BOOT] ELF entry=0x' + elf.entry.toString(16) +
                        ' segments=' + elf.segs.length);

                      // Load PT_LOAD segments into guest RAM
                      for (let si = 0; si < elf.segs.length; si++) {
                        const seg = elf.segs[si];
                        const paddr = Number(seg.paddr);
                        console.log('[FAST-BOOT] seg[' + si + '] paddr=0x' +
                          paddr.toString(16) + ' vaddr=0x' + seg.vaddr.toString(16) +
                          ' filesz=' + seg.filesz + ' memsz=' + seg.memsz);

                        if (paddr >= 0x100000 && paddr + seg.memsz <= 0x100000 + highRamSize) {
                          const dst = highRamPtr + (paddr - 0x100000);
                          // Copy file data
                          if (seg.filesz > 0)
                            mem8.set(vmlinux.subarray(seg.offset, seg.offset + seg.filesz), dst);
                          // Zero BSS (memsz > filesz)
                          if (seg.memsz > seg.filesz)
                            mem8.fill(0, dst + seg.filesz, dst + seg.memsz);
                        } else {
                          console.warn('[FAST-BOOT] seg[' + si + '] paddr out of range, skipping');
                        }
                      }

                      // Build page tables for 64-bit mode
                      const cr3 = buildPageTables64(TOTAL_RAM);
                      console.log('[FAST-BOOT] Page tables at GPA 0x' + cr3.toString(16));

                      // Write 64-bit boot metadata at guest 0x7200 for C++
                      const meta = ramBase + 0x7200;
                      dv.setUint32(meta, 0x42343644, true);  // "D64B" magic
                      dv.setUint32(meta + 4, cr3, true);     // CR3 (page table GPA)
                      // Entry point (64-bit)
                      dv.setBigUint64(meta + 8, elf.entry, true);
                      dv.setUint32(meta + 16, SETUP_GPA, true); // boot_params GPA

                      // Write GDT at guest 0x7300
                      const gdtGPA = 0x7300;
                      const gdt = ramBase + gdtGPA;
                      dv.setBigUint64(gdt, 0n, true);       // null descriptor
                      // 0x08: 32-bit code (unused, for compatibility)
                      dv.setBigUint64(gdt + 8, 0x00CF9A000000FFFFn, true);
                      // 0x10: 64-bit code (L=1, D=0)
                      dv.setBigUint64(gdt + 16, 0x00AF9B000000FFFFn, true);
                      // 0x18: 64-bit data
                      dv.setBigUint64(gdt + 24, 0x00CF93000000FFFFn, true);

                      // Signal C++ for 64-bit direct entry
                      wr32(R_CR2, 0xD64B0001);
                      _directBootDone = true;
                      fastBootDone = true;

                      console.log('[FAST-BOOT] 64-bit kernel ready! entry=0x' +
                        elf.entry.toString(16) + ' CR3=0x' + cr3.toString(16) +
                        ' — C++ will set VCPU regs for long mode');
                    } else {
                      console.warn('[FAST-BOOT] ELF parse failed, falling back to slow boot');
                    }
                  } else {
                    console.log('[FAST-BOOT] Not gzip (or decompress failed), falling back to slow boot');
                  }
                }
              }

              if (!fastBootDone) {
                // ── Fallback: 32-bit boot protocol (slow, goes through IEM decompressor) ──
                const SETUP_SEG = SETUP_GPA >>> 4; // 0x1000
                const ENTRY_SEG = SETUP_SEG + 0x20; // 0x1020

                // Write direct-boot metadata at guest 0x7000 for C++ to read
                dv.setUint16(ramBase + 0x7000, ENTRY_SEG, true);   // CS selector
                dv.setUint16(ramBase + 0x7002, SETUP_SEG, true);   // DS/ES/SS selector
                dv.setUint32(ramBase + 0x7004, 0x44424F4F, true);  // "DBOOT" magic (little-endian "BOOT" + 'D')

                // CR2 magic — this DataView write IS visible to C++
                wr32(R_CR2, 0xC0DEBA5E);
                _directBootDone = true;

                console.log('[DIRECT-BOOT] Kernel loaded (slow path)! entrySeg=0x' +
                  ENTRY_SEG.toString(16) + ' initrd@0x' +
                  INITRD_GPA.toString(16) + ' (' + (initrdLen>>10) +
                  'KB) — C++ will set VCPU regs');
              }

              // Return 0 (bail to IEM) so C++ can intercept via CR2 magic
              return 0;
            } else {
              console.error('[DIRECT-BOOT] Bad header or no high RAM!');
            }
          }
        }
      }
      lastBailOp = b; iter = maxInsn;
      break;

    // ──── Unsupported — fallback to IEM ────
    default: {
      // CPUID, RDTSC, etc. — let IEM handle
      lastBailOp = b; iter = maxInsn;
      break;
    }
    } // end switch

    // MMIO bail: if any memory access went to MMIO (outside Wasm linear
    // memory), bail this instruction to IEM which will re-execute it via
    // the PGM MMIO handler. IP must not advance so IEM decodes from scratch.
    if (mmioFault) {
      mmioFault = false;
      lastBailOp = b;
      iter = maxInsn;
      break;
    }

    // Only advance IP if this instruction completed normally (no bail).
    // If lastBailOp >= 0 the instruction was handed off to IEM; IP must
    // remain pointing at the START of that instruction (including prefixes)
    // so IEM decodes the full encoding correctly.
    if (ilen > 0 && lastBailOp < 0) {
      ip = (ip + ilen) & ipMask;
      executed++;
    }

  } // end for

  // ── Store state back ──
  wrIP(ip);
  // Reconstruct RFLAGS
  const newFlags = (flags & 0xFFFFF700) | flagsToWord(); // preserve TF/IF/DF (bits 8-10)
  wr32(R_FLAGS, newFlags);

  // Track bail opcode if we exited early
  if (lastBailOp >= 0) {
    fallbackOpcodes.set(lastBailOp, (fallbackOpcodes.get(lastBailOp) || 0) + 1);
  }

  // Store diagnostics
  statLastCSIP = (csBase>>>4).toString(16) + ':' + ip.toString(16);
  statLastFlags = newFlags;
  // Read code bytes at current CS:IP
  const diagAddr = csBase + ip;
  let cb = '';
  for (let i = 0; i < 8 && (diagAddr + i) < ramSize; i++)
    cb += guestRb(diagAddr + i).toString(16).padStart(2, '0');
  statLastCodeBytes = cb;

  // Log JIT calls from ISOLINUX (CS=3) and the transition to CS=0 zero-execution
  {
    const curCS = rr16(S_CS + SEG_SEL);
    if (!Module._isolLog) Module._isolLog = { count: 0, sawCS3: false, phase: 'wait' };
    const il = Module._isolLog;
    // Start logging when we first see CS=3 (ISOLINUX relocated)
    if (curCS === 3) il.sawCS3 = true;
    if (il.sawCS3 && il.count < 500) {
      il.count++;
      const sp16 = rr16(R_SP);
      const ss16 = rr16(S_SS + SEG_SEL);
      console.log('[JIT-ISOL] #' + il.count + ' CS=' + curCS.toString(16) +
        ' IP=' + ip.toString(16).padStart(4, '0') +
        ' exec=' + executed +
        ' bail=' + (lastBailOp >= 0 ? '0x' + lastBailOp.toString(16) : 'none') +
        ' code=' + cb +
        ' AX=0x' + gr16(0).toString(16).padStart(4, '0') +
        ' DX=0x' + gr16(2).toString(16).padStart(4, '0') +
        ' SS:SP=' + ss16.toString(16) + ':' + sp16.toString(16).padStart(4, '0'));
    }
  }

  // Track kernel execution: log final CS/IP after each JIT block
  // to detect if kernel code is actually being reached after IRET
  {
    const endCS = rr16(S_CS);
    if (!execBlock._krnlStats) execBlock._krnlStats = { total: 0, krnl: 0, logCnt: 0 };
    const ks = execBlock._krnlStats;
    ks.total++;
    if (endCS === 0x1020 || endCS === 0x1000) ks.krnl++;
    if (ks.total % 5000 === 0) {
      ks.logCnt++;
      if (ks.logCnt <= 50) {
        console.log('[JIT-KPROG] calls=' + ks.total + ' krnl=' + ks.krnl +
          ' endCS=' + endCS.toString(16) + ':' + ip.toString(16) +
          ' exec=' + executed + ' bail=' +
          (lastBailOp >= 0 ? '0x' + lastBailOp.toString(16) : 'none'));
      }
    }
  }

  return executed;
}

// ── Stats ──
let statTotalInsns = 0, statTotalCalls = 0, statFallbacks = 0;
let statFastFillLogs = 0; // throttle REP STOS fast-fill log messages
let statLastCSIP = '';
let statLastFlags = 0;
let statLastCodeBytes = '';
let statLastReport = 0;
const fallbackOpcodes = new Map(); // opcode -> count
// Stuck-detection: track how long we've been at the same IP range
let stuckLastIP = -1;
let stuckCount = 0;
let stuckDumped = false;
// Protected-mode diagnostic: one-time dump when JIT first sees CR0.PE=1
let protModeDiagDone = false;

// CPUMCTX offsets for GDTR/IDTR (packed structs with padding)
// gdtr at 0x01C6: cbGdt(u16) at +0, pGdt(u64) at +2
// idtr at 0x01D6: cbIdt(u16) at +0, pIdt(u64) at +2
const R_GDTR = 0x01C6;
const R_GDTR_LIMIT = 0x01C6;  // cbGdt (u16)
const R_GDTR_BASE = 0x01C8;   // pGdt (u64)
const R_IDTR = 0x01D6;

function execBlockWrapped(cpuP, ramB, maxInsn, highRamP, highRamSz) {
  // Set high RAM pointer directly from execBlock params (avoids EM_JS threading issues)
  if (highRamP && !highRamPtr) {
    highRamPtr = highRamP;
    highRamSize = highRamSz;
    highRamEnd = 0x100000 + highRamSz;
    console.log('[JIT] High RAM set: ptr=0x' + highRamP.toString(16) +
      ' size=' + (highRamSz >> 20) + 'MB range=0x100000-0x' + highRamEnd.toString(16));
  }
  // Write highRamPtr to guest RAM at 0x7010 so main thread can read it
  // (closure variables are thread-local in Emscripten pthreads;
  // use one-shot flag since highRamPtr may be set after first call)
  if (highRamPtr && !execBlockWrapped._highRamShared) {
    execBlockWrapped._highRamShared = true;
    const rb = Number(ramB);
    const m = new Uint8Array(wasmMemory.buffer);
    m[rb + 0x7010] = highRamPtr & 0xFF;
    m[rb + 0x7011] = (highRamPtr >> 8) & 0xFF;
    m[rb + 0x7012] = (highRamPtr >> 16) & 0xFF;
    m[rb + 0x7013] = (highRamPtr >> 24) & 0xFF;
    m[rb + 0x7014] = 0; m[rb + 0x7015] = 0;
    m[rb + 0x7016] = 0; m[rb + 0x7017] = 0;
    console.log('[JIT] Wrote highRamPtr=0x' + highRamPtr.toString(16) +
      ' to guest RAM at 0x7010 for main thread');
  }
  const fA20 = globalThis.VBoxJIT._a20;
  statTotalCalls++;
  // Write heartbeat to guest RAM at 0x7020 (main thread monitors this)
  {
    const rb = Number(ramB);
    const m = new Uint8Array(wasmMemory.buffer);
    const c = statTotalCalls;
    m[rb + 0x7020] = c & 0xFF;
    m[rb + 0x7021] = (c >> 8) & 0xFF;
    m[rb + 0x7022] = (c >> 16) & 0xFF;
    m[rb + 0x7023] = (c >> 24) & 0xFF;
    // Also write current CS and IP for diagnostics
    const dvL = new DataView(wasmMemory.buffer);
    const cs16 = dvL.getUint16(Number(cpuP) + S_CS + SEG_SEL, true);
    const ip32 = dvL.getUint32(Number(cpuP) + R_IP, true);
    const cr0 = dvL.getUint32(Number(cpuP) + R_CR0, true);
    dvL.setUint16(rb + 0x7024, cs16, true);
    dvL.setUint32(rb + 0x7026, ip32, true);
    dvL.setUint32(rb + 0x702A, cr0, true);
  }
  // Per-call diagnostics for first 20 calls, then every 100000
  if (statTotalCalls <= 20 || (statTotalCalls % 100000) === 0) {
    const cpuN = Number(cpuP), ramN = Number(ramB);
    console.log('[JIT-DBG] call#' + statTotalCalls +
      ' cpuPtr=0x' + cpuN.toString(16) +
      ' ramBase=0x' + ramN.toString(16) +
      ' highRAM=0x' + highRamPtr.toString(16) + ':' + highRamSize +
      ' romBufSize=' + romBufSize +
      ' maxInsn=' + maxInsn +
      ' A20=' + (fA20 ? 'on' : 'OFF'));
  }
  // One-time: verify ROM content is readable from flat RAM
  if (statTotalCalls === 1) {
    const rb = Number(ramB);
    const m = new Uint8Array(wasmMemory.buffer);
    const fe05b = Array.from(m.slice(rb + 0xFE05B, rb + 0xFE05B + 8)).map(x=>x.toString(16).padStart(2,'0')).join(' ');
    const c0000 = Array.from(m.slice(rb + 0xC0000, rb + 0xC0000 + 8)).map(x=>x.toString(16).padStart(2,'0')).join(' ');
    const fffff0 = Array.from(m.slice(rb + 0xFFFF0, rb + 0xFFFF0 + 8)).map(x=>x.toString(16).padStart(2,'0')).join(' ');
    console.log('[JIT-ROM-CHECK] ramBase=0x' + rb.toString(16) + ' FE05B:' + fe05b + ' C0000:' + c0000 + ' FFFF0:' + fffff0);
  }
  // ── Protected-mode entry diagnostic (one-time) ──
  if (!protModeDiagDone) {
    refreshViews();
    const diagCR0 = rr32(R_CR0);
    if (diagCR0 & 1) { // CR0.PE set — protected mode
      protModeDiagDone = true;
      const diagCR3 = rr32(R_CR3);
      const diagCR4 = rr32(R_CR4);
      const diagEIP = rr32(R_IP);
      const diagFlags = rr32(R_FLAGS);

      // Segment selectors and cached descriptors
      const segNames = ['ES','CS','SS','DS','FS','GS'];
      const segOffs  = [S_ES, S_CS, S_SS, S_DS, S_FS, S_GS];
      console.log('[JIT-PROT] === PROTECTED MODE ENTRY DIAGNOSTIC ===');
      console.log('[JIT-PROT] CR0=0x' + diagCR0.toString(16).padStart(8,'0') +
        ' CR3=0x' + diagCR3.toString(16).padStart(8,'0') +
        ' CR4=0x' + diagCR4.toString(16).padStart(8,'0') +
        ' EFLAGS=0x' + diagFlags.toString(16).padStart(8,'0'));
      for (let si = 0; si < 6; si++) {
        const sel = rr16(segOffs[si] + SEG_SEL);
        const base = Number(dv.getBigUint64(cpuPtr + segOffs[si] + SEG_BASE, true));
        const limit = rr32(segOffs[si] + SEG_LIMIT);
        const attr = rr32(segOffs[si] + SEG_ATTR);
        console.log('[JIT-PROT]   ' + segNames[si] +
          ': sel=0x' + sel.toString(16).padStart(4,'0') +
          ' base=0x' + base.toString(16).padStart(8,'0') +
          ' limit=0x' + limit.toString(16).padStart(8,'0') +
          ' attr=0x' + attr.toString(16).padStart(4,'0'));
      }

      // GDTR
      const gdtLimit = dv.getUint16(cpuPtr + R_GDTR, true);
      const gdtBase = Number(dv.getBigUint64(cpuPtr + R_GDTR + 2, true));
      console.log('[JIT-PROT] GDTR: base=0x' + gdtBase.toString(16).padStart(8,'0') +
        ' limit=0x' + gdtLimit.toString(16).padStart(4,'0'));

      // IDTR
      const idtLimit = dv.getUint16(cpuPtr + R_IDTR, true);
      const idtBase = Number(dv.getBigUint64(cpuPtr + R_IDTR + 2, true));
      console.log('[JIT-PROT] IDTR: base=0x' + idtBase.toString(16).padStart(8,'0') +
        ' limit=0x' + idtLimit.toString(16).padStart(4,'0'));

      // Dump first 8 GDT entries (each 8 bytes)
      const ramN = Number(ramB);
      const m8 = new Uint8Array(wasmMemory.buffer);
      const dvW = new DataView(wasmMemory.buffer);
      const numGdtEntries = Math.min(8, (gdtLimit + 1) / 8);
      console.log('[JIT-PROT] GDT entries (first ' + numGdtEntries + '):');
      for (let gi = 0; gi < numGdtEntries; gi++) {
        const entryAddr = gdtBase + gi * 8;
        // GDT entries are in guest physical memory (no paging since CR0.PG=0 typically)
        const off = ramN + entryAddr;
        if (off + 8 <= m8.length) {
          const lo = dvW.getUint32(off, true);
          const hi = dvW.getUint32(off + 4, true);
          // Parse x86 segment descriptor
          const dBase = ((hi & 0xFF000000) | ((hi & 0xFF) << 16) | ((lo >>> 16) & 0xFFFF)) >>> 0;
          const dLimit = ((hi & 0x000F0000) | (lo & 0xFFFF)) >>> 0;
          const dGranularity = (hi >>> 23) & 1;
          const dEffLimit = dGranularity ? ((dLimit << 12) | 0xFFF) : dLimit;
          const dAccess = (hi >>> 8) & 0xFF;
          const dFlagsHi = (hi >>> 20) & 0xF; // G, D/B, L, AVL
          console.log('[JIT-PROT]   GDT[' + gi + '] sel=0x' + (gi*8).toString(16).padStart(4,'0') +
            ': base=0x' + dBase.toString(16).padStart(8,'0') +
            ' limit=0x' + dEffLimit.toString(16).padStart(8,'0') +
            ' access=0x' + dAccess.toString(16).padStart(2,'0') +
            ' flags=0x' + dFlagsHi.toString(16) +
            ' raw=' + hi.toString(16).padStart(8,'0') + '_' + lo.toString(16).padStart(8,'0'));
        } else {
          console.log('[JIT-PROT]   GDT[' + gi + '] sel=0x' + (gi*8).toString(16).padStart(4,'0') +
            ': OUT OF RANGE (addr=0x' + entryAddr.toString(16) + ')');
        }
      }

      // Current EIP and first 32 bytes of code at CS:EIP
      const diagCSBase = Number(dv.getBigUint64(cpuPtr + S_CS + SEG_BASE, true));
      const codeAddr = diagCSBase + diagEIP;
      console.log('[JIT-PROT] EIP=0x' + diagEIP.toString(16).padStart(8,'0') +
        ' CS:EIP linear=0x' + codeAddr.toString(16).padStart(8,'0'));
      let codeDump = '';
      for (let ci = 0; ci < 32; ci++) {
        const off = ramN + codeAddr + ci;
        if (off < m8.length) {
          codeDump += m8[off].toString(16).padStart(2,'0') + ' ';
        } else if (romBufSize > 0 && (codeAddr+ci) >= romGCPhysStart && (codeAddr+ci) < romGCPhysEnd) {
          codeDump += m8[romBufBase + (codeAddr+ci - romGCPhysStart)].toString(16).padStart(2,'0') + ' ';
        } else {
          codeDump += '?? ';
        }
      }
      console.log('[JIT-PROT] code bytes: ' + codeDump.trim());

      // Check specifically: what is at CS=0x0020 offset 0x17?
      // Selector 0x0020 = GDT index 4, so look up GDT[4].base + 0x17
      const sel20Idx = 0x20 / 8; // = 4
      const gdt4Addr = gdtBase + sel20Idx * 8;
      const gdt4Off = ramN + gdt4Addr;
      if (gdt4Off + 8 <= m8.length) {
        const gdt4Lo = dvW.getUint32(gdt4Off, true);
        const gdt4Hi = dvW.getUint32(gdt4Off + 4, true);
        const gdt4Base = ((gdt4Hi & 0xFF000000) | ((gdt4Hi & 0xFF) << 16) | ((gdt4Lo >>> 16) & 0xFFFF)) >>> 0;
        const tripAddr = gdt4Base + 0x17;
        console.log('[JIT-PROT] GDT[4] (sel 0x0020) base=0x' + gdt4Base.toString(16).padStart(8,'0'));
        let tripBytes = '';
        for (let ti = -4; ti < 20; ti++) {
          const off = ramN + tripAddr + ti;
          if (off < m8.length) {
            if (ti === 0) tripBytes += '[';
            tripBytes += m8[off].toString(16).padStart(2,'0');
            if (ti === 1) tripBytes += '] ';
            else tripBytes += ' ';
          }
        }
        console.log('[JIT-PROT] bytes at 0020:0017 (linear 0x' + tripAddr.toString(16) + '): ' + tripBytes.trim());
      }

      // Also dump the last few real-mode instructions before mode switch
      // (from fallback opcodes map)
      const sorted = [...fallbackOpcodes.entries()].sort((a,b) => b[1]-a[1]).slice(0,12);
      const topStr = sorted.map(([op,cnt]) => '0x' + op.toString(16) + ':' + cnt).join(' ');
      console.log('[JIT-PROT] fallback opcodes at prot entry: [' + topStr + ']');
      console.log('[JIT-PROT] total insns so far: ' + statTotalInsns + ' calls: ' + statTotalCalls);
      console.log('[JIT-PROT] === END PROTECTED MODE DIAGNOSTIC ===');
    }
  }

  let n = 0;
  try {
    n = execBlock(cpuP, ramB, maxInsn);
  } catch (e) {
    if (!execBlockWrapped._errorCount) execBlockWrapped._errorCount = 0;
    if (execBlockWrapped._errorCount++ < 20)
      console.error('[JIT-ERROR] execBlock threw: ' + e.message + '\n' + e.stack);
    return 0;
  }
  if (n > 0) {
    statTotalInsns += n;
  } else {
    statFallbacks++;
  }

  // Stuck-detection: if we stay in the same 32-byte IP range for >50000 calls, dump full state
  {
    const curIP = rr32(R_IP);
    const curIPBlock = curIP >>> 5; // 32-byte blocks
    if (curIPBlock === stuckLastIP) {
      stuckCount++;
    } else {
      stuckLastIP = curIPBlock;
      stuckCount = 0;
      stuckDumped = false;
    }
    if (stuckCount >= 50000 && !stuckDumped) {
      stuckDumped = true;
      refreshViews();
      const csB = segBase(S_CS);
      const ssB = segBase(S_SS);
      const dsB = segBase(S_DS);
      const esB = segBase(S_ES);
      const ax = rr16(R_AX), bx = rr16(R_BX), cx = rr16(R_CX), dx = rr16(R_DX);
      const si = rr16(R_SI), di = rr16(R_DI), bp = rr16(R_BP), sp = rr16(R_SP);
      const fl = rr32(R_FLAGS);
      console.log('[JIT-STUCK] IP stuck at 0x' + curIP.toString(16) + ' for ' + stuckCount + ' calls');
      console.log('[JIT-STUCK] AX=' + ax.toString(16).padStart(4,'0') +
        ' BX=' + bx.toString(16).padStart(4,'0') +
        ' CX=' + cx.toString(16).padStart(4,'0') +
        ' DX=' + dx.toString(16).padStart(4,'0'));
      console.log('[JIT-STUCK] SI=' + si.toString(16).padStart(4,'0') +
        ' DI=' + di.toString(16).padStart(4,'0') +
        ' BP=' + bp.toString(16).padStart(4,'0') +
        ' SP=' + sp.toString(16).padStart(4,'0'));
      console.log('[JIT-STUCK] CS=' + (csB>>>4).toString(16) + ' DS=' + (dsB>>>4).toString(16) +
        ' ES=' + (esB>>>4).toString(16) + ' SS=' + (ssB>>>4).toString(16) +
        ' FLAGS=' + fl.toString(16));
      // Dump 64 bytes of code around the stuck IP
      const codeAddr = csB + curIP;
      let dump = '';
      for (let i = -16; i < 48; i++) {
        if (i === 0) dump += '[';
        const b = guestRb((codeAddr + i) & 0xFFFFF);
        dump += b.toString(16).padStart(2, '0');
        if (i === 0) dump += ']';
        else dump += ' ';
      }
      console.log('[JIT-STUCK] code @' + codeAddr.toString(16) + ': ' + dump);
      // Dump stack (top 16 words)
      let stackDump = '';
      for (let i = 0; i < 16; i++) {
        const saddr = ssB + ((sp + i*2) & 0xFFFF);
        stackDump += guestRw(saddr).toString(16).padStart(4, '0') + ' ';
      }
      console.log('[JIT-STUCK] stack @SS:SP: ' + stackDump);
      // Check if there are any IN/OUT opcodes in fallback map (port I/O activity)
      const inCount = (fallbackOpcodes.get(0xEC) || 0) + (fallbackOpcodes.get(0xED) || 0) +
                      (fallbackOpcodes.get(0xE4) || 0) + (fallbackOpcodes.get(0xE5) || 0);
      const outCount = (fallbackOpcodes.get(0xEE) || 0) + (fallbackOpcodes.get(0xEF) || 0) +
                       (fallbackOpcodes.get(0xE6) || 0) + (fallbackOpcodes.get(0xE7) || 0);
      console.log('[JIT-STUCK] port I/O fallbacks: IN=' + inCount + ' OUT=' + outCount);

      // ── Direct Boot Recovery (in stuck detector) ──
      // ISOLINUX gets stuck at CS=0x9000 IP≈0x1a45 after "ready."
      // Kernel+initrd are loaded. Do direct PM boot to startup_32.
      const cr0S = rr32(R_CR0);
      if (!(cr0S & 1) && csB >= 0x7000 && csB < 0xA0000 &&
          curIP >= 0x0010 && curIP <= 0x3000 &&
          !execBlockWrapped._directBootFired) {
        // Check if kernel is present at 0x100000 via highRamPtr
        const kP = highRamPtr;
        const hasK = kP && (mem8[kP] !== 0 || mem8[kP+1] !== 0 ||
                            mem8[kP+2] !== 0 || mem8[kP+3] !== 0);
        if (hasK) {
          execBlockWrapped._directBootFired = true;
          console.log('[JIT-STUCK-BOOT] ISOLINUX stuck after loading kernel. Doing direct PM boot!');
          console.log('[JIT-STUCK-BOOT] kernel@0x100000: ' +
            mem8[kP].toString(16).padStart(2,'0') + ' ' +
            mem8[kP+1].toString(16).padStart(2,'0') + ' ' +
            mem8[kP+2].toString(16).padStart(2,'0') + ' ' +
            mem8[kP+3].toString(16).padStart(2,'0'));

          // Write boot_params at 0x90000
          const rb = ramBase;
          const setupBase = rb + 0x90000;
          // Clear boot_params (first 4K)
          for (let i = 0; i < 0x1000; i++) mem8[setupBase + i] = 0;

          // e820 memory map at offset 0xD00
          const e820Base = 0xD00;
          // Entry 0: 0x0 - 0x9FC00 usable
          writeDword(setupBase + e820Base + 0, 0);
          writeDword(setupBase + e820Base + 4, 0);
          writeDword(setupBase + e820Base + 8, 0x9FC00);
          writeDword(setupBase + e820Base + 12, 0);
          writeDword(setupBase + e820Base + 16, 1);
          // Entry 1: 0x100000 - highRamEnd usable
          writeDword(setupBase + e820Base + 20, 0x100000);
          writeDword(setupBase + e820Base + 24, 0);
          const ramTop = highRamEnd ? highRamEnd : 0x2000000;
          writeDword(setupBase + e820Base + 28, ramTop - 0x100000);
          writeDword(setupBase + e820Base + 32, 0);
          writeDword(setupBase + e820Base + 36, 1);
          mem8[setupBase + 0x1E8] = 2; // e820_entries count

          // HdrS signature
          mem8[setupBase + 0x202] = 0x48; // H
          mem8[setupBase + 0x203] = 0x64; // d
          mem8[setupBase + 0x204] = 0x72; // r
          mem8[setupBase + 0x205] = 0x53; // S
          // Protocol 2.13
          mem8[setupBase + 0x206] = 0x0D;
          mem8[setupBase + 0x207] = 0x02;
          // loadflags: LOADED_HIGH
          mem8[setupBase + 0x211] = 0x01;
          // type_of_loader
          mem8[setupBase + 0x210] = 0xFF;
          // code32_start
          writeDword(setupBase + 0x214, 0x100000);
          // ramdisk
          // Check if initrd was loaded (typically at ~0x1000000 for 32MB systems)
          // For now, we rely on the kernel finding it via initrd= cmdline or embedded
          // Command line
          const cmdline = 'pmedia=cd BOOT_IMAGE=/vmlinuz console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 loglevel=4 idle=halt notsc clocksource=jiffies acpi=off nopti nospectre_v1 nospectre_v2 pci=lastbus=0 raid=noautodetect mitigations=off notrace lpj=100';
          for (let ci = 0; ci < cmdline.length; ci++)
            mem8[rb + 0x99000 + ci] = cmdline.charCodeAt(ci);
          mem8[rb + 0x99000 + cmdline.length] = 0;
          writeDword(setupBase + 0x228, 0x99000);

          // Write GDT — Linux boot protocol: __BOOT_CS=0x10, __BOOT_DS=0x18
          const gdtBase = rb + 0x1000;
          for (let i = 0; i < 32; i++) mem8[gdtBase + i] = 0; // 4 entries
          // Entry 2 (0x10): __BOOT_CS — flat code32
          mem8[gdtBase+16]=0xFF; mem8[gdtBase+17]=0xFF; mem8[gdtBase+18]=0; mem8[gdtBase+19]=0;
          mem8[gdtBase+20]=0; mem8[gdtBase+21]=0x9A; mem8[gdtBase+22]=0xCF; mem8[gdtBase+23]=0;
          // Entry 3 (0x18): __BOOT_DS — flat data32
          mem8[gdtBase+24]=0xFF; mem8[gdtBase+25]=0xFF; mem8[gdtBase+26]=0; mem8[gdtBase+27]=0;
          mem8[gdtBase+28]=0; mem8[gdtBase+29]=0x92; mem8[gdtBase+30]=0xCF; mem8[gdtBase+31]=0;

          // Set GDTR (4 entries, limit=31)
          wr64(R_GDTR_BASE, 0x1000);
          wr16(R_GDTR_LIMIT, 31);
          // CR0: PE=1, PG=0
          wr32(R_CR0, (cr0S | 1) & ~0x80000000);
          // CS = 0x10 (__BOOT_CS)
          wr16(S_CS + SEG_SEL, 0x10);
          wr64(S_CS + SEG_BASE, 0);
          wr32(S_CS + SEG_LIMIT, 0xFFFFFFFF);
          wr32(S_CS + SEG_ATTR, 0xC09B);
          // DS/ES/SS/FS/GS = 0x18 (__BOOT_DS)
          const dAttr = 0xC093;
          for (const seg of [S_DS, S_ES, S_SS, S_FS, S_GS]) {
            wr16(seg + SEG_SEL, 0x18);
            wr64(seg + SEG_BASE, 0);
            wr32(seg + SEG_LIMIT, 0xFFFFFFFF);
            wr32(seg + SEG_ATTR, dAttr);
          }
          // EIP = startup_32 at 0x100000
          wr32(R_IP, 0x100000);
          // ESI = boot_params
          sr32(6, 0x90000);
          // Clear other regs
          sr32(0, 0); sr32(1, 0); sr32(2, 0); sr32(3, 0);
          sr32(5, 0); sr32(7, 0);
          sr32(4, 0x90000); // ESP
          // Disable interrupts
          wr32(R_FLAGS, 2);

          console.log('[JIT-STUCK-BOOT] PM state set: CR0=0x' +
            ((cr0S | 1) & ~0x80000000).toString(16) +
            ' CS=0x10 EIP=0x100000 ESI=0x90000');
          console.log('[JIT-STUCK-BOOT] Jumping to startup_32!');

          // Reset stuck counter to prevent re-triggering
          stuckCount = 0;
          stuckDumped = false;
          // Mark as done in both locations
          execBlock._directBootDone = true;
          _directBootDone = true;
          return n; // return to IEM with new register state
        } else {
          console.log('[JIT-STUCK] No kernel at 0x100000, not triggering direct boot');
        }
      }
    }
  }

  // Log stats every 30 seconds
  const now = Date.now();
  if (now - statLastReport > 30000) {
    statLastReport = now;
    {
      // Top fallback opcodes
      const sorted = [...fallbackOpcodes.entries()].sort((a,b) => b[1]-a[1]).slice(0,8);
      const topStr = sorted.map(([op,cnt]) => '0x' + op.toString(16) + ':' + cnt).join(' ');
      const ifStr = (statLastFlags & 0x200) ? 'IF=1' : 'IF=0';
      console.log('[JIT] calls=' + statTotalCalls +
        ' insns=' + statTotalInsns +
        ' fallbacks=' + statFallbacks +
        ' avg=' + (statTotalInsns / Math.max(1, statTotalCalls - statFallbacks)).toFixed(1) +
        ' @' + statLastCSIP + ' ' + ifStr + ' code=' + statLastCodeBytes +
        ' top=[' + topStr + ']');

      // Stuck-loop diagnostic: dump register state and IVT entries
      refreshViews();
      const cr0 = rr32(R_CR0);
      const cr4 = rr32(R_CR4);
      const eip = rr32(R_IP);
      const csSel = rr16(S_CS + SEG_SEL);
      const csBaseVal = Number(dv.getBigUint64(cpuPtr + S_CS + SEG_BASE, true));
      const dsSel = rr16(S_DS + SEG_SEL);
      const ssSel = rr16(S_SS + SEG_SEL);
      const sp = rr16(R_SP);
      const ax = rr16(R_AX);
      const bx = rr16(R_BX);
      const cx = rr16(R_CX);
      const dx = rr16(R_DX);
      const si = rr16(R_SI);
      const di = rr16(R_DI);
      const fl = rr32(R_FLAGS);
      console.log('[JIT-STATE] CS=' + csSel.toString(16) + ':' + eip.toString(16) +
        ' csBase=0x' + csBaseVal.toString(16) +
        ' DS=' + dsSel.toString(16) + ' SS=' + ssSel.toString(16) + ':' + sp.toString(16) +
        ' FL=0x' + fl.toString(16));
      console.log('[JIT-STATE] AX=' + ax.toString(16) + ' BX=' + bx.toString(16) +
        ' CX=' + cx.toString(16) + ' DX=' + dx.toString(16) +
        ' SI=' + si.toString(16) + ' DI=' + di.toString(16) +
        ' CR0=0x' + cr0.toString(16) + ' CR4=0x' + cr4.toString(16));

      // Dump code bytes: 16 bytes before + 32 bytes from CS:IP
      const phys = csBaseVal + eip;
      if (phys < 0xA0000 && ramBase) {
        const preBytes = [];
        const startPre = Math.max(0, phys - 16);
        for (let ci = 0; ci < 16; ci++) preBytes.push(mem8[ramBase + startPre + ci].toString(16).padStart(2,'0'));
        console.log('[JIT-STATE] code @0x' + startPre.toString(16) + ' (before IP): ' + preBytes.join(' '));
        const codeBytes = [];
        for (let ci = 0; ci < 32; ci++) codeBytes.push(mem8[ramBase + phys + ci].toString(16).padStart(2,'0'));
        console.log('[JIT-STATE] code @0x' + phys.toString(16) + ' (at IP): ' + codeBytes.join(' '));
      }
      // Dump memory at key locations: BX+SI (what ADD instructions target)
      if (ramBase) {
        const bxsi = (dsSel * 16 + bx + si) & 0xFFFFF;
        const memBytes = [];
        for (let ci = 0; ci < 16; ci++) memBytes.push(mem8[ramBase + bxsi + ci].toString(16).padStart(2,'0'));
        console.log('[JIT-STATE] mem @DS:BX+SI=0x' + bxsi.toString(16) + ': ' + memBytes.join(' '));
        // Dump memory above 1MB to check if highRAM is accessible
        console.log('[JIT-STATE] highRamPtr=' + highRamPtr + ' highRamEnd=' + highRamEnd);
        // Check if kernel header at 0x100000 is populated (kernel should be there)
        if (highRamPtr) {
          const kernBytes = [];
          for (let ci = 0; ci < 16; ci++) kernBytes.push(mem8[highRamPtr + ci].toString(16).padStart(2,'0'));
          console.log('[JIT-STATE] @0x100000 (kernel): ' + kernBytes.join(' '));
        }
        // Check ESP register too
        const esp = rr32(R_SP);
        const ebp = rr32(R_BP);
        console.log('[JIT-STATE] ESP=0x' + esp.toString(16) + ' EBP=0x' + ebp.toString(16) +
          ' EAX=0x' + rr32(R_AX).toString(16) + ' EBX=0x' + rr32(R_BX).toString(16) +
          ' ECX=0x' + rr32(R_CX).toString(16) + ' EDX=0x' + rr32(R_DX).toString(16) +
          ' ESI=0x' + rr32(R_SI).toString(16) + ' EDI=0x' + rr32(R_DI).toString(16));
      }

      // Dump IVT[8] (INT 8 = PIT timer handler) and IVT[0x15] (INT 15h = BIOS services)
      if (ramBase) {
        const int8off  = mem8[ramBase + 0x20] | (mem8[ramBase + 0x21] << 8);
        const int8seg  = mem8[ramBase + 0x22] | (mem8[ramBase + 0x23] << 8);
        const int15off = mem8[ramBase + 0x54] | (mem8[ramBase + 0x55] << 8);
        const int15seg = mem8[ramBase + 0x56] | (mem8[ramBase + 0x57] << 8);
        const int19off = mem8[ramBase + 0x64] | (mem8[ramBase + 0x65] << 8);
        const int19seg = mem8[ramBase + 0x66] | (mem8[ramBase + 0x67] << 8);
        console.log('[JIT-IVT] INT8=' + int8seg.toString(16) + ':' + int8off.toString(16) +
          ' INT15=' + int15seg.toString(16) + ':' + int15off.toString(16) +
          ' INT19=' + int19seg.toString(16) + ':' + int19off.toString(16));

        // Dump boot_params signature at 0x1F1 (HDR offset) and setup_sects
        const hdrSig = mem8[ramBase + 0x901F1] | (mem8[ramBase + 0x901F2] << 8);
        const setupSects = mem8[ramBase + 0x901F1];
        const bootSig = mem8[ramBase + 0x901FE] | (mem8[ramBase + 0x901FF] << 8);
        console.log('[JIT-BOOT] @0x901F1: setup_sects=0x' + setupSects.toString(16) +
          ' boot_sig=0x' + bootSig.toString(16).padStart(4,'0'));

        // Dump stack top (SS:SP)
        const ssBase = ssSel * 16;
        const stackDump = [];
        for (let si = 0; si < 16; si += 2) {
          const off = ssBase + ((sp + si) & 0xFFFF);
          if (off < 0xA0000) {
            const w = mem8[ramBase + off] | (mem8[ramBase + off + 1] << 8);
            stackDump.push(w.toString(16).padStart(4,'0'));
          }
        }
        console.log('[JIT-STATE] stack @' + ssSel.toString(16) + ':' + sp.toString(16) + ': ' + stackDump.join(' '));
      }
    }
  }
  return n;
}

// ── Fast boot: decompress kernel payload in JS, called from C++ (IEMAll.cpp) ──
// Scans guest RAM at 0x100000+ for the compressed kernel payload, decompresses it,
// parses the ELF64, loads segments, builds page tables, and writes D64B metadata.
// Returns 1 on success, 0 on failure.
function fastBootDecompress() {
  if (!highRamPtr || !ramBase) {
    console.log('[FAST-BOOT-JS] No highRamPtr or ramBase');
    return 0;
  }
  const m = new Uint8Array(wasmMemory.buffer);
  const dv2 = new DataView(wasmMemory.buffer);
  const rb = Number(ramBase);
  const hp = Number(highRamPtr);
  // Try both 0x90000 and 0x10000 for setup header
  // ISOLINUX may load setup at either address
  let setupBase = rb + 0x90000;
  let payOff = 0, payLen = 0;
  let hasHdrS = (m[setupBase+0x202]===0x48 && m[setupBase+0x203]===0x64 &&
                 m[setupBase+0x204]===0x72 && m[setupBase+0x205]===0x53);
  if (!hasHdrS) {
    setupBase = rb + 0x10000;
    hasHdrS = (m[setupBase+0x202]===0x48 && m[setupBase+0x203]===0x64 &&
               m[setupBase+0x204]===0x72 && m[setupBase+0x205]===0x53);
  }
  if (hasHdrS) {
    const proto = m[setupBase+0x206]|(m[setupBase+0x207]<<8);
    if (proto >= 0x208) {
      payOff = m[setupBase+0x248]|(m[setupBase+0x249]<<8)|
        (m[setupBase+0x24A]<<16)|(m[setupBase+0x24B]<<24);
      payLen = m[setupBase+0x24C]|(m[setupBase+0x24D]<<8)|
        (m[setupBase+0x24E]<<16)|(m[setupBase+0x24F]<<24);
    }
    console.log('[FAST-BOOT-JS] HdrS found @0x' + (setupBase - rb).toString(16) +
      ' proto=0x' + proto.toString(16) +
      ' payOff=0x' + payOff.toString(16) + ' payLen=' + payLen);
  } else {
    console.log('[FAST-BOOT-JS] No HdrS at 0x90000 or 0x10000');
  }

  // If setup header didn't provide payload info, scan kernel image for
  // compression magic (gzip 0x1F8B, xz 0xFD37, lzma 0x5D00, bzip2 0x425A)
  let compOff = -1, compLen = 0, compType = '';
  if (payOff > 0 && payLen > 0) {
    compOff = payOff; // payload_offset is relative to 0x100000
    compLen = payLen;
    compType = 'header';
  } else {
    // Scan first 2MB of kernel image for compression signatures
    const scanLen = Math.min(2 * 1024 * 1024, highRamSize);
    console.log('[FAST-BOOT-JS] Scanning 0x100000+' + (scanLen>>10) + 'KB for compressed payload...');
    for (let off = 0; off < scanLen - 6; off++) {
      const b0 = m[hp + off], b1 = m[hp + off + 1];
      if (b0 === 0x1F && b1 === 0x8B && m[hp+off+2] === 0x08) {
        compOff = off;
        compLen = highRamSize - off;
        compType = 'gzip@0x' + (0x100000 + off).toString(16);
        console.log('[FAST-BOOT-JS] Found gzip at GPA 0x' + (0x100000+off).toString(16));
        break;
      }
      if (b0 === 0xFD && b1 === 0x37 && m[hp+off+2]===0x7A &&
          m[hp+off+3]===0x58 && m[hp+off+4]===0x5A && m[hp+off+5]===0x00) {
        compOff = off;
        compLen = highRamSize - off;
        compType = 'xz@0x' + (0x100000 + off).toString(16);
        console.log('[FAST-BOOT-JS] Found XZ at GPA 0x' + (0x100000+off).toString(16));
        break;
      }
      if (b0 === 0x5D && b1 === 0x00 && m[hp+off+2]===0x00) {
        compOff = off;
        compLen = highRamSize - off;
        compType = 'lzma@0x' + (0x100000 + off).toString(16);
        console.log('[FAST-BOOT-JS] Found LZMA at GPA 0x' + (0x100000+off).toString(16));
        break;
      }
      if (b0 === 0x28 && b1 === 0xB5 && m[hp+off+2]===0x2F && m[hp+off+3]===0xFD) {
        compType = 'zstd@0x' + (0x100000 + off).toString(16);
        console.log('[FAST-BOOT-JS] Found zstd at GPA 0x' + (0x100000+off).toString(16) +
          ' — zstd not supported');
        return 0;
      }
    }
  }

  if (compOff < 0) {
    console.log('[FAST-BOOT-JS] No compressed payload found');
    let d = '';
    for (let i = 0; i < 32; i++) d += m[hp+i].toString(16).padStart(2,'0') + ' ';
    console.log('[FAST-BOOT-JS] @0x100000: ' + d);
    return 0;
  }

  console.log('[FAST-BOOT-JS] Decompressing ' + compType +
    ' (' + (compLen > 1024*1024 ? (compLen>>20)+'MB' : (compLen>>10)+'KB') + ')...');

  const t0 = performance.now();
  const compStart = hp + compOff;
  let vmlinux = null;
  // Detect compression from actual data bytes, not compType string
  // (compType='header' when payload info comes from setup header)
  const b0 = m[compStart], b1 = m[compStart+1];
  const isXzOrLzma = (b0 === 0xFD && b1 === 0x37) || // XZ magic
                     (b0 === 0x5D && b1 === 0x00);    // LZMA magic

  // vmlinuxPtr: Wasm address of decompressed vmlinux (kept alive for PGMPhysWrite)
  // vmlinuxLen: length of decompressed vmlinux
  let vmlinuxPtr = 0; // BigInt (Wasm address)
  let vmlinuxLen = 0;

  if (isXzOrLzma) {
    // Use C-side liblzma via wasmXzDecompress(src, srcLen, dst, dstCap, pOutLen)
    const dstCap = 64 * 1024 * 1024;
    const pDstRaw = Module._malloc(dstCap);
    const pOutLenRaw = Module._malloc(4); // uint32_t
    if (!pDstRaw || !pOutLenRaw) {
      console.log('[FAST-BOOT-JS] malloc failed for XZ decompress buffer');
      if (pDstRaw) Module._free(pDstRaw);
      if (pOutLenRaw) Module._free(pOutLenRaw);
      return 0;
    }
    const srcBI = BigInt(compStart);
    const dstBI = BigInt(pDstRaw);
    const outBI = BigInt(pOutLenRaw);
    // Scan for relocated XZ copy (kernel self-relocates from 0x100000)
    let xzSrc = compStart;
    let xzSrcGPA = 0x100000 + compOff;
    const origHdr = [];
    for (let i = 0; i < 16; i++) origHdr.push(m[compStart+i]);
    const scanEnd = hp + highRamSize;
    for (let addr = hp + 0x100000; addr < scanEnd - 6; addr++) {
      if (m[addr]===0xFD && m[addr+1]===0x37 && m[addr+2]===0x7A &&
          m[addr+3]===0x58 && m[addr+4]===0x5A && m[addr+5]===0x00) {
        let match = true;
        for (let i = 6; i < 16; i++) {
          if (m[addr+i] !== origHdr[i]) { match = false; break; }
        }
        if (match) {
          xzSrc = addr;
          xzSrcGPA = 0x100000 + (addr - hp);
          console.error('[FAST-BOOT] Found relocated XZ at GPA=0x' +
            xzSrcGPA.toString(16));
          break;
        }
      }
    }
    const finalSrcBI = BigInt(xzSrc);
    console.error('[FAST-BOOT] XZ src=GPA 0x' + xzSrcGPA.toString(16) +
      ' len=' + compLen);
    const rc = Module._wasmXzDecompress(finalSrcBI, compLen, dstBI, dstCap, outBI);
    if (rc !== 0) {
      console.error('[FAST-BOOT] XZ decompression FAILED rc=' + rc);
      Module._free(pDstRaw);
      Module._free(pOutLenRaw);
      return 0;
    }
    const dv3 = new DataView(wasmMemory.buffer);
    vmlinuxLen = dv3.getUint32(Number(outBI), true);
    vmlinuxPtr = pDstRaw; // keep alive — will free after PGMPhysWrite
    Module._free(pOutLenRaw);
    console.error('[FAST-BOOT] XZ decompressed: ' + vmlinuxLen + ' bytes (' +
      (vmlinuxLen >> 20) + 'MB)');
  } else {
    // gzip — decompress to JS array, then copy to Wasm buffer
    const comp = m.subarray(compStart, compStart + compLen);
    const gunzipped = jsGunzip(comp);
    if (!gunzipped) {
      console.error('[FAST-BOOT] gzip decompression failed');
      return 0;
    }
    vmlinuxPtr = Module._malloc(gunzipped.length);
    if (!vmlinuxPtr) {
      console.error('[FAST-BOOT] malloc failed for gzip output');
      return 0;
    }
    const mTmp = new Uint8Array(wasmMemory.buffer);
    mTmp.set(gunzipped, Number(vmlinuxPtr));
    vmlinuxLen = gunzipped.length;
  }

  if (!vmlinuxPtr || vmlinuxLen === 0) {
    const magic = m[compStart].toString(16).padStart(2,'0') +
      m[compStart+1].toString(16).padStart(2,'0');
    console.error('[FAST-BOOT] Decompression failed, magic=0x' + magic);
    return 0;
  }
  const dt = (performance.now() - t0) | 0;
  console.error('[FAST-BOOT] Decompressed: ' + vmlinuxLen + ' bytes (' +
    (vmlinuxLen >> 20) + 'MB) in ' + dt + 'ms');

  // Parse ELF from Wasm buffer (vmlinuxPtr points to decompressed data)
  const vmOff = Number(vmlinuxPtr);
  const vmView = new Uint8Array(wasmMemory.buffer, vmOff, vmlinuxLen);
  const elf = parseELF64(vmView);
  if (!elf) {
    console.error('[FAST-BOOT] ELF parse failed');
    Module._free(BigInt(vmlinuxPtr));
    return 0;
  }
  console.error('[FAST-BOOT] ELF entry=0x' + elf.entry.toString(16) +
    ' segments=' + elf.segs.length);

  const TOTAL_RAM = 0x100000 + highRamSize;

  // Load ELF segments into guest RAM via PGMPhysWrite (PGM handles
  // non-contiguous page chunk mapping correctly, unlike direct mf.set)
  let segsLoaded = 0;
  for (let si = 0; si < elf.segs.length; si++) {
    const seg = elf.segs[si];
    const pa = Number(seg.paddr);
    const fits = (pa >= 0x100000 && pa + seg.memsz <= TOTAL_RAM);
    console.error('[FAST-BOOT] seg[' + si + '] paddr=0x' + pa.toString(16) +
      ' vaddr=0x' + seg.vaddr.toString(16) +
      ' filesz=' + seg.filesz + ' memsz=' + seg.memsz +
      (fits ? ' PGM-LOAD' : ' SKIP(oom)'));
    if (fits) {
      if (seg.filesz > 0) {
        // Source: vmlinuxPtr + seg.offset in Wasm memory
        const srcPtr = BigInt(vmOff + seg.offset);
        const rcW = Module._wasmPGMPhysWrite(BigInt(pa), srcPtr, seg.filesz);
        if (rcW !== 0)
          console.error('[FAST-BOOT] PGMPhysWrite seg[' + si + '] FAILED rc=' + rcW);
      }
      if (seg.memsz > seg.filesz) {
        const rcZ = Module._wasmPGMPhysZero(BigInt(pa + seg.filesz), seg.memsz - seg.filesz);
        if (rcZ !== 0)
          console.error('[FAST-BOOT] PGMPhysZero seg[' + si + '] BSS FAILED rc=' + rcZ);
      }
      segsLoaded++;
    }
  }
  console.error('[FAST-BOOT] Loaded ' + segsLoaded + '/' + elf.segs.length + ' segments via PGM');

  // Free decompressed vmlinux buffer
  Module._free(BigInt(vmlinuxPtr));
  vmlinuxPtr = 0;

  // Refresh mem8 for page table and low-RAM writes
  mem8 = new Uint8Array(wasmMemory.buffer);
  const mf = new Uint8Array(wasmMemory.buffer);
  const dvf = new DataView(wasmMemory.buffer);

  // Build page tables in a temp buffer, then PGMPhysWrite to guest RAM
  const PT_GPA = 0x800000;
  const PT_SIZE = 0x5000; // 5 pages: PML4 + PDPT-lo + PDPT-hi + PD-lo + PD-hi
  const ptBuf = Module._malloc(PT_SIZE);
  if (!ptBuf) {
    console.error('[FAST-BOOT] malloc failed for page tables');
    return 0;
  }
  {
    const ptOff = Number(ptBuf);
    const ptMem = new Uint8Array(wasmMemory.buffer);
    const ptDv = new DataView(wasmMemory.buffer);
    // Clear
    ptMem.fill(0, ptOff, ptOff + PT_SIZE);
    const P = 1, W = 2, PS = 0x80;
    // PML4[0] → PDPT-low, PML4[511] → PDPT-high
    ptDv.setBigUint64(ptOff, BigInt(PT_GPA + 0x1000) | BigInt(P|W), true);
    ptDv.setBigUint64(ptOff + 511*8, BigInt(PT_GPA + 0x2000) | BigInt(P|W), true);
    // PDPT-low[0] → PD-low
    ptDv.setBigUint64(ptOff + 0x1000, BigInt(PT_GPA + 0x3000) | BigInt(P|W), true);
    // PDPT-high[510] → PD-high (0xFFFFFFFF80000000 = PML4[511]:PDPT[510])
    ptDv.setBigUint64(ptOff + 0x2000 + 510*8, BigInt(PT_GPA + 0x4000) | BigInt(P|W), true);
    // PD entries: identity-map with 2MB pages
    const nPages = Math.min(512, Math.ceil(TOTAL_RAM / 0x200000));
    for (let i = 0; i < nPages; i++) {
      ptDv.setBigUint64(ptOff + 0x3000 + i*8, BigInt(i * 0x200000) | BigInt(P|W|PS), true);
      ptDv.setBigUint64(ptOff + 0x4000 + i*8, BigInt(i * 0x200000) | BigInt(P|W|PS), true);
    }
    // Write page tables to guest RAM via PGM
    const rcPT = Module._wasmPGMPhysWrite(BigInt(PT_GPA), BigInt(ptBuf), PT_SIZE);
    if (rcPT !== 0)
      console.error('[FAST-BOOT] PGMPhysWrite page tables FAILED rc=' + rcPT);
  }
  Module._free(BigInt(ptBuf));
  const cr3val = PT_GPA;
  console.error('[FAST-BOOT] Page tables written to GPA 0x' + cr3val.toString(16) + ' via PGM');

  // Determine where boot_params actually lives (where HdrS was found)
  const bpGPA = (setupBase - rb); // 0x90000 or 0x10000
  console.log('[FAST-BOOT-JS] boot_params at GPA 0x' + bpGPA.toString(16));

  // Copy boot_params to 0x10000 if they're at 0x90000
  if (bpGPA !== 0x10000) {
    for (let i = 0; i < 4096; i++) mf[rb + 0x10000 + i] = mf[setupBase + i];
  }
  // boot_params are now at 0x10000

  // Ensure serial console in command line
  const cmdPtr = mf[rb+0x10000+0x228]|(mf[rb+0x10000+0x229]<<8)|
    (mf[rb+0x10000+0x22A]<<16)|(mf[rb+0x10000+0x22B]<<24);
  if (cmdPtr > 0 && cmdPtr < 0xA0000) {
    let cmdLen = 0;
    for (let i = 0; i < 256; i++) {
      if (mf[rb + cmdPtr + i] === 0) { cmdLen = i; break; }
    }
    for (let i = 0; i < cmdLen; i++) mf[rb + 0x99000 + i] = mf[rb + cmdPtr + i];
    const serialOpts = ' console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 loglevel=4 idle=halt notsc clocksource=jiffies acpi=off nopti nospectre_v1 nospectre_v2 pci=lastbus=0 raid=noautodetect mitigations=off notrace lpj=100';
    for (let i = 0; i < serialOpts.length; i++)
      mf[rb + 0x99000 + cmdLen + i] = serialOpts.charCodeAt(i);
    mf[rb + 0x99000 + cmdLen + serialOpts.length] = 0;
    dvf.setUint32(rb + 0x10000 + 0x228, 0x99000, true);
  }

  // Log ramdisk info from boot_params for diagnostics
  const rdImg = mf[rb+0x10000+0x218]|(mf[rb+0x10000+0x219]<<8)|
    (mf[rb+0x10000+0x21A]<<16)|(mf[rb+0x10000+0x21B]<<24);
  const rdSz = mf[rb+0x10000+0x21C]|(mf[rb+0x10000+0x21D]<<8)|
    (mf[rb+0x10000+0x21E]<<16)|(mf[rb+0x10000+0x21F]<<24);
  console.error('[FAST-BOOT] boot_params ramdisk_image=0x' + rdImg.toString(16) +
    ' ramdisk_size=0x' + rdSz.toString(16) + ' (' + (rdSz >> 10) + 'KB)');

  // Write D64B metadata at guest 0x7200
  dvf.setUint32(rb + 0x7200, 0x42343644, true); // "D64B"
  dvf.setUint32(rb + 0x7204, cr3val, true);      // CR3
  dvf.setBigUint64(rb + 0x7208, elf.entry, true); // entry point (64-bit)
  dvf.setUint32(rb + 0x7210, 0x10000, true);     // boot_params GPA

  // Write GDT at guest 0x7300
  dvf.setBigUint64(rb + 0x7300, 0n, true);                         // null
  dvf.setBigUint64(rb + 0x7308, 0x00CF9A000000FFFFn, true);       // 32-bit code
  dvf.setBigUint64(rb + 0x7310, 0x00AF9B000000FFFFn, true);       // 64-bit code
  dvf.setBigUint64(rb + 0x7318, 0x00CF93000000FFFFn, true);       // data

  console.log('[FAST-BOOT-JS] Ready! entry=0x' + elf.entry.toString(16) +
    ' CR3=0x' + cr3val.toString(16) + ' boot_params=0x10000');
  return 1;
}

// ── Public API ──
return {
  _a20: 1, // default: A20 enabled; updated by wasmJitSetA20 EM_JS before each call
  execBlock: execBlockWrapped,
  init: init,
  setRomBuffer: setRomBuffer,
  setHighRAM: setHighRAM,
  setPortIO: function(inFn, outFn) { portInFn = inFn; portOutFn = outFn; },
  getStats: function() { return { totalInsns: statTotalInsns, totalCalls: statTotalCalls, fallbacks: statFallbacks }; },
  _getHighRAM: function() { return { ptr: highRamPtr, size: highRamSize, end: highRamEnd }; },
  fastBootDecompress: fastBootDecompress
};

})(); // end VBoxJIT IIFE
