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
const SEG_BASE=8, SEG_LIMIT=16, SEG_ATTR=20, SEG_SEL=0;
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

// MMIO fault flag: set when a guest memory access goes to an MMIO address
// (outside Wasm linear memory). The main loop checks this flag and bails
// to IEM so the instruction is re-executed via the PGM MMIO handler.
let mmioFault = false;

// Read byte from guest physical address, ROM-aware
function guestRb(addr) {
  if (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd)
    return mem8[romBufBase + (addr - romGCPhysStart)];
  const off = ramBase + addr;
  if (off >= mem8.length) { mmioFault = true; return 0xFF; }
  return mem8[off];
}

// Read word from guest physical address, ROM-aware
function guestRw(addr) {
  if (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd) {
    const off = romBufBase + (addr - romGCPhysStart);
    return dv.getUint16(off, true);
  }
  const off = ramBase + addr;
  if (off + 2 > mem8.length) { mmioFault = true; return 0xFFFF; }
  return dv.getUint16(off, true);
}

// Read dword from guest physical address, ROM-aware
function guestRd(addr) {
  if (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd) {
    const off = romBufBase + (addr - romGCPhysStart);
    return dv.getUint32(off, true);
  }
  const off = ramBase + addr;
  if (off + 4 > mem8.length) { mmioFault = true; return 0xFFFFFFFF; }
  return dv.getUint32(off, true);
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

// Segment base (cached)
function segBase(segOff) { return Number(dv.getBigUint64(cpuPtr + segOff + SEG_BASE, true)); }

// Guest physical memory read/write (ROM-aware for reads, paging-aware)
function rb(addr) {
  if (_pagingOn) {
    addr = translateLinear(addr >>> 0);
    if (addr < 0) { mmioFault = true; return 0xFF; }
  }
  return guestRb(addr);
}
function rw(addr) {
  if (_pagingOn) {
    addr = translateLinear(addr >>> 0);
    if (addr < 0) { mmioFault = true; return 0xFFFF; }
  }
  return guestRw(addr);
}
function rd(addr) {
  if (_pagingOn) {
    addr = translateLinear(addr >>> 0);
    if (addr < 0) { mmioFault = true; return 0xFFFFFFFF; }
  }
  return guestRd(addr);
}
function wb(addr, v) {
  if (_pagingOn) {
    addr = translateLinear(addr >>> 0);
    if (addr < 0) { mmioFault = true; return; }
  }
  // VGA memory range: bail to IEM so VGA device sees writes via PGM MMIO handler
  if (addr >= 0xA0000 && addr < 0xC0000) { mmioFault = true; return; }
  const off = ramBase + addr;
  if (off >= mem8.length) { mmioFault = true; return; }
  mem8[off] = v;
}
function ww(addr, v) {
  if (_pagingOn) {
    addr = translateLinear(addr >>> 0);
    if (addr < 0) { mmioFault = true; return; }
  }
  if (addr >= 0xA0000 && addr < 0xC0000) { mmioFault = true; return; }
  const off = ramBase + addr;
  if (off + 2 > mem8.length) { mmioFault = true; return; }
  dv.setUint16(off, v & 0xFFFF, true);
}
function wd(addr, v) {
  if (_pagingOn) {
    addr = translateLinear(addr >>> 0);
    if (addr < 0) { mmioFault = true; return; }
  }
  if (addr >= 0xA0000 && addr < 0xC0000) { mmioFault = true; return; }
  const off = ramBase + addr;
  if (off + 4 > mem8.length) { mmioFault = true; return; }
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
    sr32(4, esp);
    ww(ssBase + esp, v);
  } else {
    let sp = (gr16(4) - 2) & 0xFFFF;
    sr16(4, sp);
    ww(ssBase + sp, v);
  }
}
function pop16(ssBase) {
  if (_ssBig) {
    const esp = gr32(4);
    const v = rw(ssBase + esp);
    sr32(4, (esp + 2) >>> 0);
    return v;
  } else {
    const sp = gr16(4);
    const v = rw(ssBase + sp);
    sr16(4, (sp + 2) & 0xFFFF);
    return v;
  }
}
function push32(v, ssBase) {
  if (_ssBig) {
    let esp = (gr32(4) - 4) >>> 0;
    sr32(4, esp);
    wd(ssBase + esp, v);
  } else {
    let sp = (gr16(4) - 4) & 0xFFFF;
    sr16(4, sp);
    wd(ssBase + sp, v);
  }
}
function pop32(ssBase) {
  if (_ssBig) {
    const esp = gr32(4);
    const v = rd(ssBase + esp);
    sr32(4, (esp + 4) >>> 0);
    return v;
  } else {
    const sp = gr16(4);
    const v = rd(ssBase + sp);
    sr16(4, (sp + 4) & 0xFFFF);
    return v;
  }
}

// ── 32-bit paging support ──
let _pagingOn = false;

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
  const pdeOff = ramBase + pdeAddr;
  if (pdeOff + 4 > mem8.length) return -1;
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
  const pteOff = ramBase + pteAddr;
  if (pteOff + 4 > mem8.length) return -1;
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
  const pdeOff = ramBase + pdBase + pdeIdx * 8;
  if (pdeOff + 4 > mem8.length) return -1;
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
  const pteOff = ramBase + ptBase + pteIdx * 8;
  if (pteOff + 4 > mem8.length) return -1;
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
function execBlock(cpuP, ramB, maxInsn) {
  cpuPtr = cpuP;
  ramBase = ramB;
  refreshViews();

  // Load frequently-used state
  let flags = rr32(R_FLAGS);
  let csBase = segBase(S_CS);
  let dsBase = segBase(S_DS);
  let ssBase = segBase(S_SS);
  let esBase = segBase(S_ES);

  // CR0: check PE and PG
  const cr0 = rr32(R_CR0);
  const protMode = !!(cr0 & 1);        // CR0.PE
  const pagingOn = !!(cr0 & 0x80000000); // CR0.PG
  _pagingOn = pagingOn;
  const realMode = !protMode;

  // Flush TLB on CR3 change
  if (pagingOn) {
    const currentCR3 = rr32(R_CR3);
    if (currentCR3 !== lastCR3) { tlbFlush(); lastCR3 = currentCR3; }
  }

  // CS descriptor D bit: determines default operand/address size in protected mode
  const csAttr = rr32(S_CS + SEG_ATTR);
  const csDefBig = protMode && !!(csAttr & X86DESCATTR_D);
  const ipMask = csDefBig ? 0xFFFFFFFF : 0xFFFF;

  // SS.B for stack operations (ESP vs SP)
  const ssAttr = rr32(S_SS + SEG_ATTR);
  _ssBig = protMode && !!(ssAttr & X86DESCATTR_D);

  let ip = csDefBig ? rr32(R_IP) : rr16(R_IP);

  // Initialize lazy flags from current RFLAGS
  loadFlags(flags);
  lazySize = csDefBig ? 4 : 2; // default operand size

  // Linear PC for diagnostics and ROM checks
  const linearPC = csBase + ip;

  // Track code segment range for self-modifying code detection.
  // REP STOSB/MOVSB can overwrite the current code segment (e.g. BIOS memory
  // test). We detect this and bail to IEM rather than executing corrupted code.
  const codeSegStart = csBase;
  const csLimit = rr32(S_CS + SEG_LIMIT);
  const codeSegEnd = csDefBig ? (csBase + Math.min(csLimit + 1, 0x100000000)) : (csBase + 0x10000);

  // Helper to write IP back to CPUMCTX with correct width
  function wrIP(val) {
    if (csDefBig) wr32(R_IP, val & 0xFFFFFFFF);
    else wr16(R_IP, val & 0xFFFF);
  }

  let executed = 0;
  let lastBailOp = -1; // track the opcode that caused early exit
  const ramSize = mem8.length - ramBase; // available RAM

  // Pre-read a chunk of code for fast access
  let codeLinear = csBase + ip;
  let codePhys;
  // Check if address is in accessible range.
  // VirtualBox's PGM stores ROM (0xC0000-0xFFFFF) and MMIO (0xA0000-0xBFFFF)
  // via page handlers — these addresses are NOT in the flat RAM buffer (they read as 0).
  // Only execute from flat RAM (< 0xA0000) or the ROM buffer (when initialized).
  const addrAccessible = (addr) => {
    if (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd) return true;
    // With paging, physical addresses can be anywhere in RAM (not just < 0xA0000).
    // Without paging, linear=physical and only flat RAM below MMIO hole is usable.
    if (pagingOn) return addr >= 0 && addr + 16 <= ramSize && !(addr >= 0xA0000 && addr < 0xC0000);
    return addr >= 0 && addr < 0xA0000 && addr + 16 <= ramSize;
  };
  const inRomRange = (addr) => romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd;
  if (pagingOn) {
    codePhys = translateLinear(codeLinear >>> 0);
    if (codePhys < 0) return 0;
  } else {
    codePhys = codeLinear;
  }
  if (!addrAccessible(codePhys)) {
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
    if (codePhys < 0 || (!addrAccessible(codePhys))) break; // safety

    // Near page boundary: instruction might span two pages — bail to IEM
    if (pagingOn && (codePhys & 0xFFF) > 0xFF0) {
      executed = executed || 0;
      break;
    }

    // Read up to 15 bytes of instruction (ROM-aware)
    const c0 = guestRb(codePhys);

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

    // Code bytes after prefixes — use ROM buffer if in ROM range
    const inROM = (codePhys >= romGCPhysStart && codePhys < romGCPhysEnd);
    const ci = inROM ? (romBufBase + (codePhys - romGCPhysStart) + pos)
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
      if (wasIF0 && executed > 0) {
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
      const logLimit = isVgaPort ? 200 : (isAtaStatus ? 5 : 30);
      if (cnt <= logLimit || cnt % 10000 === 0) {
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
            const addr = esBase + di;
            // Compute range for both forward and backward
            const addrLo = dir === 1 ? addr : addr - byteCount + 1;
            const addrHi = dir === 1 ? addr + byteCount : addr + 1;
            if (addrLo >= 0 && addrHi <= ramSize && (addrHi <= 0xA0000 || addrLo >= 0xC0000)) {
              // Fast path: all in RAM (not MMIO 0xA0000-0xBFFFF)
              if (codeSegStart < 0xC0000 && addrHi > codeSegStart && addrLo < codeSegEnd) {
                ilen = 0; iter = maxInsn; break;
              }
              mem8.fill(val, ramBase + addrLo, ramBase + addrHi);
              di = (di + dir * byteCount) & aMask;
              cx = origCx - byteCount;
            } else {
              // Slow path — bail to IEM for MMIO-touching REP STOS
              // (letting the loop run corrupts DI/CX when wb sets mmioFault)
              lastBailOp = b; iter = maxInsn; break;
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
            const addr = esBase + di;
            const addrLo = dir === 1 ? addr : addr - totalBytes + sz;
            const addrHi = dir === 1 ? addr + totalBytes : addr + sz;
            if (addrLo >= 0 && addrHi <= ramSize && (addrHi <= 0xA0000 || addrLo >= 0xC0000)) {
              // Fast path: all in RAM (not MMIO)
              if (codeSegStart < 0xC0000 && addrHi > codeSegStart && addrLo < codeSegEnd) {
                ilen = 0; iter = maxInsn; break;
              }
              const physLo = ramBase + addrLo;
              if (v === 0) {
                // Most common case: zeroing memory — single native fill
                if (totalBytes >= 1024 && statFastFillLogs < 5) { statFastFillLogs++; console.log('[JIT] REP STOS fast-fill zero ' + totalBytes + ' bytes at 0x' + addrLo.toString(16)); }
                mem8.fill(0, physLo, physLo + totalBytes);
              } else {
                // Non-zero fill: write one unit, then exponential copyWithin doubling
                if (sz === 2) dv.setUint16(physLo, v, true);
                else dv.setUint32(physLo, v >>> 0, true);
                let filled = sz;
                while (filled < totalBytes) {
                  const chunk = Math.min(filled, totalBytes - filled);
                  mem8.copyWithin(physLo + filled, physLo, physLo + chunk);
                  filled += chunk;
                }
              }
              di = (di + dir * totalBytes) & aMask;
              cx = origCxAB - cx;
            } else {
              // Slow path — bail to IEM for MMIO-touching REP STOS
              lastBailOp = b; iter = maxInsn; break;
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
            // Both src and dst must be in RAM (not MMIO 0xA0000-0xBFFFF, not ROM)
            const srcInRam = srcLo >= 0 && srcHi <= ramSize && (srcHi <= 0xA0000 || srcLo >= 0xC0000);
            const dstInRam = dstLo >= 0 && dstHi <= ramSize && (dstHi <= 0xA0000 || dstLo >= 0xC0000);
            if (srcInRam && dstInRam) {
              if (codeSegStart < 0xC0000 && dstHi > codeSegStart && dstLo < codeSegEnd) {
                ilen = 0; iter = maxInsn; break;
              }
              // copyWithin handles overlapping regions correctly
              mem8.copyWithin(ramBase + dstLo, ramBase + srcLo, ramBase + srcHi);
              si = (si + dir * byteCount) & aMask;
              di = (di + dir * byteCount) & aMask;
              cx = origCxA4 - byteCount;
            } else {
              // Slow path — bail to IEM for MMIO-touching REP MOVS
              lastBailOp = b; iter = maxInsn; break;
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
            const srcOk5 = srcLo5 >= 0 && srcHi5 <= ramSize && (srcHi5 <= 0xA0000 || srcLo5 >= 0xC0000);
            const dstOk5 = dstLo5 >= 0 && dstHi5 <= ramSize && (dstHi5 <= 0xA0000 || dstLo5 >= 0xC0000);
            if (srcOk5 && dstOk5) {
              if (codeSegStart < 0xC0000 && dstHi5 > codeSegStart && dstLo5 < codeSegEnd) {
                ilen = 0; iter = maxInsn; break;
              }
              mem8.copyWithin(ramBase + dstLo5, ramBase + srcLo5, ramBase + srcHi5);
              si = (si + dir * totalBytes5) & aMask;
              di = (di + dir * totalBytes5) & aMask;
              cx = origCxA5 - cx;
            } else {
              // Slow path — bail to IEM for MMIO-touching REP MOVS
              lastBailOp = b; iter = maxInsn; break;
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
        case 0xA0: push16(rr16(S_FS + SEG_SEL), ssBase); break;
        case 0xA1: {
          if (!realMode) { lastBailOp = 0x0F00 | b2; iter = maxInsn; break; }
          const s = pop16(ssBase); wr16(S_FS + SEG_SEL, s); wr64(S_FS + SEG_BASE, s << 4); break;
        }
        case 0xA8: push16(rr16(S_GS + SEG_SEL), ssBase); break;
        case 0xA9: {
          if (!realMode) { lastBailOp = 0x0F00 | b2; iter = maxInsn; break; }
          const s = pop16(ssBase); wr16(S_GS + SEG_SEL, s); wr64(S_GS + SEG_BASE, s << 4); break;
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
    case 0x06: push16(rr16(S_ES + SEG_SEL), ssBase); ilen += 1; break;
    case 0x0E: push16(rr16(S_CS + SEG_SEL), ssBase); ilen += 1; break;
    case 0x16: push16(rr16(S_SS + SEG_SEL), ssBase); ilen += 1; break;
    case 0x1E: push16(rr16(S_DS + SEG_SEL), ssBase); ilen += 1; break;

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
      // Bail to IEM for video BIOS (INT 10h) — needs MMIO for VGA memory writes
      if (intNum === 0x10) {
        if (!execBlock._int10Log) execBlock._int10Log = { count: 0 };
        const cnt = ++execBlock._int10Log.count;
        const ah = (gr16(0) >> 8) & 0xFF;
        if (cnt <= 100)
          console.log('[INT10] AH=0x' + ah.toString(16).padStart(2,'0') +
            ' AL=0x' + (gr16(0)&0xFF).toString(16).padStart(2,'0') +
            ' BX=0x' + gr16(3).toString(16).padStart(4,'0') +
            ' @' + (csBase>>>4).toString(16) + ':' + ip.toString(16) +
            ' #' + cnt);
        lastBailOp = b; iter = maxInsn; break;
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

function execBlockWrapped(cpuP, ramB, maxInsn) {
  statTotalCalls++;
  // Per-call diagnostics for first 20 calls, then every 100000
  if (statTotalCalls <= 20 || (statTotalCalls % 100000) === 0) {
    const cpuN = Number(cpuP), ramN = Number(ramB);
    console.log('[JIT-DBG] call#' + statTotalCalls +
      ' cpuPtr=0x' + cpuN.toString(16) +
      ' ramBase=0x' + ramN.toString(16) +
      ' romBufSize=' + romBufSize +
      ' maxInsn=' + maxInsn);
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
  const n = execBlock(cpuP, ramB, maxInsn);
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
    }
  }
  return n;
}

// ── Public API ──
return {
  execBlock: execBlockWrapped,
  init: init,
  setRomBuffer: setRomBuffer,
  setPortIO: function(inFn, outFn) { portInFn = inFn; portOutFn = outFn; },
  getStats: function() { return { totalInsns: statTotalInsns, totalCalls: statTotalCalls, fallbacks: statFallbacks }; }
};

})(); // end VBoxJIT IIFE
