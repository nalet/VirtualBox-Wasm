// include: shell.js
// include: minimum_runtime_check.js
// end include: minimum_runtime_check.js
// The Module object: Our interface to the outside world. We import
// and export values on it. There are various ways Module can be used:
// 1. Not defined. We create it here
// 2. A function parameter, function(moduleArg) => Promise<Module>
// 3. pre-run appended it, var Module = {}; ..generated code..
// 4. External script tag defines var Module.
// We need to check if Module already exists (e.g. case 3 above).
// Substitution will be replaced with actual code on later stage of the build,
// this way Closure Compiler will not mangle it (e.g. case 4. above).
// Note that if you want to run closure, and also to use Module
// after the generated code, you will need to define   var Module = {};
// before the code. Then that object will be used in the code, and you
// can continue to use Module afterwards as well.
var Module = typeof Module != "undefined" ? Module : {};

// Determine the runtime environment we are in. You can customize this by
// setting the ENVIRONMENT setting at compile time (see settings.js).
// Attempt to auto-detect the environment
var ENVIRONMENT_IS_WEB = !!globalThis.window;

var ENVIRONMENT_IS_WORKER = !!globalThis.WorkerGlobalScope;

// N.b. Electron.js environment is simultaneously a NODE-environment, but
// also a web environment.
var ENVIRONMENT_IS_NODE = globalThis.process?.versions?.node && globalThis.process?.type != "renderer";

var ENVIRONMENT_IS_SHELL = !ENVIRONMENT_IS_WEB && !ENVIRONMENT_IS_NODE && !ENVIRONMENT_IS_WORKER;

// Three configurations we can be running in:
// 1) We could be the application main() thread running in the main JS UI thread. (ENVIRONMENT_IS_WORKER == false and ENVIRONMENT_IS_PTHREAD == false)
// 2) We could be the application main() running directly in a worker. (ENVIRONMENT_IS_WORKER == true, ENVIRONMENT_IS_PTHREAD == false)
// 3) We could be an application pthread running in a worker. (ENVIRONMENT_IS_WORKER == true and ENVIRONMENT_IS_PTHREAD == true)
// The way we signal to a worker that it is hosting a pthread is to construct
// it with a specific name.
var ENVIRONMENT_IS_PTHREAD = ENVIRONMENT_IS_WORKER && globalThis.name == "em-pthread";

// --pre-jses are emitted after the Module integration code, so that they can
// refer to Module (if they choose; they can also define Module)
// include: jit-pre.js
// jit-pre.js — x86 fast interpreter for VirtualBox/Wasm
// Embedded via --pre-js. Runs in all threads (main + workers).
// Called from EM_JS hook in IEM execution loop.
// V8/SpiderMonkey JIT-compiles this interpreter into fast native code.
"use strict";

globalThis.VBoxJIT = (function() {
  // ── CPUMCTX register offsets (from cpumctx-x86-amd64.h) ──
  const R_AX = 0, R_CX = 8, R_DX = 16, R_BX = 24, R_SP = 32, R_BP = 40, R_SI = 48, R_DI = 56;
  const R_IP = 320, R_FLAGS = 328;
  // Segment registers: each 24 bytes (sel[2],pad[2],validSel[2],flags[2],base[8],limit[4],attr[4])
  const S_ES = 128, S_CS = 152, S_SS = 176, S_DS = 200, S_FS = 224, S_GS = 248;
  const SEG_BASE = 8, SEG_LIMIT = 16, SEG_ATTR = 20, SEG_SEL = 0;
  // CR0
  const R_CR0 = 352;
  // ── Lazy flags ──
  const OP_NONE = 0, OP_ADD = 1, OP_SUB = 2, OP_AND = 3, OP_OR = 4, OP_XOR = 5, OP_INC = 6, OP_DEC = 7, OP_SHL = 8, OP_SHR = 9, OP_SAR = 10, OP_ROL = 11, OP_ROR = 12;
  let lazyOp = OP_NONE, lazyRes = 0, lazyOp1 = 0, lazyOp2 = 0, lazySize = 16, lazyCF = 0;
  // Parity lookup (even parity = 1)
  const parityTable = new Uint8Array(256);
  for (let i = 0; i < 256; i++) {
    let b = i, p = 0;
    for (let j = 0; j < 8; j++) {
      p ^= (b & 1);
      b >>= 1;
    }
    parityTable[i] = p ? 0 : 1;
  }
  const SIZE_MASK = [ 0, 255, 65535, 0, 4294967295 ];
  const SIZE_SIGN = [ 0, 128, 32768, 0, 2147483648 ];
  function getCF() {
    switch (lazyOp) {
     case OP_NONE:
      return lazyCF;

     case OP_ADD:
      return (lazyRes & SIZE_MASK[lazySize]) < (lazyOp1 & SIZE_MASK[lazySize]) ? 1 : 0;

     case OP_SUB:
      return (lazyOp1 & SIZE_MASK[lazySize]) < (lazyOp2 & SIZE_MASK[lazySize]) ? 1 : 0;

     case OP_AND:
     case OP_OR:
     case OP_XOR:
      return 0;

     case OP_INC:
     case OP_DEC:
      return lazyCF;

     // unchanged
      case OP_SHL:
      return lazyCF;

     case OP_SHR:
     case OP_SAR:
      return lazyCF;

     default:
      return lazyCF;
    }
  }
  function getZF() {
    return ((lazyRes & SIZE_MASK[lazySize]) === 0) ? 1 : 0;
  }
  function getSF() {
    return ((lazyRes & SIZE_SIGN[lazySize]) !== 0) ? 1 : 0;
  }
  function getOF() {
    const m = SIZE_MASK[lazySize], s = SIZE_SIGN[lazySize];
    switch (lazyOp) {
     case OP_NONE:
      return 0;

     case OP_ADD:
      return ((~(lazyOp1 ^ lazyOp2) & (lazyOp1 ^ lazyRes)) & s) ? 1 : 0;

     case OP_SUB:
      return (((lazyOp1 ^ lazyOp2) & (lazyOp1 ^ lazyRes)) & s) ? 1 : 0;

     case OP_AND:
     case OP_OR:
     case OP_XOR:
      return 0;

     case OP_INC:
      return ((lazyRes & m) === (s & m)) ? 1 : 0;

     // overflow if result is 0x80..
      case OP_DEC:
      return ((lazyRes & m) === ((s - 1) & m)) ? 1 : 0;

     default:
      return 0;
    }
  }
  function getPF() {
    return parityTable[lazyRes & 255];
  }
  function getAF() {
    if (lazyOp === OP_ADD || lazyOp === OP_SUB || lazyOp === OP_INC || lazyOp === OP_DEC) return ((lazyOp1 ^ lazyOp2 ^ lazyRes) & 16) ? 1 : 0;
    return 0;
  }
  function flagsToWord() {
    return (getCF()) | (getPF() << 2) | (getAF() << 4) | (getZF() << 6) | (getSF() << 7) | (getOF() << 11) | 2;
  }
  function loadFlags(val) {
    lazyOp = OP_NONE;
    lazyCF = val & 1;
    // Reconstruct lazy state from explicit flags
    lazyRes = 0;
    if (!(val & 64)) lazyRes = 1;
    // ZF=0 means result != 0
    if (val & 128) lazyRes |= SIZE_SIGN[lazySize];
  }
  function setFlagsArith(op, res, op1, op2, size) {
    lazyOp = op;
    lazyRes = res;
    lazyOp1 = op1;
    lazyOp2 = op2;
    lazySize = size;
  }
  // ── Memory access helpers ──
  let mem8, dv;
  let cpuPtr = 0, ramBase = 0;
  // ROM overlay buffer (set by C++ via wasmJitSetRomBuffer)
  let romBufBase = 0;
  // offset in Wasm linear memory
  let romBufSize = 0;
  // size in bytes (256KB)
  let romGCPhysStart = 0;
  // guest physical start (0xC0000)
  let romGCPhysEnd = 0;
  // guest physical end (0x100000)
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
    console.log("[JIT] ROM buffer set: base=0x" + bufPtr.toString(16) + " size=" + (bufSize / 1024) + "KB range=0x" + gcPhysStart.toString(16) + "-0x" + romGCPhysEnd.toString(16));
  }
  // Read byte from guest physical address, ROM-aware
  function guestRb(addr) {
    if (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd) return mem8[romBufBase + (addr - romGCPhysStart)];
    return mem8[ramBase + addr];
  }
  // Read word from guest physical address, ROM-aware
  function guestRw(addr) {
    if (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd) {
      const off = romBufBase + (addr - romGCPhysStart);
      return dv.getUint16(off, true);
    }
    return dv.getUint16(ramBase + addr, true);
  }
  // Read dword from guest physical address, ROM-aware
  function guestRd(addr) {
    if (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd) {
      const off = romBufBase + (addr - romGCPhysStart);
      return dv.getUint32(off, true);
    }
    return dv.getUint32(ramBase + addr, true);
  }
  // Read CPU register (64-bit, return as Number — safe for 32-bit values)
  function rr64(off) {
    return Number(dv.getBigUint64(cpuPtr + off, true));
  }
  function wr64(off, v) {
    dv.setBigUint64(cpuPtr + off, BigInt(v) & 18446744073709551615n, true);
  }
  function rr32(off) {
    return dv.getUint32(cpuPtr + off, true);
  }
  function wr32(off, v) {
    dv.setUint32(cpuPtr + off, v >>> 0, true);
  }
  function rr16(off) {
    return dv.getUint16(cpuPtr + off, true);
  }
  function wr16(off, v) {
    dv.setUint16(cpuPtr + off, v & 65535, true);
  }
  function rr8(off) {
    return dv.getUint8(cpuPtr + off);
  }
  function wr8(off, v) {
    dv.setUint8(cpuPtr + off, v & 255);
  }
  // Segment base (cached)
  function segBase(segOff) {
    return Number(dv.getBigUint64(cpuPtr + segOff + SEG_BASE, true));
  }
  // Guest physical memory read/write (ROM-aware for reads)
  function rb(addr) {
    return guestRb(addr);
  }
  function rw(addr) {
    return guestRw(addr);
  }
  function rd(addr) {
    return guestRd(addr);
  }
  function wb(addr, v) {
    mem8[ramBase + addr] = v;
  }
  function ww(addr, v) {
    dv.setUint16(ramBase + addr, v & 65535, true);
  }
  function wd(addr, v) {
    dv.setUint32(ramBase + addr, v >>> 0, true);
  }
  // ── GPR access by index ──
  const GPR_OFFS = [ R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI ];
  function gr16(idx) {
    return rr16(GPR_OFFS[idx]);
  }
  function sr16(idx, v) {
    wr16(GPR_OFFS[idx], v);
  }
  function gr32(idx) {
    return rr32(GPR_OFFS[idx]);
  }
  function sr32(idx, v) {
    wr32(GPR_OFFS[idx], v);
  }
  // 8-bit: 0-3 = AL,CL,DL,BL; 4-7 = AH,CH,DH,BH
  function gr8(idx) {
    if (idx < 4) return rr8(GPR_OFFS[idx]);
    return rr8(GPR_OFFS[idx - 4] + 1);
  }
  function sr8(idx, v) {
    if (idx < 4) wr8(GPR_OFFS[idx], v); else wr8(GPR_OFFS[idx - 4] + 1, v);
  }
  // ── Segment register access by index ──
  const SEG_OFFS = [ S_ES, S_CS, S_SS, S_DS, S_FS, S_GS, 0, 0 ];
  // SREG encoding: 0=ES,1=CS,2=SS,3=DS,4=FS,5=GS
  // ── Port I/O callback (set by C++ side) ──
  let portInFn = null, portOutFn = null;
  function portIn(port, size) {
    if (portInFn) return portInFn(port, size);
    return 255;
  }
  function portOut(port, size, val) {
    if (portOutFn) portOutFn(port, size, val);
  }
  // ── ModR/M decoding (16-bit addressing) ──
  // Returns { ea: effective address (physical), disp: bytes consumed }
  function decodeModRM16(modrm, code, codeOff, dsBase, ssBase) {
    const mod = (modrm >> 6) & 3;
    const rm = modrm & 7;
    if (mod === 3) return {
      ea: -1,
      reg: rm,
      len: 0
    };
    // register operand
    let ea = 0, len = 0;
    switch (rm) {
     case 0:
      ea = (gr16(3) + gr16(6)) & 65535;
      break;

     // BX+SI
      case 1:
      ea = (gr16(3) + gr16(7)) & 65535;
      break;

     // BX+DI
      case 2:
      ea = (gr16(5) + gr16(6)) & 65535;
      break;

     // BP+SI
      case 3:
      ea = (gr16(5) + gr16(7)) & 65535;
      break;

     // BP+DI
      case 4:
      ea = gr16(6);
      break;

     // SI
      case 5:
      ea = gr16(7);
      break;

     // DI
      case 6:
      if (mod === 0) {
        ea = code[codeOff] | (code[codeOff + 1] << 8);
        len = 2;
      } else {
        ea = gr16(5);
      }
      break;

     case 7:
      ea = gr16(3);
      break;
    }
    // Default segment: SS for BP-based, DS for others
    let base = (rm === 2 || rm === 3 || (rm === 6 && mod !== 0)) ? ssBase : dsBase;
    if (mod === 1) {
      let d = code[codeOff + len];
      if (d > 127) d -= 256;
      // sign extend
      ea = (ea + d) & 65535;
      len += 1;
    } else if (mod === 2) {
      ea = (ea + (code[codeOff + len] | (code[codeOff + len + 1] << 8))) & 65535;
      len += 2;
    }
    return {
      ea: base + ea,
      len
    };
  }
  // ── ModR/M decoding (32-bit addressing) ──
  function decodeModRM32(modrm, code, codeOff, dsBase, ssBase) {
    const mod = (modrm >> 6) & 3;
    const rm = modrm & 7;
    if (mod === 3) return {
      ea: -1,
      reg: rm,
      len: 0
    };
    let ea = 0, len = 0;
    let base = dsBase;
    if (rm === 4) {
      // SIB byte
      const sib = code[codeOff];
      len = 1;
      const scale = (sib >> 6) & 3;
      const index = (sib >> 3) & 7;
      const sibBase = sib & 7;
      if (sibBase === 5 && mod === 0) {
        ea = code[codeOff + 1] | (code[codeOff + 2] << 8) | (code[codeOff + 3] << 16) | (code[codeOff + 4] << 24);
        len = 5;
      } else {
        ea = gr32(sibBase);
        if (sibBase === 4 || sibBase === 5) base = ssBase;
      }
      if (index !== 4) {
        ea = (ea + (gr32(index) << scale)) >>> 0;
      }
    } else if (rm === 5 && mod === 0) {
      ea = code[codeOff] | (code[codeOff + 1] << 8) | (code[codeOff + 2] << 16) | (code[codeOff + 3] << 24);
      len = 4;
    } else {
      ea = gr32(rm);
      if (rm === 4 || rm === 5) base = ssBase;
    }
    if (mod === 1) {
      let d = code[codeOff + len];
      if (d > 127) d -= 256;
      ea = (ea + d) >>> 0;
      len += 1;
    } else if (mod === 2) {
      ea = (ea + (code[codeOff + len] | (code[codeOff + len + 1] << 8) | (code[codeOff + len + 2] << 16) | (code[codeOff + len + 3] << 24))) >>> 0;
      len += 4;
    }
    return {
      ea: (base + ea) >>> 0,
      len
    };
  }
  // ── ALU operations ──
  function alu8(op, a, b) {
    let r;
    switch (op) {
     case 0:
      r = a + b;
      setFlagsArith(OP_ADD, r, a, b, 1);
      lazyCF = (r > 255) ? 1 : 0;
      return r & 255;

     case 1:
      r = a | b;
      setFlagsArith(OP_OR, r, a, b, 1);
      return r & 255;

     case 2:
      r = a + b + getCF();
      setFlagsArith(OP_ADD, r, a, b + getCF(), 1);
      lazyCF = (r > 255) ? 1 : 0;
      return r & 255;

     case 3:
      r = a - b - getCF();
      setFlagsArith(OP_SUB, r, a, b + getCF(), 1);
      lazyCF = (r < 0) ? 1 : 0;
      return ((r & 255) + 256) & 255;

     case 4:
      r = a & b;
      setFlagsArith(OP_AND, r, a, b, 1);
      return r & 255;

     case 5:
      r = a - b;
      setFlagsArith(OP_SUB, r, a, b, 1);
      lazyCF = (a < b) ? 1 : 0;
      return ((r & 255) + 256) & 255;

     case 6:
      r = a ^ b;
      setFlagsArith(OP_XOR, r, a, b, 1);
      return r & 255;

     case 7:
      r = a - b;
      setFlagsArith(OP_SUB, r, a, b, 1);
      lazyCF = (a < b) ? 1 : 0;
      return a;

     // CMP: don't store
      default:
      return a;
    }
  }
  function alu16(op, a, b) {
    a &= 65535;
    b &= 65535;
    let r;
    switch (op) {
     case 0:
      r = a + b;
      setFlagsArith(OP_ADD, r, a, b, 2);
      lazyCF = (r > 65535) ? 1 : 0;
      return r & 65535;

     case 1:
      r = a | b;
      setFlagsArith(OP_OR, r, a, b, 2);
      return r & 65535;

     case 2:
      {
        const c = getCF();
        r = a + b + c;
        setFlagsArith(OP_ADD, r, a, b + c, 2);
        lazyCF = (r > 65535) ? 1 : 0;
        return r & 65535;
      }

     case 3:
      {
        const c = getCF();
        r = a - b - c;
        setFlagsArith(OP_SUB, r, a, b + c, 2);
        lazyCF = (r < 0) ? 1 : 0;
        return r & 65535;
      }

     case 4:
      r = a & b;
      setFlagsArith(OP_AND, r, a, b, 2);
      return r;

     case 5:
      r = a - b;
      setFlagsArith(OP_SUB, r, a, b, 2);
      lazyCF = (a < b) ? 1 : 0;
      return r & 65535;

     case 6:
      r = a ^ b;
      setFlagsArith(OP_XOR, r, a, b, 2);
      return r;

     case 7:
      r = a - b;
      setFlagsArith(OP_SUB, r, a, b, 2);
      lazyCF = (a < b) ? 1 : 0;
      return a;

     default:
      return a;
    }
  }
  function alu32(op, a, b) {
    a = a >>> 0;
    b = b >>> 0;
    let r;
    switch (op) {
     case 0:
      r = (a + b) >>> 0;
      setFlagsArith(OP_ADD, r, a, b, 4);
      lazyCF = (r < a) ? 1 : 0;
      return r;

     case 1:
      r = (a | b) >>> 0;
      setFlagsArith(OP_OR, r, a, b, 4);
      return r;

     case 2:
      {
        const c = getCF();
        r = (a + b + c) >>> 0;
        setFlagsArith(OP_ADD, r, a, b + c, 4);
        lazyCF = (c ? r <= a : r < a) ? 1 : 0;
        return r;
      }

     case 3:
      {
        const c = getCF();
        r = (a - b - c) >>> 0;
        setFlagsArith(OP_SUB, r, a, b + c, 4);
        lazyCF = (c ? a <= b : a < b) ? 1 : 0;
        return r;
      }

     case 4:
      r = (a & b) >>> 0;
      setFlagsArith(OP_AND, r, a, b, 4);
      return r;

     case 5:
      r = (a - b) >>> 0;
      setFlagsArith(OP_SUB, r, a, b, 4);
      lazyCF = (a < b) ? 1 : 0;
      return r;

     case 6:
      r = (a ^ b) >>> 0;
      setFlagsArith(OP_XOR, r, a, b, 4);
      return r;

     case 7:
      r = (a - b) >>> 0;
      setFlagsArith(OP_SUB, r, a, b, 4);
      lazyCF = (a < b) ? 1 : 0;
      return a;

     default:
      return a;
    }
  }
  // ── Condition code testing ──
  function testCC(cc) {
    switch (cc) {
     case 0:
      return getOF();

     // O
      case 1:
      return !getOF();

     // NO
      case 2:
      return getCF();

     // B/C
      case 3:
      return !getCF();

     // AE/NC
      case 4:
      return getZF();

     // E/Z
      case 5:
      return !getZF();

     // NE/NZ
      case 6:
      return getCF() || getZF();

     // BE
      case 7:
      return !getCF() && !getZF();

     // A
      case 8:
      return getSF();

     // S
      case 9:
      return !getSF();

     // NS
      case 10:
      return getPF();

     // P
      case 11:
      return !getPF();

     // NP
      case 12:
      return getSF() !== getOF();

     // L
      case 13:
      return getSF() === getOF();

     // GE
      case 14:
      return getZF() || (getSF() !== getOF());

     // LE
      case 15:
      return !getZF() && (getSF() === getOF());
    }
    return false;
  }
  // ── Stack operations ──
  function push16(v, ssBase) {
    let sp = (gr16(4) - 2) & 65535;
    sr16(4, sp);
    ww(ssBase + sp, v);
  }
  function pop16(ssBase) {
    const sp = gr16(4);
    const v = rw(ssBase + sp);
    sr16(4, (sp + 2) & 65535);
    return v;
  }
  function push32(v, ssBase) {
    // In real mode, SP is always 16-bit
    let sp = (gr16(4) - 4) & 65535;
    sr16(4, sp);
    wd(ssBase + sp, v);
  }
  function pop32(ssBase) {
    const sp = gr16(4);
    const v = rd(ssBase + sp);
    sr16(4, (sp + 4) & 65535);
    return v;
  }
  // ═══════════════════════════════════════════════════════
  // MAIN INTERPRETER LOOP
  // ═══════════════════════════════════════════════════════
  // Returns: number of instructions executed (>0), or 0 for fallback needed
  // cpuP:    pointer to CPUMCTX in Wasm linear memory
  // ramB:    pointer to guest RAM base in Wasm linear memory
  // maxInsn: max instructions to execute before returning
  function execBlock(cpuP, ramB, maxInsn) {
    cpuPtr = cpuP;
    ramBase = ramB;
    refreshViews();
    // Load frequently-used state
    let ip = rr16(R_IP);
    // only low 16 bits for real mode
    let flags = rr32(R_FLAGS);
    const csBase = segBase(S_CS);
    let dsBase = segBase(S_DS);
    let ssBase = segBase(S_SS);
    let esBase = segBase(S_ES);
    // Initialize lazy flags from current RFLAGS
    lazyCF = flags & 1;
    lazyOp = OP_NONE;
    lazyRes = 0;
    if (!(flags & 64)) lazyRes = 1;
    // ZF
    if (flags & 128) lazyRes |= 32768;
    // SF
    lazySize = 2;
    // default 16-bit
    // Bail if executing in ROM space without a ROM buffer
    const linearPC = csBase + ip;
    if (linearPC >= 786432 && romBufSize === 0) return 0;
    // Check if we're in real mode or protected mode
    const cr0 = rr32(R_CR0);
    const realMode = !(cr0 & 1);
    // PE bit
    // For now, only handle real mode and simple protected mode (no paging)
    if (cr0 & 2147483648) {
      if (statTotalCalls < 5) console.log("[JIT] bail: paging enabled cr0=0x" + cr0.toString(16));
      return 0;
    }
    let executed = 0;
    let lastBailOp = -1;
    // track the opcode that caused early exit
    const ramSize = mem8.length - ramBase;
    // available RAM
    // Pre-read a chunk of code for fast access
    let codePhys = csBase + ip;
    // Check if address is in accessible range (RAM or ROM buffer)
    const addrAccessible = addr => addr + 16 <= ramSize || (romBufSize > 0 && addr >= romGCPhysStart && addr < romGCPhysEnd);
    if (!addrAccessible(codePhys)) {
      if (statTotalCalls < 5) console.log("[JIT] bail: codePhys=0x" + codePhys.toString(16) + " ramSize=0x" + ramSize.toString(16) + " ramBase=0x" + ramBase.toString(16) + " csBase=0x" + csBase.toString(16) + " ip=0x" + ip.toString(16));
      return 0;
    }
    for (let iter = 0; iter < maxInsn; iter++) {
      codePhys = csBase + ip;
      if (codePhys < 0 || (!addrAccessible(codePhys))) break;
      // safety
      // Read up to 15 bytes of instruction (ROM-aware)
      const c0 = guestRb(codePhys);
      // ── Prefix handling ──
      let segOverride = -1;
      // -1 = default
      let opSizeOverride = false;
      let addrSizeOverride = false;
      let repPrefix = 0;
      // 0=none, 0xF2=REPNE, 0xF3=REP/REPE
      let pos = 0;
      // bytes consumed for prefixes
      let b = c0;
      let scanning = true;
      while (scanning && pos < 4) {
        switch (b) {
         case 38:
          segOverride = S_ES;
          break;

         case 46:
          segOverride = S_CS;
          break;

         case 54:
          segOverride = S_SS;
          break;

         case 62:
          segOverride = S_DS;
          break;

         case 100:
          segOverride = S_FS;
          break;

         case 101:
          segOverride = S_GS;
          break;

         case 102:
          opSizeOverride = true;
          break;

         case 103:
          addrSizeOverride = true;
          break;

         case 240:
          break;

         // LOCK prefix — consumed, no special behavior in JIT
          case 242:
          repPrefix = 242;
          break;

         case 243:
          repPrefix = 243;
          break;

         default:
          scanning = false;
          continue;
        }
        pos++;
        b = guestRb(codePhys + pos);
      }
      // Effective segment bases
      const effDS = segOverride >= 0 ? segBase(segOverride) : dsBase;
      const effSS = ssBase;
      // stack segment rarely overridden
      // operand size: default 16-bit in real mode
      const opSize = opSizeOverride ? 4 : 2;
      // address size: default 16-bit in real mode
      const addrSize = addrSizeOverride ? 4 : 2;
      // Code bytes after prefixes — use ROM buffer if in ROM range
      const inROM = (codePhys >= romGCPhysStart && codePhys < romGCPhysEnd);
      const ci = inROM ? (romBufBase + (codePhys - romGCPhysStart) + pos) : (ramBase + codePhys + pos);
      let ilen = pos;
      // instruction length accumulator
      // ── Opcode dispatch ──
      switch (b) {
       // ──── NOP ────
        case 144:
        ilen += 1;
        break;

       // ──── MOV r8, r/m8 (0x8A) ────
        case 138:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          if ((modrm >> 6) === 3) {
            sr8(reg, gr8(modrm & 7));
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            sr8(reg, rb(m.ea));
          }
          break;
        }

       // ──── MOV r/m8, r8 (0x88) ────
        case 136:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          if ((modrm >> 6) === 3) {
            sr8(modrm & 7, gr8(reg));
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            wb(m.ea, gr8(reg));
          }
          break;
        }

       // ──── MOV r16/32, r/m16/32 (0x8B) ────
        case 139:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          if ((modrm >> 6) === 3) {
            if (opSize === 2) sr16(reg, gr16(modrm & 7)); else sr32(reg, gr32(modrm & 7));
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            if (opSize === 2) sr16(reg, rw(m.ea)); else sr32(reg, rd(m.ea));
          }
          break;
        }

       // ──── MOV r/m16/32, r16/32 (0x89) ────
        case 137:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          if ((modrm >> 6) === 3) {
            if (opSize === 2) sr16(modrm & 7, gr16(reg)); else sr32(modrm & 7, gr32(reg));
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            if (opSize === 2) ww(m.ea, gr16(reg)); else wd(m.ea, gr32(reg));
          }
          break;
        }

       // ──── MOV r/m8, imm8 (0xC6) ────
        case 198:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          if ((modrm >> 6) === 3) {
            sr8(modrm & 7, mem8[ci + 2]);
            ilen += 1;
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            wb(m.ea, mem8[ci + 2 + m.len]);
            ilen += 1;
          }
          break;
        }

       // ──── MOV r/m16/32, imm16/32 (0xC7) ────
        case 199:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          if ((modrm >> 6) === 3) {
            if (opSize === 2) {
              sr16(modrm & 7, mem8[ci + 2] | (mem8[ci + 3] << 8));
              ilen += 2;
            } else {
              sr32(modrm & 7, mem8[ci + 2] | (mem8[ci + 3] << 8) | (mem8[ci + 4] << 16) | (mem8[ci + 5] << 24));
              ilen += 4;
            }
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            const imOff = ci + 2 + m.len;
            if (opSize === 2) {
              ww(m.ea, mem8[imOff] | (mem8[imOff + 1] << 8));
              ilen += 2;
            } else {
              wd(m.ea, mem8[imOff] | (mem8[imOff + 1] << 8) | (mem8[imOff + 2] << 16) | (mem8[imOff + 3] << 24));
              ilen += 4;
            }
          }
          break;
        }

       // ──── MOV r16/32, imm (0xB8-0xBF) ────
        case 184:
       case 185:
       case 186:
       case 187:
       case 188:
       case 189:
       case 190:
       case 191:
        {
          const reg = b - 184;
          if (opSize === 2) {
            sr16(reg, mem8[ci + 1] | (mem8[ci + 2] << 8));
            ilen += 3;
          } else {
            sr32(reg, mem8[ci + 1] | (mem8[ci + 2] << 8) | (mem8[ci + 3] << 16) | (mem8[ci + 4] << 24));
            ilen += 5;
          }
          break;
        }

       // ──── MOV r8, imm8 (0xB0-0xB7) ────
        case 176:
       case 177:
       case 178:
       case 179:
       case 180:
       case 181:
       case 182:
       case 183:
        sr8(b - 176, mem8[ci + 1]);
        ilen += 2;
        break;

       // ──── MOV AL, moffs8 (0xA0) ────
        case 160:
        {
          const addr = addrSize === 2 ? (mem8[ci + 1] | (mem8[ci + 2] << 8)) : (mem8[ci + 1] | (mem8[ci + 2] << 8) | (mem8[ci + 3] << 16) | (mem8[ci + 4] << 24));
          sr8(0, rb(effDS + addr));
          ilen += 1 + addrSize;
          break;
        }

       // ──── MOV moffs8, AL (0xA2) ────
        case 162:
        {
          const addr = addrSize === 2 ? (mem8[ci + 1] | (mem8[ci + 2] << 8)) : (mem8[ci + 1] | (mem8[ci + 2] << 8) | (mem8[ci + 3] << 16) | (mem8[ci + 4] << 24));
          wb(effDS + addr, gr8(0));
          ilen += 1 + addrSize;
          break;
        }

       // ──── MOV AX, moffs16 (0xA1) ────
        case 161:
        {
          const addr = addrSize === 2 ? (mem8[ci + 1] | (mem8[ci + 2] << 8)) : (mem8[ci + 1] | (mem8[ci + 2] << 8) | (mem8[ci + 3] << 16) | (mem8[ci + 4] << 24));
          if (opSize === 2) sr16(0, rw(effDS + addr)); else sr32(0, rd(effDS + addr));
          ilen += 1 + addrSize;
          break;
        }

       // ──── MOV moffs16, AX (0xA3) ────
        case 163:
        {
          const addr = addrSize === 2 ? (mem8[ci + 1] | (mem8[ci + 2] << 8)) : (mem8[ci + 1] | (mem8[ci + 2] << 8) | (mem8[ci + 3] << 16) | (mem8[ci + 4] << 24));
          if (opSize === 2) ww(effDS + addr, gr16(0)); else wd(effDS + addr, gr32(0));
          ilen += 1 + addrSize;
          break;
        }

       // ──── MOV Sreg, r/m16 (0x8E) ────
        case 142:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const sreg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) {
            val = gr16(modrm & 7);
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            val = rw(m.ea);
          }
          // Update selector and base (real mode: base = sel << 4)
          const sOff = SEG_OFFS[sreg];
          if (!sOff && sreg !== 0) {
            ip = (ip + ilen) & 65535;
            break;
          }
          // invalid sreg
          wr16(sOff + SEG_SEL, val);
          if (realMode) {
            wr64(sOff + SEG_BASE, val << 4);
            // Refresh cached bases
            if (sreg === 3) dsBase = val << 4; else if (sreg === 2) ssBase = val << 4; else if (sreg === 0) esBase = val << 4;
          }
          break;
        }

       // ──── MOV r/m16, Sreg (0x8C) ────
        case 140:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const sreg = (modrm >> 3) & 7;
          const sOff = SEG_OFFS[sreg];
          const val = sOff ? rr16(sOff + SEG_SEL) : (sreg === 0 ? rr16(S_ES + SEG_SEL) : 0);
          if ((modrm >> 6) === 3) {
            sr16(modrm & 7, val);
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            ww(m.ea, val);
          }
          break;
        }

       // ──── ALU r/m8, r8 (0x00,0x08,0x10,0x18,0x20,0x28,0x30,0x38) ────
        case 0:
       case 8:
       case 16:
       case 24:
       case 32:
       case 40:
       case 48:
       case 56:
        {
          const op = b >> 3;
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          const rv = gr8(reg);
          if ((modrm >> 6) === 3) {
            const rm = modrm & 7;
            const res = alu8(op, gr8(rm), rv);
            if (op !== 7) sr8(rm, res);
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            const res = alu8(op, rb(m.ea), rv);
            if (op !== 7) wb(m.ea, res);
          }
          break;
        }

       // ──── ALU r8, r/m8 (0x02,0x0A,0x12,0x1A,0x22,0x2A,0x32,0x3A) ────
        case 2:
       case 10:
       case 18:
       case 26:
       case 34:
       case 42:
       case 50:
       case 58:
        {
          const op = (b - 2) >> 3;
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) {
            val = gr8(modrm & 7);
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            val = rb(m.ea);
          }
          const res = alu8(op, gr8(reg), val);
          if (op !== 7) sr8(reg, res);
          break;
        }

       // ──── ALU r/m16/32, r16/32 (0x01,0x09,0x11,0x19,0x21,0x29,0x31,0x39) ────
        case 1:
       case 9:
       case 17:
       case 25:
       case 33:
       case 41:
       case 49:
       case 57:
        {
          const op = (b - 1) >> 3;
          const modrm = mem8[ci + 1];
          ilen += 2;
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
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
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
        case 3:
       case 11:
       case 19:
       case 27:
       case 35:
       case 43:
       case 51:
       case 59:
        {
          const op = (b - 3) >> 3;
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) {
            val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
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
        case 4:
       case 12:
       case 20:
       case 28:
       case 36:
       case 44:
       case 52:
       case 60:
        {
          const op = (b - 4) >> 3;
          const imm = mem8[ci + 1];
          ilen += 2;
          const res = alu8(op, gr8(0), imm);
          if (op !== 7) sr8(0, res);
          break;
        }

       // ──── ALU AX, imm16/32 (0x05,0x0D,0x15,0x1D,0x25,0x2D,0x35,0x3D) ────
        case 5:
       case 13:
       case 21:
       case 29:
       case 37:
       case 45:
       case 53:
       case 61:
        {
          const op = (b - 5) >> 3;
          ilen += 1;
          if (opSize === 2) {
            const imm = mem8[ci + 1] | (mem8[ci + 2] << 8);
            ilen += 2;
            const res = alu16(op, gr16(0), imm);
            if (op !== 7) sr16(0, res);
          } else {
            const imm = mem8[ci + 1] | (mem8[ci + 2] << 8) | (mem8[ci + 3] << 16) | (mem8[ci + 4] << 24);
            ilen += 4;
            const res = alu32(op, gr32(0), imm);
            if (op !== 7) sr32(0, res);
          }
          break;
        }

       // ──── ALU r/m8, imm8 (0x80) ────
        case 128:
       case 130:
        {
          // 0x82 is undocumented alias for 0x80
          const modrm = mem8[ci + 1];
          ilen += 2;
          const op = (modrm >> 3) & 7;
          if ((modrm >> 6) === 3) {
            const imm = mem8[ci + 2];
            ilen += 1;
            const res = alu8(op, gr8(modrm & 7), imm);
            if (op !== 7) sr8(modrm & 7, res);
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            const imm = mem8[ci + 2 + m.len];
            ilen += 1;
            const res = alu8(op, rb(m.ea), imm);
            if (op !== 7) wb(m.ea, res);
          }
          break;
        }

       // ──── ALU r/m16/32, imm16/32 (0x81) ────
        case 129:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const op = (modrm >> 3) & 7;
          if ((modrm >> 6) === 3) {
            const rm = modrm & 7;
            if (opSize === 2) {
              const imm = mem8[ci + 2] | (mem8[ci + 3] << 8);
              ilen += 2;
              const res = alu16(op, gr16(rm), imm);
              if (op !== 7) sr16(rm, res);
            } else {
              const imm = mem8[ci + 2] | (mem8[ci + 3] << 8) | (mem8[ci + 4] << 16) | (mem8[ci + 5] << 24);
              ilen += 4;
              const res = alu32(op, gr32(rm), imm);
              if (op !== 7) sr32(rm, res);
            }
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            const imOff = ci + 2 + m.len;
            if (opSize === 2) {
              const imm = mem8[imOff] | (mem8[imOff + 1] << 8);
              ilen += 2;
              const res = alu16(op, rw(m.ea), imm);
              if (op !== 7) ww(m.ea, res);
            } else {
              const imm = mem8[imOff] | (mem8[imOff + 1] << 8) | (mem8[imOff + 2] << 16) | (mem8[imOff + 3] << 24);
              ilen += 4;
              const res = alu32(op, rd(m.ea), imm);
              if (op !== 7) wd(m.ea, res);
            }
          }
          break;
        }

       // ──── ALU r/m16/32, imm8 sign-extended (0x83) ────
        case 131:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const op = (modrm >> 3) & 7;
          let imm = mem8[ci + 2];
          ilen += 1;
          // But wait — imm is after modrm+displacement, not at ci+2 for memory operands
          // Need to handle this correctly
          if ((modrm >> 6) === 3) {
            // imm is at ci+2
            if (imm > 127) imm = opSize === 2 ? (imm | 65280) : ((imm | 4294967040) >>> 0);
            if (opSize === 2) {
              const res = alu16(op, gr16(modrm & 7), imm & 65535);
              if (op !== 7) sr16(modrm & 7, res);
            } else {
              const res = alu32(op, gr32(modrm & 7), imm);
              if (op !== 7) sr32(modrm & 7, res);
            }
          } else {
            ilen -= 1;
            // undo imm consumption, recalculate after displacement
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            imm = mem8[ci + 2 + m.len];
            ilen += 1;
            if (imm > 127) imm = opSize === 2 ? (imm | 65280) : ((imm | 4294967040) >>> 0);
            if (opSize === 2) {
              const res = alu16(op, rw(m.ea), imm & 65535);
              if (op !== 7) ww(m.ea, res);
            } else {
              const res = alu32(op, rd(m.ea), imm);
              if (op !== 7) wd(m.ea, res);
            }
          }
          break;
        }

       // ──── TEST r/m8, r8 (0x84) ────
        case 132:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) val = gr8(modrm & 7); else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            val = rb(m.ea);
          }
          alu8(4, val, gr8(reg));
          // AND but don't store
          break;
        }

       // ──── TEST r/m16/32, r16/32 (0x85) ────
        case 133:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7); else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            val = opSize === 2 ? rw(m.ea) : rd(m.ea);
          }
          if (opSize === 2) alu16(4, val, gr16(reg)); else alu32(4, val, gr32(reg));
          break;
        }

       // ──── TEST AL, imm8 (0xA8) ────
        case 168:
        alu8(4, gr8(0), mem8[ci + 1]);
        ilen += 2;
        break;

       // ──── TEST AX, imm16/32 (0xA9) ────
        case 169:
        ilen += 1;
        if (opSize === 2) {
          alu16(4, gr16(0), mem8[ci + 1] | (mem8[ci + 2] << 8));
          ilen += 2;
        } else {
          alu32(4, gr32(0), mem8[ci + 1] | (mem8[ci + 2] << 8) | (mem8[ci + 3] << 16) | (mem8[ci + 4] << 24));
          ilen += 4;
        }
        break;

       // ──── INC r16/32 (0x40-0x47) ────
        case 64:
       case 65:
       case 66:
       case 67:
       case 68:
       case 69:
       case 70:
       case 71:
        {
          const reg = b - 64;
          const oldCF = getCF();
          if (opSize === 2) {
            const v = (gr16(reg) + 1) & 65535;
            sr16(reg, v);
            setFlagsArith(OP_INC, v, v - 1, 1, 2);
          } else {
            const v = (gr32(reg) + 1) >>> 0;
            sr32(reg, v);
            setFlagsArith(OP_INC, v, v - 1, 1, 4);
          }
          lazyCF = oldCF;
          ilen += 1;
          break;
        }

       // ──── DEC r16/32 (0x48-0x4F) ────
        case 72:
       case 73:
       case 74:
       case 75:
       case 76:
       case 77:
       case 78:
       case 79:
        {
          const reg = b - 72;
          const oldCF = getCF();
          if (opSize === 2) {
            const v = (gr16(reg) - 1) & 65535;
            sr16(reg, v);
            setFlagsArith(OP_DEC, v, v + 1, 1, 2);
          } else {
            const v = (gr32(reg) - 1) >>> 0;
            sr32(reg, v);
            setFlagsArith(OP_DEC, v, v + 1, 1, 4);
          }
          lazyCF = oldCF;
          ilen += 1;
          break;
        }

       // ──── PUSH r16/32 (0x50-0x57) ────
        case 80:
       case 81:
       case 82:
       case 83:
       case 84:
       case 85:
       case 86:
       case 87:
        if (opSize === 2) push16(gr16(b - 80), ssBase); else push32(gr32(b - 80), ssBase);
        ilen += 1;
        break;

       // ──── POP r16/32 (0x58-0x5F) ────
        case 88:
       case 89:
       case 90:
       case 91:
       case 92:
       case 93:
       case 94:
       case 95:
        if (opSize === 2) sr16(b - 88, pop16(ssBase)); else sr32(b - 88, pop32(ssBase));
        ilen += 1;
        break;

       // ──── PUSH imm16/32 (0x68) ────
        case 104:
        ilen += 1;
        if (opSize === 2) {
          push16(mem8[ci + 1] | (mem8[ci + 2] << 8), ssBase);
          ilen += 2;
        } else {
          push32(mem8[ci + 1] | (mem8[ci + 2] << 8) | (mem8[ci + 3] << 16) | (mem8[ci + 4] << 24), ssBase);
          ilen += 4;
        }
        break;

       // ──── PUSH imm8 sign-extended (0x6A) ────
        case 106:
        {
          let v = mem8[ci + 1];
          if (v > 127) v = opSize === 2 ? (v | 65280) : ((v | 4294967040) >>> 0);
          if (opSize === 2) push16(v & 65535, ssBase); else push32(v, ssBase);
          ilen += 2;
          break;
        }

       // ──── IMUL r, r/m, imm16/32 (0x69) ────
        case 105:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) {
            val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            val = opSize === 2 ? rw(m.ea) : rd(m.ea);
          }
          if (opSize === 2) {
            let imm = mem8[ci + ilen] | (mem8[ci + ilen + 1] << 8);
            ilen += 2;
            if (imm > 32767) imm -= 65536;
            const sval = (val << 16) >> 16;
            const result = sval * imm;
            sr16(reg, result & 65535);
            lazyCF = (result !== ((result << 16) >> 16)) ? 1 : 0;
          } else {
            let imm = mem8[ci + ilen] | (mem8[ci + ilen + 1] << 8) | (mem8[ci + ilen + 2] << 16) | (mem8[ci + ilen + 3] << 24);
            ilen += 4;
            const result = Math.imul(val, imm);
            sr32(reg, result >>> 0);
            const big = BigInt(val | 0) * BigInt(imm | 0);
            lazyCF = (big !== BigInt(result | 0)) ? 1 : 0;
          }
          lazyOp = OP_NONE;
          lazyRes = opSize === 2 ? gr16(reg) : gr32(reg);
          lazySize = opSize;
          break;
        }

       // ──── IMUL r, r/m, imm8 (0x6B) ────
        case 107:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          let val;
          if ((modrm >> 6) === 3) {
            val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            val = opSize === 2 ? rw(m.ea) : rd(m.ea);
          }
          let imm = mem8[ci + ilen];
          ilen += 1;
          if (imm > 127) imm -= 256;
          // sign-extend
          if (opSize === 2) {
            const sval = (val << 16) >> 16;
            const result = sval * imm;
            sr16(reg, result & 65535);
            lazyCF = (result !== ((result << 16) >> 16)) ? 1 : 0;
          } else {
            const result = Math.imul(val, imm);
            sr32(reg, result >>> 0);
            const big = BigInt(val | 0) * BigInt(imm);
            lazyCF = (big !== BigInt(result | 0)) ? 1 : 0;
          }
          lazyOp = OP_NONE;
          lazyRes = opSize === 2 ? gr16(reg) : gr32(reg);
          lazySize = opSize;
          break;
        }

       // ──── PUSHF (0x9C) ────
        case 156:
        {
          const f = flagsToWord() | (flags & 4294963200);
          // preserve upper bits
          if (opSize === 2) push16(f & 65535, ssBase); else push32(f, ssBase);
          ilen += 1;
          break;
        }

       // ──── POPF (0x9D) ────
        case 157:
        {
          let f;
          if (opSize === 2) f = pop16(ssBase); else f = pop32(ssBase);
          flags = f;
          loadFlags(f);
          ilen += 1;
          break;
        }

       // ──── XCHG r16/32, AX (0x91-0x97) ────
        case 145:
       case 146:
       case 147:
       case 148:
       case 149:
       case 150:
       case 151:
        {
          const reg = b - 144;
          if (opSize === 2) {
            const t = gr16(0);
            sr16(0, gr16(reg));
            sr16(reg, t);
          } else {
            const t = gr32(0);
            sr32(0, gr32(reg));
            sr32(reg, t);
          }
          ilen += 1;
          break;
        }

       // ──── XCHG r/m8, r8 (0x86) ────
        case 134:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          if ((modrm >> 6) === 3) {
            const t = gr8(reg);
            sr8(reg, gr8(modrm & 7));
            sr8(modrm & 7, t);
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            const t = gr8(reg);
            sr8(reg, rb(m.ea));
            wb(m.ea, t);
          }
          break;
        }

       // ──── XCHG r/m16/32, r16/32 (0x87) ────
        case 135:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          if ((modrm >> 6) === 3) {
            if (opSize === 2) {
              const t = gr16(reg);
              sr16(reg, gr16(modrm & 7));
              sr16(modrm & 7, t);
            } else {
              const t = gr32(reg);
              sr32(reg, gr32(modrm & 7));
              sr32(modrm & 7, t);
            }
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            if (opSize === 2) {
              const t = gr16(reg);
              sr16(reg, rw(m.ea));
              ww(m.ea, t);
            } else {
              const t = gr32(reg);
              sr32(reg, rd(m.ea));
              wd(m.ea, t);
            }
          }
          break;
        }

       // ──── LEA r16/32, m (0x8D) ────
        case 141:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const reg = (modrm >> 3) & 7;
          // LEA computes effective address but doesn't add segment base
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, 0, 0) : decodeModRM32(modrm, mem8, ci + 2, 0, 0);
          ilen += m.len;
          if (opSize === 2) sr16(reg, m.ea & 65535); else sr32(reg, m.ea);
          break;
        }

       // ──── JMP rel8 (0xEB) ────
        case 235:
        {
          let rel = mem8[ci + 1];
          if (rel > 127) rel -= 256;
          ip = (ip + 2 + pos + rel) & 65535;
          ilen = 0;
          // ip already set
          executed++;
          // Store state and continue from new IP
          wr16(R_IP, ip);
          continue;
        }

       // ──── JMP rel16/32 (0xE9) ────
        case 233:
        {
          let rel;
          if (opSize === 2) {
            rel = mem8[ci + 1] | (mem8[ci + 2] << 8);
            if (rel > 32767) rel -= 65536;
            ip = (ip + 3 + pos + rel) & 65535;
          } else {
            rel = mem8[ci + 1] | (mem8[ci + 2] << 8) | (mem8[ci + 3] << 16) | (mem8[ci + 4] << 24);
            ip = (ip + 5 + pos + rel) & 65535;
          }
          ilen = 0;
          executed++;
          wr16(R_IP, ip);
          continue;
        }

       // ──── Jcc rel8 (0x70-0x7F) ────
        case 112:
       case 113:
       case 114:
       case 115:
       case 116:
       case 117:
       case 118:
       case 119:
       case 120:
       case 121:
       case 122:
       case 123:
       case 124:
       case 125:
       case 126:
       case 127:
        {
          let rel = mem8[ci + 1];
          if (rel > 127) rel -= 256;
          ilen += 2;
          if (testCC(b - 112)) {
            ip = (ip + ilen + rel) & 65535;
            ilen = 0;
            executed++;
            wr16(R_IP, ip);
            continue;
          }
          break;
        }

       // ──── LOOP/LOOPcc (0xE0-0xE2) ────
        case 226:
        {
          // LOOP rel8
          let rel = mem8[ci + 1];
          if (rel > 127) rel -= 256;
          ilen += 2;
          const cx = (gr16(1) - 1) & 65535;
          sr16(1, cx);
          if (cx !== 0) {
            ip = (ip + ilen + rel) & 65535;
            ilen = 0;
            executed++;
            wr16(R_IP, ip);
            continue;
          }
          break;
        }

       case 225:
        {
          // LOOPE rel8
          let rel = mem8[ci + 1];
          if (rel > 127) rel -= 256;
          ilen += 2;
          const cx = (gr16(1) - 1) & 65535;
          sr16(1, cx);
          if (cx !== 0 && getZF()) {
            ip = (ip + ilen + rel) & 65535;
            ilen = 0;
            executed++;
            wr16(R_IP, ip);
            continue;
          }
          break;
        }

       case 224:
        {
          // LOOPNE rel8
          let rel = mem8[ci + 1];
          if (rel > 127) rel -= 256;
          ilen += 2;
          const cx = (gr16(1) - 1) & 65535;
          sr16(1, cx);
          if (cx !== 0 && !getZF()) {
            ip = (ip + ilen + rel) & 65535;
            ilen = 0;
            executed++;
            wr16(R_IP, ip);
            continue;
          }
          break;
        }

       // ──── CALL rel16/32 (0xE8) ────
        case 232:
        {
          let rel;
          if (opSize === 2) {
            rel = mem8[ci + 1] | (mem8[ci + 2] << 8);
            if (rel > 32767) rel -= 65536;
            ilen += 3;
            push16((ip + ilen) & 65535, ssBase);
            ip = (ip + ilen + rel) & 65535;
          } else {
            rel = mem8[ci + 1] | (mem8[ci + 2] << 8) | (mem8[ci + 3] << 16) | (mem8[ci + 4] << 24);
            ilen += 5;
            push32((ip + ilen) & 4294967295, ssBase);
            ip = (ip + ilen + rel) & 65535;
          }
          ilen = 0;
          executed++;
          wr16(R_IP, ip);
          continue;
        }

       // ──── RET near (0xC3) ────
        case 195:
        if (opSize === 2) ip = pop16(ssBase); else ip = pop32(ssBase) & 65535;
        ilen = 0;
        executed++;
        wr16(R_IP, ip);
        continue;

       // ──── RET near imm16 (0xC2) ────
        case 194:
        {
          const imm = mem8[ci + 1] | (mem8[ci + 2] << 8);
          if (opSize === 2) {
            ip = pop16(ssBase);
            sr16(4, (gr16(4) + imm) & 65535);
          } else {
            ip = pop32(ssBase) & 65535;
            sr32(4, (gr32(4) + imm) >>> 0);
          }
          ilen = 0;
          executed++;
          wr16(R_IP, ip);
          continue;
        }

       // ──── CLI (0xFA), STI (0xFB) ────
        case 250:
        flags &= ~512;
        ilen += 1;
        break;

       case 251:
        flags |= 512;
        ilen += 1;
        break;

       // ──── CLD (0xFC), STD (0xFD) ────
        case 252:
        flags &= ~1024;
        ilen += 1;
        break;

       case 253:
        flags |= 1024;
        ilen += 1;
        break;

       // ──── CLC (0xF8), STC (0xF9), CMC (0xF5) ────
        case 248:
        lazyCF = 0;
        lazyOp = OP_NONE;
        ilen += 1;
        break;

       case 249:
        lazyCF = 1;
        lazyOp = OP_NONE;
        ilen += 1;
        break;

       case 245:
        lazyCF = getCF() ? 0 : 1;
        lazyOp = OP_NONE;
        ilen += 1;
        break;

       // ──── CBW / CWDE (0x98) ────
        case 152:
        if (opSize === 2) {
          let al = gr8(0);
          if (al > 127) al |= 65280;
          sr16(0, al & 65535);
        } else {
          let ax = gr16(0);
          if (ax > 32767) ax |= 4294901760;
          sr32(0, ax >>> 0);
        }
        ilen += 1;
        break;

       // ──── CWD / CDQ (0x99) ────
        case 153:
        if (opSize === 2) {
          sr16(2, (gr16(0) & 32768) ? 65535 : 0);
        } else {
          sr32(2, (gr32(0) & 2147483648) ? 4294967295 : 0);
        }
        ilen += 1;
        break;

       // ──── MOVZX r16/32, r/m8 (0x0F 0xB6) — handled in 0x0F block ────
        // ──── MOVSX r16/32, r/m8 (0x0F 0xBE) — handled in 0x0F block ────
        // ──── IN/OUT: bail to IEM for proper I/O port handling ────
        // IN/OUT must go through VBox's I/O port infrastructure so devices
        // (keyboard controller, PIT, PIC, VGA, IDE) respond correctly.
        // Without this, portIn returns 0xFF causing infinite polling loops.
        case 228:
       case 229:
       case 236:
       case 237:
       // IN
        case 230:
       case 231:
       case 238:
       case 239:
        // OUT
        lastBailOp = b;
        iter = maxInsn;
        break;

       // ──── REP/REPNE + string ops ────
        case 170:
       case 171:
       case 172:
       case 173:
       case 174:
       case 175:
       case 164:
       case 165:
       case 166:
       case 167:
       case 108:
       case 109:
       case 110:
       case 111:
        {
          // String operations
          const dir = (flags & 1024) ? -1 : 1;
          // DF flag
          ilen += 1;
          if (repPrefix && (b === 164 || b === 165 || b === 170 || b === 171 || b === 172 || b === 173 || b === 108 || b === 109 || b === 110 || b === 111)) {
            // REP prefix — repeat CX times
            let cx = gr16(1);
            if (cx === 0) break;
            const srcSeg = segOverride >= 0 ? segBase(segOverride) : dsBase;
            switch (b) {
             case 170:
              {
                // STOSB
                let di = gr16(7);
                while (cx > 0) {
                  wb(esBase + di, gr8(0));
                  di = (di + dir) & 65535;
                  cx--;
                }
                sr16(7, di);
                sr16(1, 0);
                break;
              }

             case 171:
              {
                // STOSW/STOSD
                let di = gr16(7);
                if (opSize === 2) {
                  const v = gr16(0);
                  while (cx > 0) {
                    ww(esBase + di, v);
                    di = (di + dir * 2) & 65535;
                    cx--;
                  }
                } else {
                  const v = gr32(0);
                  while (cx > 0) {
                    wd(esBase + di, v);
                    di = (di + dir * 4) & 65535;
                    cx--;
                  }
                }
                sr16(7, di);
                sr16(1, 0);
                break;
              }

             case 164:
              {
                // MOVSB
                let si = gr16(6), di = gr16(7);
                while (cx > 0) {
                  wb(esBase + di, rb(srcSeg + si));
                  si = (si + dir) & 65535;
                  di = (di + dir) & 65535;
                  cx--;
                }
                sr16(6, si);
                sr16(7, di);
                sr16(1, 0);
                break;
              }

             case 165:
              {
                // MOVSW/MOVSD
                let si = gr16(6), di = gr16(7);
                if (opSize === 2) {
                  while (cx > 0) {
                    ww(esBase + di, rw(srcSeg + si));
                    si = (si + dir * 2) & 65535;
                    di = (di + dir * 2) & 65535;
                    cx--;
                  }
                } else {
                  while (cx > 0) {
                    wd(esBase + di, rd(srcSeg + si));
                    si = (si + dir * 4) & 65535;
                    di = (di + dir * 4) & 65535;
                    cx--;
                  }
                }
                sr16(6, si);
                sr16(7, di);
                sr16(1, 0);
                break;
              }

             case 172:
              {
                // LODSB
                let si = gr16(6);
                while (cx > 0) {
                  sr8(0, rb(srcSeg + si));
                  si = (si + dir) & 65535;
                  cx--;
                }
                sr16(6, si);
                sr16(1, 0);
                break;
              }

             case 173:
              {
                // LODSW/LODSD
                let si = gr16(6);
                while (cx > 0) {
                  if (opSize === 2) sr16(0, rw(srcSeg + si)); else sr32(0, rd(srcSeg + si));
                  si = (si + dir * opSize) & 65535;
                  cx--;
                }
                sr16(6, si);
                sr16(1, 0);
                break;
              }

             default:
              // INSB/INSW/OUTSB/OUTSW — fall back to IEM
              lastBailOp = b;
              iter = maxInsn;
              // force exit
              break;
            }
          } else if (repPrefix && (b === 174 || b === 175)) {
            // REPE/REPNE SCAS
            let cx = gr16(1), di = gr16(7);
            const isRepNE = (repPrefix === 242);
            if (b === 174) {
              // SCASB
              const al = gr8(0);
              while (cx > 0) {
                const v = rb(esBase + di);
                di = (di + dir) & 65535;
                cx--;
                alu8(7, al, v);
                // CMP
                if (isRepNE ? getZF() : !getZF()) break;
              }
            } else {
              // SCASW/SCASD
              if (opSize === 2) {
                const ax = gr16(0);
                while (cx > 0) {
                  const v = rw(esBase + di);
                  di = (di + dir * 2) & 65535;
                  cx--;
                  alu16(7, ax, v);
                  if (isRepNE ? getZF() : !getZF()) break;
                }
              } else {
                const eax = gr32(0);
                while (cx > 0) {
                  const v = rd(esBase + di);
                  di = (di + dir * 4) & 65535;
                  cx--;
                  alu32(7, eax, v);
                  if (isRepNE ? getZF() : !getZF()) break;
                }
              }
            }
            sr16(7, di);
            sr16(1, cx);
          } else if (repPrefix && (b === 166 || b === 167)) {
            // REPE/REPNE CMPS
            let cx = gr16(1), si = gr16(6), di = gr16(7);
            const isRepNE = (repPrefix === 242);
            const srcSeg = segOverride >= 0 ? segBase(segOverride) : dsBase;
            if (b === 166) {
              // CMPSB
              while (cx > 0) {
                const a = rb(srcSeg + si), bv = rb(esBase + di);
                si = (si + dir) & 65535;
                di = (di + dir) & 65535;
                cx--;
                alu8(7, a, bv);
                if (isRepNE ? getZF() : !getZF()) break;
              }
            } else {
              // CMPSW/CMPSD
              const sz = opSize;
              while (cx > 0) {
                const a = sz === 2 ? rw(srcSeg + si) : rd(srcSeg + si);
                const bv = sz === 2 ? rw(esBase + di) : rd(esBase + di);
                si = (si + dir * sz) & 65535;
                di = (di + dir * sz) & 65535;
                cx--;
                if (sz === 2) alu16(7, a, bv); else alu32(7, a, bv);
                if (isRepNE ? getZF() : !getZF()) break;
              }
            }
            sr16(6, si);
            sr16(7, di);
            sr16(1, cx);
          } else {
            // Single string op (no REP prefix)
            switch (b) {
             case 170:
              wb(esBase + gr16(7), gr8(0));
              sr16(7, (gr16(7) + dir) & 65535);
              break;

             case 171:
              if (opSize === 2) {
                ww(esBase + gr16(7), gr16(0));
                sr16(7, (gr16(7) + dir * 2) & 65535);
              } else {
                wd(esBase + gr16(7), gr32(0));
                sr16(7, (gr16(7) + dir * 4) & 65535);
              }
              break;

             case 164:
              {
                const srcSeg2 = segOverride >= 0 ? segBase(segOverride) : dsBase;
                wb(esBase + gr16(7), rb(srcSeg2 + gr16(6)));
                sr16(6, (gr16(6) + dir) & 65535);
                sr16(7, (gr16(7) + dir) & 65535);
                break;
              }

             case 165:
              {
                const srcSeg2 = segOverride >= 0 ? segBase(segOverride) : dsBase;
                if (opSize === 2) ww(esBase + gr16(7), rw(srcSeg2 + gr16(6))); else wd(esBase + gr16(7), rd(srcSeg2 + gr16(6)));
                sr16(6, (gr16(6) + dir * opSize) & 65535);
                sr16(7, (gr16(7) + dir * opSize) & 65535);
                break;
              }

             case 172:
              {
                const srcSeg2 = segOverride >= 0 ? segBase(segOverride) : dsBase;
                sr8(0, rb(srcSeg2 + gr16(6)));
                sr16(6, (gr16(6) + dir) & 65535);
                break;
              }

             case 173:
              {
                const srcSeg2 = segOverride >= 0 ? segBase(segOverride) : dsBase;
                if (opSize === 2) sr16(0, rw(srcSeg2 + gr16(6))); else sr32(0, rd(srcSeg2 + gr16(6)));
                sr16(6, (gr16(6) + dir * opSize) & 65535);
                break;
              }

             case 174:
              alu8(7, gr8(0), rb(esBase + gr16(7)));
              sr16(7, (gr16(7) + dir) & 65535);
              break;

             case 175:
              if (opSize === 2) alu16(7, gr16(0), rw(esBase + gr16(7))); else alu32(7, gr32(0), rd(esBase + gr16(7)));
              sr16(7, (gr16(7) + dir * opSize) & 65535);
              break;

             default:
              lastBailOp = b;
              iter = maxInsn;
              break;
            }
          }
          break;
        }

       // ──── SAHF (0x9E), LAHF (0x9F) ────
        case 158:
        {
          // SAHF: load low 8 of flags from AH
          const ah = gr8(4);
          // AH
          lazyCF = ah & 1;
          // Reconstruct lazy state
          lazyOp = OP_NONE;
          lazyRes = (ah & 64) ? 0 : 1;
          // ZF
          if (ah & 128) lazyRes |= SIZE_SIGN[lazySize];
          // SF
          ilen += 1;
          break;
        }

       case 159:
        {
          // LAHF: store flags low 8 into AH
          sr8(4, flagsToWord() & 255);
          ilen += 1;
          break;
        }

       // ──── NOT / NEG (0xF6, 0xF7) ────
        case 246:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const op = (modrm >> 3) & 7;
          if (op === 2) {
            // NOT r/m8
            if ((modrm >> 6) === 3) sr8(modrm & 7, ~gr8(modrm & 7) & 255); else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              wb(m.ea, ~rb(m.ea) & 255);
            }
          } else if (op === 3) {
            // NEG r/m8
            let val;
            if ((modrm >> 6) === 3) {
              val = gr8(modrm & 7);
              const r = (-val) & 255;
              sr8(modrm & 7, r);
              setFlagsArith(OP_SUB, r, 0, val, 1);
              lazyCF = val !== 0 ? 1 : 0;
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              val = rb(m.ea);
              const r = (-val) & 255;
              wb(m.ea, r);
              setFlagsArith(OP_SUB, r, 0, val, 1);
              lazyCF = val !== 0 ? 1 : 0;
            }
          } else if (op === 0) {
            // TEST r/m8, imm8
            let val;
            if ((modrm >> 6) === 3) {
              val = gr8(modrm & 7);
              alu8(4, val, mem8[ci + 2]);
              ilen += 1;
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              val = rb(m.ea);
              alu8(4, val, mem8[ci + 2 + m.len]);
              ilen += 1;
            }
          } else if (op === 4) {
            // MUL r/m8 — AX = AL * r/m8
            let val;
            if ((modrm >> 6) === 3) {
              val = gr8(modrm & 7);
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              val = rb(m.ea);
            }
            const result = (gr8(0) & 255) * (val & 255);
            sr16(0, result & 65535);
            // AX
            lazyCF = (result & 65280) ? 1 : 0;
            lazyOp = OP_NONE;
            lazyRes = result & 255;
            lazySize = 1;
          } else if (op === 5) {
            // IMUL r/m8 — AX = AL * r/m8 (signed)
            let val;
            if ((modrm >> 6) === 3) {
              val = gr8(modrm & 7);
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              val = rb(m.ea);
            }
            const a = (gr8(0) << 24) >> 24;
            // sign-extend AL
            const b2 = (val << 24) >> 24;
            const result = a * b2;
            sr16(0, result & 65535);
            lazyCF = ((result & 65535) !== ((result << 24) >> 24) & 65535) ? 1 : 0;
            lazyOp = OP_NONE;
            lazyRes = result & 255;
            lazySize = 1;
          } else if (op === 6) {
            // DIV r/m8 — AL = AX / r/m8, AH = AX % r/m8
            let val;
            if ((modrm >> 6) === 3) {
              val = gr8(modrm & 7);
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              val = rb(m.ea);
            }
            if (val === 0) {
              lastBailOp = b;
              iter = maxInsn;
              break;
            }
            // #DE
            const ax = gr16(0);
            const quot = (ax / val) >>> 0;
            if (quot > 255) {
              lastBailOp = b;
              iter = maxInsn;
              break;
            }
            // #DE
            const rem = ax % val;
            sr8(0, quot & 255);
            sr8(4, rem & 255);
          } else if (op === 7) {
            // IDIV r/m8
            let val;
            if ((modrm >> 6) === 3) {
              val = gr8(modrm & 7);
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              val = rb(m.ea);
            }
            const divisor = (val << 24) >> 24;
            if (divisor === 0) {
              lastBailOp = b;
              iter = maxInsn;
              break;
            }
            const ax = (gr16(0) << 16) >> 16;
            // sign-extend AX
            const quot = (ax / divisor) | 0;
            if (quot > 127 || quot < -128) {
              lastBailOp = b;
              iter = maxInsn;
              break;
            }
            const rem = (ax % divisor) | 0;
            sr8(0, quot & 255);
            sr8(4, rem & 255);
          } else {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
          break;
        }

       case 247:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const op = (modrm >> 3) & 7;
          if (op === 2) {
            // NOT r/m16/32
            if ((modrm >> 6) === 3) {
              if (opSize === 2) sr16(modrm & 7, ~gr16(modrm & 7) & 65535); else sr32(modrm & 7, ~gr32(modrm & 7) >>> 0);
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              if (opSize === 2) ww(m.ea, ~rw(m.ea) & 65535); else wd(m.ea, ~rd(m.ea) >>> 0);
            }
          } else if (op === 3) {
            // NEG r/m16/32
            if ((modrm >> 6) === 3) {
              if (opSize === 2) {
                const v = gr16(modrm & 7), r = (-v) & 65535;
                sr16(modrm & 7, r);
                setFlagsArith(OP_SUB, r, 0, v, 2);
                lazyCF = v ? 1 : 0;
              } else {
                const v = gr32(modrm & 7), r = (-v) >>> 0;
                sr32(modrm & 7, r);
                setFlagsArith(OP_SUB, r, 0, v, 4);
                lazyCF = v ? 1 : 0;
              }
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              if (opSize === 2) {
                const v = rw(m.ea), r = (-v) & 65535;
                ww(m.ea, r);
                setFlagsArith(OP_SUB, r, 0, v, 2);
                lazyCF = v ? 1 : 0;
              } else {
                const v = rd(m.ea), r = (-v) >>> 0;
                wd(m.ea, r);
                setFlagsArith(OP_SUB, r, 0, v, 4);
                lazyCF = v ? 1 : 0;
              }
            }
          } else if (op === 0) {
            // TEST r/m16/32, imm
            if ((modrm >> 6) === 3) {
              if (opSize === 2) {
                alu16(4, gr16(modrm & 7), mem8[ci + 2] | (mem8[ci + 3] << 8));
                ilen += 2;
              } else {
                alu32(4, gr32(modrm & 7), mem8[ci + 2] | (mem8[ci + 3] << 8) | (mem8[ci + 4] << 16) | (mem8[ci + 5] << 24));
                ilen += 4;
              }
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              const off = ci + 2 + m.len;
              if (opSize === 2) {
                alu16(4, rw(m.ea), mem8[off] | (mem8[off + 1] << 8));
                ilen += 2;
              } else {
                alu32(4, rd(m.ea), mem8[off] | (mem8[off + 1] << 8) | (mem8[off + 2] << 16) | (mem8[off + 3] << 24));
                ilen += 4;
              }
            }
          } else if (op === 4) {
            // MUL r/m16/32
            let val;
            if ((modrm >> 6) === 3) {
              val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              val = opSize === 2 ? rw(m.ea) : rd(m.ea);
            }
            if (opSize === 2) {
              const result = (gr16(0) & 65535) * (val & 65535);
              sr16(0, result & 65535);
              // AX
              sr16(2, (result >>> 16) & 65535);
              // DX
              lazyCF = (result & 4294901760) ? 1 : 0;
            } else {
              // 32-bit MUL: EDX:EAX = EAX * r/m32
              const a = gr32(0) >>> 0, b2 = val >>> 0;
              const result = BigInt(a) * BigInt(b2);
              sr32(0, Number(result & 4294967295n));
              // EAX
              sr32(2, Number((result >> 32n) & 4294967295n));
              // EDX
              lazyCF = (result >> 32n) ? 1 : 0;
            }
            lazyOp = OP_NONE;
            lazyRes = gr16(0);
            lazySize = opSize;
          } else if (op === 5) {
            // IMUL r/m16/32
            let val;
            if ((modrm >> 6) === 3) {
              val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              val = opSize === 2 ? rw(m.ea) : rd(m.ea);
            }
            if (opSize === 2) {
              const a = (gr16(0) << 16) >> 16, b2 = (val << 16) >> 16;
              const result = a * b2;
              sr16(0, result & 65535);
              sr16(2, (result >> 16) & 65535);
              lazyCF = (result !== ((result << 16) >> 16)) ? 1 : 0;
            } else {
              const a = gr32(0) | 0, b2 = val | 0;
              const result = BigInt(a) * BigInt(b2);
              sr32(0, Number(result & 4294967295n));
              sr32(2, Number((result >> 32n) & 4294967295n));
              lazyCF = (result !== BigInt(Number(result & 4294967295n) | 0)) ? 1 : 0;
            }
            lazyOp = OP_NONE;
            lazyRes = gr16(0);
            lazySize = opSize;
          } else if (op === 6) {
            // DIV r/m16/32
            let val;
            if ((modrm >> 6) === 3) {
              val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              val = opSize === 2 ? rw(m.ea) : rd(m.ea);
            }
            if (val === 0) {
              lastBailOp = b;
              iter = maxInsn;
              break;
            }
            if (opSize === 2) {
              const dividend = ((gr16(2) & 65535) << 16) | (gr16(0) & 65535);
              const quot = (dividend / val) >>> 0;
              if (quot > 65535) {
                lastBailOp = b;
                iter = maxInsn;
                break;
              }
              sr16(0, quot & 65535);
              sr16(2, (dividend % val) & 65535);
            } else {
              const dividend = (BigInt(gr32(2) >>> 0) << 32n) | BigInt(gr32(0) >>> 0);
              const divisor = BigInt(val >>> 0);
              const quot = dividend / divisor;
              if (quot > 4294967295n) {
                lastBailOp = b;
                iter = maxInsn;
                break;
              }
              sr32(0, Number(quot & 4294967295n));
              sr32(2, Number((dividend % divisor) & 4294967295n));
            }
          } else if (op === 7) {
            // IDIV r/m16/32
            let val;
            if ((modrm >> 6) === 3) {
              val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              val = opSize === 2 ? rw(m.ea) : rd(m.ea);
            }
            if (opSize === 2) {
              const divisor = (val << 16) >> 16;
              if (divisor === 0) {
                lastBailOp = b;
                iter = maxInsn;
                break;
              }
              const dividend = ((gr16(2) << 16) | (gr16(0) & 65535));
              const quot = (dividend / divisor) | 0;
              if (quot > 32767 || quot < -32768) {
                lastBailOp = b;
                iter = maxInsn;
                break;
              }
              sr16(0, quot & 65535);
              sr16(2, (dividend % divisor) & 65535);
            } else {
              const divisor = val | 0;
              if (divisor === 0) {
                lastBailOp = b;
                iter = maxInsn;
                break;
              }
              const dividend = (BigInt(gr32(2) | 0) << 32n) | BigInt(gr32(0) >>> 0);
              const quot = dividend / BigInt(divisor);
              if (quot > 2147483647n || quot < -2147483648n) {
                lastBailOp = b;
                iter = maxInsn;
                break;
              }
              sr32(0, Number(quot & 4294967295n));
              sr32(2, Number((dividend % BigInt(divisor)) & 4294967295n));
            }
          } else {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
          break;
        }

       // ──── SHL/SHR/SAR/ROL/ROR (0xD0, 0xD1, 0xD2, 0xD3, 0xC0, 0xC1) ────
        case 208:
       case 209:
       case 192:
       case 193:
       case 210:
       case 211:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const shOp = (modrm >> 3) & 7;
          const isWord = (b & 1);
          // 0=byte, 1=word/dword
          const sz = isWord ? opSize : 1;
          let count;
          if (b === 208 || b === 209) count = 1; else if (b === 192 || b === 193) {
            count = mem8[ci + 2] & 31;
            ilen += 1;
          } else count = gr8(1) & 31;
          // CL
          // Handle shift only for SHL(4), SHR(5), SAR(7)
          if (count === 0) break;
          let val;
          let isMem = (modrm >> 6) !== 3;
          let mea = 0, mlen = 0;
          if (isMem) {
            // Need to recalculate ilen for memory operand before count byte
            if (b === 192 || b === 193) ilen -= 1;
            // undo count byte
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            mea = m.ea;
            mlen = m.len;
            ilen += mlen;
            if (b === 192 || b === 193) {
              count = mem8[ci + 2 + mlen] & 31;
              ilen += 1;
            }
            if (sz === 1) val = rb(mea); else if (sz === 2) val = rw(mea); else val = rd(mea);
          } else {
            const rm = modrm & 7;
            if (sz === 1) val = gr8(rm); else if (sz === 2) val = gr16(rm); else val = gr32(rm);
          }
          if (count === 0) break;
          let res;
          const mask = SIZE_MASK[sz];
          switch (shOp) {
           case 4:
            // SHL
            lazyCF = ((val >> (sz * 8 - count)) & 1);
            res = (val << count) & mask;
            setFlagsArith(OP_SHL, res, val, count, sz);
            break;

           case 5:
            // SHR
            lazyCF = ((val >> (count - 1)) & 1);
            res = (val >>> count) & mask;
            setFlagsArith(OP_SHR, res, val, count, sz);
            break;

           case 7:
            {
              // SAR
              lazyCF = ((val >> (count - 1)) & 1);
              let sv = val;
              if (sv & SIZE_SIGN[sz]) sv |= ~mask;
              // sign extend
              res = (sv >> count) & mask;
              setFlagsArith(OP_SAR, res, val, count, sz);
              break;
            }

           case 0:
            {
              // ROL
              const bc = sz * 8;
              res = ((val << (count % bc)) | (val >>> (bc - count % bc))) & mask;
              lazyCF = res & 1;
              lazyOp = OP_NONE;
              break;
            }

           case 1:
            {
              // ROR
              const bc = sz * 8;
              res = ((val >>> (count % bc)) | (val << (bc - count % bc))) & mask;
              lazyCF = (res >>> (bc - 1)) & 1;
              lazyOp = OP_NONE;
              break;
            }

           default:
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
          if (isMem) {
            if (sz === 1) wb(mea, res); else if (sz === 2) ww(mea, res); else wd(mea, res);
          } else {
            const rm = modrm & 7;
            if (sz === 1) sr8(rm, res); else if (sz === 2) sr16(rm, res); else sr32(rm, res);
          }
          break;
        }

       // ──── INC/DEC r/m8 (0xFE) ────
        case 254:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const op = (modrm >> 3) & 7;
          if (op > 1) {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
          const oldCF = getCF();
          if ((modrm >> 6) === 3) {
            const rm = modrm & 7;
            let v = gr8(rm);
            if (op === 0) {
              v = (v + 1) & 255;
              setFlagsArith(OP_INC, v, v - 1, 1, 1);
            } else {
              v = (v - 1) & 255;
              setFlagsArith(OP_DEC, v, v + 1, 1, 1);
            }
            sr8(rm, v);
          } else {
            const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
            ilen += m.len;
            let v = rb(m.ea);
            if (op === 0) {
              v = (v + 1) & 255;
              setFlagsArith(OP_INC, v, v - 1, 1, 1);
            } else {
              v = (v - 1) & 255;
              setFlagsArith(OP_DEC, v, v + 1, 1, 1);
            }
            wb(m.ea, v);
          }
          lazyCF = oldCF;
          break;
        }

       // ──── INC/DEC/CALL/JMP/PUSH r/m16/32 (0xFF) ────
        case 255:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          const op = (modrm >> 3) & 7;
          if (op === 0 || op === 1) {
            // INC / DEC
            const oldCF = getCF();
            if ((modrm >> 6) === 3) {
              const rm = modrm & 7;
              if (opSize === 2) {
                let v = gr16(rm);
                if (op === 0) {
                  v = (v + 1) & 65535;
                  setFlagsArith(OP_INC, v, v - 1, 1, 2);
                } else {
                  v = (v - 1) & 65535;
                  setFlagsArith(OP_DEC, v, v + 1, 1, 2);
                }
                sr16(rm, v);
              } else {
                let v = gr32(rm);
                if (op === 0) {
                  v = (v + 1) >>> 0;
                  setFlagsArith(OP_INC, v, v - 1, 1, 4);
                } else {
                  v = (v - 1) >>> 0;
                  setFlagsArith(OP_DEC, v, v + 1, 1, 4);
                }
                sr32(rm, v);
              }
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              if (opSize === 2) {
                let v = rw(m.ea);
                if (op === 0) {
                  v = (v + 1) & 65535;
                  setFlagsArith(OP_INC, v, v - 1, 1, 2);
                } else {
                  v = (v - 1) & 65535;
                  setFlagsArith(OP_DEC, v, v + 1, 1, 2);
                }
                ww(m.ea, v);
              } else {
                let v = rd(m.ea);
                if (op === 0) {
                  v = (v + 1) >>> 0;
                  setFlagsArith(OP_INC, v, v - 1, 1, 4);
                } else {
                  v = (v - 1) >>> 0;
                  setFlagsArith(OP_DEC, v, v + 1, 1, 4);
                }
                wd(m.ea, v);
              }
            }
            lazyCF = oldCF;
          } else if (op === 2) {
            // CALL r/m16/32 (indirect)
            let target;
            if ((modrm >> 6) === 3) {
              target = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              target = opSize === 2 ? rw(m.ea) : rd(m.ea);
            }
            if (opSize === 2) push16((ip + ilen) & 65535, ssBase); else push32((ip + ilen) & 4294967295, ssBase);
            ip = target & 65535;
            ilen = 0;
            executed++;
            wr16(R_IP, ip);
            continue;
          } else if (op === 4) {
            // JMP r/m16/32 (indirect)
            let target;
            if ((modrm >> 6) === 3) {
              target = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              target = opSize === 2 ? rw(m.ea) : rd(m.ea);
            }
            ip = target & 65535;
            ilen = 0;
            executed++;
            wr16(R_IP, ip);
            continue;
          } else if (op === 6) {
            // PUSH r/m16/32
            let val;
            if ((modrm >> 6) === 3) {
              val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
            } else {
              const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
              ilen += m.len;
              val = opSize === 2 ? rw(m.ea) : rd(m.ea);
            }
            if (opSize === 2) push16(val, ssBase); else push32(val, ssBase);
          } else {
            // CALL/JMP far or undefined /7 — fallback
            if (statTotalCalls < 100) console.log("[JIT] FF/" + op + " modrm=0x" + modrm.toString(16) + " at " + csBase.toString(16) + ":" + ip.toString(16) + " ci+1=" + mem8[ci].toString(16) + "," + mem8[ci + 1].toString(16) + "," + mem8[ci + 2].toString(16));
            lastBailOp = 65280 | op;
            iter = maxInsn;
            break;
          }
          break;
        }

       // ──── 0x0F two-byte opcodes ────
        case 15:
        {
          const b2 = mem8[ci + 1];
          ilen += 2;
          switch (b2) {
           // Jcc rel16/32 (0x0F 0x80-0x8F)
            case 128:
           case 129:
           case 130:
           case 131:
           case 132:
           case 133:
           case 134:
           case 135:
           case 136:
           case 137:
           case 138:
           case 139:
           case 140:
           case 141:
           case 142:
           case 143:
            {
              let rel;
              if (opSize === 2) {
                rel = mem8[ci + 2] | (mem8[ci + 3] << 8);
                if (rel > 32767) rel -= 65536;
                ilen += 2;
              } else {
                rel = mem8[ci + 2] | (mem8[ci + 3] << 8) | (mem8[ci + 4] << 16) | (mem8[ci + 5] << 24);
                ilen += 4;
              }
              if (testCC(b2 - 128)) {
                ip = (ip + ilen + rel) & 65535;
                ilen = 0;
                executed++;
                wr16(R_IP, ip);
                continue;
              }
              break;
            }

           // MOVZX r16/32, r/m8 (0x0F 0xB6)
            case 182:
            {
              const modrm = mem8[ci + 2];
              ilen += 1;
              const reg = (modrm >> 3) & 7;
              let val;
              if ((modrm >> 6) === 3) val = gr8(modrm & 7); else {
                const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 3, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 3, effDS, effSS);
                ilen += m.len;
                val = rb(m.ea);
              }
              if (opSize === 2) sr16(reg, val); else sr32(reg, val);
              break;
            }

           // MOVZX r16/32, r/m16 (0x0F 0xB7)
            case 183:
            {
              const modrm = mem8[ci + 2];
              ilen += 1;
              const reg = (modrm >> 3) & 7;
              let val;
              if ((modrm >> 6) === 3) val = gr16(modrm & 7); else {
                const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 3, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 3, effDS, effSS);
                ilen += m.len;
                val = rw(m.ea);
              }
              sr32(reg, val);
              // zero-extend to 32
              break;
            }

           // MOVSX r16/32, r/m8 (0x0F 0xBE)
            case 190:
            {
              const modrm = mem8[ci + 2];
              ilen += 1;
              const reg = (modrm >> 3) & 7;
              let val;
              if ((modrm >> 6) === 3) val = gr8(modrm & 7); else {
                const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 3, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 3, effDS, effSS);
                ilen += m.len;
                val = rb(m.ea);
              }
              if (val > 127) val |= (opSize === 2 ? 65280 : 4294967040);
              if (opSize === 2) sr16(reg, val & 65535); else sr32(reg, val >>> 0);
              break;
            }

           // MOVSX r32, r/m16 (0x0F 0xBF)
            case 191:
            {
              const modrm = mem8[ci + 2];
              ilen += 1;
              const reg = (modrm >> 3) & 7;
              let val;
              if ((modrm >> 6) === 3) val = gr16(modrm & 7); else {
                const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 3, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 3, effDS, effSS);
                ilen += m.len;
                val = rw(m.ea);
              }
              if (val > 32767) val |= 4294901760;
              sr32(reg, val >>> 0);
              break;
            }

           // SETcc r/m8 (0x0F 0x90-0x9F)
            case 144:
           case 145:
           case 146:
           case 147:
           case 148:
           case 149:
           case 150:
           case 151:
           case 152:
           case 153:
           case 154:
           case 155:
           case 156:
           case 157:
           case 158:
           case 159:
            {
              const modrm = mem8[ci + 2];
              ilen += 1;
              const val = testCC(b2 - 144) ? 1 : 0;
              if ((modrm >> 6) === 3) sr8(modrm & 7, val); else {
                const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 3, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 3, effDS, effSS);
                ilen += m.len;
                wb(m.ea, val);
              }
              break;
            }

           // IMUL r16/32, r/m16/32 (0x0F 0xAF)
            case 175:
            {
              const modrm = mem8[ci + 2];
              ilen += 1;
              const reg = (modrm >> 3) & 7;
              let val;
              if ((modrm >> 6) === 3) {
                val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
              } else {
                const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 3, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 3, effDS, effSS);
                ilen += m.len;
                val = opSize === 2 ? rw(m.ea) : rd(m.ea);
              }
              if (opSize === 2) {
                const a = (gr16(reg) << 16) >> 16, b2 = (val << 16) >> 16;
                const result = a * b2;
                sr16(reg, result & 65535);
                lazyCF = (result !== ((result << 16) >> 16)) ? 1 : 0;
              } else {
                const result = Math.imul(gr32(reg), val);
                sr32(reg, result >>> 0);
                // Approximate OF/CF (exact requires 64-bit product)
                const a = gr32(reg) | 0, b3 = val | 0;
                const big = BigInt(a) * BigInt(b3);
                lazyCF = (big !== BigInt(result | 0)) ? 1 : 0;
              }
              lazyOp = OP_NONE;
              lazyRes = opSize === 2 ? gr16(reg) : gr32(reg);
              lazySize = opSize;
              break;
            }

           // BSF r16/32, r/m16/32 (0x0F 0xBC)
            case 188:
            {
              const modrm = mem8[ci + 2];
              ilen += 1;
              const reg = (modrm >> 3) & 7;
              let val;
              if ((modrm >> 6) === 3) {
                val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
              } else {
                const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 3, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 3, effDS, effSS);
                ilen += m.len;
                val = opSize === 2 ? rw(m.ea) : rd(m.ea);
              }
              if (val === 0) {
                lazyOp = OP_NONE;
                lazyRes = 0;
                lazySize = opSize;
              } else {
                let bit = 0;
                while (!(val & (1 << bit))) bit++;
                if (opSize === 2) sr16(reg, bit); else sr32(reg, bit);
                lazyOp = OP_NONE;
                lazyRes = 1;
                lazySize = opSize;
              }
              break;
            }

           // BSR r16/32, r/m16/32 (0x0F 0xBD)
            case 189:
            {
              const modrm = mem8[ci + 2];
              ilen += 1;
              const reg = (modrm >> 3) & 7;
              let val;
              if ((modrm >> 6) === 3) {
                val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
              } else {
                const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 3, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 3, effDS, effSS);
                ilen += m.len;
                val = opSize === 2 ? rw(m.ea) : rd(m.ea);
              }
              if (val === 0) {
                lazyOp = OP_NONE;
                lazyRes = 0;
                lazySize = opSize;
              } else {
                let bit = opSize === 2 ? 15 : 31;
                while (!(val & (1 << bit))) bit--;
                if (opSize === 2) sr16(reg, bit); else sr32(reg, bit);
                lazyOp = OP_NONE;
                lazyRes = 1;
                lazySize = opSize;
              }
              break;
            }

           // BT/BTS/BTR/BTC r/m, imm8 (0x0F 0xBA)
            case 186:
            {
              const modrm = mem8[ci + 2];
              ilen += 1;
              const btOp = (modrm >> 3) & 7;
              if (btOp < 4) {
                lastBailOp = 3840 | b2;
                iter = maxInsn;
                break;
              }
              let val, bitIdx;
              if ((modrm >> 6) === 3) {
                val = opSize === 2 ? gr16(modrm & 7) : gr32(modrm & 7);
                bitIdx = mem8[ci + 3] & (opSize === 2 ? 15 : 31);
                ilen += 1;
              } else {
                const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 3, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 3, effDS, effSS);
                ilen += m.len;
                val = opSize === 2 ? rw(m.ea) : rd(m.ea);
                bitIdx = mem8[ci + 3 + m.len] & (opSize === 2 ? 15 : 31);
                ilen += 1;
              }
              lazyCF = (val >> bitIdx) & 1;
              lazyOp = OP_NONE;
              if (btOp === 5) val |= (1 << bitIdx); else if (btOp === 6) val &= ~(1 << bitIdx); else if (btOp === 7) val ^= (1 << bitIdx);
              // BTC
              // btOp === 4 is BT (no modification)
              if (btOp !== 4) {
                if ((modrm >> 6) === 3) {
                  if (opSize === 2) sr16(modrm & 7, val & 65535); else sr32(modrm & 7, val >>> 0);
                }
              }
              break;
            }

           default:
            // Unsupported 0x0F opcode — fallback
            lastBailOp = 3840 | b2;
            iter = maxInsn;
            break;
          }
          break;
        }

       // ──── JMP far (0xEA) ────
        case 234:
        {
          if (opSize === 2) {
            const newIP = mem8[ci + 1] | (mem8[ci + 2] << 8);
            const newCS = mem8[ci + 3] | (mem8[ci + 4] << 8);
            ilen += 5;
            // Update CS
            wr16(S_CS + SEG_SEL, newCS);
            if (realMode) wr64(S_CS + SEG_BASE, newCS << 4);
            ip = newIP;
            ilen = 0;
            executed++;
            wr16(R_IP, ip);
            continue;
          } else {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
        }

       // ──── CALL far (0x9A) ────
        case 154:
        {
          if (opSize === 2) {
            const newIP = mem8[ci + 1] | (mem8[ci + 2] << 8);
            const newCS = mem8[ci + 3] | (mem8[ci + 4] << 8);
            ilen += 5;
            push16(rr16(S_CS + SEG_SEL), ssBase);
            // push CS
            push16((ip + ilen) & 65535, ssBase);
            // push IP
            wr16(S_CS + SEG_SEL, newCS);
            if (realMode) wr64(S_CS + SEG_BASE, newCS << 4);
            ip = newIP;
            ilen = 0;
            executed++;
            wr16(R_IP, ip);
            continue;
          } else {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
        }

       // ──── RETF (0xCB) ────
        case 203:
        {
          if (opSize === 2) {
            const newIP = pop16(ssBase);
            const newCS = pop16(ssBase);
            wr16(S_CS + SEG_SEL, newCS);
            if (realMode) wr64(S_CS + SEG_BASE, newCS << 4);
            ip = newIP;
            ilen = 0;
            executed++;
            wr16(R_IP, ip);
            continue;
          } else {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
        }

       // ──── PUSHA (0x60) ────
        case 96:
        {
          const sp0 = gr16(4);
          if (opSize === 2) {
            push16(gr16(0), ssBase);
            push16(gr16(1), ssBase);
            push16(gr16(2), ssBase);
            push16(gr16(3), ssBase);
            push16(sp0, ssBase);
            push16(gr16(5), ssBase);
            push16(gr16(6), ssBase);
            push16(gr16(7), ssBase);
          } else {
            const esp0 = gr32(4);
            push32(gr32(0), ssBase);
            push32(gr32(1), ssBase);
            push32(gr32(2), ssBase);
            push32(gr32(3), ssBase);
            push32(esp0, ssBase);
            push32(gr32(5), ssBase);
            push32(gr32(6), ssBase);
            push32(gr32(7), ssBase);
          }
          ilen += 1;
          break;
        }

       // ──── POPA (0x61) ────
        case 97:
        if (opSize === 2) {
          sr16(7, pop16(ssBase));
          sr16(6, pop16(ssBase));
          sr16(5, pop16(ssBase));
          pop16(ssBase);
          // skip SP
          sr16(3, pop16(ssBase));
          sr16(2, pop16(ssBase));
          sr16(1, pop16(ssBase));
          sr16(0, pop16(ssBase));
        } else {
          sr32(7, pop32(ssBase));
          sr32(6, pop32(ssBase));
          sr32(5, pop32(ssBase));
          pop32(ssBase);
          sr32(3, pop32(ssBase));
          sr32(2, pop32(ssBase));
          sr32(1, pop32(ssBase));
          sr32(0, pop32(ssBase));
        }
        ilen += 1;
        break;

       // ──── PUSH ES/CS/SS/DS (0x06,0x0E,0x16,0x1E) ────
        case 6:
        push16(rr16(S_ES + SEG_SEL), ssBase);
        ilen += 1;
        break;

       case 14:
        push16(rr16(S_CS + SEG_SEL), ssBase);
        ilen += 1;
        break;

       case 22:
        push16(rr16(S_SS + SEG_SEL), ssBase);
        ilen += 1;
        break;

       case 30:
        push16(rr16(S_DS + SEG_SEL), ssBase);
        ilen += 1;
        break;

       // ──── POP ES/SS/DS (0x07,0x17,0x1F) ────
        case 7:
        {
          const v = pop16(ssBase);
          wr16(S_ES + SEG_SEL, v);
          if (realMode) {
            wr64(S_ES + SEG_BASE, v << 4);
            esBase = v << 4;
          }
          ilen += 1;
          break;
        }

       case 23:
        {
          const v = pop16(ssBase);
          wr16(S_SS + SEG_SEL, v);
          if (realMode) {
            wr64(S_SS + SEG_BASE, v << 4);
            ssBase = v << 4;
          }
          ilen += 1;
          break;
        }

       case 31:
        {
          const v = pop16(ssBase);
          wr16(S_DS + SEG_SEL, v);
          if (realMode) {
            wr64(S_DS + SEG_BASE, v << 4);
            dsBase = v << 4;
          }
          ilen += 1;
          break;
        }

       // ──── LES r, m (0xC4) ────
        case 196:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          if ((modrm >> 6) === 3) {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
          // must be memory
          const reg = (modrm >> 3) & 7;
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
          ilen += m.len;
          if (opSize === 2) {
            sr16(reg, rw(m.ea));
          } else {
            sr32(reg, rd(m.ea));
          }
          const seg = rw(m.ea + opSize);
          wr16(S_ES + SEG_SEL, seg);
          if (realMode) {
            wr64(S_ES + SEG_BASE, seg << 4);
            esBase = seg << 4;
          }
          break;
        }

       // ──── LDS r, m (0xC5) ────
        case 197:
        {
          const modrm = mem8[ci + 1];
          ilen += 2;
          if ((modrm >> 6) === 3) {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
          const reg = (modrm >> 3) & 7;
          const m = addrSize === 2 ? decodeModRM16(modrm, mem8, ci + 2, effDS, effSS) : decodeModRM32(modrm, mem8, ci + 2, effDS, effSS);
          ilen += m.len;
          if (opSize === 2) {
            sr16(reg, rw(m.ea));
          } else {
            sr32(reg, rd(m.ea));
          }
          const seg = rw(m.ea + opSize);
          wr16(S_DS + SEG_SEL, seg);
          if (realMode) {
            wr64(S_DS + SEG_BASE, seg << 4);
            dsBase = seg << 4;
          }
          break;
        }

       // ──── LEAVE (0xC9) ────
        case 201:
        sr16(4, gr16(5));
        // SP = BP
        if (opSize === 2) sr16(5, pop16(ssBase)); else sr32(5, pop32(ssBase));
        ilen += 1;
        break;

       // ──── XLAT (0xD7) — AL = [DS:BX+AL] ────
        case 215:
        {
          const addr = ((segOverride >= 0 ? segBase(segOverride) : dsBase) + ((gr16(3) + gr8(0)) & 65535)) & 1048575;
          sr8(0, rb(addr));
          ilen += 1;
          break;
        }

       // ──── AAA (0x37) — ASCII adjust after addition ────
        case 55:
        {
          let al = gr8(0), ah = gr8(4);
          if ((al & 15) > 9 || getAF()) {
            al = (al + 6) & 255;
            ah = (ah + 1) & 255;
            sr8(4, ah);
            lazyCF = 1;
            lazyOp = OP_NONE;
          } else {
            lazyCF = 0;
            lazyOp = OP_NONE;
          }
          sr8(0, al & 15);
          ilen += 1;
          break;
        }

       // ──── AAS (0x3F) — ASCII adjust after subtraction ────
        case 63:
        {
          let al = gr8(0), ah = gr8(4);
          if ((al & 15) > 9 || getAF()) {
            al = (al - 6) & 255;
            ah = (ah - 1) & 255;
            sr8(4, ah);
            lazyCF = 1;
            lazyOp = OP_NONE;
          } else {
            lazyCF = 0;
            lazyOp = OP_NONE;
          }
          sr8(0, al & 15);
          ilen += 1;
          break;
        }

       // ──── DAA (0x27) — Decimal adjust after addition ────
        case 39:
        {
          let al = gr8(0), cf = getCF(), af = getAF();
          let newCF = 0;
          if ((al & 15) > 9 || af) {
            al += 6;
            newCF = cf || (al > 255 ? 1 : 0);
            al &= 255;
            af = 1;
          } else af = 0;
          if (al > 153 || cf) {
            al = (al + 96) & 255;
            newCF = 1;
          }
          sr8(0, al);
          lazyCF = newCF;
          lazyOp = OP_NONE;
          lazyRes = al;
          lazySize = 1;
          ilen += 1;
          break;
        }

       // ──── DAS (0x2F) — Decimal adjust after subtraction ────
        case 47:
        {
          let al = gr8(0), cf = getCF(), af = getAF();
          let newCF = 0;
          if ((al & 15) > 9 || af) {
            al -= 6;
            newCF = cf || (al < 0 ? 1 : 0);
            al &= 255;
            af = 1;
          } else af = 0;
          if (al > 153 || cf) {
            al = (al - 96) & 255;
            newCF = 1;
          }
          sr8(0, al);
          lazyCF = newCF;
          lazyOp = OP_NONE;
          lazyRes = al;
          lazySize = 1;
          ilen += 1;
          break;
        }

       // ──── AAM (0xD4) — ASCII adjust after multiply ────
        case 212:
        {
          const base = mem8[ci + 1] || 10;
          // usually 0x0A
          if (base === 0) {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
          const al = gr8(0);
          sr8(4, (al / base) & 255);
          sr8(0, (al % base) & 255);
          lazyOp = OP_NONE;
          lazyRes = gr8(0);
          lazySize = 1;
          lazyCF = 0;
          ilen += 2;
          break;
        }

       // ──── AAD (0xD5) — ASCII adjust before division ────
        case 213:
        {
          const base = mem8[ci + 1] || 10;
          const al = ((gr8(4) * base) + gr8(0)) & 255;
          sr8(0, al);
          sr8(4, 0);
          lazyOp = OP_NONE;
          lazyRes = al;
          lazySize = 1;
          lazyCF = 0;
          ilen += 2;
          break;
        }

       // ──── INT n (0xCD imm8) — software interrupt ────
        case 205:
        {
          if (!realMode) {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
          // protected mode INT needs IDT
          const intNum = mem8[ci + 1];
          // Bail to IEM for video BIOS (INT 10h) — needs MMIO for VGA memory writes
          if (intNum === 16) {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
          // Materialize FLAGS: arithmetic bits from lazy, IF/DF/IOPL from stored flags
          const arithFlags = flagsToWord();
          const pushFlags = (flags & ~2261) | (arithFlags & 2261);
          // Push FLAGS, CS, IP (return address = after INT instruction)
          const retIP = (ip + pos + 2) & 65535;
          push16(pushFlags, ssBase);
          push16(rr16(S_CS + SEG_SEL), ssBase);
          push16(retIP, ssBase);
          // Clear IF and TF
          flags = pushFlags & ~768;
          // IF=0, TF=0
          loadFlags(flags);
          // Read IVT entry: [IP:CS] at intNum*4
          const ivtAddr = intNum * 4;
          const newIP = rw(ivtAddr);
          const newCS = rw(ivtAddr + 2);
          wr16(S_CS + SEG_SEL, newCS);
          wr64(S_CS + SEG_BASE, newCS << 4);
          ip = newIP;
          ilen = 0;
          executed++;
          wr16(R_IP, ip);
          wr32(R_FLAGS, flags);
          continue;
        }

       // ──── INT3 (0xCC) — breakpoint ────
        case 204:
        {
          if (!realMode) {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
          const arithF3 = flagsToWord();
          const pushF3 = (flags & ~2261) | (arithF3 & 2261);
          const retIP3 = (ip + pos + 1) & 65535;
          push16(pushF3, ssBase);
          push16(rr16(S_CS + SEG_SEL), ssBase);
          push16(retIP3, ssBase);
          flags = pushF3 & ~768;
          loadFlags(flags);
          const newIP3 = rw(3 * 4);
          const newCS3 = rw(3 * 4 + 2);
          wr16(S_CS + SEG_SEL, newCS3);
          wr64(S_CS + SEG_BASE, newCS3 << 4);
          ip = newIP3;
          ilen = 0;
          executed++;
          wr16(R_IP, ip);
          wr32(R_FLAGS, flags);
          continue;
        }

       // ──── IRET (0xCF) — return from interrupt ────
        case 207:
        {
          if (!realMode) {
            lastBailOp = b;
            iter = maxInsn;
            break;
          }
          const iretIP = pop16(ssBase);
          const iretCS = pop16(ssBase);
          const iretFlags = pop16(ssBase);
          wr16(S_CS + SEG_SEL, iretCS);
          wr64(S_CS + SEG_BASE, iretCS << 4);
          ip = iretIP;
          // Restore full flags
          flags = (iretFlags & 65535) | 2;
          // bit 1 always set
          loadFlags(flags);
          ilen = 0;
          executed++;
          wr16(R_IP, ip);
          wr32(R_FLAGS, flags);
          continue;
        }

       // ──── HLT (0xF4) — halt processor ────
        case 244:
        // Advance IP past HLT then bail — let IEM handle the halt state
        ip = (ip + pos + 1) & 65535;
        wr16(R_IP, ip);
        executed++;
        lastBailOp = b;
        iter = maxInsn;
        break;

       // ──── Unsupported — fallback to IEM ────
        default:
        {
          // CPUID, RDTSC, etc. — let IEM handle
          lastBailOp = b;
          iter = maxInsn;
          break;
        }
      }
      // end switch
      if (ilen > 0) {
        ip = (ip + ilen) & 65535;
        executed++;
      }
    }
    // end for
    // ── Store state back ──
    wr16(R_IP, ip);
    // Reconstruct RFLAGS
    const newFlags = (flags & 4294963200) | flagsToWord();
    wr32(R_FLAGS, newFlags);
    // Track bail opcode if we exited early
    if (lastBailOp >= 0) {
      fallbackOpcodes.set(lastBailOp, (fallbackOpcodes.get(lastBailOp) || 0) + 1);
    }
    return executed;
  }
  // ── Stats ──
  let statTotalInsns = 0, statTotalCalls = 0, statFallbacks = 0;
  let statLastReport = 0;
  const fallbackOpcodes = new Map;
  // opcode -> count
  function execBlockWrapped(cpuP, ramB, maxInsn) {
    statTotalCalls++;
    const n = execBlock(cpuP, ramB, maxInsn);
    if (n > 0) {
      statTotalInsns += n;
    } else {
      statFallbacks++;
    }
    // Log stats every 5 seconds
    const now = Date.now();
    if (now - statLastReport > 5e3) {
      statLastReport = now;
      {
        // Top fallback opcodes
        const sorted = [ ...fallbackOpcodes.entries() ].sort((a, b) => b[1] - a[1]).slice(0, 8);
        const topStr = sorted.map(([op, cnt]) => "0x" + op.toString(16) + ":" + cnt).join(" ");
        console.log("[JIT] calls=" + statTotalCalls + " insns=" + statTotalInsns + " fallbacks=" + statFallbacks + " avg=" + (statTotalInsns / Math.max(1, statTotalCalls - statFallbacks)).toFixed(1) + " top=[" + topStr + "]");
      }
    }
    return n;
  }
  // ── Public API ──
  return {
    execBlock: execBlockWrapped,
    init,
    setRomBuffer,
    setPortIO: function(inFn, outFn) {
      portInFn = inFn;
      portOutFn = outFn;
    },
    getStats: function() {
      return {
        totalInsns: statTotalInsns,
        totalCalls: statTotalCalls,
        fallbacks: statFallbacks
      };
    }
  };
})();

// end VBoxJIT IIFE
// end include: jit-pre.js
var arguments_ = [];

var thisProgram = "./this.program";

var quit_ = (status, toThrow) => {
  throw toThrow;
};

// In MODULARIZE mode _scriptName needs to be captured already at the very top of the page immediately when the page is parsed, so it is generated there
// before the page load. In non-MODULARIZE modes generate it here.
var _scriptName = globalThis.document?.currentScript?.src;

if (ENVIRONMENT_IS_WORKER) {
  _scriptName = self.location.href;
}

// `/` should be present at the end if `scriptDirectory` is not empty
var scriptDirectory = "";

function locateFile(path) {
  if (Module["locateFile"]) {
    return Module["locateFile"](path, scriptDirectory);
  }
  return scriptDirectory + path;
}

// Hooks that are implemented differently in different runtime environments.
var readAsync, readBinary;

// Note that this includes Node.js workers when relevant (pthreads is enabled).
// Node.js workers are detected as a combination of ENVIRONMENT_IS_WORKER and
// ENVIRONMENT_IS_NODE.
if (ENVIRONMENT_IS_WEB || ENVIRONMENT_IS_WORKER) {
  try {
    scriptDirectory = new URL(".", _scriptName).href;
  } catch {}
  {
    // include: web_or_worker_shell_read.js
    if (ENVIRONMENT_IS_WORKER) {
      readBinary = url => {
        var xhr = new XMLHttpRequest;
        xhr.open("GET", url, false);
        xhr.responseType = "arraybuffer";
        xhr.send(null);
        return new Uint8Array(/** @type{!ArrayBuffer} */ (xhr.response));
      };
    }
    readAsync = async url => {
      var response = await fetch(url, {
        credentials: "same-origin"
      });
      if (response.ok) {
        return response.arrayBuffer();
      }
      throw new Error(response.status + " : " + response.url);
    };
  }
} else {}

var out = console.log.bind(console);

var err = console.error.bind(console);

// end include: shell.js
// include: preamble.js
// === Preamble library stuff ===
// Documentation for the public APIs defined in this file must be updated in:
//    site/source/docs/api_reference/preamble.js.rst
// A prebuilt local version of the documentation is available at:
//    site/build/text/docs/api_reference/preamble.js.txt
// You can also build docs locally as HTML or other formats in site/
// An online HTML version (which may be of a different version of Emscripten)
//    is up at http://kripken.github.io/emscripten-site/docs/api_reference/preamble.js.html
var wasmBinary;

// Wasm globals
// For sending to workers.
var wasmModule;

//========================================
// Runtime essentials
//========================================
// whether we are quitting the application. no code should run after this.
// set in exit() and abort()
var ABORT = false;

// set by exit() and abort().  Passed to 'onExit' handler.
// NOTE: This is also used as the process return code in shell environments
// but only when noExitRuntime is false.
var EXITSTATUS;

// In STRICT mode, we only define assert() when ASSERTIONS is set.  i.e. we
// don't define it at all in release modes.  This matches the behaviour of
// MINIMAL_RUNTIME.
// TODO(sbc): Make this the default even without STRICT enabled.
/** @type {function(*, string=)} */ function assert(condition, text) {
  if (!condition) {
    // This build was created without ASSERTIONS defined.  `assert()` should not
    // ever be called in this configuration but in case there are callers in
    // the wild leave this simple abort() implementation here for now.
    abort(text);
  }
}

/**
 * Indicates whether filename is delivered via file protocol (as opposed to http/https)
 * @noinline
 */ var isFileURI = filename => filename.startsWith("file://");

// include: runtime_common.js
// include: runtime_stack_check.js
// end include: runtime_stack_check.js
// include: runtime_exceptions.js
// end include: runtime_exceptions.js
// include: runtime_debug.js
// end include: runtime_debug.js
// Support for growable heap + pthreads, where the buffer may change, so JS views
// must be updated.
function growMemViews() {
  // `updateMemoryViews` updates all the views simultaneously, so it's enough to check any of them.
  if (wasmMemory.buffer != HEAP8.buffer) {
    updateMemoryViews();
  }
}

// include: runtime_pthread.js
// Pthread Web Worker handling code.
// This code runs only on pthread web workers and handles pthread setup
// and communication with the main thread via postMessage.
var startWorker;

if (ENVIRONMENT_IS_PTHREAD) {
  // Thread-local guard variable for one-time init of the JS state
  var initializedJS = false;
  // Turn unhandled rejected promises into errors so that the main thread will be
  // notified about them.
  self.onunhandledrejection = e => {
    throw e.reason || e;
  };
  function handleMessage(e) {
    try {
      var msgData = e["data"];
      //dbg('msgData: ' + Object.keys(msgData));
      var cmd = msgData.cmd;
      if (cmd === "load") {
        // Preload command that is called once per worker to parse and load the Emscripten code.
        // Until we initialize the runtime, queue up any further incoming messages.
        let messageQueue = [];
        self.onmessage = e => messageQueue.push(e);
        // And add a callback for when the runtime is initialized.
        startWorker = () => {
          // Notify the main thread that this thread has loaded.
          postMessage({
            cmd: "loaded"
          });
          // Process any messages that were queued before the thread was ready.
          for (let msg of messageQueue) {
            handleMessage(msg);
          }
          // Restore the real message handler.
          self.onmessage = handleMessage;
        };
        // Use `const` here to ensure that the variable is scoped only to
        // that iteration, allowing safe reference from a closure.
        for (const handler of msgData.handlers) {
          // If the main module has a handler for a certain event, but no
          // handler exists on the pthread worker, then proxy that handler
          // back to the main thread.
          if (!Module[handler] || Module[handler].proxy) {
            Module[handler] = (...args) => {
              postMessage({
                cmd: "callHandler",
                handler,
                args
              });
            };
            // Rebind the out / err handlers if needed
            if (handler == "print") out = Module[handler];
            if (handler == "printErr") err = Module[handler];
          }
        }
        wasmMemory = msgData.wasmMemory;
        updateMemoryViews();
        wasmModule = msgData.wasmModule;
        createWasm();
        run();
      } else if (cmd === "run") {
        // Call inside JS module to set up the stack frame for this pthread in JS module scope.
        // This needs to be the first thing that we do, as we cannot call to any C/C++ functions
        // until the thread stack is initialized.
        establishStackSpace(msgData.pthread_ptr);
        // Pass the thread address to wasm to store it for fast access.
        __emscripten_thread_init(msgData.pthread_ptr, /*is_main=*/ 0, /*is_runtime=*/ 0, /*can_block=*/ 1, 0, 0);
        PThread.threadInitTLS();
        // Await mailbox notifications with `Atomics.waitAsync` so we can start
        // using the fast `Atomics.notify` notification path.
        __emscripten_thread_mailbox_await(msgData.pthread_ptr);
        if (!initializedJS) {
          initializedJS = true;
        }
        try {
          invokeEntryPoint(msgData.start_routine, msgData.arg);
        } catch (ex) {
          if (ex != "unwind") {
            // The pthread "crashed".  Do not call `_emscripten_thread_exit` (which
            // would make this thread joinable).  Instead, re-throw the exception
            // and let the top level handler propagate it back to the main thread.
            throw ex;
          }
        }
      } else if (msgData.target === "setimmediate") {} else if (cmd === "checkMailbox") {
        if (initializedJS) {
          checkMailbox();
        }
      } else if (cmd) {
        // The received message looks like something that should be handled by this message
        // handler, (since there is a cmd field present), but is not one of the
        // recognized commands:
        err(`worker: received unknown command ${cmd}`);
        err(msgData);
      }
    } catch (ex) {
      __emscripten_thread_crashed();
      throw ex;
    }
  }
  self.onmessage = handleMessage;
}

// ENVIRONMENT_IS_PTHREAD
// end include: runtime_pthread.js
// Memory management
var /** @type {!Int8Array} */ HEAP8, /** @type {!Uint8Array} */ HEAPU8, /** @type {!Int16Array} */ HEAP16, /** @type {!Uint16Array} */ HEAPU16, /** @type {!Int32Array} */ HEAP32, /** @type {!Uint32Array} */ HEAPU32, /** @type {!Float32Array} */ HEAPF32, /** @type {!Float64Array} */ HEAPF64;

// BigInt64Array type is not correctly defined in closure
var /** not-@type {!BigInt64Array} */ HEAP64, /* BigUint64Array type is not correctly defined in closure
/** not-@type {!BigUint64Array} */ HEAPU64;

var runtimeInitialized = false;

function updateMemoryViews() {
  var b = wasmMemory.buffer;
  HEAP8 = new Int8Array(b);
  HEAP16 = new Int16Array(b);
  HEAPU8 = new Uint8Array(b);
  HEAPU16 = new Uint16Array(b);
  HEAP32 = new Int32Array(b);
  HEAPU32 = new Uint32Array(b);
  HEAPF32 = new Float32Array(b);
  HEAPF64 = new Float64Array(b);
  HEAP64 = new BigInt64Array(b);
  HEAPU64 = new BigUint64Array(b);
}

// In non-standalone/normal mode, we create the memory here.
// include: runtime_init_memory.js
// Create the wasm memory. (Note: this only applies if IMPORTED_MEMORY is defined)
// check for full engine support (use string 'subarray' to avoid closure compiler confusion)
function initMemory() {
  if ((ENVIRONMENT_IS_PTHREAD)) {
    return;
  }
  if (Module["wasmMemory"]) {
    wasmMemory = Module["wasmMemory"];
  } else {
    var INITIAL_MEMORY = Module["INITIAL_MEMORY"] || 536870912;
    /** @suppress {checkTypes} */ wasmMemory = new WebAssembly.Memory({
      "initial": BigInt(INITIAL_MEMORY / 65536),
      // In theory we should not need to emit the maximum if we want "unlimited"
      // or 4GB of memory, but VMs error on that atm, see
      // https://github.com/emscripten-core/emscripten/issues/14130
      // And in the pthreads case we definitely need to emit a maximum. So
      // always emit one.
      "maximum": 32768n,
      "shared": true,
      "address": "i64"
    });
  }
  updateMemoryViews();
}

// end include: runtime_init_memory.js
// include: memoryprofiler.js
// end include: memoryprofiler.js
// end include: runtime_common.js
function preRun() {
  if (Module["preRun"]) {
    if (typeof Module["preRun"] == "function") Module["preRun"] = [ Module["preRun"] ];
    while (Module["preRun"].length) {
      addOnPreRun(Module["preRun"].shift());
    }
  }
  // Begin ATPRERUNS hooks
  callRuntimeCallbacks(onPreRuns);
}

function initRuntime() {
  runtimeInitialized = true;
  if (ENVIRONMENT_IS_PTHREAD) return startWorker();
  // Begin ATINITS hooks
  if (!Module["noFSInit"] && !FS.initialized) FS.init();
  TTY.init();
  SOCKFS.root = FS.mount(SOCKFS, {}, null);
  // End ATINITS hooks
  wasmExports["__wasm_call_ctors"]();
  // Begin ATPOSTCTORS hooks
  FS.ignorePermissions = false;
}

function preMain() {}

function postRun() {
  if ((ENVIRONMENT_IS_PTHREAD)) {
    return;
  }
  // PThreads reuse the runtime from the main thread.
  if (Module["postRun"]) {
    if (typeof Module["postRun"] == "function") Module["postRun"] = [ Module["postRun"] ];
    while (Module["postRun"].length) {
      addOnPostRun(Module["postRun"].shift());
    }
  }
  // Begin ATPOSTRUNS hooks
  callRuntimeCallbacks(onPostRuns);
}

/** @param {string|number=} what */ function abort(what) {
  Module["onAbort"]?.(what);
  what = "Aborted(" + what + ")";
  // TODO(sbc): Should we remove printing and leave it up to whoever
  // catches the exception?
  err(what);
  ABORT = true;
  what += ". Build with -sASSERTIONS for more info.";
  // Use a wasm runtime error, because a JS error might be seen as a foreign
  // exception, which means we'd run destructors on it. We need the error to
  // simply make the program stop.
  // FIXME This approach does not work in Wasm EH because it currently does not assume
  // all RuntimeErrors are from traps; it decides whether a RuntimeError is from
  // a trap or not based on a hidden field within the object. So at the moment
  // we don't have a way of throwing a wasm trap from JS. TODO Make a JS API that
  // allows this in the wasm spec.
  // Suppress closure compiler warning here. Closure compiler's builtin extern
  // definition for WebAssembly.RuntimeError claims it takes no arguments even
  // though it can.
  // TODO(https://github.com/google/closure-compiler/pull/3913): Remove if/when upstream closure gets fixed.
  /** @suppress {checkTypes} */ var e = new WebAssembly.RuntimeError(what);
  // Throw the error whether or not MODULARIZE is set because abort is used
  // in code paths apart from instantiation where an exception is expected
  // to be thrown when abort is called.
  throw e;
}

var wasmBinaryFile;

function findWasmBinary() {
  return locateFile("vbox-wasm.wasm");
}

function getBinarySync(file) {
  if (file == wasmBinaryFile && wasmBinary) {
    return new Uint8Array(wasmBinary);
  }
  if (readBinary) {
    return readBinary(file);
  }
  // Throwing a plain string here, even though it not normally advisable since
  // this gets turning into an `abort` in instantiateArrayBuffer.
  throw "both async and sync fetching of the wasm failed";
}

async function getWasmBinary(binaryFile) {
  // If we don't have the binary yet, load it asynchronously using readAsync.
  if (!wasmBinary) {
    // Fetch the binary using readAsync
    try {
      var response = await readAsync(binaryFile);
      return new Uint8Array(response);
    } catch {}
  }
  // Otherwise, getBinarySync should be able to get it synchronously
  return getBinarySync(binaryFile);
}

async function instantiateArrayBuffer(binaryFile, imports) {
  try {
    var binary = await getWasmBinary(binaryFile);
    var instance = await WebAssembly.instantiate(binary, imports);
    return instance;
  } catch (reason) {
    err(`failed to asynchronously prepare wasm: ${reason}`);
    abort(reason);
  }
}

async function instantiateAsync(binary, binaryFile, imports) {
  if (!binary) {
    try {
      var response = fetch(binaryFile, {
        credentials: "same-origin"
      });
      var instantiationResult = await WebAssembly.instantiateStreaming(response, imports);
      return instantiationResult;
    } catch (reason) {
      // We expect the most common failure cause to be a bad MIME type for the binary,
      // in which case falling back to ArrayBuffer instantiation should work.
      err(`wasm streaming compile failed: ${reason}`);
      err("falling back to ArrayBuffer instantiation");
    }
  }
  return instantiateArrayBuffer(binaryFile, imports);
}

function getWasmImports() {
  assignWasmImports();
  // prepare imports
  var imports = {
    "env": wasmImports,
    "wasi_snapshot_preview1": wasmImports
  };
  return imports;
}

// Create the wasm instance.
// Receives the wasm imports, returns the exports.
async function createWasm() {
  // Load the wasm module and create an instance of using native support in the JS engine.
  // handle a generated wasm instance, receiving its exports and
  // performing other necessary setup
  /** @param {WebAssembly.Module=} module*/ function receiveInstance(instance, module) {
    wasmExports = instance.exports;
    wasmExports = applySignatureConversions(wasmExports);
    registerTLSInit(wasmExports["_emscripten_tls_init"]);
    assignWasmExports(wasmExports);
    // We now have the Wasm module loaded up, keep a reference to the compiled module so we can post it to the workers.
    wasmModule = module;
    removeRunDependency("wasm-instantiate");
    return wasmExports;
  }
  addRunDependency("wasm-instantiate");
  // Prefer streaming instantiation if available.
  function receiveInstantiationResult(result) {
    // 'result' is a ResultObject object which has both the module and instance.
    // receiveInstance() will swap in the exports (to Module.asm) so they can be called
    return receiveInstance(result["instance"], result["module"]);
  }
  var info = getWasmImports();
  // User shell pages can write their own Module.instantiateWasm = function(imports, successCallback) callback
  // to manually instantiate the Wasm module themselves. This allows pages to
  // run the instantiation parallel to any other async startup actions they are
  // performing.
  // Also pthreads and wasm workers initialize the wasm instance through this
  // path.
  if (Module["instantiateWasm"]) {
    return new Promise((resolve, reject) => {
      Module["instantiateWasm"](info, (inst, mod) => {
        resolve(receiveInstance(inst, mod));
      });
    });
  }
  if ((ENVIRONMENT_IS_PTHREAD)) {
    // Instantiate from the module that was received via postMessage from
    // the main thread. We can just use sync instantiation in the worker.
    var instance = new WebAssembly.Instance(wasmModule, getWasmImports());
    return receiveInstance(instance, wasmModule);
  }
  wasmBinaryFile ??= findWasmBinary();
  var result = await instantiateAsync(wasmBinary, wasmBinaryFile, info);
  var exports = receiveInstantiationResult(result);
  return exports;
}

// end include: preamble.js
// Begin JS library code
class ExitStatus {
  name="ExitStatus";
  constructor(status) {
    this.message = `Program terminated with exit(${status})`;
    this.status = status;
  }
}

var terminateWorker = worker => {
  worker.terminate();
  // terminate() can be asynchronous, so in theory the worker can continue
  // to run for some amount of time after termination.  However from our POV
  // the worker is now dead and we don't want to hear from it again, so we stub
  // out its message handler here.  This avoids having to check in each of
  // the onmessage handlers if the message was coming from a valid worker.
  worker.onmessage = e => {};
};

var cleanupThread = pthread_ptr => {
  var worker = PThread.pthreads[pthread_ptr];
  PThread.returnWorkerToPool(worker);
};

var callRuntimeCallbacks = callbacks => {
  while (callbacks.length > 0) {
    // Pass the module as the first argument.
    callbacks.shift()(Module);
  }
};

var onPreRuns = [];

var addOnPreRun = cb => onPreRuns.push(cb);

var runDependencies = 0;

var dependenciesFulfilled = null;

var removeRunDependency = id => {
  runDependencies--;
  Module["monitorRunDependencies"]?.(runDependencies);
  if (runDependencies == 0) {
    if (dependenciesFulfilled) {
      var callback = dependenciesFulfilled;
      dependenciesFulfilled = null;
      callback();
    }
  }
};

var addRunDependency = id => {
  runDependencies++;
  Module["monitorRunDependencies"]?.(runDependencies);
};

var spawnThread = threadParams => {
  var worker = PThread.getNewWorker();
  if (!worker) {
    // No available workers in the PThread pool.
    return 6;
  }
  PThread.runningWorkers.push(worker);
  // Add to pthreads map
  PThread.pthreads[threadParams.pthread_ptr] = worker;
  worker.pthread_ptr = threadParams.pthread_ptr;
  var msg = {
    cmd: "run",
    start_routine: threadParams.startRoutine,
    arg: threadParams.arg,
    pthread_ptr: threadParams.pthread_ptr
  };
  // Ask the worker to start executing its pthread entry point function.
  worker.postMessage(msg, threadParams.transferList);
  return 0;
};

var runtimeKeepaliveCounter = 0;

var keepRuntimeAlive = () => noExitRuntime || runtimeKeepaliveCounter > 0;

var stackSave = () => _emscripten_stack_get_current();

var stackRestore = val => __emscripten_stack_restore(val);

var stackAlloc = sz => __emscripten_stack_alloc(sz);

/** @type{function(number, (number|boolean), ...number)} */ var proxyToMainThread = (funcIndex, emAsmAddr, proxyMode, ...callArgs) => {
  // EM_ASM proxying is done by passing a pointer to the address of the EM_ASM
  // content as `emAsmAddr`.  JS library proxying is done by passing an index
  // into `proxiedJSCallArgs` as `funcIndex`. If `emAsmAddr` is non-zero then
  // `funcIndex` will be ignored.
  // Additional arguments are passed after the first three are the actual
  // function arguments.
  // The serialization buffer contains the number of call params, and then
  // all the args here.
  // We also pass 'proxyMode' to C separately, since C needs to look at it.
  // Allocate a buffer (on the stack), which will be copied if necessary by
  // the C code.
  // First passed parameter specifies the number of arguments to the function.
  // When BigInt support is enabled, we must handle types in a more complex
  // way, detecting at runtime if a value is a BigInt or not (as we have no
  // type info here). To do that, add a "prefix" before each value that
  // indicates if it is a BigInt, which effectively doubles the number of
  // values we serialize for proxying. TODO: pack this?
  var bufSize = 8 * callArgs.length * 2;
  var sp = stackSave();
  var args = stackAlloc(bufSize);
  var b = ((args) / 8);
  for (var arg of callArgs) {
    if (typeof arg == "bigint") {
      // The prefix is non-zero to indicate a bigint.
      (growMemViews(), HEAP64)[b++] = 1n;
      (growMemViews(), HEAP64)[b++] = arg;
    } else {
      // The prefix is zero to indicate a JS Number.
      (growMemViews(), HEAP64)[b++] = 0n;
      (growMemViews(), HEAPF64)[b++] = arg;
    }
  }
  var rtn = __emscripten_run_js_on_main_thread(funcIndex, emAsmAddr, bufSize, args, proxyMode);
  stackRestore(sp);
  return rtn;
};

function _proc_exit(code) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(0, 0, 1, code);
  EXITSTATUS = code;
  if (!keepRuntimeAlive()) {
    PThread.terminateAllThreads();
    Module["onExit"]?.(code);
    ABORT = true;
  }
  quit_(code, new ExitStatus(code));
}

var runtimeKeepalivePop = () => {
  runtimeKeepaliveCounter -= 1;
};

function exitOnMainThread(returnCode) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(1, 0, 0, returnCode);
  runtimeKeepalivePop();
  _exit(returnCode);
}

/** @param {boolean|number=} implicit */ var exitJS = (status, implicit) => {
  EXITSTATUS = status;
  if (ENVIRONMENT_IS_PTHREAD) {
    // implicit exit can never happen on a pthread
    // When running in a pthread we propagate the exit back to the main thread
    // where it can decide if the whole process should be shut down or not.
    // The pthread may have decided not to exit its own runtime, for example
    // because it runs a main loop, but that doesn't affect the main thread.
    exitOnMainThread(status);
    throw "unwind";
  }
  _proc_exit(status);
};

var _exit = exitJS;

var PThread = {
  unusedWorkers: [],
  runningWorkers: [],
  tlsInitFunctions: [],
  pthreads: {},
  init() {
    if ((!(ENVIRONMENT_IS_PTHREAD))) {
      PThread.initMainThread();
    }
  },
  initMainThread() {
    var pthreadPoolSize = 8;
    // Start loading up the Worker pool, if requested.
    while (pthreadPoolSize--) {
      PThread.allocateUnusedWorker();
    }
    // MINIMAL_RUNTIME takes care of calling loadWasmModuleToAllWorkers
    // in postamble_minimal.js
    addOnPreRun(async () => {
      var pthreadPoolReady = PThread.loadWasmModuleToAllWorkers();
      addRunDependency("loading-workers");
      await pthreadPoolReady;
      removeRunDependency("loading-workers");
    });
  },
  terminateAllThreads: () => {
    // Attempt to kill all workers.  Sadly (at least on the web) there is no
    // way to terminate a worker synchronously, or to be notified when a
    // worker is actually terminated.  This means there is some risk that
    // pthreads will continue to be executing after `worker.terminate` has
    // returned.  For this reason, we don't call `returnWorkerToPool` here or
    // free the underlying pthread data structures.
    for (var worker of PThread.runningWorkers) {
      terminateWorker(worker);
    }
    for (var worker of PThread.unusedWorkers) {
      terminateWorker(worker);
    }
    PThread.unusedWorkers = [];
    PThread.runningWorkers = [];
    PThread.pthreads = {};
  },
  returnWorkerToPool: worker => {
    // We don't want to run main thread queued calls here, since we are doing
    // some operations that leave the worker queue in an invalid state until
    // we are completely done (it would be bad if free() ends up calling a
    // queued pthread_create which looks at the global data structures we are
    // modifying). To achieve that, defer the free() until the very end, when
    // we are all done.
    var pthread_ptr = worker.pthread_ptr;
    delete PThread.pthreads[pthread_ptr];
    // Note: worker is intentionally not terminated so the pool can
    // dynamically grow.
    PThread.unusedWorkers.push(worker);
    PThread.runningWorkers.splice(PThread.runningWorkers.indexOf(worker), 1);
    // Not a running Worker anymore
    // Detach the worker from the pthread object, and return it to the
    // worker pool as an unused worker.
    worker.pthread_ptr = 0;
    // Finally, free the underlying (and now-unused) pthread structure in
    // linear memory.
    __emscripten_thread_free_data(pthread_ptr);
  },
  threadInitTLS() {
    // Call thread init functions (these are the _emscripten_tls_init for each
    // module loaded.
    PThread.tlsInitFunctions.forEach(f => f());
  },
  loadWasmModuleToWorker: worker => new Promise(onFinishedLoading => {
    worker.onmessage = e => {
      var d = e["data"];
      var cmd = d.cmd;
      // If this message is intended to a recipient that is not the main
      // thread, forward it to the target thread.
      if (d.targetThread && d.targetThread != _pthread_self()) {
        var targetWorker = PThread.pthreads[d.targetThread];
        if (targetWorker) {
          targetWorker.postMessage(d, d.transferList);
        } else {
          err(`Internal error! Worker sent a message "${cmd}" to target pthread ${d.targetThread}, but that thread no longer exists!`);
        }
        return;
      }
      if (cmd === "checkMailbox") {
        checkMailbox();
      } else if (cmd === "spawnThread") {
        spawnThread(d);
      } else if (cmd === "cleanupThread") {
        // cleanupThread needs to be run via callUserCallback since it calls
        // back into user code to free thread data. Without this it's possible
        // the unwind or ExitStatus exception could escape here.
        callUserCallback(() => cleanupThread(d.thread));
      } else if (cmd === "loaded") {
        worker.loaded = true;
        onFinishedLoading(worker);
      } else if (d.target === "setimmediate") {
        // Worker wants to postMessage() to itself to implement setImmediate()
        // emulation.
        worker.postMessage(d);
      } else if (cmd === "callHandler") {
        Module[d.handler](...d.args);
      } else if (cmd) {
        // The received message looks like something that should be handled by this message
        // handler, (since there is a e.data.cmd field present), but is not one of the
        // recognized commands:
        err(`worker sent an unknown command ${cmd}`);
      }
    };
    worker.onerror = e => {
      var message = "worker sent an error!";
      err(`${message} ${e.filename}:${e.lineno}: ${e.message}`);
      throw e;
    };
    // When running on a pthread, none of the incoming parameters on the module
    // object are present. Proxy known handlers back to the main thread if specified.
    var handlers = [];
    var knownHandlers = [ "onExit", "onAbort", "print", "printErr" ];
    for (var handler of knownHandlers) {
      if (Module.propertyIsEnumerable(handler)) {
        handlers.push(handler);
      }
    }
    // Ask the new worker to load up the Emscripten-compiled page. This is a heavy operation.
    worker.postMessage({
      cmd: "load",
      handlers,
      wasmMemory,
      wasmModule
    });
  }),
  async loadWasmModuleToAllWorkers() {
    // Instantiation is synchronous in pthreads.
    if (ENVIRONMENT_IS_PTHREAD) {
      return;
    }
    let pthreadPoolReady = Promise.all(PThread.unusedWorkers.map(PThread.loadWasmModuleToWorker));
    return pthreadPoolReady;
  },
  allocateUnusedWorker() {
    var worker;
    var pthreadMainJs = _scriptName;
    // We can't use makeModuleReceiveWithVar here since we want to also
    // call URL.createObjectURL on the mainScriptUrlOrBlob.
    if (Module["mainScriptUrlOrBlob"]) {
      pthreadMainJs = Module["mainScriptUrlOrBlob"];
      if (typeof pthreadMainJs != "string") {
        pthreadMainJs = URL.createObjectURL(pthreadMainJs);
      }
    }
    worker = new Worker(pthreadMainJs, {
      // This is the way that we signal to the Web Worker that it is hosting
      // a pthread.
      "name": "em-pthread"
    });
    PThread.unusedWorkers.push(worker);
  },
  getNewWorker() {
    if (PThread.unusedWorkers.length == 0) {
      // PTHREAD_POOL_SIZE_STRICT should show a warning and, if set to level `2`, return from the function.
      PThread.allocateUnusedWorker();
      PThread.loadWasmModuleToWorker(PThread.unusedWorkers[0]);
    }
    return PThread.unusedWorkers.pop();
  }
};

var onPostRuns = [];

var addOnPostRun = cb => onPostRuns.push(cb);

function establishStackSpace(pthread_ptr) {
  var stackHigh = Number((growMemViews(), HEAPU64)[(((pthread_ptr) + (88)) / 8)]);
  var stackSize = Number((growMemViews(), HEAPU64)[(((pthread_ptr) + (96)) / 8)]);
  var stackLow = stackHigh - stackSize;
  // Set stack limits used by `emscripten/stack.h` function.  These limits are
  // cached in wasm-side globals to make checks as fast as possible.
  _emscripten_stack_set_limits(stackHigh, stackLow);
  // Call inside wasm module to set up the stack frame for this pthread in wasm module scope
  stackRestore(stackHigh);
}

/**
   * @param {number} ptr
   * @param {string} type
   */ function getValue(ptr, type = "i8") {
  if (type.endsWith("*")) type = "*";
  switch (type) {
   case "i1":
    return (growMemViews(), HEAP8)[ptr];

   case "i8":
    return (growMemViews(), HEAP8)[ptr];

   case "i16":
    return (growMemViews(), HEAP16)[((ptr) / 2)];

   case "i32":
    return (growMemViews(), HEAP32)[((ptr) / 4)];

   case "i64":
    return (growMemViews(), HEAP64)[((ptr) / 8)];

   case "float":
    return (growMemViews(), HEAPF32)[((ptr) / 4)];

   case "double":
    return (growMemViews(), HEAPF64)[((ptr) / 8)];

   case "*":
    return Number((growMemViews(), HEAPU64)[((ptr) / 8)]);

   default:
    abort(`invalid type for getValue: ${type}`);
  }
}

var wasmTableMirror = [];

var getWasmTableEntry = funcPtr => {
  // Function pointers should show up as numbers, even under wasm64, but
  // we still have some places where bigint values can flow here.
  // https://github.com/emscripten-core/emscripten/issues/18200
  funcPtr = Number(funcPtr);
  var func = wasmTableMirror[funcPtr];
  if (!func) {
    /** @suppress {checkTypes} */ wasmTableMirror[funcPtr] = func = wasmTable.get(BigInt(funcPtr));
  }
  return func;
};

var invokeEntryPoint = (ptr, arg) => {
  // An old thread on this worker may have been canceled without returning the
  // `runtimeKeepaliveCounter` to zero. Reset it now so the new thread won't
  // be affected.
  runtimeKeepaliveCounter = 0;
  // Same for noExitRuntime.  The default for pthreads should always be false
  // otherwise pthreads would never complete and attempts to pthread_join to
  // them would block forever.
  // pthreads can still choose to set `noExitRuntime` explicitly, or
  // call emscripten_unwind_to_js_event_loop to extend their lifetime beyond
  // their main function.  See comment in src/runtime_pthread.js for more.
  noExitRuntime = 0;
  // pthread entry points are always of signature 'void *ThreadMain(void *arg)'
  // Native codebases sometimes spawn threads with other thread entry point
  // signatures, such as void ThreadMain(void *arg), void *ThreadMain(), or
  // void ThreadMain().  That is not acceptable per C/C++ specification, but
  // x86 compiler ABI extensions enable that to work. If you find the
  // following line to crash, either change the signature to "proper" void
  // *ThreadMain(void *arg) form, or try linking with the Emscripten linker
  // flag -sEMULATE_FUNCTION_POINTER_CASTS to add in emulation for this x86
  // ABI extension.
  var result = (a1 => getWasmTableEntry(ptr).call(null, BigInt(a1)))(arg);
  function finish(result) {
    // In MINIMAL_RUNTIME the noExitRuntime concept does not apply to
    // pthreads. To exit a pthread with live runtime, use the function
    // emscripten_unwind_to_js_event_loop() in the pthread body.
    if (keepRuntimeAlive()) {
      EXITSTATUS = result;
      return;
    }
    __emscripten_thread_exit(result);
  }
  finish(result);
};

var noExitRuntime = true;

var registerTLSInit = tlsInitFunc => PThread.tlsInitFunctions.push(tlsInitFunc);

var runtimeKeepalivePush = () => {
  runtimeKeepaliveCounter += 1;
};

/**
   * @param {number} ptr
   * @param {number} value
   * @param {string} type
   */ function setValue(ptr, value, type = "i8") {
  if (type.endsWith("*")) type = "*";
  switch (type) {
   case "i1":
    (growMemViews(), HEAP8)[ptr] = value;
    break;

   case "i8":
    (growMemViews(), HEAP8)[ptr] = value;
    break;

   case "i16":
    (growMemViews(), HEAP16)[((ptr) / 2)] = value;
    break;

   case "i32":
    (growMemViews(), HEAP32)[((ptr) / 4)] = value;
    break;

   case "i64":
    (growMemViews(), HEAP64)[((ptr) / 8)] = BigInt(value);
    break;

   case "float":
    (growMemViews(), HEAPF32)[((ptr) / 4)] = value;
    break;

   case "double":
    (growMemViews(), HEAPF64)[((ptr) / 8)] = value;
    break;

   case "*":
    (growMemViews(), HEAPU64)[((ptr) / 8)] = BigInt(value);
    break;

   default:
    abort(`invalid type for setValue: ${type}`);
  }
}

var wasmMemory;

function _ATAPIPassthroughParseCdb(...args) {
  abort("missing function: ATAPIPassthroughParseCdb");
}

_ATAPIPassthroughParseCdb.stub = true;

function _ATAPIPassthroughTrackListClear(...args) {
  abort("missing function: ATAPIPassthroughTrackListClear");
}

_ATAPIPassthroughTrackListClear.stub = true;

function _ATAPIPassthroughTrackListCreateEmpty(...args) {
  abort("missing function: ATAPIPassthroughTrackListCreateEmpty");
}

_ATAPIPassthroughTrackListCreateEmpty.stub = true;

function _ATAPIPassthroughTrackListDestroy(...args) {
  abort("missing function: ATAPIPassthroughTrackListDestroy");
}

_ATAPIPassthroughTrackListDestroy.stub = true;

function _ATAPIPassthroughTrackListUpdate(...args) {
  abort("missing function: ATAPIPassthroughTrackListUpdate");
}

_ATAPIPassthroughTrackListUpdate.stub = true;

function _DBGFR3FlowTraceModAddProbe(...args) {
  abort("missing function: DBGFR3FlowTraceModAddProbe");
}

_DBGFR3FlowTraceModAddProbe.stub = true;

function _DBGFR3FlowTraceModClear(...args) {
  abort("missing function: DBGFR3FlowTraceModClear");
}

_DBGFR3FlowTraceModClear.stub = true;

function _DBGFR3FlowTraceModCreate(...args) {
  abort("missing function: DBGFR3FlowTraceModCreate");
}

_DBGFR3FlowTraceModCreate.stub = true;

function _DBGFR3FlowTraceModCreateFromFlowGraph(...args) {
  abort("missing function: DBGFR3FlowTraceModCreateFromFlowGraph");
}

_DBGFR3FlowTraceModCreateFromFlowGraph.stub = true;

function _DBGFR3FlowTraceModDisable(...args) {
  abort("missing function: DBGFR3FlowTraceModDisable");
}

_DBGFR3FlowTraceModDisable.stub = true;

function _DBGFR3FlowTraceModEnable(...args) {
  abort("missing function: DBGFR3FlowTraceModEnable");
}

_DBGFR3FlowTraceModEnable.stub = true;

function _DBGFR3FlowTraceModQueryReport(...args) {
  abort("missing function: DBGFR3FlowTraceModQueryReport");
}

_DBGFR3FlowTraceModQueryReport.stub = true;

function _DBGFR3FlowTraceModRelease(...args) {
  abort("missing function: DBGFR3FlowTraceModRelease");
}

_DBGFR3FlowTraceModRelease.stub = true;

function _DBGFR3FlowTraceModRetain(...args) {
  abort("missing function: DBGFR3FlowTraceModRetain");
}

_DBGFR3FlowTraceModRetain.stub = true;

function _DBGFR3FlowTraceProbeCreate(...args) {
  abort("missing function: DBGFR3FlowTraceProbeCreate");
}

_DBGFR3FlowTraceProbeCreate.stub = true;

function _DBGFR3FlowTraceProbeEntriesAdd(...args) {
  abort("missing function: DBGFR3FlowTraceProbeEntriesAdd");
}

_DBGFR3FlowTraceProbeEntriesAdd.stub = true;

function _DBGFR3FlowTraceProbeRelease(...args) {
  abort("missing function: DBGFR3FlowTraceProbeRelease");
}

_DBGFR3FlowTraceProbeRelease.stub = true;

function _DBGFR3FlowTraceProbeRetain(...args) {
  abort("missing function: DBGFR3FlowTraceProbeRetain");
}

_DBGFR3FlowTraceProbeRetain.stub = true;

function _DBGFR3FlowTraceRecordGetAddr(...args) {
  abort("missing function: DBGFR3FlowTraceRecordGetAddr");
}

_DBGFR3FlowTraceRecordGetAddr.stub = true;

function _DBGFR3FlowTraceRecordGetCpuId(...args) {
  abort("missing function: DBGFR3FlowTraceRecordGetCpuId");
}

_DBGFR3FlowTraceRecordGetCpuId.stub = true;

function _DBGFR3FlowTraceRecordGetProbe(...args) {
  abort("missing function: DBGFR3FlowTraceRecordGetProbe");
}

_DBGFR3FlowTraceRecordGetProbe.stub = true;

function _DBGFR3FlowTraceRecordGetSeqNo(...args) {
  abort("missing function: DBGFR3FlowTraceRecordGetSeqNo");
}

_DBGFR3FlowTraceRecordGetSeqNo.stub = true;

function _DBGFR3FlowTraceRecordGetTimestamp(...args) {
  abort("missing function: DBGFR3FlowTraceRecordGetTimestamp");
}

_DBGFR3FlowTraceRecordGetTimestamp.stub = true;

function _DBGFR3FlowTraceRecordGetValCount(...args) {
  abort("missing function: DBGFR3FlowTraceRecordGetValCount");
}

_DBGFR3FlowTraceRecordGetValCount.stub = true;

function _DBGFR3FlowTraceRecordGetVals(...args) {
  abort("missing function: DBGFR3FlowTraceRecordGetVals");
}

_DBGFR3FlowTraceRecordGetVals.stub = true;

function _DBGFR3FlowTraceRecordGetValsCommon(...args) {
  abort("missing function: DBGFR3FlowTraceRecordGetValsCommon");
}

_DBGFR3FlowTraceRecordGetValsCommon.stub = true;

function _DBGFR3FlowTraceRecordRelease(...args) {
  abort("missing function: DBGFR3FlowTraceRecordRelease");
}

_DBGFR3FlowTraceRecordRelease.stub = true;

function _DBGFR3FlowTraceRecordRetain(...args) {
  abort("missing function: DBGFR3FlowTraceRecordRetain");
}

_DBGFR3FlowTraceRecordRetain.stub = true;

function _DBGFR3FlowTraceReportEnumRecords(...args) {
  abort("missing function: DBGFR3FlowTraceReportEnumRecords");
}

_DBGFR3FlowTraceReportEnumRecords.stub = true;

function _DBGFR3FlowTraceReportGetRecordCount(...args) {
  abort("missing function: DBGFR3FlowTraceReportGetRecordCount");
}

_DBGFR3FlowTraceReportGetRecordCount.stub = true;

function _DBGFR3FlowTraceReportQueryFiltered(...args) {
  abort("missing function: DBGFR3FlowTraceReportQueryFiltered");
}

_DBGFR3FlowTraceReportQueryFiltered.stub = true;

function _DBGFR3FlowTraceReportQueryRecord(...args) {
  abort("missing function: DBGFR3FlowTraceReportQueryRecord");
}

_DBGFR3FlowTraceReportQueryRecord.stub = true;

function _DBGFR3FlowTraceReportRelease(...args) {
  abort("missing function: DBGFR3FlowTraceReportRelease");
}

_DBGFR3FlowTraceReportRelease.stub = true;

function _DBGFR3FlowTraceReportRetain(...args) {
  abort("missing function: DBGFR3FlowTraceReportRetain");
}

_DBGFR3FlowTraceReportRetain.stub = true;

function _HBDMgrDestroy(...args) {
  abort("missing function: HBDMgrDestroy");
}

_HBDMgrDestroy.stub = true;

function _PDMR3AsyncCompletionBwMgrSetMaxForFile(...args) {
  abort("missing function: PDMR3AsyncCompletionBwMgrSetMaxForFile");
}

_PDMR3AsyncCompletionBwMgrSetMaxForFile.stub = true;

function _PDMR3AsyncCompletionEpClose(...args) {
  abort("missing function: PDMR3AsyncCompletionEpClose");
}

_PDMR3AsyncCompletionEpClose.stub = true;

function _PDMR3AsyncCompletionEpCreateForFile(...args) {
  abort("missing function: PDMR3AsyncCompletionEpCreateForFile");
}

_PDMR3AsyncCompletionEpCreateForFile.stub = true;

function _PDMR3AsyncCompletionEpFlush(...args) {
  abort("missing function: PDMR3AsyncCompletionEpFlush");
}

_PDMR3AsyncCompletionEpFlush.stub = true;

function _PDMR3AsyncCompletionEpGetSize(...args) {
  abort("missing function: PDMR3AsyncCompletionEpGetSize");
}

_PDMR3AsyncCompletionEpGetSize.stub = true;

function _PDMR3AsyncCompletionEpRead(...args) {
  abort("missing function: PDMR3AsyncCompletionEpRead");
}

_PDMR3AsyncCompletionEpRead.stub = true;

function _PDMR3AsyncCompletionEpSetBwMgr(...args) {
  abort("missing function: PDMR3AsyncCompletionEpSetBwMgr");
}

_PDMR3AsyncCompletionEpSetBwMgr.stub = true;

function _PDMR3AsyncCompletionEpSetSize(...args) {
  abort("missing function: PDMR3AsyncCompletionEpSetSize");
}

_PDMR3AsyncCompletionEpSetSize.stub = true;

function _PDMR3AsyncCompletionEpWrite(...args) {
  abort("missing function: PDMR3AsyncCompletionEpWrite");
}

_PDMR3AsyncCompletionEpWrite.stub = true;

function _PDMR3AsyncCompletionTemplateDestroy(...args) {
  abort("missing function: PDMR3AsyncCompletionTemplateDestroy");
}

_PDMR3AsyncCompletionTemplateDestroy.stub = true;

function _PDMR3NsBwGroupSetLimit(...args) {
  abort("missing function: PDMR3NsBwGroupSetLimit");
}

_PDMR3NsBwGroupSetLimit.stub = true;

function _PDMR3UsbCreateEmulatedDevice(...args) {
  abort("missing function: PDMR3UsbCreateEmulatedDevice");
}

_PDMR3UsbCreateEmulatedDevice.stub = true;

function _PDMR3UsbCreateProxyDevice(...args) {
  abort("missing function: PDMR3UsbCreateProxyDevice");
}

_PDMR3UsbCreateProxyDevice.stub = true;

function _PDMR3UsbDetachDevice(...args) {
  abort("missing function: PDMR3UsbDetachDevice");
}

_PDMR3UsbDetachDevice.stub = true;

function _PDMR3UsbDriverAttach(...args) {
  abort("missing function: PDMR3UsbDriverAttach");
}

_PDMR3UsbDriverAttach.stub = true;

function _PDMR3UsbDriverDetach(...args) {
  abort("missing function: PDMR3UsbDriverDetach");
}

_PDMR3UsbDriverDetach.stub = true;

function _PDMR3UsbHasHub(...args) {
  abort("missing function: PDMR3UsbHasHub");
}

_PDMR3UsbHasHub.stub = true;

function _PDMR3UsbQueryDeviceLun(...args) {
  abort("missing function: PDMR3UsbQueryDeviceLun");
}

_PDMR3UsbQueryDeviceLun.stub = true;

function _PDMR3UsbQueryDriverOnLun(...args) {
  abort("missing function: PDMR3UsbQueryDriverOnLun");
}

_PDMR3UsbQueryDriverOnLun.stub = true;

function _PDMR3UsbQueryLun(...args) {
  abort("missing function: PDMR3UsbQueryLun");
}

_PDMR3UsbQueryLun.stub = true;

function _RTFileCopyPartCleanup(...args) {
  abort("missing function: RTFileCopyPartCleanup");
}

_RTFileCopyPartCleanup.stub = true;

function _RTFileCopyPartEx(...args) {
  abort("missing function: RTFileCopyPartEx");
}

_RTFileCopyPartEx.stub = true;

function _RTFileCopyPartPrep(...args) {
  abort("missing function: RTFileCopyPartPrep");
}

_RTFileCopyPartPrep.stub = true;

function _RTFileMove(...args) {
  abort("missing function: RTFileMove");
}

_RTFileMove.stub = true;

function _RTFileQueryFsSizes(...args) {
  abort("missing function: RTFileQueryFsSizes");
}

_RTFileQueryFsSizes.stub = true;

function _RTFileQuerySectorSize(...args) {
  abort("missing function: RTFileQuerySectorSize");
}

_RTFileQuerySectorSize.stub = true;

function _RTFileSetAllocationSize(...args) {
  abort("missing function: RTFileSetAllocationSize");
}

_RTFileSetAllocationSize.stub = true;

var INT53_MAX = 9007199254740992;

var INT53_MIN = -9007199254740992;

var bigintToI53Checked = num => (num < INT53_MIN || num > INT53_MAX) ? NaN : Number(num);

function ___call_sighandler(fp, sig) {
  fp = bigintToI53Checked(fp);
  return getWasmTableEntry(fp)(sig);
}

class ExceptionInfo {
  // excPtr - Thrown object pointer to wrap. Metadata pointer is calculated from it.
  constructor(excPtr) {
    this.excPtr = excPtr;
    this.ptr = excPtr - 48;
  }
  set_type(type) {
    (growMemViews(), HEAPU64)[(((this.ptr) + (8)) / 8)] = BigInt(type);
  }
  get_type() {
    return Number((growMemViews(), HEAPU64)[(((this.ptr) + (8)) / 8)]);
  }
  set_destructor(destructor) {
    (growMemViews(), HEAPU64)[(((this.ptr) + (16)) / 8)] = BigInt(destructor);
  }
  get_destructor() {
    return Number((growMemViews(), HEAPU64)[(((this.ptr) + (16)) / 8)]);
  }
  set_caught(caught) {
    caught = caught ? 1 : 0;
    (growMemViews(), HEAP8)[(this.ptr) + (24)] = caught;
  }
  get_caught() {
    return (growMemViews(), HEAP8)[(this.ptr) + (24)] != 0;
  }
  set_rethrown(rethrown) {
    rethrown = rethrown ? 1 : 0;
    (growMemViews(), HEAP8)[(this.ptr) + (25)] = rethrown;
  }
  get_rethrown() {
    return (growMemViews(), HEAP8)[(this.ptr) + (25)] != 0;
  }
  // Initialize native structure fields. Should be called once after allocated.
  init(type, destructor) {
    this.set_adjusted_ptr(0);
    this.set_type(type);
    this.set_destructor(destructor);
  }
  set_adjusted_ptr(adjustedPtr) {
    (growMemViews(), HEAPU64)[(((this.ptr) + (32)) / 8)] = BigInt(adjustedPtr);
  }
  get_adjusted_ptr() {
    return Number((growMemViews(), HEAPU64)[(((this.ptr) + (32)) / 8)]);
  }
}

var exceptionLast = 0;

var uncaughtExceptionCount = 0;

function ___cxa_throw(ptr, type, destructor) {
  ptr = bigintToI53Checked(ptr);
  type = bigintToI53Checked(type);
  destructor = bigintToI53Checked(destructor);
  var info = new ExceptionInfo(ptr);
  // Initialize ExceptionInfo content after it was allocated in __cxa_allocate_exception.
  info.init(type, destructor);
  exceptionLast = ptr;
  uncaughtExceptionCount++;
  throw exceptionLast;
}

function pthreadCreateProxied(pthread_ptr, attr, startRoutine, arg) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(2, 0, 1, pthread_ptr, attr, startRoutine, arg);
  return ___pthread_create_js(pthread_ptr, attr, startRoutine, arg);
}

var _emscripten_has_threading_support = () => !!globalThis.SharedArrayBuffer;

function ___pthread_create_js(pthread_ptr, attr, startRoutine, arg) {
  pthread_ptr = bigintToI53Checked(pthread_ptr);
  attr = bigintToI53Checked(attr);
  startRoutine = bigintToI53Checked(startRoutine);
  arg = bigintToI53Checked(arg);
  if (!_emscripten_has_threading_support()) {
    return 6;
  }
  // List of JS objects that will transfer ownership to the Worker hosting the thread
  var transferList = [];
  var error = 0;
  // Synchronously proxy the thread creation to main thread if possible. If we
  // need to transfer ownership of objects, then proxy asynchronously via
  // postMessage.
  if (ENVIRONMENT_IS_PTHREAD && (transferList.length === 0 || error)) {
    return pthreadCreateProxied(pthread_ptr, attr, startRoutine, arg);
  }
  // If on the main thread, and accessing Canvas/OffscreenCanvas failed, abort
  // with the detected error.
  if (error) return error;
  var threadParams = {
    startRoutine,
    pthread_ptr,
    arg,
    transferList
  };
  if (ENVIRONMENT_IS_PTHREAD) {
    // The prepopulated pool of web workers that can host pthreads is stored
    // in the main JS thread. Therefore if a pthread is attempting to spawn a
    // new thread, the thread creation must be deferred to the main JS thread.
    threadParams.cmd = "spawnThread";
    postMessage(threadParams, transferList);
    // When we defer thread creation this way, we have no way to detect thread
    // creation synchronously today, so we have to assume success and return 0.
    return 0;
  }
  // We are the main thread, so we have the pthread warmup pool in this
  // thread and can fire off JS thread creation directly ourselves.
  return spawnThread(threadParams);
}

var PATH = {
  isAbs: path => path.charAt(0) === "/",
  splitPath: filename => {
    var splitPathRe = /^(\/?|)([\s\S]*?)((?:\.{1,2}|[^\/]+?|)(\.[^.\/]*|))(?:[\/]*)$/;
    return splitPathRe.exec(filename).slice(1);
  },
  normalizeArray: (parts, allowAboveRoot) => {
    // if the path tries to go above the root, `up` ends up > 0
    var up = 0;
    for (var i = parts.length - 1; i >= 0; i--) {
      var last = parts[i];
      if (last === ".") {
        parts.splice(i, 1);
      } else if (last === "..") {
        parts.splice(i, 1);
        up++;
      } else if (up) {
        parts.splice(i, 1);
        up--;
      }
    }
    // if the path is allowed to go above the root, restore leading ..s
    if (allowAboveRoot) {
      for (;up; up--) {
        parts.unshift("..");
      }
    }
    return parts;
  },
  normalize: path => {
    var isAbsolute = PATH.isAbs(path), trailingSlash = path.slice(-1) === "/";
    // Normalize the path
    path = PATH.normalizeArray(path.split("/").filter(p => !!p), !isAbsolute).join("/");
    if (!path && !isAbsolute) {
      path = ".";
    }
    if (path && trailingSlash) {
      path += "/";
    }
    return (isAbsolute ? "/" : "") + path;
  },
  dirname: path => {
    var result = PATH.splitPath(path), root = result[0], dir = result[1];
    if (!root && !dir) {
      // No dirname whatsoever
      return ".";
    }
    if (dir) {
      // It has a dirname, strip trailing slash
      dir = dir.slice(0, -1);
    }
    return root + dir;
  },
  basename: path => path && path.match(/([^\/]+|\/)\/*$/)[1],
  join: (...paths) => PATH.normalize(paths.join("/")),
  join2: (l, r) => PATH.normalize(l + "/" + r)
};

var initRandomFill = () => view => view.set(crypto.getRandomValues(new Uint8Array(view.byteLength)));

var randomFill = view => {
  // Lazily init on the first invocation.
  (randomFill = initRandomFill())(view);
};

var PATH_FS = {
  resolve: (...args) => {
    var resolvedPath = "", resolvedAbsolute = false;
    for (var i = args.length - 1; i >= -1 && !resolvedAbsolute; i--) {
      var path = (i >= 0) ? args[i] : FS.cwd();
      // Skip empty and invalid entries
      if (typeof path != "string") {
        throw new TypeError("Arguments to path.resolve must be strings");
      } else if (!path) {
        return "";
      }
      resolvedPath = path + "/" + resolvedPath;
      resolvedAbsolute = PATH.isAbs(path);
    }
    // At this point the path should be resolved to a full absolute path, but
    // handle relative paths to be safe (might happen when process.cwd() fails)
    resolvedPath = PATH.normalizeArray(resolvedPath.split("/").filter(p => !!p), !resolvedAbsolute).join("/");
    return ((resolvedAbsolute ? "/" : "") + resolvedPath) || ".";
  },
  relative: (from, to) => {
    from = PATH_FS.resolve(from).slice(1);
    to = PATH_FS.resolve(to).slice(1);
    function trim(arr) {
      var start = 0;
      for (;start < arr.length; start++) {
        if (arr[start] !== "") break;
      }
      var end = arr.length - 1;
      for (;end >= 0; end--) {
        if (arr[end] !== "") break;
      }
      if (start > end) return [];
      return arr.slice(start, end - start + 1);
    }
    var fromParts = trim(from.split("/"));
    var toParts = trim(to.split("/"));
    var length = Math.min(fromParts.length, toParts.length);
    var samePartsLength = length;
    for (var i = 0; i < length; i++) {
      if (fromParts[i] !== toParts[i]) {
        samePartsLength = i;
        break;
      }
    }
    var outputParts = [];
    for (var i = samePartsLength; i < fromParts.length; i++) {
      outputParts.push("..");
    }
    outputParts = outputParts.concat(toParts.slice(samePartsLength));
    return outputParts.join("/");
  }
};

var UTF8Decoder = globalThis.TextDecoder && new TextDecoder;

var findStringEnd = (heapOrArray, idx, maxBytesToRead, ignoreNul) => {
  var maxIdx = idx + maxBytesToRead;
  if (ignoreNul) return maxIdx;
  // TextDecoder needs to know the byte length in advance, it doesn't stop on
  // null terminator by itself.
  // As a tiny code save trick, compare idx against maxIdx using a negation,
  // so that maxBytesToRead=undefined/NaN means Infinity.
  while (heapOrArray[idx] && !(idx >= maxIdx)) ++idx;
  return idx;
};

/**
   * Given a pointer 'idx' to a null-terminated UTF8-encoded string in the given
   * array that contains uint8 values, returns a copy of that string as a
   * Javascript String object.
   * heapOrArray is either a regular array, or a JavaScript typed array view.
   * @param {number=} idx
   * @param {number=} maxBytesToRead
   * @param {boolean=} ignoreNul - If true, the function will not stop on a NUL character.
   * @return {string}
   */ var UTF8ArrayToString = (heapOrArray, idx = 0, maxBytesToRead, ignoreNul) => {
  var endPtr = findStringEnd(heapOrArray, idx, maxBytesToRead, ignoreNul);
  // When using conditional TextDecoder, skip it for short strings as the overhead of the native call is not worth it.
  if (endPtr - idx > 16 && heapOrArray.buffer && UTF8Decoder) {
    return UTF8Decoder.decode(heapOrArray.buffer instanceof ArrayBuffer ? heapOrArray.subarray(idx, endPtr) : heapOrArray.slice(idx, endPtr));
  }
  var str = "";
  while (idx < endPtr) {
    // For UTF8 byte structure, see:
    // http://en.wikipedia.org/wiki/UTF-8#Description
    // https://www.ietf.org/rfc/rfc2279.txt
    // https://tools.ietf.org/html/rfc3629
    var u0 = heapOrArray[idx++];
    if (!(u0 & 128)) {
      str += String.fromCharCode(u0);
      continue;
    }
    var u1 = heapOrArray[idx++] & 63;
    if ((u0 & 224) == 192) {
      str += String.fromCharCode(((u0 & 31) << 6) | u1);
      continue;
    }
    var u2 = heapOrArray[idx++] & 63;
    if ((u0 & 240) == 224) {
      u0 = ((u0 & 15) << 12) | (u1 << 6) | u2;
    } else {
      u0 = ((u0 & 7) << 18) | (u1 << 12) | (u2 << 6) | (heapOrArray[idx++] & 63);
    }
    if (u0 < 65536) {
      str += String.fromCharCode(u0);
    } else {
      var ch = u0 - 65536;
      str += String.fromCharCode(55296 | (ch >> 10), 56320 | (ch & 1023));
    }
  }
  return str;
};

var FS_stdin_getChar_buffer = [];

var lengthBytesUTF8 = str => {
  var len = 0;
  for (var i = 0; i < str.length; ++i) {
    // Gotcha: charCodeAt returns a 16-bit word that is a UTF-16 encoded code
    // unit, not a Unicode code point of the character! So decode
    // UTF16->UTF32->UTF8.
    // See http://unicode.org/faq/utf_bom.html#utf16-3
    var c = str.charCodeAt(i);
    // possibly a lead surrogate
    if (c <= 127) {
      len++;
    } else if (c <= 2047) {
      len += 2;
    } else if (c >= 55296 && c <= 57343) {
      len += 4;
      ++i;
    } else {
      len += 3;
    }
  }
  return len;
};

var stringToUTF8Array = (str, heap, outIdx, maxBytesToWrite) => {
  // Parameter maxBytesToWrite is not optional. Negative values, 0, null,
  // undefined and false each don't write out any bytes.
  if (!(maxBytesToWrite > 0)) return 0;
  var startIdx = outIdx;
  var endIdx = outIdx + maxBytesToWrite - 1;
  // -1 for string null terminator.
  for (var i = 0; i < str.length; ++i) {
    // For UTF8 byte structure, see http://en.wikipedia.org/wiki/UTF-8#Description
    // and https://www.ietf.org/rfc/rfc2279.txt
    // and https://tools.ietf.org/html/rfc3629
    var u = str.codePointAt(i);
    if (u <= 127) {
      if (outIdx >= endIdx) break;
      heap[outIdx++] = u;
    } else if (u <= 2047) {
      if (outIdx + 1 >= endIdx) break;
      heap[outIdx++] = 192 | (u >> 6);
      heap[outIdx++] = 128 | (u & 63);
    } else if (u <= 65535) {
      if (outIdx + 2 >= endIdx) break;
      heap[outIdx++] = 224 | (u >> 12);
      heap[outIdx++] = 128 | ((u >> 6) & 63);
      heap[outIdx++] = 128 | (u & 63);
    } else {
      if (outIdx + 3 >= endIdx) break;
      heap[outIdx++] = 240 | (u >> 18);
      heap[outIdx++] = 128 | ((u >> 12) & 63);
      heap[outIdx++] = 128 | ((u >> 6) & 63);
      heap[outIdx++] = 128 | (u & 63);
      // Gotcha: if codePoint is over 0xFFFF, it is represented as a surrogate pair in UTF-16.
      // We need to manually skip over the second code unit for correct iteration.
      i++;
    }
  }
  // Null-terminate the pointer to the buffer.
  heap[outIdx] = 0;
  return outIdx - startIdx;
};

/** @type {function(string, boolean=, number=)} */ var intArrayFromString = (stringy, dontAddNull, length) => {
  var len = length > 0 ? length : lengthBytesUTF8(stringy) + 1;
  var u8array = new Array(len);
  var numBytesWritten = stringToUTF8Array(stringy, u8array, 0, u8array.length);
  if (dontAddNull) u8array.length = numBytesWritten;
  return u8array;
};

var FS_stdin_getChar = () => {
  if (!FS_stdin_getChar_buffer.length) {
    var result = null;
    if (globalThis.window?.prompt) {
      // Browser.
      result = window.prompt("Input: ");
      // returns null on cancel
      if (result !== null) {
        result += "\n";
      }
    } else {}
    if (!result) {
      return null;
    }
    FS_stdin_getChar_buffer = intArrayFromString(result, true);
  }
  return FS_stdin_getChar_buffer.shift();
};

var TTY = {
  ttys: [],
  init() {},
  shutdown() {},
  register(dev, ops) {
    TTY.ttys[dev] = {
      input: [],
      output: [],
      ops
    };
    FS.registerDevice(dev, TTY.stream_ops);
  },
  stream_ops: {
    open(stream) {
      var tty = TTY.ttys[stream.node.rdev];
      if (!tty) {
        throw new FS.ErrnoError(43);
      }
      stream.tty = tty;
      stream.seekable = false;
    },
    close(stream) {
      // flush any pending line data
      stream.tty.ops.fsync(stream.tty);
    },
    fsync(stream) {
      stream.tty.ops.fsync(stream.tty);
    },
    read(stream, buffer, offset, length, pos) {
      if (!stream.tty || !stream.tty.ops.get_char) {
        throw new FS.ErrnoError(60);
      }
      var bytesRead = 0;
      for (var i = 0; i < length; i++) {
        var result;
        try {
          result = stream.tty.ops.get_char(stream.tty);
        } catch (e) {
          throw new FS.ErrnoError(29);
        }
        if (result === undefined && bytesRead === 0) {
          throw new FS.ErrnoError(6);
        }
        if (result === null || result === undefined) break;
        bytesRead++;
        buffer[offset + i] = result;
      }
      if (bytesRead) {
        stream.node.atime = Date.now();
      }
      return bytesRead;
    },
    write(stream, buffer, offset, length, pos) {
      if (!stream.tty || !stream.tty.ops.put_char) {
        throw new FS.ErrnoError(60);
      }
      try {
        for (var i = 0; i < length; i++) {
          stream.tty.ops.put_char(stream.tty, buffer[offset + i]);
        }
      } catch (e) {
        throw new FS.ErrnoError(29);
      }
      if (length) {
        stream.node.mtime = stream.node.ctime = Date.now();
      }
      return i;
    }
  },
  default_tty_ops: {
    get_char(tty) {
      return FS_stdin_getChar();
    },
    put_char(tty, val) {
      if (val === null || val === 10) {
        out(UTF8ArrayToString(tty.output));
        tty.output = [];
      } else {
        if (val != 0) tty.output.push(val);
      }
    },
    fsync(tty) {
      if (tty.output?.length > 0) {
        out(UTF8ArrayToString(tty.output));
        tty.output = [];
      }
    },
    ioctl_tcgets(tty) {
      // typical setting
      return {
        c_iflag: 25856,
        c_oflag: 5,
        c_cflag: 191,
        c_lflag: 35387,
        c_cc: [ 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 ]
      };
    },
    ioctl_tcsets(tty, optional_actions, data) {
      // currently just ignore
      return 0;
    },
    ioctl_tiocgwinsz(tty) {
      return [ 24, 80 ];
    }
  },
  default_tty1_ops: {
    put_char(tty, val) {
      if (val === null || val === 10) {
        err(UTF8ArrayToString(tty.output));
        tty.output = [];
      } else {
        if (val != 0) tty.output.push(val);
      }
    },
    fsync(tty) {
      if (tty.output?.length > 0) {
        err(UTF8ArrayToString(tty.output));
        tty.output = [];
      }
    }
  }
};

var mmapAlloc = size => {
  abort();
};

var MEMFS = {
  ops_table: null,
  mount(mount) {
    return MEMFS.createNode(null, "/", 16895, 0);
  },
  createNode(parent, name, mode, dev) {
    if (FS.isBlkdev(mode) || FS.isFIFO(mode)) {
      // not supported
      throw new FS.ErrnoError(63);
    }
    MEMFS.ops_table ||= {
      dir: {
        node: {
          getattr: MEMFS.node_ops.getattr,
          setattr: MEMFS.node_ops.setattr,
          lookup: MEMFS.node_ops.lookup,
          mknod: MEMFS.node_ops.mknod,
          rename: MEMFS.node_ops.rename,
          unlink: MEMFS.node_ops.unlink,
          rmdir: MEMFS.node_ops.rmdir,
          readdir: MEMFS.node_ops.readdir,
          symlink: MEMFS.node_ops.symlink
        },
        stream: {
          llseek: MEMFS.stream_ops.llseek
        }
      },
      file: {
        node: {
          getattr: MEMFS.node_ops.getattr,
          setattr: MEMFS.node_ops.setattr
        },
        stream: {
          llseek: MEMFS.stream_ops.llseek,
          read: MEMFS.stream_ops.read,
          write: MEMFS.stream_ops.write,
          mmap: MEMFS.stream_ops.mmap,
          msync: MEMFS.stream_ops.msync
        }
      },
      link: {
        node: {
          getattr: MEMFS.node_ops.getattr,
          setattr: MEMFS.node_ops.setattr,
          readlink: MEMFS.node_ops.readlink
        },
        stream: {}
      },
      chrdev: {
        node: {
          getattr: MEMFS.node_ops.getattr,
          setattr: MEMFS.node_ops.setattr
        },
        stream: FS.chrdev_stream_ops
      }
    };
    var node = FS.createNode(parent, name, mode, dev);
    if (FS.isDir(node.mode)) {
      node.node_ops = MEMFS.ops_table.dir.node;
      node.stream_ops = MEMFS.ops_table.dir.stream;
      node.contents = {};
    } else if (FS.isFile(node.mode)) {
      node.node_ops = MEMFS.ops_table.file.node;
      node.stream_ops = MEMFS.ops_table.file.stream;
      node.usedBytes = 0;
      // The actual number of bytes used in the typed array, as opposed to contents.length which gives the whole capacity.
      // When the byte data of the file is populated, this will point to either a typed array, or a normal JS array. Typed arrays are preferred
      // for performance, and used by default. However, typed arrays are not resizable like normal JS arrays are, so there is a small disk size
      // penalty involved for appending file writes that continuously grow a file similar to std::vector capacity vs used -scheme.
      node.contents = null;
    } else if (FS.isLink(node.mode)) {
      node.node_ops = MEMFS.ops_table.link.node;
      node.stream_ops = MEMFS.ops_table.link.stream;
    } else if (FS.isChrdev(node.mode)) {
      node.node_ops = MEMFS.ops_table.chrdev.node;
      node.stream_ops = MEMFS.ops_table.chrdev.stream;
    }
    node.atime = node.mtime = node.ctime = Date.now();
    // add the new node to the parent
    if (parent) {
      parent.contents[name] = node;
      parent.atime = parent.mtime = parent.ctime = node.atime;
    }
    return node;
  },
  getFileDataAsTypedArray(node) {
    if (!node.contents) return new Uint8Array(0);
    if (node.contents.subarray) return node.contents.subarray(0, node.usedBytes);
    // Make sure to not return excess unused bytes.
    return new Uint8Array(node.contents);
  },
  expandFileStorage(node, newCapacity) {
    var prevCapacity = node.contents ? node.contents.length : 0;
    if (prevCapacity >= newCapacity) return;
    // No need to expand, the storage was already large enough.
    // Don't expand strictly to the given requested limit if it's only a very small increase, but instead geometrically grow capacity.
    // For small filesizes (<1MB), perform size*2 geometric increase, but for large sizes, do a much more conservative size*1.125 increase to
    // avoid overshooting the allocation cap by a very large margin.
    var CAPACITY_DOUBLING_MAX = 1024 * 1024;
    newCapacity = Math.max(newCapacity, (prevCapacity * (prevCapacity < CAPACITY_DOUBLING_MAX ? 2 : 1.125)) >>> 0);
    if (prevCapacity != 0) newCapacity = Math.max(newCapacity, 256);
    // At minimum allocate 256b for each file when expanding.
    var oldContents = node.contents;
    node.contents = new Uint8Array(newCapacity);
    // Allocate new storage.
    if (node.usedBytes > 0) node.contents.set(oldContents.subarray(0, node.usedBytes), 0);
  },
  resizeFileStorage(node, newSize) {
    if (node.usedBytes == newSize) return;
    if (newSize == 0) {
      node.contents = null;
      // Fully decommit when requesting a resize to zero.
      node.usedBytes = 0;
    } else {
      var oldContents = node.contents;
      node.contents = new Uint8Array(newSize);
      // Allocate new storage.
      if (oldContents) {
        node.contents.set(oldContents.subarray(0, Math.min(newSize, node.usedBytes)));
      }
      node.usedBytes = newSize;
    }
  },
  node_ops: {
    getattr(node) {
      var attr = {};
      // device numbers reuse inode numbers.
      attr.dev = FS.isChrdev(node.mode) ? node.id : 1;
      attr.ino = node.id;
      attr.mode = node.mode;
      attr.nlink = 1;
      attr.uid = 0;
      attr.gid = 0;
      attr.rdev = node.rdev;
      if (FS.isDir(node.mode)) {
        attr.size = 4096;
      } else if (FS.isFile(node.mode)) {
        attr.size = node.usedBytes;
      } else if (FS.isLink(node.mode)) {
        attr.size = node.link.length;
      } else {
        attr.size = 0;
      }
      attr.atime = new Date(node.atime);
      attr.mtime = new Date(node.mtime);
      attr.ctime = new Date(node.ctime);
      // NOTE: In our implementation, st_blocks = Math.ceil(st_size/st_blksize),
      //       but this is not required by the standard.
      attr.blksize = 4096;
      attr.blocks = Math.ceil(attr.size / attr.blksize);
      return attr;
    },
    setattr(node, attr) {
      for (const key of [ "mode", "atime", "mtime", "ctime" ]) {
        if (attr[key] != null) {
          node[key] = attr[key];
        }
      }
      if (attr.size !== undefined) {
        MEMFS.resizeFileStorage(node, attr.size);
      }
    },
    lookup(parent, name) {
      // This error may happen quite a bit. To avoid overhead we reuse it (and
      // suffer a lack of stack info).
      if (!MEMFS.doesNotExistError) {
        MEMFS.doesNotExistError = new FS.ErrnoError(44);
        /** @suppress {checkTypes} */ MEMFS.doesNotExistError.stack = "<generic error, no stack>";
      }
      throw MEMFS.doesNotExistError;
    },
    mknod(parent, name, mode, dev) {
      return MEMFS.createNode(parent, name, mode, dev);
    },
    rename(old_node, new_dir, new_name) {
      var new_node;
      try {
        new_node = FS.lookupNode(new_dir, new_name);
      } catch (e) {}
      if (new_node) {
        if (FS.isDir(old_node.mode)) {
          // if we're overwriting a directory at new_name, make sure it's empty.
          for (var i in new_node.contents) {
            throw new FS.ErrnoError(55);
          }
        }
        FS.hashRemoveNode(new_node);
      }
      // do the internal rewiring
      delete old_node.parent.contents[old_node.name];
      new_dir.contents[new_name] = old_node;
      old_node.name = new_name;
      new_dir.ctime = new_dir.mtime = old_node.parent.ctime = old_node.parent.mtime = Date.now();
    },
    unlink(parent, name) {
      delete parent.contents[name];
      parent.ctime = parent.mtime = Date.now();
    },
    rmdir(parent, name) {
      var node = FS.lookupNode(parent, name);
      for (var i in node.contents) {
        throw new FS.ErrnoError(55);
      }
      delete parent.contents[name];
      parent.ctime = parent.mtime = Date.now();
    },
    readdir(node) {
      return [ ".", "..", ...Object.keys(node.contents) ];
    },
    symlink(parent, newname, oldpath) {
      var node = MEMFS.createNode(parent, newname, 511 | 40960, 0);
      node.link = oldpath;
      return node;
    },
    readlink(node) {
      if (!FS.isLink(node.mode)) {
        throw new FS.ErrnoError(28);
      }
      return node.link;
    }
  },
  stream_ops: {
    read(stream, buffer, offset, length, position) {
      var contents = stream.node.contents;
      if (position >= stream.node.usedBytes) return 0;
      var size = Math.min(stream.node.usedBytes - position, length);
      if (size > 8 && contents.subarray) {
        // non-trivial, and typed array
        buffer.set(contents.subarray(position, position + size), offset);
      } else {
        for (var i = 0; i < size; i++) buffer[offset + i] = contents[position + i];
      }
      return size;
    },
    write(stream, buffer, offset, length, position, canOwn) {
      // If the buffer is located in main memory (HEAP), and if
      // memory can grow, we can't hold on to references of the
      // memory buffer, as they may get invalidated. That means we
      // need to copy its contents.
      if (buffer.buffer === (growMemViews(), HEAP8).buffer) {
        canOwn = false;
      }
      if (!length) return 0;
      var node = stream.node;
      node.mtime = node.ctime = Date.now();
      if (buffer.subarray && (!node.contents || node.contents.subarray)) {
        // This write is from a typed array to a typed array?
        if (canOwn) {
          node.contents = buffer.subarray(offset, offset + length);
          node.usedBytes = length;
          return length;
        } else if (node.usedBytes === 0 && position === 0) {
          // If this is a simple first write to an empty file, do a fast set since we don't need to care about old data.
          node.contents = buffer.slice(offset, offset + length);
          node.usedBytes = length;
          return length;
        } else if (position + length <= node.usedBytes) {
          // Writing to an already allocated and used subrange of the file?
          node.contents.set(buffer.subarray(offset, offset + length), position);
          return length;
        }
      }
      // Appending to an existing file and we need to reallocate, or source data did not come as a typed array.
      MEMFS.expandFileStorage(node, position + length);
      if (node.contents.subarray && buffer.subarray) {
        // Use typed array write which is available.
        node.contents.set(buffer.subarray(offset, offset + length), position);
      } else {
        for (var i = 0; i < length; i++) {
          node.contents[position + i] = buffer[offset + i];
        }
      }
      node.usedBytes = Math.max(node.usedBytes, position + length);
      return length;
    },
    llseek(stream, offset, whence) {
      var position = offset;
      if (whence === 1) {
        position += stream.position;
      } else if (whence === 2) {
        if (FS.isFile(stream.node.mode)) {
          position += stream.node.usedBytes;
        }
      }
      if (position < 0) {
        throw new FS.ErrnoError(28);
      }
      return position;
    },
    mmap(stream, length, position, prot, flags) {
      if (!FS.isFile(stream.node.mode)) {
        throw new FS.ErrnoError(43);
      }
      var ptr;
      var allocated;
      var contents = stream.node.contents;
      // Only make a new copy when MAP_PRIVATE is specified.
      if (!(flags & 2) && contents && contents.buffer === (growMemViews(), HEAP8).buffer) {
        // We can't emulate MAP_SHARED when the file is not backed by the
        // buffer we're mapping to (e.g. the HEAP buffer).
        allocated = false;
        ptr = contents.byteOffset;
      } else {
        allocated = true;
        ptr = mmapAlloc(length);
        if (!ptr) {
          throw new FS.ErrnoError(48);
        }
        if (contents) {
          // Try to avoid unnecessary slices.
          if (position > 0 || position + length < contents.length) {
            if (contents.subarray) {
              contents = contents.subarray(position, position + length);
            } else {
              contents = Array.prototype.slice.call(contents, position, position + length);
            }
          }
          (growMemViews(), HEAP8).set(contents, ptr);
        }
      }
      return {
        ptr,
        allocated
      };
    },
    msync(stream, buffer, offset, length, mmapFlags) {
      MEMFS.stream_ops.write(stream, buffer, 0, length, offset, false);
      // should we check if bytesWritten and length are the same?
      return 0;
    }
  }
};

var FS_modeStringToFlags = str => {
  var flagModes = {
    "r": 0,
    "r+": 2,
    "w": 512 | 64 | 1,
    "w+": 512 | 64 | 2,
    "a": 1024 | 64 | 1,
    "a+": 1024 | 64 | 2
  };
  var flags = flagModes[str];
  if (typeof flags == "undefined") {
    throw new Error(`Unknown file open mode: ${str}`);
  }
  return flags;
};

var FS_getMode = (canRead, canWrite) => {
  var mode = 0;
  if (canRead) mode |= 292 | 73;
  if (canWrite) mode |= 146;
  return mode;
};

var asyncLoad = async url => {
  var arrayBuffer = await readAsync(url);
  return new Uint8Array(arrayBuffer);
};

var FS_createDataFile = (...args) => FS.createDataFile(...args);

var getUniqueRunDependency = id => id;

var preloadPlugins = [];

var FS_handledByPreloadPlugin = async (byteArray, fullname) => {
  // Ensure plugins are ready.
  if (typeof Browser != "undefined") Browser.init();
  for (var plugin of preloadPlugins) {
    if (plugin["canHandle"](fullname)) {
      return plugin["handle"](byteArray, fullname);
    }
  }
  // If no plugin handled this file then return the original/unmodified
  // byteArray.
  return byteArray;
};

var FS_preloadFile = async (parent, name, url, canRead, canWrite, dontCreateFile, canOwn, preFinish) => {
  // TODO we should allow people to just pass in a complete filename instead
  // of parent and name being that we just join them anyways
  var fullname = name ? PATH_FS.resolve(PATH.join2(parent, name)) : parent;
  var dep = getUniqueRunDependency(`cp ${fullname}`);
  // might have several active requests for the same fullname
  addRunDependency(dep);
  try {
    var byteArray = url;
    if (typeof url == "string") {
      byteArray = await asyncLoad(url);
    }
    byteArray = await FS_handledByPreloadPlugin(byteArray, fullname);
    preFinish?.();
    if (!dontCreateFile) {
      FS_createDataFile(parent, name, byteArray, canRead, canWrite, canOwn);
    }
  } finally {
    removeRunDependency(dep);
  }
};

var FS_createPreloadedFile = (parent, name, url, canRead, canWrite, onload, onerror, dontCreateFile, canOwn, preFinish) => {
  FS_preloadFile(parent, name, url, canRead, canWrite, dontCreateFile, canOwn, preFinish).then(onload).catch(onerror);
};

var FS = {
  root: null,
  mounts: [],
  devices: {},
  streams: [],
  nextInode: 1,
  nameTable: null,
  currentPath: "/",
  initialized: false,
  ignorePermissions: true,
  filesystems: null,
  syncFSRequests: 0,
  ErrnoError: class {
    name="ErrnoError";
    // We set the `name` property to be able to identify `FS.ErrnoError`
    // - the `name` is a standard ECMA-262 property of error objects. Kind of good to have it anyway.
    // - when using PROXYFS, an error can come from an underlying FS
    // as different FS objects have their own FS.ErrnoError each,
    // the test `err instanceof FS.ErrnoError` won't detect an error coming from another filesystem, causing bugs.
    // we'll use the reliable test `err.name == "ErrnoError"` instead
    constructor(errno) {
      this.errno = errno;
    }
  },
  FSStream: class {
    shared={};
    get object() {
      return this.node;
    }
    set object(val) {
      this.node = val;
    }
    get isRead() {
      return (this.flags & 2097155) !== 1;
    }
    get isWrite() {
      return (this.flags & 2097155) !== 0;
    }
    get isAppend() {
      return (this.flags & 1024);
    }
    get flags() {
      return this.shared.flags;
    }
    set flags(val) {
      this.shared.flags = val;
    }
    get position() {
      return this.shared.position;
    }
    set position(val) {
      this.shared.position = val;
    }
  },
  FSNode: class {
    node_ops={};
    stream_ops={};
    readMode=292 | 73;
    writeMode=146;
    mounted=null;
    constructor(parent, name, mode, rdev) {
      if (!parent) {
        parent = this;
      }
      this.parent = parent;
      this.mount = parent.mount;
      this.id = FS.nextInode++;
      this.name = name;
      this.mode = mode;
      this.rdev = rdev;
      this.atime = this.mtime = this.ctime = Date.now();
    }
    get read() {
      return (this.mode & this.readMode) === this.readMode;
    }
    set read(val) {
      val ? this.mode |= this.readMode : this.mode &= ~this.readMode;
    }
    get write() {
      return (this.mode & this.writeMode) === this.writeMode;
    }
    set write(val) {
      val ? this.mode |= this.writeMode : this.mode &= ~this.writeMode;
    }
    get isFolder() {
      return FS.isDir(this.mode);
    }
    get isDevice() {
      return FS.isChrdev(this.mode);
    }
  },
  lookupPath(path, opts = {}) {
    if (!path) {
      throw new FS.ErrnoError(44);
    }
    opts.follow_mount ??= true;
    if (!PATH.isAbs(path)) {
      path = FS.cwd() + "/" + path;
    }
    // limit max consecutive symlinks to 40 (SYMLOOP_MAX).
    linkloop: for (var nlinks = 0; nlinks < 40; nlinks++) {
      // split the absolute path
      var parts = path.split("/").filter(p => !!p);
      // start at the root
      var current = FS.root;
      var current_path = "/";
      for (var i = 0; i < parts.length; i++) {
        var islast = (i === parts.length - 1);
        if (islast && opts.parent) {
          // stop resolving
          break;
        }
        if (parts[i] === ".") {
          continue;
        }
        if (parts[i] === "..") {
          current_path = PATH.dirname(current_path);
          if (FS.isRoot(current)) {
            path = current_path + "/" + parts.slice(i + 1).join("/");
            // We're making progress here, don't let many consecutive ..'s
            // lead to ELOOP
            nlinks--;
            continue linkloop;
          } else {
            current = current.parent;
          }
          continue;
        }
        current_path = PATH.join2(current_path, parts[i]);
        try {
          current = FS.lookupNode(current, parts[i]);
        } catch (e) {
          // if noent_okay is true, suppress a ENOENT in the last component
          // and return an object with an undefined node. This is needed for
          // resolving symlinks in the path when creating a file.
          if ((e?.errno === 44) && islast && opts.noent_okay) {
            return {
              path: current_path
            };
          }
          throw e;
        }
        // jump to the mount's root node if this is a mountpoint
        if (FS.isMountpoint(current) && (!islast || opts.follow_mount)) {
          current = current.mounted.root;
        }
        // by default, lookupPath will not follow a symlink if it is the final path component.
        // setting opts.follow = true will override this behavior.
        if (FS.isLink(current.mode) && (!islast || opts.follow)) {
          if (!current.node_ops.readlink) {
            throw new FS.ErrnoError(52);
          }
          var link = current.node_ops.readlink(current);
          if (!PATH.isAbs(link)) {
            link = PATH.dirname(current_path) + "/" + link;
          }
          path = link + "/" + parts.slice(i + 1).join("/");
          continue linkloop;
        }
      }
      return {
        path: current_path,
        node: current
      };
    }
    throw new FS.ErrnoError(32);
  },
  getPath(node) {
    var path;
    while (true) {
      if (FS.isRoot(node)) {
        var mount = node.mount.mountpoint;
        if (!path) return mount;
        return mount[mount.length - 1] !== "/" ? `${mount}/${path}` : mount + path;
      }
      path = path ? `${node.name}/${path}` : node.name;
      node = node.parent;
    }
  },
  hashName(parentid, name) {
    var hash = 0;
    for (var i = 0; i < name.length; i++) {
      hash = ((hash << 5) - hash + name.charCodeAt(i)) | 0;
    }
    return ((parentid + hash) >>> 0) % FS.nameTable.length;
  },
  hashAddNode(node) {
    var hash = FS.hashName(node.parent.id, node.name);
    node.name_next = FS.nameTable[hash];
    FS.nameTable[hash] = node;
  },
  hashRemoveNode(node) {
    var hash = FS.hashName(node.parent.id, node.name);
    if (FS.nameTable[hash] === node) {
      FS.nameTable[hash] = node.name_next;
    } else {
      var current = FS.nameTable[hash];
      while (current) {
        if (current.name_next === node) {
          current.name_next = node.name_next;
          break;
        }
        current = current.name_next;
      }
    }
  },
  lookupNode(parent, name) {
    var errCode = FS.mayLookup(parent);
    if (errCode) {
      throw new FS.ErrnoError(errCode);
    }
    var hash = FS.hashName(parent.id, name);
    for (var node = FS.nameTable[hash]; node; node = node.name_next) {
      var nodeName = node.name;
      if (node.parent.id === parent.id && nodeName === name) {
        return node;
      }
    }
    // if we failed to find it in the cache, call into the VFS
    return FS.lookup(parent, name);
  },
  createNode(parent, name, mode, rdev) {
    var node = new FS.FSNode(parent, name, mode, rdev);
    FS.hashAddNode(node);
    return node;
  },
  destroyNode(node) {
    FS.hashRemoveNode(node);
  },
  isRoot(node) {
    return node === node.parent;
  },
  isMountpoint(node) {
    return !!node.mounted;
  },
  isFile(mode) {
    return (mode & 61440) === 32768;
  },
  isDir(mode) {
    return (mode & 61440) === 16384;
  },
  isLink(mode) {
    return (mode & 61440) === 40960;
  },
  isChrdev(mode) {
    return (mode & 61440) === 8192;
  },
  isBlkdev(mode) {
    return (mode & 61440) === 24576;
  },
  isFIFO(mode) {
    return (mode & 61440) === 4096;
  },
  isSocket(mode) {
    return (mode & 49152) === 49152;
  },
  flagsToPermissionString(flag) {
    var perms = [ "r", "w", "rw" ][flag & 3];
    if ((flag & 512)) {
      perms += "w";
    }
    return perms;
  },
  nodePermissions(node, perms) {
    if (FS.ignorePermissions) {
      return 0;
    }
    // return 0 if any user, group or owner bits are set.
    if (perms.includes("r") && !(node.mode & 292)) {
      return 2;
    }
    if (perms.includes("w") && !(node.mode & 146)) {
      return 2;
    }
    if (perms.includes("x") && !(node.mode & 73)) {
      return 2;
    }
    return 0;
  },
  mayLookup(dir) {
    if (!FS.isDir(dir.mode)) return 54;
    var errCode = FS.nodePermissions(dir, "x");
    if (errCode) return errCode;
    if (!dir.node_ops.lookup) return 2;
    return 0;
  },
  mayCreate(dir, name) {
    if (!FS.isDir(dir.mode)) {
      return 54;
    }
    try {
      var node = FS.lookupNode(dir, name);
      return 20;
    } catch (e) {}
    return FS.nodePermissions(dir, "wx");
  },
  mayDelete(dir, name, isdir) {
    var node;
    try {
      node = FS.lookupNode(dir, name);
    } catch (e) {
      return e.errno;
    }
    var errCode = FS.nodePermissions(dir, "wx");
    if (errCode) {
      return errCode;
    }
    if (isdir) {
      if (!FS.isDir(node.mode)) {
        return 54;
      }
      if (FS.isRoot(node) || FS.getPath(node) === FS.cwd()) {
        return 10;
      }
    } else if (FS.isDir(node.mode)) {
      return 31;
    }
    return 0;
  },
  mayOpen(node, flags) {
    if (!node) {
      return 44;
    }
    if (FS.isLink(node.mode)) {
      return 32;
    }
    var mode = FS.flagsToPermissionString(flags);
    if (FS.isDir(node.mode)) {
      // opening for write
      // TODO: check for O_SEARCH? (== search for dir only)
      if (mode !== "r" || (flags & (512 | 64))) {
        return 31;
      }
    }
    return FS.nodePermissions(node, mode);
  },
  checkOpExists(op, err) {
    if (!op) {
      throw new FS.ErrnoError(err);
    }
    return op;
  },
  MAX_OPEN_FDS: 4096,
  nextfd() {
    for (var fd = 0; fd <= FS.MAX_OPEN_FDS; fd++) {
      if (!FS.streams[fd]) {
        return fd;
      }
    }
    throw new FS.ErrnoError(33);
  },
  getStreamChecked(fd) {
    var stream = FS.getStream(fd);
    if (!stream) {
      throw new FS.ErrnoError(8);
    }
    return stream;
  },
  getStream: fd => FS.streams[fd],
  createStream(stream, fd = -1) {
    // clone it, so we can return an instance of FSStream
    stream = Object.assign(new FS.FSStream, stream);
    if (fd == -1) {
      fd = FS.nextfd();
    }
    stream.fd = fd;
    FS.streams[fd] = stream;
    return stream;
  },
  closeStream(fd) {
    FS.streams[fd] = null;
  },
  dupStream(origStream, fd = -1) {
    var stream = FS.createStream(origStream, fd);
    stream.stream_ops?.dup?.(stream);
    return stream;
  },
  doSetAttr(stream, node, attr) {
    var setattr = stream?.stream_ops.setattr;
    var arg = setattr ? stream : node;
    setattr ??= node.node_ops.setattr;
    FS.checkOpExists(setattr, 63);
    setattr(arg, attr);
  },
  chrdev_stream_ops: {
    open(stream) {
      var device = FS.getDevice(stream.node.rdev);
      // override node's stream ops with the device's
      stream.stream_ops = device.stream_ops;
      // forward the open call
      stream.stream_ops.open?.(stream);
    },
    llseek() {
      throw new FS.ErrnoError(70);
    }
  },
  major: dev => ((dev) >> 8),
  minor: dev => ((dev) & 255),
  makedev: (ma, mi) => ((ma) << 8 | (mi)),
  registerDevice(dev, ops) {
    FS.devices[dev] = {
      stream_ops: ops
    };
  },
  getDevice: dev => FS.devices[dev],
  getMounts(mount) {
    var mounts = [];
    var check = [ mount ];
    while (check.length) {
      var m = check.pop();
      mounts.push(m);
      check.push(...m.mounts);
    }
    return mounts;
  },
  syncfs(populate, callback) {
    if (typeof populate == "function") {
      callback = populate;
      populate = false;
    }
    FS.syncFSRequests++;
    if (FS.syncFSRequests > 1) {
      err(`warning: ${FS.syncFSRequests} FS.syncfs operations in flight at once, probably just doing extra work`);
    }
    var mounts = FS.getMounts(FS.root.mount);
    var completed = 0;
    function doCallback(errCode) {
      FS.syncFSRequests--;
      return callback(errCode);
    }
    function done(errCode) {
      if (errCode) {
        if (!done.errored) {
          done.errored = true;
          return doCallback(errCode);
        }
        return;
      }
      if (++completed >= mounts.length) {
        doCallback(null);
      }
    }
    // sync all mounts
    for (var mount of mounts) {
      if (mount.type.syncfs) {
        mount.type.syncfs(mount, populate, done);
      } else {
        done(null);
      }
    }
  },
  mount(type, opts, mountpoint) {
    var root = mountpoint === "/";
    var pseudo = !mountpoint;
    var node;
    if (root && FS.root) {
      throw new FS.ErrnoError(10);
    } else if (!root && !pseudo) {
      var lookup = FS.lookupPath(mountpoint, {
        follow_mount: false
      });
      mountpoint = lookup.path;
      // use the absolute path
      node = lookup.node;
      if (FS.isMountpoint(node)) {
        throw new FS.ErrnoError(10);
      }
      if (!FS.isDir(node.mode)) {
        throw new FS.ErrnoError(54);
      }
    }
    var mount = {
      type,
      opts,
      mountpoint,
      mounts: []
    };
    // create a root node for the fs
    var mountRoot = type.mount(mount);
    mountRoot.mount = mount;
    mount.root = mountRoot;
    if (root) {
      FS.root = mountRoot;
    } else if (node) {
      // set as a mountpoint
      node.mounted = mount;
      // add the new mount to the current mount's children
      if (node.mount) {
        node.mount.mounts.push(mount);
      }
    }
    return mountRoot;
  },
  unmount(mountpoint) {
    var lookup = FS.lookupPath(mountpoint, {
      follow_mount: false
    });
    if (!FS.isMountpoint(lookup.node)) {
      throw new FS.ErrnoError(28);
    }
    // destroy the nodes for this mount, and all its child mounts
    var node = lookup.node;
    var mount = node.mounted;
    var mounts = FS.getMounts(mount);
    for (var [hash, current] of Object.entries(FS.nameTable)) {
      while (current) {
        var next = current.name_next;
        if (mounts.includes(current.mount)) {
          FS.destroyNode(current);
        }
        current = next;
      }
    }
    // no longer a mountpoint
    node.mounted = null;
    // remove this mount from the child mounts
    var idx = node.mount.mounts.indexOf(mount);
    node.mount.mounts.splice(idx, 1);
  },
  lookup(parent, name) {
    return parent.node_ops.lookup(parent, name);
  },
  mknod(path, mode, dev) {
    var lookup = FS.lookupPath(path, {
      parent: true
    });
    var parent = lookup.node;
    var name = PATH.basename(path);
    if (!name) {
      throw new FS.ErrnoError(28);
    }
    if (name === "." || name === "..") {
      throw new FS.ErrnoError(20);
    }
    var errCode = FS.mayCreate(parent, name);
    if (errCode) {
      throw new FS.ErrnoError(errCode);
    }
    if (!parent.node_ops.mknod) {
      throw new FS.ErrnoError(63);
    }
    return parent.node_ops.mknod(parent, name, mode, dev);
  },
  statfs(path) {
    return FS.statfsNode(FS.lookupPath(path, {
      follow: true
    }).node);
  },
  statfsStream(stream) {
    // We keep a separate statfsStream function because noderawfs overrides
    // it. In noderawfs, stream.node is sometimes null. Instead, we need to
    // look at stream.path.
    return FS.statfsNode(stream.node);
  },
  statfsNode(node) {
    // NOTE: None of the defaults here are true. We're just returning safe and
    //       sane values. Currently nodefs and rawfs replace these defaults,
    //       other file systems leave them alone.
    var rtn = {
      bsize: 4096,
      frsize: 4096,
      blocks: 1e6,
      bfree: 5e5,
      bavail: 5e5,
      files: FS.nextInode,
      ffree: FS.nextInode - 1,
      fsid: 42,
      flags: 2,
      namelen: 255
    };
    if (node.node_ops.statfs) {
      Object.assign(rtn, node.node_ops.statfs(node.mount.opts.root));
    }
    return rtn;
  },
  create(path, mode = 438) {
    mode &= 4095;
    mode |= 32768;
    return FS.mknod(path, mode, 0);
  },
  mkdir(path, mode = 511) {
    mode &= 511 | 512;
    mode |= 16384;
    return FS.mknod(path, mode, 0);
  },
  mkdirTree(path, mode) {
    var dirs = path.split("/");
    var d = "";
    for (var dir of dirs) {
      if (!dir) continue;
      if (d || PATH.isAbs(path)) d += "/";
      d += dir;
      try {
        FS.mkdir(d, mode);
      } catch (e) {
        if (e.errno != 20) throw e;
      }
    }
  },
  mkdev(path, mode, dev) {
    if (typeof dev == "undefined") {
      dev = mode;
      mode = 438;
    }
    mode |= 8192;
    return FS.mknod(path, mode, dev);
  },
  symlink(oldpath, newpath) {
    if (!PATH_FS.resolve(oldpath)) {
      throw new FS.ErrnoError(44);
    }
    var lookup = FS.lookupPath(newpath, {
      parent: true
    });
    var parent = lookup.node;
    if (!parent) {
      throw new FS.ErrnoError(44);
    }
    var newname = PATH.basename(newpath);
    var errCode = FS.mayCreate(parent, newname);
    if (errCode) {
      throw new FS.ErrnoError(errCode);
    }
    if (!parent.node_ops.symlink) {
      throw new FS.ErrnoError(63);
    }
    return parent.node_ops.symlink(parent, newname, oldpath);
  },
  rename(old_path, new_path) {
    var old_dirname = PATH.dirname(old_path);
    var new_dirname = PATH.dirname(new_path);
    var old_name = PATH.basename(old_path);
    var new_name = PATH.basename(new_path);
    // parents must exist
    var lookup, old_dir, new_dir;
    // let the errors from non existent directories percolate up
    lookup = FS.lookupPath(old_path, {
      parent: true
    });
    old_dir = lookup.node;
    lookup = FS.lookupPath(new_path, {
      parent: true
    });
    new_dir = lookup.node;
    if (!old_dir || !new_dir) throw new FS.ErrnoError(44);
    // need to be part of the same mount
    if (old_dir.mount !== new_dir.mount) {
      throw new FS.ErrnoError(75);
    }
    // source must exist
    var old_node = FS.lookupNode(old_dir, old_name);
    // old path should not be an ancestor of the new path
    var relative = PATH_FS.relative(old_path, new_dirname);
    if (relative.charAt(0) !== ".") {
      throw new FS.ErrnoError(28);
    }
    // new path should not be an ancestor of the old path
    relative = PATH_FS.relative(new_path, old_dirname);
    if (relative.charAt(0) !== ".") {
      throw new FS.ErrnoError(55);
    }
    // see if the new path already exists
    var new_node;
    try {
      new_node = FS.lookupNode(new_dir, new_name);
    } catch (e) {}
    // early out if nothing needs to change
    if (old_node === new_node) {
      return;
    }
    // we'll need to delete the old entry
    var isdir = FS.isDir(old_node.mode);
    var errCode = FS.mayDelete(old_dir, old_name, isdir);
    if (errCode) {
      throw new FS.ErrnoError(errCode);
    }
    // need delete permissions if we'll be overwriting.
    // need create permissions if new doesn't already exist.
    errCode = new_node ? FS.mayDelete(new_dir, new_name, isdir) : FS.mayCreate(new_dir, new_name);
    if (errCode) {
      throw new FS.ErrnoError(errCode);
    }
    if (!old_dir.node_ops.rename) {
      throw new FS.ErrnoError(63);
    }
    if (FS.isMountpoint(old_node) || (new_node && FS.isMountpoint(new_node))) {
      throw new FS.ErrnoError(10);
    }
    // if we are going to change the parent, check write permissions
    if (new_dir !== old_dir) {
      errCode = FS.nodePermissions(old_dir, "w");
      if (errCode) {
        throw new FS.ErrnoError(errCode);
      }
    }
    // remove the node from the lookup hash
    FS.hashRemoveNode(old_node);
    // do the underlying fs rename
    try {
      old_dir.node_ops.rename(old_node, new_dir, new_name);
      // update old node (we do this here to avoid each backend
      // needing to)
      old_node.parent = new_dir;
    } catch (e) {
      throw e;
    } finally {
      // add the node back to the hash (in case node_ops.rename
      // changed its name)
      FS.hashAddNode(old_node);
    }
  },
  rmdir(path) {
    var lookup = FS.lookupPath(path, {
      parent: true
    });
    var parent = lookup.node;
    var name = PATH.basename(path);
    var node = FS.lookupNode(parent, name);
    var errCode = FS.mayDelete(parent, name, true);
    if (errCode) {
      throw new FS.ErrnoError(errCode);
    }
    if (!parent.node_ops.rmdir) {
      throw new FS.ErrnoError(63);
    }
    if (FS.isMountpoint(node)) {
      throw new FS.ErrnoError(10);
    }
    parent.node_ops.rmdir(parent, name);
    FS.destroyNode(node);
  },
  readdir(path) {
    var lookup = FS.lookupPath(path, {
      follow: true
    });
    var node = lookup.node;
    var readdir = FS.checkOpExists(node.node_ops.readdir, 54);
    return readdir(node);
  },
  unlink(path) {
    var lookup = FS.lookupPath(path, {
      parent: true
    });
    var parent = lookup.node;
    if (!parent) {
      throw new FS.ErrnoError(44);
    }
    var name = PATH.basename(path);
    var node = FS.lookupNode(parent, name);
    var errCode = FS.mayDelete(parent, name, false);
    if (errCode) {
      // According to POSIX, we should map EISDIR to EPERM, but
      // we instead do what Linux does (and we must, as we use
      // the musl linux libc).
      throw new FS.ErrnoError(errCode);
    }
    if (!parent.node_ops.unlink) {
      throw new FS.ErrnoError(63);
    }
    if (FS.isMountpoint(node)) {
      throw new FS.ErrnoError(10);
    }
    parent.node_ops.unlink(parent, name);
    FS.destroyNode(node);
  },
  readlink(path) {
    var lookup = FS.lookupPath(path);
    var link = lookup.node;
    if (!link) {
      throw new FS.ErrnoError(44);
    }
    if (!link.node_ops.readlink) {
      throw new FS.ErrnoError(28);
    }
    return link.node_ops.readlink(link);
  },
  stat(path, dontFollow) {
    var lookup = FS.lookupPath(path, {
      follow: !dontFollow
    });
    var node = lookup.node;
    var getattr = FS.checkOpExists(node.node_ops.getattr, 63);
    return getattr(node);
  },
  fstat(fd) {
    var stream = FS.getStreamChecked(fd);
    var node = stream.node;
    var getattr = stream.stream_ops.getattr;
    var arg = getattr ? stream : node;
    getattr ??= node.node_ops.getattr;
    FS.checkOpExists(getattr, 63);
    return getattr(arg);
  },
  lstat(path) {
    return FS.stat(path, true);
  },
  doChmod(stream, node, mode, dontFollow) {
    FS.doSetAttr(stream, node, {
      mode: (mode & 4095) | (node.mode & ~4095),
      ctime: Date.now(),
      dontFollow
    });
  },
  chmod(path, mode, dontFollow) {
    var node;
    if (typeof path == "string") {
      var lookup = FS.lookupPath(path, {
        follow: !dontFollow
      });
      node = lookup.node;
    } else {
      node = path;
    }
    FS.doChmod(null, node, mode, dontFollow);
  },
  lchmod(path, mode) {
    FS.chmod(path, mode, true);
  },
  fchmod(fd, mode) {
    var stream = FS.getStreamChecked(fd);
    FS.doChmod(stream, stream.node, mode, false);
  },
  doChown(stream, node, dontFollow) {
    FS.doSetAttr(stream, node, {
      timestamp: Date.now(),
      dontFollow
    });
  },
  chown(path, uid, gid, dontFollow) {
    var node;
    if (typeof path == "string") {
      var lookup = FS.lookupPath(path, {
        follow: !dontFollow
      });
      node = lookup.node;
    } else {
      node = path;
    }
    FS.doChown(null, node, dontFollow);
  },
  lchown(path, uid, gid) {
    FS.chown(path, uid, gid, true);
  },
  fchown(fd, uid, gid) {
    var stream = FS.getStreamChecked(fd);
    FS.doChown(stream, stream.node, false);
  },
  doTruncate(stream, node, len) {
    if (FS.isDir(node.mode)) {
      throw new FS.ErrnoError(31);
    }
    if (!FS.isFile(node.mode)) {
      throw new FS.ErrnoError(28);
    }
    var errCode = FS.nodePermissions(node, "w");
    if (errCode) {
      throw new FS.ErrnoError(errCode);
    }
    FS.doSetAttr(stream, node, {
      size: len,
      timestamp: Date.now()
    });
  },
  truncate(path, len) {
    if (len < 0) {
      throw new FS.ErrnoError(28);
    }
    var node;
    if (typeof path == "string") {
      var lookup = FS.lookupPath(path, {
        follow: true
      });
      node = lookup.node;
    } else {
      node = path;
    }
    FS.doTruncate(null, node, len);
  },
  ftruncate(fd, len) {
    var stream = FS.getStreamChecked(fd);
    if (len < 0 || (stream.flags & 2097155) === 0) {
      throw new FS.ErrnoError(28);
    }
    FS.doTruncate(stream, stream.node, len);
  },
  utime(path, atime, mtime) {
    var lookup = FS.lookupPath(path, {
      follow: true
    });
    var node = lookup.node;
    var setattr = FS.checkOpExists(node.node_ops.setattr, 63);
    setattr(node, {
      atime,
      mtime
    });
  },
  open(path, flags, mode = 438) {
    if (path === "") {
      throw new FS.ErrnoError(44);
    }
    flags = typeof flags == "string" ? FS_modeStringToFlags(flags) : flags;
    if ((flags & 64)) {
      mode = (mode & 4095) | 32768;
    } else {
      mode = 0;
    }
    var node;
    var isDirPath;
    if (typeof path == "object") {
      node = path;
    } else {
      isDirPath = path.endsWith("/");
      // noent_okay makes it so that if the final component of the path
      // doesn't exist, lookupPath returns `node: undefined`. `path` will be
      // updated to point to the target of all symlinks.
      var lookup = FS.lookupPath(path, {
        follow: !(flags & 131072),
        noent_okay: true
      });
      node = lookup.node;
      path = lookup.path;
    }
    // perhaps we need to create the node
    var created = false;
    if ((flags & 64)) {
      if (node) {
        // if O_CREAT and O_EXCL are set, error out if the node already exists
        if ((flags & 128)) {
          throw new FS.ErrnoError(20);
        }
      } else if (isDirPath) {
        throw new FS.ErrnoError(31);
      } else {
        // node doesn't exist, try to create it
        // Ignore the permission bits here to ensure we can `open` this new
        // file below. We use chmod below to apply the permissions once the
        // file is open.
        node = FS.mknod(path, mode | 511, 0);
        created = true;
      }
    }
    if (!node) {
      throw new FS.ErrnoError(44);
    }
    // can't truncate a device
    if (FS.isChrdev(node.mode)) {
      flags &= ~512;
    }
    // if asked only for a directory, then this must be one
    if ((flags & 65536) && !FS.isDir(node.mode)) {
      throw new FS.ErrnoError(54);
    }
    // check permissions, if this is not a file we just created now (it is ok to
    // create and write to a file with read-only permissions; it is read-only
    // for later use)
    if (!created) {
      var errCode = FS.mayOpen(node, flags);
      if (errCode) {
        throw new FS.ErrnoError(errCode);
      }
    }
    // do truncation if necessary
    if ((flags & 512) && !created) {
      FS.truncate(node, 0);
    }
    // we've already handled these, don't pass down to the underlying vfs
    flags &= ~(128 | 512 | 131072);
    // register the stream with the filesystem
    var stream = FS.createStream({
      node,
      path: FS.getPath(node),
      // we want the absolute path to the node
      flags,
      seekable: true,
      position: 0,
      stream_ops: node.stream_ops,
      // used by the file family libc calls (fopen, fwrite, ferror, etc.)
      ungotten: [],
      error: false
    });
    // call the new stream's open function
    if (stream.stream_ops.open) {
      stream.stream_ops.open(stream);
    }
    if (created) {
      FS.chmod(node, mode & 511);
    }
    return stream;
  },
  close(stream) {
    if (FS.isClosed(stream)) {
      throw new FS.ErrnoError(8);
    }
    if (stream.getdents) stream.getdents = null;
    // free readdir state
    try {
      if (stream.stream_ops.close) {
        stream.stream_ops.close(stream);
      }
    } catch (e) {
      throw e;
    } finally {
      FS.closeStream(stream.fd);
    }
    stream.fd = null;
  },
  isClosed(stream) {
    return stream.fd === null;
  },
  llseek(stream, offset, whence) {
    if (FS.isClosed(stream)) {
      throw new FS.ErrnoError(8);
    }
    if (!stream.seekable || !stream.stream_ops.llseek) {
      throw new FS.ErrnoError(70);
    }
    if (whence != 0 && whence != 1 && whence != 2) {
      throw new FS.ErrnoError(28);
    }
    stream.position = stream.stream_ops.llseek(stream, offset, whence);
    stream.ungotten = [];
    return stream.position;
  },
  read(stream, buffer, offset, length, position) {
    if (length < 0 || position < 0) {
      throw new FS.ErrnoError(28);
    }
    if (FS.isClosed(stream)) {
      throw new FS.ErrnoError(8);
    }
    if ((stream.flags & 2097155) === 1) {
      throw new FS.ErrnoError(8);
    }
    if (FS.isDir(stream.node.mode)) {
      throw new FS.ErrnoError(31);
    }
    if (!stream.stream_ops.read) {
      throw new FS.ErrnoError(28);
    }
    var seeking = typeof position != "undefined";
    if (!seeking) {
      position = stream.position;
    } else if (!stream.seekable) {
      throw new FS.ErrnoError(70);
    }
    var bytesRead = stream.stream_ops.read(stream, buffer, offset, length, position);
    if (!seeking) stream.position += bytesRead;
    return bytesRead;
  },
  write(stream, buffer, offset, length, position, canOwn) {
    if (length < 0 || position < 0) {
      throw new FS.ErrnoError(28);
    }
    if (FS.isClosed(stream)) {
      throw new FS.ErrnoError(8);
    }
    if ((stream.flags & 2097155) === 0) {
      throw new FS.ErrnoError(8);
    }
    if (FS.isDir(stream.node.mode)) {
      throw new FS.ErrnoError(31);
    }
    if (!stream.stream_ops.write) {
      throw new FS.ErrnoError(28);
    }
    if (stream.seekable && stream.flags & 1024) {
      // seek to the end before writing in append mode
      FS.llseek(stream, 0, 2);
    }
    var seeking = typeof position != "undefined";
    if (!seeking) {
      position = stream.position;
    } else if (!stream.seekable) {
      throw new FS.ErrnoError(70);
    }
    var bytesWritten = stream.stream_ops.write(stream, buffer, offset, length, position, canOwn);
    if (!seeking) stream.position += bytesWritten;
    return bytesWritten;
  },
  mmap(stream, length, position, prot, flags) {
    // User requests writing to file (prot & PROT_WRITE != 0).
    // Checking if we have permissions to write to the file unless
    // MAP_PRIVATE flag is set. According to POSIX spec it is possible
    // to write to file opened in read-only mode with MAP_PRIVATE flag,
    // as all modifications will be visible only in the memory of
    // the current process.
    if ((prot & 2) !== 0 && (flags & 2) === 0 && (stream.flags & 2097155) !== 2) {
      throw new FS.ErrnoError(2);
    }
    if ((stream.flags & 2097155) === 1) {
      throw new FS.ErrnoError(2);
    }
    if (!stream.stream_ops.mmap) {
      throw new FS.ErrnoError(43);
    }
    if (!length) {
      throw new FS.ErrnoError(28);
    }
    return stream.stream_ops.mmap(stream, length, position, prot, flags);
  },
  msync(stream, buffer, offset, length, mmapFlags) {
    if (!stream.stream_ops.msync) {
      return 0;
    }
    return stream.stream_ops.msync(stream, buffer, offset, length, mmapFlags);
  },
  ioctl(stream, cmd, arg) {
    if (!stream.stream_ops.ioctl) {
      throw new FS.ErrnoError(59);
    }
    return stream.stream_ops.ioctl(stream, cmd, arg);
  },
  readFile(path, opts = {}) {
    opts.flags = opts.flags || 0;
    opts.encoding = opts.encoding || "binary";
    if (opts.encoding !== "utf8" && opts.encoding !== "binary") {
      abort(`Invalid encoding type "${opts.encoding}"`);
    }
    var stream = FS.open(path, opts.flags);
    var stat = FS.stat(path);
    var length = stat.size;
    var buf = new Uint8Array(length);
    FS.read(stream, buf, 0, length, 0);
    if (opts.encoding === "utf8") {
      buf = UTF8ArrayToString(buf);
    }
    FS.close(stream);
    return buf;
  },
  writeFile(path, data, opts = {}) {
    opts.flags = opts.flags || 577;
    var stream = FS.open(path, opts.flags, opts.mode);
    if (typeof data == "string") {
      data = new Uint8Array(intArrayFromString(data, true));
    }
    if (ArrayBuffer.isView(data)) {
      FS.write(stream, data, 0, data.byteLength, undefined, opts.canOwn);
    } else {
      abort("Unsupported data type");
    }
    FS.close(stream);
  },
  cwd: () => FS.currentPath,
  chdir(path) {
    var lookup = FS.lookupPath(path, {
      follow: true
    });
    if (lookup.node === null) {
      throw new FS.ErrnoError(44);
    }
    if (!FS.isDir(lookup.node.mode)) {
      throw new FS.ErrnoError(54);
    }
    var errCode = FS.nodePermissions(lookup.node, "x");
    if (errCode) {
      throw new FS.ErrnoError(errCode);
    }
    FS.currentPath = lookup.path;
  },
  createDefaultDirectories() {
    FS.mkdir("/tmp");
    FS.mkdir("/home");
    FS.mkdir("/home/web_user");
  },
  createDefaultDevices() {
    // create /dev
    FS.mkdir("/dev");
    // setup /dev/null
    FS.registerDevice(FS.makedev(1, 3), {
      read: () => 0,
      write: (stream, buffer, offset, length, pos) => length,
      llseek: () => 0
    });
    FS.mkdev("/dev/null", FS.makedev(1, 3));
    // setup /dev/tty and /dev/tty1
    // stderr needs to print output using err() rather than out()
    // so we register a second tty just for it.
    TTY.register(FS.makedev(5, 0), TTY.default_tty_ops);
    TTY.register(FS.makedev(6, 0), TTY.default_tty1_ops);
    FS.mkdev("/dev/tty", FS.makedev(5, 0));
    FS.mkdev("/dev/tty1", FS.makedev(6, 0));
    // setup /dev/[u]random
    // use a buffer to avoid overhead of individual crypto calls per byte
    var randomBuffer = new Uint8Array(1024), randomLeft = 0;
    var randomByte = () => {
      if (randomLeft === 0) {
        randomFill(randomBuffer);
        randomLeft = randomBuffer.byteLength;
      }
      return randomBuffer[--randomLeft];
    };
    FS.createDevice("/dev", "random", randomByte);
    FS.createDevice("/dev", "urandom", randomByte);
    // we're not going to emulate the actual shm device,
    // just create the tmp dirs that reside in it commonly
    FS.mkdir("/dev/shm");
    FS.mkdir("/dev/shm/tmp");
  },
  createSpecialDirectories() {
    // create /proc/self/fd which allows /proc/self/fd/6 => readlink gives the
    // name of the stream for fd 6 (see test_unistd_ttyname)
    FS.mkdir("/proc");
    var proc_self = FS.mkdir("/proc/self");
    FS.mkdir("/proc/self/fd");
    FS.mount({
      mount() {
        var node = FS.createNode(proc_self, "fd", 16895, 73);
        node.stream_ops = {
          llseek: MEMFS.stream_ops.llseek
        };
        node.node_ops = {
          lookup(parent, name) {
            var fd = +name;
            var stream = FS.getStreamChecked(fd);
            var ret = {
              parent: null,
              mount: {
                mountpoint: "fake"
              },
              node_ops: {
                readlink: () => stream.path
              },
              id: fd + 1
            };
            ret.parent = ret;
            // make it look like a simple root node
            return ret;
          },
          readdir() {
            return Array.from(FS.streams.entries()).filter(([k, v]) => v).map(([k, v]) => k.toString());
          }
        };
        return node;
      }
    }, {}, "/proc/self/fd");
  },
  createStandardStreams(input, output, error) {
    // TODO deprecate the old functionality of a single
    // input / output callback and that utilizes FS.createDevice
    // and instead require a unique set of stream ops
    // by default, we symlink the standard streams to the
    // default tty devices. however, if the standard streams
    // have been overwritten we create a unique device for
    // them instead.
    if (input) {
      FS.createDevice("/dev", "stdin", input);
    } else {
      FS.symlink("/dev/tty", "/dev/stdin");
    }
    if (output) {
      FS.createDevice("/dev", "stdout", null, output);
    } else {
      FS.symlink("/dev/tty", "/dev/stdout");
    }
    if (error) {
      FS.createDevice("/dev", "stderr", null, error);
    } else {
      FS.symlink("/dev/tty1", "/dev/stderr");
    }
    // open default streams for the stdin, stdout and stderr devices
    var stdin = FS.open("/dev/stdin", 0);
    var stdout = FS.open("/dev/stdout", 1);
    var stderr = FS.open("/dev/stderr", 1);
  },
  staticInit() {
    FS.nameTable = new Array(4096);
    FS.mount(MEMFS, {}, "/");
    FS.createDefaultDirectories();
    FS.createDefaultDevices();
    FS.createSpecialDirectories();
    FS.filesystems = {
      "MEMFS": MEMFS
    };
  },
  init(input, output, error) {
    FS.initialized = true;
    // Allow Module.stdin etc. to provide defaults, if none explicitly passed to us here
    input ??= Module["stdin"];
    output ??= Module["stdout"];
    error ??= Module["stderr"];
    FS.createStandardStreams(input, output, error);
  },
  quit() {
    FS.initialized = false;
    // force-flush all streams, so we get musl std streams printed out
    // close all of our streams
    for (var stream of FS.streams) {
      if (stream) {
        FS.close(stream);
      }
    }
  },
  findObject(path, dontResolveLastLink) {
    var ret = FS.analyzePath(path, dontResolveLastLink);
    if (!ret.exists) {
      return null;
    }
    return ret.object;
  },
  analyzePath(path, dontResolveLastLink) {
    // operate from within the context of the symlink's target
    try {
      var lookup = FS.lookupPath(path, {
        follow: !dontResolveLastLink
      });
      path = lookup.path;
    } catch (e) {}
    var ret = {
      isRoot: false,
      exists: false,
      error: 0,
      name: null,
      path: null,
      object: null,
      parentExists: false,
      parentPath: null,
      parentObject: null
    };
    try {
      var lookup = FS.lookupPath(path, {
        parent: true
      });
      ret.parentExists = true;
      ret.parentPath = lookup.path;
      ret.parentObject = lookup.node;
      ret.name = PATH.basename(path);
      lookup = FS.lookupPath(path, {
        follow: !dontResolveLastLink
      });
      ret.exists = true;
      ret.path = lookup.path;
      ret.object = lookup.node;
      ret.name = lookup.node.name;
      ret.isRoot = lookup.path === "/";
    } catch (e) {
      ret.error = e.errno;
    }
    return ret;
  },
  createPath(parent, path, canRead, canWrite) {
    parent = typeof parent == "string" ? parent : FS.getPath(parent);
    var parts = path.split("/").reverse();
    while (parts.length) {
      var part = parts.pop();
      if (!part) continue;
      var current = PATH.join2(parent, part);
      try {
        FS.mkdir(current);
      } catch (e) {
        if (e.errno != 20) throw e;
      }
      parent = current;
    }
    return current;
  },
  createFile(parent, name, properties, canRead, canWrite) {
    var path = PATH.join2(typeof parent == "string" ? parent : FS.getPath(parent), name);
    var mode = FS_getMode(canRead, canWrite);
    return FS.create(path, mode);
  },
  createDataFile(parent, name, data, canRead, canWrite, canOwn) {
    var path = name;
    if (parent) {
      parent = typeof parent == "string" ? parent : FS.getPath(parent);
      path = name ? PATH.join2(parent, name) : parent;
    }
    var mode = FS_getMode(canRead, canWrite);
    var node = FS.create(path, mode);
    if (data) {
      if (typeof data == "string") {
        var arr = new Array(data.length);
        for (var i = 0, len = data.length; i < len; ++i) arr[i] = data.charCodeAt(i);
        data = arr;
      }
      // make sure we can write to the file
      FS.chmod(node, mode | 146);
      var stream = FS.open(node, 577);
      FS.write(stream, data, 0, data.length, 0, canOwn);
      FS.close(stream);
      FS.chmod(node, mode);
    }
  },
  createDevice(parent, name, input, output) {
    var path = PATH.join2(typeof parent == "string" ? parent : FS.getPath(parent), name);
    var mode = FS_getMode(!!input, !!output);
    FS.createDevice.major ??= 64;
    var dev = FS.makedev(FS.createDevice.major++, 0);
    // Create a fake device that a set of stream ops to emulate
    // the old behavior.
    FS.registerDevice(dev, {
      open(stream) {
        stream.seekable = false;
      },
      close(stream) {
        // flush any pending line data
        if (output?.buffer?.length) {
          output(10);
        }
      },
      read(stream, buffer, offset, length, pos) {
        var bytesRead = 0;
        for (var i = 0; i < length; i++) {
          var result;
          try {
            result = input();
          } catch (e) {
            throw new FS.ErrnoError(29);
          }
          if (result === undefined && bytesRead === 0) {
            throw new FS.ErrnoError(6);
          }
          if (result === null || result === undefined) break;
          bytesRead++;
          buffer[offset + i] = result;
        }
        if (bytesRead) {
          stream.node.atime = Date.now();
        }
        return bytesRead;
      },
      write(stream, buffer, offset, length, pos) {
        for (var i = 0; i < length; i++) {
          try {
            output(buffer[offset + i]);
          } catch (e) {
            throw new FS.ErrnoError(29);
          }
        }
        if (length) {
          stream.node.mtime = stream.node.ctime = Date.now();
        }
        return i;
      }
    });
    return FS.mkdev(path, mode, dev);
  },
  forceLoadFile(obj) {
    if (obj.isDevice || obj.isFolder || obj.link || obj.contents) return true;
    if (globalThis.XMLHttpRequest) {
      abort("Lazy loading should have been performed (contents set) in createLazyFile, but it was not. Lazy loading only works in web workers. Use --embed-file or --preload-file in emcc on the main thread.");
    } else {
      // Command-line.
      try {
        obj.contents = readBinary(obj.url);
      } catch (e) {
        throw new FS.ErrnoError(29);
      }
    }
  },
  createLazyFile(parent, name, url, canRead, canWrite) {
    // Lazy chunked Uint8Array (implements get and length from Uint8Array).
    // Actual getting is abstracted away for eventual reuse.
    class LazyUint8Array {
      lengthKnown=false;
      chunks=[];
      // Loaded chunks. Index is the chunk number
      get(idx) {
        if (idx > this.length - 1 || idx < 0) {
          return undefined;
        }
        var chunkOffset = idx % this.chunkSize;
        var chunkNum = (idx / this.chunkSize) | 0;
        return this.getter(chunkNum)[chunkOffset];
      }
      setDataGetter(getter) {
        this.getter = getter;
      }
      cacheLength() {
        // Find length
        var xhr = new XMLHttpRequest;
        xhr.open("HEAD", url, false);
        xhr.send(null);
        if (!(xhr.status >= 200 && xhr.status < 300 || xhr.status === 304)) abort("Couldn't load " + url + ". Status: " + xhr.status);
        var datalength = Number(xhr.getResponseHeader("Content-length"));
        var header;
        var hasByteServing = (header = xhr.getResponseHeader("Accept-Ranges")) && header === "bytes";
        var usesGzip = (header = xhr.getResponseHeader("Content-Encoding")) && header === "gzip";
        var chunkSize = 1024 * 1024;
        // Chunk size in bytes
        if (!hasByteServing) chunkSize = datalength;
        // Function to get a range from the remote URL.
        var doXHR = (from, to) => {
          if (from > to) abort("invalid range (" + from + ", " + to + ") or no bytes requested!");
          if (to > datalength - 1) abort("only " + datalength + " bytes available! programmer error!");
          // TODO: Use mozResponseArrayBuffer, responseStream, etc. if available.
          var xhr = new XMLHttpRequest;
          xhr.open("GET", url, false);
          if (datalength !== chunkSize) xhr.setRequestHeader("Range", "bytes=" + from + "-" + to);
          // Some hints to the browser that we want binary data.
          xhr.responseType = "arraybuffer";
          if (xhr.overrideMimeType) {
            xhr.overrideMimeType("text/plain; charset=x-user-defined");
          }
          xhr.send(null);
          if (!(xhr.status >= 200 && xhr.status < 300 || xhr.status === 304)) abort("Couldn't load " + url + ". Status: " + xhr.status);
          if (xhr.response !== undefined) {
            return new Uint8Array(/** @type{Array<number>} */ (xhr.response || []));
          }
          return intArrayFromString(xhr.responseText || "", true);
        };
        var lazyArray = this;
        lazyArray.setDataGetter(chunkNum => {
          var start = chunkNum * chunkSize;
          var end = (chunkNum + 1) * chunkSize - 1;
          // including this byte
          end = Math.min(end, datalength - 1);
          // if datalength-1 is selected, this is the last block
          if (typeof lazyArray.chunks[chunkNum] == "undefined") {
            lazyArray.chunks[chunkNum] = doXHR(start, end);
          }
          if (typeof lazyArray.chunks[chunkNum] == "undefined") abort("doXHR failed!");
          return lazyArray.chunks[chunkNum];
        });
        if (usesGzip || !datalength) {
          // if the server uses gzip or doesn't supply the length, we have to download the whole file to get the (uncompressed) length
          chunkSize = datalength = 1;
          // this will force getter(0)/doXHR do download the whole file
          datalength = this.getter(0).length;
          chunkSize = datalength;
          out("LazyFiles on gzip forces download of the whole file when length is accessed");
        }
        this._length = datalength;
        this._chunkSize = chunkSize;
        this.lengthKnown = true;
      }
      get length() {
        if (!this.lengthKnown) {
          this.cacheLength();
        }
        return this._length;
      }
      get chunkSize() {
        if (!this.lengthKnown) {
          this.cacheLength();
        }
        return this._chunkSize;
      }
    }
    if (globalThis.XMLHttpRequest) {
      if (!ENVIRONMENT_IS_WORKER) abort("Cannot do synchronous binary XHRs outside webworkers in modern browsers. Use --embed-file or --preload-file in emcc");
      var lazyArray = new LazyUint8Array;
      var properties = {
        isDevice: false,
        contents: lazyArray
      };
    } else {
      var properties = {
        isDevice: false,
        url
      };
    }
    var node = FS.createFile(parent, name, properties, canRead, canWrite);
    // This is a total hack, but I want to get this lazy file code out of the
    // core of MEMFS. If we want to keep this lazy file concept I feel it should
    // be its own thin LAZYFS proxying calls to MEMFS.
    if (properties.contents) {
      node.contents = properties.contents;
    } else if (properties.url) {
      node.contents = null;
      node.url = properties.url;
    }
    // Add a function that defers querying the file size until it is asked the first time.
    Object.defineProperties(node, {
      usedBytes: {
        get: function() {
          return this.contents.length;
        }
      }
    });
    // override each stream op with one that tries to force load the lazy file first
    var stream_ops = {};
    for (const [key, fn] of Object.entries(node.stream_ops)) {
      stream_ops[key] = (...args) => {
        FS.forceLoadFile(node);
        return fn(...args);
      };
    }
    function writeChunks(stream, buffer, offset, length, position) {
      var contents = stream.node.contents;
      if (position >= contents.length) return 0;
      var size = Math.min(contents.length - position, length);
      if (contents.slice) {
        // normal array
        for (var i = 0; i < size; i++) {
          buffer[offset + i] = contents[position + i];
        }
      } else {
        for (var i = 0; i < size; i++) {
          // LazyUint8Array from sync binary XHR
          buffer[offset + i] = contents.get(position + i);
        }
      }
      return size;
    }
    // use a custom read function
    stream_ops.read = (stream, buffer, offset, length, position) => {
      FS.forceLoadFile(node);
      return writeChunks(stream, buffer, offset, length, position);
    };
    // use a custom mmap function
    stream_ops.mmap = (stream, length, position, prot, flags) => {
      FS.forceLoadFile(node);
      var ptr = mmapAlloc(length);
      if (!ptr) {
        throw new FS.ErrnoError(48);
      }
      writeChunks(stream, (growMemViews(), HEAP8), ptr, length, position);
      return {
        ptr,
        allocated: true
      };
    };
    node.stream_ops = stream_ops;
    return node;
  }
};

/**
   * Given a pointer 'ptr' to a null-terminated UTF8-encoded string in the
   * emscripten HEAP, returns a copy of that string as a Javascript String object.
   *
   * @param {number} ptr
   * @param {number=} maxBytesToRead - An optional length that specifies the
   *   maximum number of bytes to read. You can omit this parameter to scan the
   *   string until the first 0 byte. If maxBytesToRead is passed, and the string
   *   at [ptr, ptr+maxBytesToReadr[ contains a null byte in the middle, then the
   *   string will cut short at that byte index.
   * @param {boolean=} ignoreNul - If true, the function will not stop on a NUL character.
   * @return {string}
   */ var UTF8ToString = (ptr, maxBytesToRead, ignoreNul) => ptr ? UTF8ArrayToString((growMemViews(), 
HEAPU8), ptr, maxBytesToRead, ignoreNul) : "";

var SYSCALLS = {
  calculateAt(dirfd, path, allowEmpty) {
    if (PATH.isAbs(path)) {
      return path;
    }
    // relative path
    var dir;
    if (dirfd === -100) {
      dir = FS.cwd();
    } else {
      var dirstream = SYSCALLS.getStreamFromFD(dirfd);
      dir = dirstream.path;
    }
    if (path.length == 0) {
      if (!allowEmpty) {
        throw new FS.ErrnoError(44);
      }
      return dir;
    }
    return dir + "/" + path;
  },
  writeStat(buf, stat) {
    (growMemViews(), HEAPU32)[((buf) / 4)] = stat.dev;
    (growMemViews(), HEAPU32)[(((buf) + (4)) / 4)] = stat.mode;
    (growMemViews(), HEAPU64)[(((buf) + (8)) / 8)] = BigInt(stat.nlink);
    (growMemViews(), HEAPU32)[(((buf) + (16)) / 4)] = stat.uid;
    (growMemViews(), HEAPU32)[(((buf) + (20)) / 4)] = stat.gid;
    (growMemViews(), HEAPU32)[(((buf) + (24)) / 4)] = stat.rdev;
    (growMemViews(), HEAP64)[(((buf) + (32)) / 8)] = BigInt(stat.size);
    (growMemViews(), HEAP32)[(((buf) + (40)) / 4)] = 4096;
    (growMemViews(), HEAP32)[(((buf) + (44)) / 4)] = stat.blocks;
    var atime = stat.atime.getTime();
    var mtime = stat.mtime.getTime();
    var ctime = stat.ctime.getTime();
    (growMemViews(), HEAP64)[(((buf) + (48)) / 8)] = BigInt(Math.floor(atime / 1e3));
    (growMemViews(), HEAPU64)[(((buf) + (56)) / 8)] = BigInt((atime % 1e3) * 1e3 * 1e3);
    (growMemViews(), HEAP64)[(((buf) + (64)) / 8)] = BigInt(Math.floor(mtime / 1e3));
    (growMemViews(), HEAPU64)[(((buf) + (72)) / 8)] = BigInt((mtime % 1e3) * 1e3 * 1e3);
    (growMemViews(), HEAP64)[(((buf) + (80)) / 8)] = BigInt(Math.floor(ctime / 1e3));
    (growMemViews(), HEAPU64)[(((buf) + (88)) / 8)] = BigInt((ctime % 1e3) * 1e3 * 1e3);
    (growMemViews(), HEAP64)[(((buf) + (96)) / 8)] = BigInt(stat.ino);
    return 0;
  },
  writeStatFs(buf, stats) {
    (growMemViews(), HEAPU32)[(((buf) + (8)) / 4)] = stats.bsize;
    (growMemViews(), HEAPU32)[(((buf) + (72)) / 4)] = stats.bsize;
    (growMemViews(), HEAP64)[(((buf) + (16)) / 8)] = BigInt(stats.blocks);
    (growMemViews(), HEAP64)[(((buf) + (24)) / 8)] = BigInt(stats.bfree);
    (growMemViews(), HEAP64)[(((buf) + (32)) / 8)] = BigInt(stats.bavail);
    (growMemViews(), HEAP64)[(((buf) + (40)) / 8)] = BigInt(stats.files);
    (growMemViews(), HEAP64)[(((buf) + (48)) / 8)] = BigInt(stats.ffree);
    (growMemViews(), HEAPU32)[(((buf) + (56)) / 4)] = stats.fsid;
    (growMemViews(), HEAPU32)[(((buf) + (80)) / 4)] = stats.flags;
    // ST_NOSUID
    (growMemViews(), HEAPU32)[(((buf) + (64)) / 4)] = stats.namelen;
  },
  doMsync(addr, stream, len, flags, offset) {
    if (!FS.isFile(stream.node.mode)) {
      throw new FS.ErrnoError(43);
    }
    if (flags & 2) {
      // MAP_PRIVATE calls need not to be synced back to underlying fs
      return 0;
    }
    var buffer = (growMemViews(), HEAPU8).slice(addr, addr + len);
    FS.msync(stream, buffer, offset, len, flags);
  },
  getStreamFromFD(fd) {
    var stream = FS.getStreamChecked(fd);
    return stream;
  },
  varargs: undefined,
  getStr(ptr) {
    var ret = UTF8ToString(ptr);
    return ret;
  }
};

function ___syscall_chmod(path, mode) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(3, 0, 1, path, mode);
  path = bigintToI53Checked(path);
  try {
    path = SYSCALLS.getStr(path);
    FS.chmod(path, mode);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

var SOCKFS = {
  websocketArgs: {},
  callbacks: {},
  on(event, callback) {
    SOCKFS.callbacks[event] = callback;
  },
  emit(event, param) {
    SOCKFS.callbacks[event]?.(param);
  },
  mount(mount) {
    // The incoming Module['websocket'] can be used for configuring 
    // subprotocol/url, etc
    SOCKFS.websocketArgs = Module["websocket"] || {};
    // Add the Event registration mechanism to the exported websocket configuration
    // object so we can register network callbacks from native JavaScript too.
    // For more documentation see system/include/emscripten/emscripten.h
    (Module["websocket"] ??= {})["on"] = SOCKFS.on;
    return FS.createNode(null, "/", 16895, 0);
  },
  createSocket(family, type, protocol) {
    // Emscripten only supports AF_INET
    if (family != 2) {
      throw new FS.ErrnoError(5);
    }
    type &= ~526336;
    // Some applications may pass it; it makes no sense for a single process.
    // Emscripten only supports SOCK_STREAM and SOCK_DGRAM
    if (type != 1 && type != 2) {
      throw new FS.ErrnoError(28);
    }
    var streaming = type == 1;
    if (streaming && protocol && protocol != 6) {
      throw new FS.ErrnoError(66);
    }
    // create our internal socket structure
    var sock = {
      family,
      type,
      protocol,
      server: null,
      error: null,
      // Used in getsockopt for SOL_SOCKET/SO_ERROR test
      peers: {},
      pending: [],
      recv_queue: [],
      sock_ops: SOCKFS.websocket_sock_ops
    };
    // create the filesystem node to store the socket structure
    var name = SOCKFS.nextname();
    var node = FS.createNode(SOCKFS.root, name, 49152, 0);
    node.sock = sock;
    // and the wrapping stream that enables library functions such
    // as read and write to indirectly interact with the socket
    var stream = FS.createStream({
      path: name,
      node,
      flags: 2,
      seekable: false,
      stream_ops: SOCKFS.stream_ops
    });
    // map the new stream to the socket structure (sockets have a 1:1
    // relationship with a stream)
    sock.stream = stream;
    return sock;
  },
  getSocket(fd) {
    var stream = FS.getStream(fd);
    if (!stream || !FS.isSocket(stream.node.mode)) {
      return null;
    }
    return stream.node.sock;
  },
  stream_ops: {
    poll(stream) {
      var sock = stream.node.sock;
      return sock.sock_ops.poll(sock);
    },
    ioctl(stream, request, varargs) {
      var sock = stream.node.sock;
      return sock.sock_ops.ioctl(sock, request, varargs);
    },
    read(stream, buffer, offset, length, position) {
      var sock = stream.node.sock;
      var msg = sock.sock_ops.recvmsg(sock, length);
      if (!msg) {
        // socket is closed
        return 0;
      }
      buffer.set(msg.buffer, offset);
      return msg.buffer.length;
    },
    write(stream, buffer, offset, length, position) {
      var sock = stream.node.sock;
      return sock.sock_ops.sendmsg(sock, buffer, offset, length);
    },
    close(stream) {
      var sock = stream.node.sock;
      sock.sock_ops.close(sock);
    }
  },
  nextname() {
    if (!SOCKFS.nextname.current) {
      SOCKFS.nextname.current = 0;
    }
    return `socket[${SOCKFS.nextname.current++}]`;
  },
  websocket_sock_ops: {
    createPeer(sock, addr, port) {
      var ws;
      if (typeof addr == "object") {
        ws = addr;
        addr = null;
        port = null;
      }
      if (ws) {
        // for sockets that've already connected (e.g. we're the server)
        // we can inspect the _socket property for the address
        if (ws._socket) {
          addr = ws._socket.remoteAddress;
          port = ws._socket.remotePort;
        } else {
          var result = /ws[s]?:\/\/([^:]+):(\d+)/.exec(ws.url);
          if (!result) {
            throw new Error("WebSocket URL must be in the format ws(s)://address:port");
          }
          addr = result[1];
          port = parseInt(result[2], 10);
        }
      } else {
        // create the actual websocket object and connect
        try {
          // The default value is 'ws://' the replace is needed because the compiler replaces '//' comments with '#'
          // comments without checking context, so we'd end up with ws:#, the replace swaps the '#' for '//' again.
          var url = "ws://".replace("#", "//");
          // Make the WebSocket subprotocol (Sec-WebSocket-Protocol) default to binary if no configuration is set.
          var subProtocols = "binary";
          // The default value is 'binary'
          // The default WebSocket options
          var opts = undefined;
          // Fetch runtime WebSocket URL config.
          if (SOCKFS.websocketArgs["url"]) {
            url = SOCKFS.websocketArgs["url"];
          }
          // Fetch runtime WebSocket subprotocol config.
          if (SOCKFS.websocketArgs["subprotocol"]) {
            subProtocols = SOCKFS.websocketArgs["subprotocol"];
          } else if (SOCKFS.websocketArgs["subprotocol"] === null) {
            subProtocols = "null";
          }
          if (url === "ws://" || url === "wss://") {
            // Is the supplied URL config just a prefix, if so complete it.
            var parts = addr.split("/");
            url = url + parts[0] + ":" + port + "/" + parts.slice(1).join("/");
          }
          if (subProtocols !== "null") {
            // The regex trims the string (removes spaces at the beginning and end), then splits the string by
            // <any space>,<any space> into an Array. Whitespace removal is important for Websockify and ws.
            subProtocols = subProtocols.replace(/^ +| +$/g, "").split(/ *, */);
            opts = subProtocols;
          }
          // If node we use the ws library.
          var WebSocketConstructor;
          {
            WebSocketConstructor = WebSocket;
          }
          ws = new WebSocketConstructor(url, opts);
          ws.binaryType = "arraybuffer";
        } catch (e) {
          throw new FS.ErrnoError(23);
        }
      }
      var peer = {
        addr,
        port,
        socket: ws,
        msg_send_queue: []
      };
      SOCKFS.websocket_sock_ops.addPeer(sock, peer);
      SOCKFS.websocket_sock_ops.handlePeerEvents(sock, peer);
      // if this is a bound dgram socket, send the port number first to allow
      // us to override the ephemeral port reported to us by remotePort on the
      // remote end.
      if (sock.type === 2 && typeof sock.sport != "undefined") {
        peer.msg_send_queue.push(new Uint8Array([ 255, 255, 255, 255, "p".charCodeAt(0), "o".charCodeAt(0), "r".charCodeAt(0), "t".charCodeAt(0), ((sock.sport & 65280) >> 8), (sock.sport & 255) ]));
      }
      return peer;
    },
    getPeer(sock, addr, port) {
      return sock.peers[addr + ":" + port];
    },
    addPeer(sock, peer) {
      sock.peers[peer.addr + ":" + peer.port] = peer;
    },
    removePeer(sock, peer) {
      delete sock.peers[peer.addr + ":" + peer.port];
    },
    handlePeerEvents(sock, peer) {
      var first = true;
      var handleOpen = function() {
        sock.connecting = false;
        SOCKFS.emit("open", sock.stream.fd);
        try {
          var queued = peer.msg_send_queue.shift();
          while (queued) {
            peer.socket.send(queued);
            queued = peer.msg_send_queue.shift();
          }
        } catch (e) {
          // not much we can do here in the way of proper error handling as we've already
          // lied and said this data was sent. shut it down.
          peer.socket.close();
        }
      };
      function handleMessage(data) {
        if (typeof data == "string") {
          var encoder = new TextEncoder;
          // should be utf-8
          data = encoder.encode(data);
        } else {
          if (data.byteLength == 0) {
            // An empty ArrayBuffer will emit a pseudo disconnect event
            // as recv/recvmsg will return zero which indicates that a socket
            // has performed a shutdown although the connection has not been disconnected yet.
            return;
          }
          data = new Uint8Array(data);
        }
        // if this is the port message, override the peer's port with it
        var wasfirst = first;
        first = false;
        if (wasfirst && data.length === 10 && data[0] === 255 && data[1] === 255 && data[2] === 255 && data[3] === 255 && data[4] === "p".charCodeAt(0) && data[5] === "o".charCodeAt(0) && data[6] === "r".charCodeAt(0) && data[7] === "t".charCodeAt(0)) {
          // update the peer's port and its key in the peer map
          var newport = ((data[8] << 8) | data[9]);
          SOCKFS.websocket_sock_ops.removePeer(sock, peer);
          peer.port = newport;
          SOCKFS.websocket_sock_ops.addPeer(sock, peer);
          return;
        }
        sock.recv_queue.push({
          addr: peer.addr,
          port: peer.port,
          data
        });
        SOCKFS.emit("message", sock.stream.fd);
      }
      if (ENVIRONMENT_IS_NODE) {
        peer.socket.on("open", handleOpen);
        peer.socket.on("message", function(data, isBinary) {
          if (!isBinary) {
            return;
          }
          handleMessage((new Uint8Array(data)).buffer);
        });
        peer.socket.on("close", function() {
          SOCKFS.emit("close", sock.stream.fd);
        });
        peer.socket.on("error", function(error) {
          // Although the ws library may pass errors that may be more descriptive than
          // ECONNREFUSED they are not necessarily the expected error code e.g.
          // ENOTFOUND on getaddrinfo seems to be node.js specific, so using ECONNREFUSED
          // is still probably the most useful thing to do.
          sock.error = 14;
          // Used in getsockopt for SOL_SOCKET/SO_ERROR test.
          SOCKFS.emit("error", [ sock.stream.fd, sock.error, "ECONNREFUSED: Connection refused" ]);
        });
      } else {
        peer.socket.onopen = handleOpen;
        peer.socket.onclose = function() {
          SOCKFS.emit("close", sock.stream.fd);
        };
        peer.socket.onmessage = function peer_socket_onmessage(event) {
          handleMessage(event.data);
        };
        peer.socket.onerror = function(error) {
          // The WebSocket spec only allows a 'simple event' to be thrown on error,
          // so we only really know as much as ECONNREFUSED.
          sock.error = 14;
          // Used in getsockopt for SOL_SOCKET/SO_ERROR test.
          SOCKFS.emit("error", [ sock.stream.fd, sock.error, "ECONNREFUSED: Connection refused" ]);
        };
      }
    },
    poll(sock) {
      if (sock.type === 1 && sock.server) {
        // listen sockets should only say they're available for reading
        // if there are pending clients.
        return sock.pending.length ? (64 | 1) : 0;
      }
      var mask = 0;
      var dest = sock.type === 1 ? // we only care about the socket state for connection-based sockets
      SOCKFS.websocket_sock_ops.getPeer(sock, sock.daddr, sock.dport) : null;
      if (sock.recv_queue.length || !dest || // connection-less sockets are always ready to read
      (dest && dest.socket.readyState === dest.socket.CLOSING) || (dest && dest.socket.readyState === dest.socket.CLOSED)) {
        // let recv return 0 once closed
        mask |= (64 | 1);
      }
      if (!dest || // connection-less sockets are always ready to write
      (dest && dest.socket.readyState === dest.socket.OPEN)) {
        mask |= 4;
      }
      if ((dest && dest.socket.readyState === dest.socket.CLOSING) || (dest && dest.socket.readyState === dest.socket.CLOSED)) {
        // When an non-blocking connect fails mark the socket as writable.
        // Its up to the calling code to then use getsockopt with SO_ERROR to
        // retrieve the error.
        // See https://man7.org/linux/man-pages/man2/connect.2.html
        if (sock.connecting) {
          mask |= 4;
        } else {
          mask |= 16;
        }
      }
      return mask;
    },
    ioctl(sock, request, arg) {
      switch (request) {
       case 21531:
        var bytes = 0;
        if (sock.recv_queue.length) {
          bytes = sock.recv_queue[0].data.length;
        }
        (growMemViews(), HEAP32)[((arg) / 4)] = bytes;
        return 0;

       case 21537:
        var on = (growMemViews(), HEAP32)[((arg) / 4)];
        if (on) {
          sock.stream.flags |= 2048;
        } else {
          sock.stream.flags &= ~2048;
        }
        return 0;

       default:
        return 28;
      }
    },
    close(sock) {
      // if we've spawned a listen server, close it
      if (sock.server) {
        try {
          sock.server.close();
        } catch (e) {}
        sock.server = null;
      }
      // close any peer connections
      for (var peer of Object.values(sock.peers)) {
        try {
          peer.socket.close();
        } catch (e) {}
        SOCKFS.websocket_sock_ops.removePeer(sock, peer);
      }
      return 0;
    },
    bind(sock, addr, port) {
      if (typeof sock.saddr != "undefined" || typeof sock.sport != "undefined") {
        throw new FS.ErrnoError(28);
      }
      sock.saddr = addr;
      sock.sport = port;
      // in order to emulate dgram sockets, we need to launch a listen server when
      // binding on a connection-less socket
      // note: this is only required on the server side
      if (sock.type === 2) {
        // close the existing server if it exists
        if (sock.server) {
          sock.server.close();
          sock.server = null;
        }
        // swallow error operation not supported error that occurs when binding in the
        // browser where this isn't supported
        try {
          sock.sock_ops.listen(sock, 0);
        } catch (e) {
          if (!(e.name === "ErrnoError")) throw e;
          if (e.errno !== 138) throw e;
        }
      }
    },
    connect(sock, addr, port) {
      if (sock.server) {
        throw new FS.ErrnoError(138);
      }
      // TODO autobind
      // if (!sock.addr && sock.type == 2) {
      // }
      // early out if we're already connected / in the middle of connecting
      if (typeof sock.daddr != "undefined" && typeof sock.dport != "undefined") {
        var dest = SOCKFS.websocket_sock_ops.getPeer(sock, sock.daddr, sock.dport);
        if (dest) {
          if (dest.socket.readyState === dest.socket.CONNECTING) {
            throw new FS.ErrnoError(7);
          } else {
            throw new FS.ErrnoError(30);
          }
        }
      }
      // add the socket to our peer list and set our
      // destination address / port to match
      var peer = SOCKFS.websocket_sock_ops.createPeer(sock, addr, port);
      sock.daddr = peer.addr;
      sock.dport = peer.port;
      // because we cannot synchronously block to wait for the WebSocket
      // connection to complete, we return here pretending that the connection
      // was a success.
      sock.connecting = true;
    },
    listen(sock, backlog) {
      if (!ENVIRONMENT_IS_NODE) {
        throw new FS.ErrnoError(138);
      }
    },
    accept(listensock) {
      if (!listensock.server || !listensock.pending.length) {
        throw new FS.ErrnoError(28);
      }
      var newsock = listensock.pending.shift();
      newsock.stream.flags = listensock.stream.flags;
      return newsock;
    },
    getname(sock, peer) {
      var addr, port;
      if (peer) {
        if (sock.daddr === undefined || sock.dport === undefined) {
          throw new FS.ErrnoError(53);
        }
        addr = sock.daddr;
        port = sock.dport;
      } else {
        // TODO saddr and sport will be set for bind()'d UDP sockets, but what
        // should we be returning for TCP sockets that've been connect()'d?
        addr = sock.saddr || 0;
        port = sock.sport || 0;
      }
      return {
        addr,
        port
      };
    },
    sendmsg(sock, buffer, offset, length, addr, port) {
      if (sock.type === 2) {
        // connection-less sockets will honor the message address,
        // and otherwise fall back to the bound destination address
        if (addr === undefined || port === undefined) {
          addr = sock.daddr;
          port = sock.dport;
        }
        // if there was no address to fall back to, error out
        if (addr === undefined || port === undefined) {
          throw new FS.ErrnoError(17);
        }
      } else {
        // connection-based sockets will only use the bound
        addr = sock.daddr;
        port = sock.dport;
      }
      // find the peer for the destination address
      var dest = SOCKFS.websocket_sock_ops.getPeer(sock, addr, port);
      // early out if not connected with a connection-based socket
      if (sock.type === 1) {
        if (!dest || dest.socket.readyState === dest.socket.CLOSING || dest.socket.readyState === dest.socket.CLOSED) {
          throw new FS.ErrnoError(53);
        }
      }
      // create a copy of the incoming data to send, as the WebSocket API
      // doesn't work entirely with an ArrayBufferView, it'll just send
      // the entire underlying buffer
      if (ArrayBuffer.isView(buffer)) {
        offset += buffer.byteOffset;
        buffer = buffer.buffer;
      }
      var data = buffer.slice(offset, offset + length);
      // WebSockets .send() does not allow passing a SharedArrayBuffer, so
      // clone the SharedArrayBuffer as regular ArrayBuffer before
      // sending.
      if (data instanceof SharedArrayBuffer) {
        data = new Uint8Array(new Uint8Array(data)).buffer;
      }
      // if we don't have a cached connectionless UDP datagram connection, or
      // the TCP socket is still connecting, queue the message to be sent upon
      // connect, and lie, saying the data was sent now.
      if (!dest || dest.socket.readyState !== dest.socket.OPEN) {
        // if we're not connected, open a new connection
        if (sock.type === 2) {
          if (!dest || dest.socket.readyState === dest.socket.CLOSING || dest.socket.readyState === dest.socket.CLOSED) {
            dest = SOCKFS.websocket_sock_ops.createPeer(sock, addr, port);
          }
        }
        dest.msg_send_queue.push(data);
        return length;
      }
      try {
        // send the actual data
        dest.socket.send(data);
        return length;
      } catch (e) {
        throw new FS.ErrnoError(28);
      }
    },
    recvmsg(sock, length) {
      // http://pubs.opengroup.org/onlinepubs/7908799/xns/recvmsg.html
      if (sock.type === 1 && sock.server) {
        // tcp servers should not be recv()'ing on the listen socket
        throw new FS.ErrnoError(53);
      }
      var queued = sock.recv_queue.shift();
      if (!queued) {
        if (sock.type === 1) {
          var dest = SOCKFS.websocket_sock_ops.getPeer(sock, sock.daddr, sock.dport);
          if (!dest) {
            // if we have a destination address but are not connected, error out
            throw new FS.ErrnoError(53);
          }
          if (dest.socket.readyState === dest.socket.CLOSING || dest.socket.readyState === dest.socket.CLOSED) {
            // return null if the socket has closed
            return null;
          }
          // else, our socket is in a valid state but truly has nothing available
          throw new FS.ErrnoError(6);
        }
        throw new FS.ErrnoError(6);
      }
      // queued.data will be an ArrayBuffer if it's unadulterated, but if it's
      // requeued TCP data it'll be an ArrayBufferView
      var queuedLength = queued.data.byteLength || queued.data.length;
      var queuedOffset = queued.data.byteOffset || 0;
      var queuedBuffer = queued.data.buffer || queued.data;
      var bytesRead = Math.min(length, queuedLength);
      var res = {
        buffer: new Uint8Array(queuedBuffer, queuedOffset, bytesRead),
        addr: queued.addr,
        port: queued.port
      };
      // push back any unread data for TCP connections
      if (sock.type === 1 && bytesRead < queuedLength) {
        var bytesRemaining = queuedLength - bytesRead;
        queued.data = new Uint8Array(queuedBuffer, queuedOffset + bytesRead, bytesRemaining);
        sock.recv_queue.unshift(queued);
      }
      return res;
    }
  }
};

var getSocketFromFD = fd => {
  var socket = SOCKFS.getSocket(fd);
  if (!socket) throw new FS.ErrnoError(8);
  return socket;
};

var inetNtop4 = addr => (addr & 255) + "." + ((addr >> 8) & 255) + "." + ((addr >> 16) & 255) + "." + ((addr >> 24) & 255);

var inetNtop6 = ints => {
  //  ref:  http://www.ietf.org/rfc/rfc2373.txt - section 2.5.4
  //  Format for IPv4 compatible and mapped  128-bit IPv6 Addresses
  //  128-bits are split into eight 16-bit words
  //  stored in network byte order (big-endian)
  //  |                80 bits               | 16 |      32 bits        |
  //  +-----------------------------------------------------------------+
  //  |               10 bytes               |  2 |      4 bytes        |
  //  +--------------------------------------+--------------------------+
  //  +               5 words                |  1 |      2 words        |
  //  +--------------------------------------+--------------------------+
  //  |0000..............................0000|0000|    IPv4 ADDRESS     | (compatible)
  //  +--------------------------------------+----+---------------------+
  //  |0000..............................0000|FFFF|    IPv4 ADDRESS     | (mapped)
  //  +--------------------------------------+----+---------------------+
  var str = "";
  var word = 0;
  var longest = 0;
  var lastzero = 0;
  var zstart = 0;
  var len = 0;
  var i = 0;
  var parts = [ ints[0] & 65535, (ints[0] >> 16), ints[1] & 65535, (ints[1] >> 16), ints[2] & 65535, (ints[2] >> 16), ints[3] & 65535, (ints[3] >> 16) ];
  // Handle IPv4-compatible, IPv4-mapped, loopback and any/unspecified addresses
  var hasipv4 = true;
  var v4part = "";
  // check if the 10 high-order bytes are all zeros (first 5 words)
  for (i = 0; i < 5; i++) {
    if (parts[i] !== 0) {
      hasipv4 = false;
      break;
    }
  }
  if (hasipv4) {
    // low-order 32-bits store an IPv4 address (bytes 13 to 16) (last 2 words)
    v4part = inetNtop4(parts[6] | (parts[7] << 16));
    // IPv4-mapped IPv6 address if 16-bit value (bytes 11 and 12) == 0xFFFF (6th word)
    if (parts[5] === -1) {
      str = "::ffff:";
      str += v4part;
      return str;
    }
    // IPv4-compatible IPv6 address if 16-bit value (bytes 11 and 12) == 0x0000 (6th word)
    if (parts[5] === 0) {
      str = "::";
      // special case IPv6 addresses
      if (v4part === "0.0.0.0") v4part = "";
      // any/unspecified address
      if (v4part === "0.0.0.1") v4part = "1";
      // loopback address
      str += v4part;
      return str;
    }
  }
  // Handle all other IPv6 addresses
  // first run to find the longest contiguous zero words
  for (word = 0; word < 8; word++) {
    if (parts[word] === 0) {
      if (word - lastzero > 1) {
        len = 0;
      }
      lastzero = word;
      len++;
    }
    if (len > longest) {
      longest = len;
      zstart = word - longest + 1;
    }
  }
  for (word = 0; word < 8; word++) {
    if (longest > 1) {
      // compress contiguous zeros - to produce "::"
      if (parts[word] === 0 && word >= zstart && word < (zstart + longest)) {
        if (word === zstart) {
          str += ":";
          if (zstart === 0) str += ":";
        }
        continue;
      }
    }
    // converts 16-bit words from big-endian to little-endian before converting to hex string
    str += Number(_ntohs(parts[word] & 65535)).toString(16);
    str += word < 7 ? ":" : "";
  }
  return str;
};

var readSockaddr = (sa, salen) => {
  // family / port offsets are common to both sockaddr_in and sockaddr_in6
  var family = (growMemViews(), HEAP16)[((sa) / 2)];
  var port = _ntohs((growMemViews(), HEAPU16)[(((sa) + (2)) / 2)]);
  var addr;
  switch (family) {
   case 2:
    if (salen !== 16) {
      return {
        errno: 28
      };
    }
    addr = (growMemViews(), HEAP32)[(((sa) + (4)) / 4)];
    addr = inetNtop4(addr);
    break;

   case 10:
    if (salen !== 28) {
      return {
        errno: 28
      };
    }
    addr = [ (growMemViews(), HEAP32)[(((sa) + (8)) / 4)], (growMemViews(), HEAP32)[(((sa) + (12)) / 4)], (growMemViews(), 
    HEAP32)[(((sa) + (16)) / 4)], (growMemViews(), HEAP32)[(((sa) + (20)) / 4)] ];
    addr = inetNtop6(addr);
    break;

   default:
    return {
      errno: 5
    };
  }
  return {
    family,
    addr,
    port
  };
};

var inetPton4 = str => {
  var b = str.split(".");
  for (var i = 0; i < 4; i++) {
    var tmp = Number(b[i]);
    if (isNaN(tmp)) return null;
    b[i] = tmp;
  }
  return (b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)) >>> 0;
};

var inetPton6 = str => {
  var words;
  var w, offset, z, i;
  /* http://home.deds.nl/~aeron/regex/ */ var valid6regx = /^((?=.*::)(?!.*::.+::)(::)?([\dA-F]{1,4}:(:|\b)|){5}|([\dA-F]{1,4}:){6})((([\dA-F]{1,4}((?!\3)::|:\b|$))|(?!\2\3)){2}|(((2[0-4]|1\d|[1-9])?\d|25[0-5])\.?\b){4})$/i;
  var parts = [];
  if (!valid6regx.test(str)) {
    return null;
  }
  if (str === "::") {
    return [ 0, 0, 0, 0, 0, 0, 0, 0 ];
  }
  // Z placeholder to keep track of zeros when splitting the string on ":"
  if (str.startsWith("::")) {
    str = str.replace("::", "Z:");
  } else {
    str = str.replace("::", ":Z:");
  }
  if (str.indexOf(".") > 0) {
    // parse IPv4 embedded address
    str = str.replace(new RegExp("[.]", "g"), ":");
    words = str.split(":");
    words[words.length - 4] = Number(words[words.length - 4]) + Number(words[words.length - 3]) * 256;
    words[words.length - 3] = Number(words[words.length - 2]) + Number(words[words.length - 1]) * 256;
    words = words.slice(0, words.length - 2);
  } else {
    words = str.split(":");
  }
  offset = 0;
  z = 0;
  for (w = 0; w < words.length; w++) {
    if (typeof words[w] == "string") {
      if (words[w] === "Z") {
        // compressed zeros - write appropriate number of zero words
        for (z = 0; z < (8 - words.length + 1); z++) {
          parts[w + z] = 0;
        }
        offset = z - 1;
      } else {
        // parse hex field to 16-bit value and write it in network byte-order
        parts[w + offset] = _htons(parseInt(words[w], 16));
      }
    } else {
      // parsed IPv4 words
      parts[w + offset] = words[w];
    }
  }
  return [ (parts[1] << 16) | parts[0], (parts[3] << 16) | parts[2], (parts[5] << 16) | parts[4], (parts[7] << 16) | parts[6] ];
};

var DNS = {
  address_map: {
    id: 1,
    addrs: {},
    names: {}
  },
  lookup_name(name) {
    // If the name is already a valid ipv4 / ipv6 address, don't generate a fake one.
    var res = inetPton4(name);
    if (res !== null) {
      return name;
    }
    res = inetPton6(name);
    if (res !== null) {
      return name;
    }
    // See if this name is already mapped.
    var addr;
    if (DNS.address_map.addrs[name]) {
      addr = DNS.address_map.addrs[name];
    } else {
      var id = DNS.address_map.id++;
      addr = "172.29." + (id & 255) + "." + (id & 65280);
      DNS.address_map.names[addr] = name;
      DNS.address_map.addrs[name] = addr;
    }
    return addr;
  },
  lookup_addr(addr) {
    if (DNS.address_map.names[addr]) {
      return DNS.address_map.names[addr];
    }
    return null;
  }
};

var getSocketAddress = (addrp, addrlen) => {
  var info = readSockaddr(addrp, addrlen);
  if (info.errno) throw new FS.ErrnoError(info.errno);
  info.addr = DNS.lookup_addr(info.addr) || info.addr;
  return info;
};

function ___syscall_connect(fd, addr, addrlen, d1, d2, d3) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(4, 0, 1, fd, addr, addrlen, d1, d2, d3);
  addr = bigintToI53Checked(addr);
  addrlen = bigintToI53Checked(addrlen);
  try {
    var sock = getSocketFromFD(fd);
    var info = getSocketAddress(addr, addrlen);
    sock.sock_ops.connect(sock, info.addr, info.port);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_fchmod(fd, mode) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(5, 0, 1, fd, mode);
  try {
    FS.fchmod(fd, mode);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

var syscallGetVarargP = () => {
  var ret = Number((growMemViews(), HEAPU64)[((SYSCALLS.varargs) / 8)]);
  SYSCALLS.varargs += 8;
  return ret;
};

var syscallGetVarargI = () => {
  // the `+` prepended here is necessary to convince the JSCompiler that varargs is indeed a number.
  var ret = (growMemViews(), HEAP32)[((+SYSCALLS.varargs) / 4)];
  SYSCALLS.varargs += 4;
  return ret;
};

function ___syscall_fcntl64(fd, cmd, varargs) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(6, 0, 1, fd, cmd, varargs);
  varargs = bigintToI53Checked(varargs);
  SYSCALLS.varargs = varargs;
  try {
    var stream = SYSCALLS.getStreamFromFD(fd);
    switch (cmd) {
     case 0:
      {
        var arg = syscallGetVarargI();
        if (arg < 0) {
          return -28;
        }
        while (FS.streams[arg]) {
          arg++;
        }
        var newStream;
        newStream = FS.dupStream(stream, arg);
        return newStream.fd;
      }

     case 1:
     case 2:
      return 0;

     // FD_CLOEXEC makes no sense for a single process.
      case 3:
      return stream.flags;

     case 4:
      {
        var arg = syscallGetVarargI();
        stream.flags |= arg;
        return 0;
      }

     case 5:
      {
        var arg = syscallGetVarargP();
        var offset = 0;
        // We're always unlocked.
        (growMemViews(), HEAP16)[(((arg) + (offset)) / 2)] = 2;
        return 0;
      }

     case 6:
     case 7:
      // Pretend that the locking is successful. These are process-level locks,
      // and Emscripten programs are a single process. If we supported linking a
      // filesystem between programs, we'd need to do more here.
      // See https://github.com/emscripten-core/emscripten/issues/23697
      return 0;
    }
    return -28;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_fstat64(fd, buf) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(7, 0, 1, fd, buf);
  buf = bigintToI53Checked(buf);
  try {
    return SYSCALLS.writeStat(buf, FS.fstat(fd));
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_ftruncate64(fd, length) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(8, 0, 1, fd, length);
  length = bigintToI53Checked(length);
  try {
    if (isNaN(length)) return -61;
    FS.ftruncate(fd, length);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

var zeroMemory = (ptr, size) => (growMemViews(), HEAPU8).fill(0, ptr, ptr + size);

/** @param {number=} addrlen */ var writeSockaddr = (sa, family, addr, port, addrlen) => {
  switch (family) {
   case 2:
    addr = inetPton4(addr);
    zeroMemory(sa, 16);
    if (addrlen) {
      (growMemViews(), HEAP32)[((addrlen) / 4)] = 16;
    }
    (growMemViews(), HEAP16)[((sa) / 2)] = family;
    (growMemViews(), HEAP32)[(((sa) + (4)) / 4)] = addr;
    (growMemViews(), HEAP16)[(((sa) + (2)) / 2)] = _htons(port);
    break;

   case 10:
    addr = inetPton6(addr);
    zeroMemory(sa, 28);
    if (addrlen) {
      (growMemViews(), HEAP32)[((addrlen) / 4)] = 28;
    }
    (growMemViews(), HEAP32)[((sa) / 4)] = family;
    (growMemViews(), HEAP32)[(((sa) + (8)) / 4)] = addr[0];
    (growMemViews(), HEAP32)[(((sa) + (12)) / 4)] = addr[1];
    (growMemViews(), HEAP32)[(((sa) + (16)) / 4)] = addr[2];
    (growMemViews(), HEAP32)[(((sa) + (20)) / 4)] = addr[3];
    (growMemViews(), HEAP16)[(((sa) + (2)) / 2)] = _htons(port);
    break;

   default:
    return 5;
  }
  return 0;
};

function ___syscall_getpeername(fd, addr, addrlen, d1, d2, d3) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(9, 0, 1, fd, addr, addrlen, d1, d2, d3);
  addr = bigintToI53Checked(addr);
  addrlen = bigintToI53Checked(addrlen);
  try {
    var sock = getSocketFromFD(fd);
    if (!sock.daddr) {
      return -53;
    }
    var errno = writeSockaddr(addr, sock.family, DNS.lookup_name(sock.daddr), sock.dport, addrlen);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_getsockname(fd, addr, addrlen, d1, d2, d3) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(10, 0, 1, fd, addr, addrlen, d1, d2, d3);
  addr = bigintToI53Checked(addr);
  addrlen = bigintToI53Checked(addrlen);
  try {
    var sock = getSocketFromFD(fd);
    // TODO: sock.saddr should never be undefined, see TODO in websocket_sock_ops.getname
    var errno = writeSockaddr(addr, sock.family, DNS.lookup_name(sock.saddr || "0.0.0.0"), sock.sport, addrlen);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_getsockopt(fd, level, optname, optval, optlen, d1) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(11, 0, 1, fd, level, optname, optval, optlen, d1);
  optval = bigintToI53Checked(optval);
  optlen = bigintToI53Checked(optlen);
  try {
    var sock = getSocketFromFD(fd);
    // Minimal getsockopt aimed at resolving https://github.com/emscripten-core/emscripten/issues/2211
    // so only supports SOL_SOCKET with SO_ERROR.
    if (level === 1) {
      if (optname === 4) {
        (growMemViews(), HEAP32)[((optval) / 4)] = sock.error;
        (growMemViews(), HEAP32)[((optlen) / 4)] = 4;
        sock.error = null;
        // Clear the error (The SO_ERROR option obtains and then clears this field).
        return 0;
      }
    }
    return -50;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_ioctl(fd, op, varargs) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(12, 0, 1, fd, op, varargs);
  varargs = bigintToI53Checked(varargs);
  SYSCALLS.varargs = varargs;
  try {
    var stream = SYSCALLS.getStreamFromFD(fd);
    switch (op) {
     case 21509:
      {
        if (!stream.tty) return -59;
        return 0;
      }

     case 21505:
      {
        if (!stream.tty) return -59;
        if (stream.tty.ops.ioctl_tcgets) {
          var termios = stream.tty.ops.ioctl_tcgets(stream);
          var argp = syscallGetVarargP();
          (growMemViews(), HEAP32)[((argp) / 4)] = termios.c_iflag || 0;
          (growMemViews(), HEAP32)[(((argp) + (4)) / 4)] = termios.c_oflag || 0;
          (growMemViews(), HEAP32)[(((argp) + (8)) / 4)] = termios.c_cflag || 0;
          (growMemViews(), HEAP32)[(((argp) + (12)) / 4)] = termios.c_lflag || 0;
          for (var i = 0; i < 32; i++) {
            (growMemViews(), HEAP8)[(argp + i) + (17)] = termios.c_cc[i] || 0;
          }
          return 0;
        }
        return 0;
      }

     case 21510:
     case 21511:
     case 21512:
      {
        if (!stream.tty) return -59;
        return 0;
      }

     case 21506:
     case 21507:
     case 21508:
      {
        if (!stream.tty) return -59;
        if (stream.tty.ops.ioctl_tcsets) {
          var argp = syscallGetVarargP();
          var c_iflag = (growMemViews(), HEAP32)[((argp) / 4)];
          var c_oflag = (growMemViews(), HEAP32)[(((argp) + (4)) / 4)];
          var c_cflag = (growMemViews(), HEAP32)[(((argp) + (8)) / 4)];
          var c_lflag = (growMemViews(), HEAP32)[(((argp) + (12)) / 4)];
          var c_cc = [];
          for (var i = 0; i < 32; i++) {
            c_cc.push((growMemViews(), HEAP8)[(argp + i) + (17)]);
          }
          return stream.tty.ops.ioctl_tcsets(stream.tty, op, {
            c_iflag,
            c_oflag,
            c_cflag,
            c_lflag,
            c_cc
          });
        }
        return 0;
      }

     case 21519:
      {
        if (!stream.tty) return -59;
        var argp = syscallGetVarargP();
        (growMemViews(), HEAP32)[((argp) / 4)] = 0;
        return 0;
      }

     case 21520:
      {
        if (!stream.tty) return -59;
        return -28;
      }

     case 21537:
     case 21531:
      {
        var argp = syscallGetVarargP();
        return FS.ioctl(stream, op, argp);
      }

     case 21523:
      {
        // TODO: in theory we should write to the winsize struct that gets
        // passed in, but for now musl doesn't read anything on it
        if (!stream.tty) return -59;
        if (stream.tty.ops.ioctl_tiocgwinsz) {
          var winsize = stream.tty.ops.ioctl_tiocgwinsz(stream.tty);
          var argp = syscallGetVarargP();
          (growMemViews(), HEAP16)[((argp) / 2)] = winsize[0];
          (growMemViews(), HEAP16)[(((argp) + (2)) / 2)] = winsize[1];
        }
        return 0;
      }

     case 21524:
      {
        // TODO: technically, this ioctl call should change the window size.
        // but, since emscripten doesn't have any concept of a terminal window
        // yet, we'll just silently throw it away as we do TIOCGWINSZ
        if (!stream.tty) return -59;
        return 0;
      }

     case 21515:
      {
        if (!stream.tty) return -59;
        return 0;
      }

     default:
      return -28;
    }
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_lstat64(path, buf) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(13, 0, 1, path, buf);
  path = bigintToI53Checked(path);
  buf = bigintToI53Checked(buf);
  try {
    path = SYSCALLS.getStr(path);
    return SYSCALLS.writeStat(buf, FS.lstat(path));
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_newfstatat(dirfd, path, buf, flags) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(14, 0, 1, dirfd, path, buf, flags);
  path = bigintToI53Checked(path);
  buf = bigintToI53Checked(buf);
  try {
    path = SYSCALLS.getStr(path);
    var nofollow = flags & 256;
    var allowEmpty = flags & 4096;
    flags = flags & (~6400);
    path = SYSCALLS.calculateAt(dirfd, path, allowEmpty);
    return SYSCALLS.writeStat(buf, nofollow ? FS.lstat(path) : FS.stat(path));
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_openat(dirfd, path, flags, varargs) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(15, 0, 1, dirfd, path, flags, varargs);
  path = bigintToI53Checked(path);
  varargs = bigintToI53Checked(varargs);
  SYSCALLS.varargs = varargs;
  try {
    path = SYSCALLS.getStr(path);
    path = SYSCALLS.calculateAt(dirfd, path);
    var mode = varargs ? syscallGetVarargI() : 0;
    return FS.open(path, flags, mode).fd;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

var ___syscall_poll = function(fds, nfds, timeout) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(16, 0, 2, fds, nfds, timeout);
  fds = bigintToI53Checked(fds);
  try {
    const isAsyncContext = PThread.currentProxiedOperationCallerThread;
    // Enable event handlers only when the poll call is proxied from a worker.
    // TODO: Could use `Promise.withResolvers` here if we know its available.
    var resolve;
    var promise = new Promise(resolve_ => {
      resolve = resolve_;
    });
    var cleanupFuncs = [];
    var notifyDone = false;
    function asyncPollComplete(count) {
      if (notifyDone) {
        return;
      }
      notifyDone = true;
      cleanupFuncs.forEach(cb => cb());
      resolve(count);
    }
    function makeNotifyCallback(stream, pollfd) {
      var cb = flags => {
        if (notifyDone) {
          return;
        }
        var events = (growMemViews(), HEAP16)[(((pollfd) + (4)) / 2)];
        flags &= events | 8 | 16;
        (growMemViews(), HEAP16)[(((pollfd) + (6)) / 2)] = flags;
        asyncPollComplete(1);
      };
      cb.registerCleanupFunc = f => {
        if (f) cleanupFuncs.push(f);
      };
      return cb;
    }
    if (isAsyncContext) {
      if (timeout > 0) {
        var t = setTimeout(() => {
          asyncPollComplete(0);
        }, timeout);
        cleanupFuncs.push(() => clearTimeout(t));
      }
    }
    var count = 0;
    for (var i = 0; i < nfds; i++) {
      var pollfd = fds + 8 * i;
      var fd = (growMemViews(), HEAP32)[((pollfd) / 4)];
      var events = (growMemViews(), HEAP16)[(((pollfd) + (4)) / 2)];
      var flags = 32;
      var stream = FS.getStream(fd);
      if (stream) {
        if (stream.stream_ops.poll) {
          if (isAsyncContext && timeout) {
            flags = stream.stream_ops.poll(stream, timeout, makeNotifyCallback(stream, pollfd));
          } else flags = stream.stream_ops.poll(stream, -1);
        } else {
          flags = 5;
        }
      }
      flags &= events | 8 | 16;
      if (flags) count++;
      (growMemViews(), HEAP16)[(((pollfd) + (6)) / 2)] = flags;
    }
    if (isAsyncContext) {
      if (count || !timeout) {
        asyncPollComplete(count);
      }
      return promise;
    }
    return count;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
};

function ___syscall_recvfrom(fd, buf, len, flags, addr, addrlen) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(17, 0, 1, fd, buf, len, flags, addr, addrlen);
  buf = bigintToI53Checked(buf);
  len = bigintToI53Checked(len);
  addr = bigintToI53Checked(addr);
  addrlen = bigintToI53Checked(addrlen);
  try {
    var sock = getSocketFromFD(fd);
    var msg = sock.sock_ops.recvmsg(sock, len);
    if (!msg) return 0;
    // socket is closed
    if (addr) {
      var errno = writeSockaddr(addr, sock.family, DNS.lookup_name(msg.addr), msg.port, addrlen);
    }
    (growMemViews(), HEAPU8).set(msg.buffer, buf);
    return msg.buffer.byteLength;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_renameat(olddirfd, oldpath, newdirfd, newpath) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(18, 0, 1, olddirfd, oldpath, newdirfd, newpath);
  oldpath = bigintToI53Checked(oldpath);
  newpath = bigintToI53Checked(newpath);
  try {
    oldpath = SYSCALLS.getStr(oldpath);
    newpath = SYSCALLS.getStr(newpath);
    oldpath = SYSCALLS.calculateAt(olddirfd, oldpath);
    newpath = SYSCALLS.calculateAt(newdirfd, newpath);
    FS.rename(oldpath, newpath);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_sendmsg(fd, message, flags, d1, d2, d3) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(19, 0, 1, fd, message, flags, d1, d2, d3);
  message = bigintToI53Checked(message);
  d1 = bigintToI53Checked(d1);
  d2 = bigintToI53Checked(d2);
  try {
    var sock = getSocketFromFD(fd);
    var iov = Number((growMemViews(), HEAPU64)[(((message) + (16)) / 8)]);
    var num = (growMemViews(), HEAP32)[(((message) + (24)) / 4)];
    // read the address and port to send to
    var addr, port;
    var name = Number((growMemViews(), HEAPU64)[((message) / 8)]);
    var namelen = (growMemViews(), HEAP32)[(((message) + (8)) / 4)];
    if (name) {
      var info = getSocketAddress(name, namelen);
      port = info.port;
      addr = info.addr;
    }
    // concatenate scatter-gather arrays into one message buffer
    var total = 0;
    for (var i = 0; i < num; i++) {
      total += (growMemViews(), HEAP32)[(((iov) + ((16 * i) + 8)) / 4)];
    }
    var view = new Uint8Array(total);
    var offset = 0;
    for (var i = 0; i < num; i++) {
      var iovbase = Number((growMemViews(), HEAPU64)[(((iov) + ((16 * i) + 0)) / 8)]);
      var iovlen = (growMemViews(), HEAP32)[(((iov) + ((16 * i) + 8)) / 4)];
      for (var j = 0; j < iovlen; j++) {
        view[offset++] = (growMemViews(), HEAP8)[(iovbase) + (j)];
      }
    }
    // write the buffer
    return sock.sock_ops.sendmsg(sock, view, 0, total, addr, port);
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_sendto(fd, message, length, flags, addr, addr_len) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(20, 0, 1, fd, message, length, flags, addr, addr_len);
  message = bigintToI53Checked(message);
  length = bigintToI53Checked(length);
  addr = bigintToI53Checked(addr);
  addr_len = bigintToI53Checked(addr_len);
  try {
    var sock = getSocketFromFD(fd);
    if (!addr) {
      // send, no address provided
      return FS.write(sock.stream, (growMemViews(), HEAP8), message, length);
    }
    var dest = getSocketAddress(addr, addr_len);
    // sendto an address
    return sock.sock_ops.sendmsg(sock, (growMemViews(), HEAP8), message, length, dest.addr, dest.port);
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_socket(domain, type, protocol) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(21, 0, 1, domain, type, protocol);
  try {
    var sock = SOCKFS.createSocket(domain, type, protocol);
    return sock.stream.fd;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_stat64(path, buf) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(22, 0, 1, path, buf);
  path = bigintToI53Checked(path);
  buf = bigintToI53Checked(buf);
  try {
    path = SYSCALLS.getStr(path);
    return SYSCALLS.writeStat(buf, FS.stat(path));
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

function ___syscall_unlinkat(dirfd, path, flags) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(23, 0, 1, dirfd, path, flags);
  path = bigintToI53Checked(path);
  try {
    path = SYSCALLS.getStr(path);
    path = SYSCALLS.calculateAt(dirfd, path);
    if (!flags) {
      FS.unlink(path);
    } else if (flags === 512) {
      FS.rmdir(path);
    } else {
      return -28;
    }
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return -e.errno;
  }
}

var __abort_js = () => abort("");

function __emscripten_init_main_thread_js(tb) {
  tb = bigintToI53Checked(tb);
  // Pass the thread address to the native code where they are stored in wasm
  // globals which act as a form of TLS. Global constructors trying
  // to access this value will read the wrong value, but that is UB anyway.
  __emscripten_thread_init(tb, /*is_main=*/ !ENVIRONMENT_IS_WORKER, /*is_runtime=*/ 1, /*can_block=*/ !ENVIRONMENT_IS_WEB, /*default_stacksize=*/ 1048576, /*start_profiling=*/ false);
  PThread.threadInitTLS();
}

function __emscripten_lookup_name(name) {
  name = bigintToI53Checked(name);
  // uint32_t _emscripten_lookup_name(const char *name);
  var nameString = UTF8ToString(name);
  return inetPton4(DNS.lookup_name(nameString));
}

var handleException = e => {
  // Certain exception types we do not treat as errors since they are used for
  // internal control flow.
  // 1. ExitStatus, which is thrown by exit()
  // 2. "unwind", which is thrown by emscripten_unwind_to_js_event_loop() and others
  //    that wish to return to JS event loop.
  if (e instanceof ExitStatus || e == "unwind") {
    return EXITSTATUS;
  }
  quit_(1, e);
};

var maybeExit = () => {
  if (!keepRuntimeAlive()) {
    try {
      if (ENVIRONMENT_IS_PTHREAD) {
        // exit the current thread, but only if there is one active.
        // TODO(https://github.com/emscripten-core/emscripten/issues/25076):
        // Unify this check with the runtimeExited check above
        if (_pthread_self()) __emscripten_thread_exit(EXITSTATUS);
        return;
      }
      _exit(EXITSTATUS);
    } catch (e) {
      handleException(e);
    }
  }
};

var callUserCallback = func => {
  if (ABORT) {
    return;
  }
  try {
    return func();
  } catch (e) {
    handleException(e);
  } finally {
    maybeExit();
  }
};

var waitAsyncPolyfilled = (!Atomics.waitAsync || (globalThis.navigator?.userAgent && Number((navigator.userAgent.match(/Chrom(e|ium)\/([0-9]+)\./) || [])[2]) < 91));

function __emscripten_thread_mailbox_await(pthread_ptr) {
  pthread_ptr = bigintToI53Checked(pthread_ptr);
  if (!waitAsyncPolyfilled) {
    // Wait on the pthread's initial self-pointer field because it is easy and
    // safe to access from sending threads that need to notify the waiting
    // thread.
    // TODO: How to make this work with wasm64?
    var wait = Atomics.waitAsync((growMemViews(), HEAP32), ((pthread_ptr) / 4), pthread_ptr);
    wait.value.then(checkMailbox);
    var waitingAsync = pthread_ptr + 228;
    Atomics.store((growMemViews(), HEAP32), ((waitingAsync) / 4), 1);
  }
}

var checkMailbox = () => callUserCallback(() => {
  // Only check the mailbox if we have a live pthread runtime. We implement
  // pthread_self to return 0 if there is no live runtime.
  // TODO(https://github.com/emscripten-core/emscripten/issues/25076):
  // Is this check still needed?  `callUserCallback` is supposed to
  // ensure the runtime is alive, and if `_pthread_self` is NULL then the
  // runtime certainly is *not* alive, so this should be a redundant check.
  var pthread_ptr = _pthread_self();
  if (pthread_ptr) {
    // If we are using Atomics.waitAsync as our notification mechanism, wait
    // for a notification before processing the mailbox to avoid missing any
    // work that could otherwise arrive after we've finished processing the
    // mailbox and before we're ready for the next notification.
    __emscripten_thread_mailbox_await(pthread_ptr);
    __emscripten_check_mailbox();
  }
});

function __emscripten_notify_mailbox_postmessage(targetThread, currThreadId) {
  targetThread = bigintToI53Checked(targetThread);
  currThreadId = bigintToI53Checked(currThreadId);
  if (targetThread == currThreadId) {
    setTimeout(checkMailbox);
  } else if (ENVIRONMENT_IS_PTHREAD) {
    postMessage({
      targetThread,
      cmd: "checkMailbox"
    });
  } else {
    var worker = PThread.pthreads[targetThread];
    if (!worker) {
      return;
    }
    worker.postMessage({
      cmd: "checkMailbox"
    });
  }
}

var proxiedJSCallArgs = [];

function __emscripten_receive_on_main_thread_js(funcIndex, emAsmAddr, callingThread, bufSize, args, ctx, ctxArgs) {
  emAsmAddr = bigintToI53Checked(emAsmAddr);
  callingThread = bigintToI53Checked(callingThread);
  args = bigintToI53Checked(args);
  ctx = bigintToI53Checked(ctx);
  ctxArgs = bigintToI53Checked(ctxArgs);
  // Sometimes we need to backproxy events to the calling thread (e.g.
  // HTML5 DOM events handlers such as
  // emscripten_set_mousemove_callback()), so keep track in a globally
  // accessible variable about the thread that initiated the proxying.
  proxiedJSCallArgs.length = 0;
  var b = ((args) / 8);
  var end = ((args + bufSize) / 8);
  while (b < end) {
    var arg;
    if ((growMemViews(), HEAP64)[b++]) {
      // It's a BigInt.
      arg = (growMemViews(), HEAP64)[b++];
    } else {
      // It's a Number.
      arg = (growMemViews(), HEAPF64)[b++];
    }
    proxiedJSCallArgs.push(arg);
  }
  // Proxied JS library funcs use funcIndex and EM_ASM functions use emAsmAddr
  var func = proxiedFunctionTable[funcIndex];
  PThread.currentProxiedOperationCallerThread = callingThread;
  var rtn = func(...proxiedJSCallArgs);
  PThread.currentProxiedOperationCallerThread = 0;
  if (ctx) {
    rtn.then(rtn => __emscripten_run_js_on_main_thread_done(ctx, ctxArgs, rtn));
    return;
  }
  // In memory64 mode some proxied functions return bigint/pointer but
  // our return type is i53/double.
  if (typeof rtn == "bigint") {
    rtn = bigintToI53Checked(rtn);
  }
  return rtn;
}

var __emscripten_runtime_keepalive_clear = () => {
  noExitRuntime = false;
  runtimeKeepaliveCounter = 0;
};

function __emscripten_thread_cleanup(thread) {
  thread = bigintToI53Checked(thread);
  // Called when a thread needs to be cleaned up so it can be reused.
  // A thread is considered reusable when it either returns from its
  // entry point, calls pthread_exit, or acts upon a cancellation.
  // Detached threads are responsible for calling this themselves,
  // otherwise pthread_join is responsible for calling this.
  if (!ENVIRONMENT_IS_PTHREAD) cleanupThread(thread); else postMessage({
    cmd: "cleanupThread",
    thread
  });
}

function __emscripten_thread_set_strongref(thread) {
  thread = bigintToI53Checked(thread);
}

var __emscripten_throw_longjmp = () => {
  throw Infinity;
};

var _emscripten_get_now = () => performance.timeOrigin + performance.now();

var _emscripten_date_now = () => Date.now();

var nowIsMonotonic = 1;

var checkWasiClock = clock_id => clock_id >= 0 && clock_id <= 3;

function _clock_time_get(clk_id, ignored_precision, ptime) {
  ignored_precision = bigintToI53Checked(ignored_precision);
  ptime = bigintToI53Checked(ptime);
  if (!checkWasiClock(clk_id)) {
    return 28;
  }
  var now;
  // all wasi clocks but realtime are monotonic
  if (clk_id === 0) {
    now = _emscripten_date_now();
  } else if (nowIsMonotonic) {
    now = _emscripten_get_now();
  } else {
    return 52;
  }
  // "now" is in ms, and wasi times are in ns.
  var nsec = Math.round(now * 1e3 * 1e3);
  (growMemViews(), HEAP64)[((ptime) / 8)] = BigInt(nsec);
  return 0;
}

var _emscripten_check_blocking_allowed = () => {};

var _emscripten_exit_with_live_runtime = () => {
  runtimeKeepalivePush();
  throw "unwind";
};

var getHeapMax = () => 2147483648;

var alignMemory = (size, alignment) => Math.ceil(size / alignment) * alignment;

var growMemory = size => {
  var oldHeapSize = wasmMemory.buffer.byteLength;
  var pages = ((size - oldHeapSize + 65535) / 65536) | 0;
  try {
    // round size grow request up to wasm page size (fixed 64KB per spec)
    wasmMemory.grow(BigInt(pages));
    // .grow() takes a delta compared to the previous size
    updateMemoryViews();
    return 1;
  } catch (e) {}
};

function _emscripten_resize_heap(requestedSize) {
  requestedSize = bigintToI53Checked(requestedSize);
  var oldSize = (growMemViews(), HEAPU8).length;
  // With multithreaded builds, races can happen (another thread might increase the size
  // in between), so return a failure, and let the caller retry.
  if (requestedSize <= oldSize) {
    return false;
  }
  // Memory resize rules:
  // 1.  Always increase heap size to at least the requested size, rounded up
  //     to next page multiple.
  // 2a. If MEMORY_GROWTH_LINEAR_STEP == -1, excessively resize the heap
  //     geometrically: increase the heap size according to
  //     MEMORY_GROWTH_GEOMETRIC_STEP factor (default +20%), At most
  //     overreserve by MEMORY_GROWTH_GEOMETRIC_CAP bytes (default 96MB).
  // 2b. If MEMORY_GROWTH_LINEAR_STEP != -1, excessively resize the heap
  //     linearly: increase the heap size by at least
  //     MEMORY_GROWTH_LINEAR_STEP bytes.
  // 3.  Max size for the heap is capped at 2048MB-WASM_PAGE_SIZE, or by
  //     MAXIMUM_MEMORY, or by ASAN limit, depending on which is smallest
  // 4.  If we were unable to allocate as much memory, it may be due to
  //     over-eager decision to excessively reserve due to (3) above.
  //     Hence if an allocation fails, cut down on the amount of excess
  //     growth, in an attempt to succeed to perform a smaller allocation.
  // A limit is set for how much we can grow. We should not exceed that
  // (the wasm binary specifies it, so if we tried, we'd fail anyhow).
  var maxHeapSize = getHeapMax();
  if (requestedSize > maxHeapSize) {
    return false;
  }
  // Loop through potential heap size increases. If we attempt a too eager
  // reservation that fails, cut down on the attempted size and reserve a
  // smaller bump instead. (max 3 times, chosen somewhat arbitrarily)
  for (var cutDown = 1; cutDown <= 4; cutDown *= 2) {
    var overGrownHeapSize = oldSize * (1 + .2 / cutDown);
    // ensure geometric growth
    // but limit overreserving (default to capping at +96MB overgrowth at most)
    overGrownHeapSize = Math.min(overGrownHeapSize, requestedSize + 100663296);
    var newSize = Math.min(maxHeapSize, alignMemory(Math.max(requestedSize, overGrownHeapSize), 65536));
    var replacement = growMemory(newSize);
    if (replacement) {
      return true;
    }
  }
  return false;
}

/** @returns {number} */ var convertFrameToPC = frame => {
  var match;
  if (match = /\bwasm-function\[\d+\]:(0x[0-9a-f]+)/.exec(frame)) {
    // Wasm engines give the binary offset directly, so we use that as return address
    return +match[1];
  } else if (match = /:(\d+):\d+(?:\)|$)/.exec(frame)) {
    // If we are in js, we can use the js line number as the "return address".
    // This should work for wasm2js.  We tag the high bit to distinguish this
    // from wasm addresses.
    return 2147483648 | +match[1];
  }
  // return 0 if we can't find any
  return 0;
};

var jsStackTrace = () => (new Error).stack.toString();

var _emscripten_return_address = function(level) {
  var ret = (() => {
    var callstack = jsStackTrace().split("\n");
    if (callstack[0] == "Error") {
      callstack.shift();
    }
    // skip this function and the caller to get caller's return address
    // MEMORY64 injects an extra wrapper within emscripten_return_address
    // to handle BigInt conversions.
    var caller = callstack[level + 4];
    return convertFrameToPC(caller);
  })();
  return BigInt(ret);
};

var _emscripten_runtime_keepalive_check = keepRuntimeAlive;

var ENV = {};

var getExecutableName = () => thisProgram || "./this.program";

var getEnvStrings = () => {
  if (!getEnvStrings.strings) {
    // Default values.
    // Browser language detection #8751
    var lang = (globalThis.navigator?.language ?? "C").replace("-", "_") + ".UTF-8";
    var env = {
      "USER": "web_user",
      "LOGNAME": "web_user",
      "PATH": "/",
      "PWD": "/",
      "HOME": "/home/web_user",
      "LANG": lang,
      "_": getExecutableName()
    };
    // Apply the user-provided values, if any.
    for (var x in ENV) {
      // x is a key in ENV; if ENV[x] is undefined, that means it was
      // explicitly set to be so. We allow user code to do that to
      // force variables with default values to remain unset.
      if (ENV[x] === undefined) delete env[x]; else env[x] = ENV[x];
    }
    var strings = [];
    for (var x in env) {
      strings.push(`${x}=${env[x]}`);
    }
    getEnvStrings.strings = strings;
  }
  return getEnvStrings.strings;
};

var stringToUTF8 = (str, outPtr, maxBytesToWrite) => stringToUTF8Array(str, (growMemViews(), 
HEAPU8), outPtr, maxBytesToWrite);

function _environ_get(__environ, environ_buf) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(24, 0, 1, __environ, environ_buf);
  __environ = bigintToI53Checked(__environ);
  environ_buf = bigintToI53Checked(environ_buf);
  var bufSize = 0;
  var envp = 0;
  for (var string of getEnvStrings()) {
    var ptr = environ_buf + bufSize;
    (growMemViews(), HEAPU64)[(((__environ) + (envp)) / 8)] = BigInt(ptr);
    bufSize += stringToUTF8(string, ptr, Infinity) + 1;
    envp += 8;
  }
  return 0;
}

function _environ_sizes_get(penviron_count, penviron_buf_size) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(25, 0, 1, penviron_count, penviron_buf_size);
  penviron_count = bigintToI53Checked(penviron_count);
  penviron_buf_size = bigintToI53Checked(penviron_buf_size);
  var strings = getEnvStrings();
  (growMemViews(), HEAPU64)[((penviron_count) / 8)] = BigInt(strings.length);
  var bufSize = 0;
  for (var string of strings) {
    bufSize += lengthBytesUTF8(string) + 1;
  }
  (growMemViews(), HEAPU64)[((penviron_buf_size) / 8)] = BigInt(bufSize);
  return 0;
}

function _fd_close(fd) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(26, 0, 1, fd);
  try {
    var stream = SYSCALLS.getStreamFromFD(fd);
    FS.close(stream);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return e.errno;
  }
}

function _fd_fdstat_get(fd, pbuf) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(27, 0, 1, fd, pbuf);
  pbuf = bigintToI53Checked(pbuf);
  try {
    var rightsBase = 0;
    var rightsInheriting = 0;
    var flags = 0;
    {
      var stream = SYSCALLS.getStreamFromFD(fd);
      // All character devices are terminals (other things a Linux system would
      // assume is a character device, like the mouse, we have special APIs for).
      var type = stream.tty ? 2 : FS.isDir(stream.mode) ? 3 : FS.isLink(stream.mode) ? 7 : 4;
    }
    (growMemViews(), HEAP8)[pbuf] = type;
    (growMemViews(), HEAP16)[(((pbuf) + (2)) / 2)] = flags;
    (growMemViews(), HEAP64)[(((pbuf) + (8)) / 8)] = BigInt(rightsBase);
    (growMemViews(), HEAP64)[(((pbuf) + (16)) / 8)] = BigInt(rightsInheriting);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return e.errno;
  }
}

/** @param {number=} offset */ var doReadv = (stream, iov, iovcnt, offset) => {
  var ret = 0;
  for (var i = 0; i < iovcnt; i++) {
    var ptr = Number((growMemViews(), HEAPU64)[((iov) / 8)]);
    var len = Number((growMemViews(), HEAPU64)[(((iov) + (8)) / 8)]);
    iov += 16;
    var curr = FS.read(stream, (growMemViews(), HEAP8), ptr, len, offset);
    if (curr < 0) return -1;
    ret += curr;
    if (curr < len) break;
    // nothing more to read
    if (typeof offset != "undefined") {
      offset += curr;
    }
  }
  return ret;
};

function _fd_pread(fd, iov, iovcnt, offset, pnum) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(28, 0, 1, fd, iov, iovcnt, offset, pnum);
  iov = bigintToI53Checked(iov);
  iovcnt = bigintToI53Checked(iovcnt);
  offset = bigintToI53Checked(offset);
  pnum = bigintToI53Checked(pnum);
  try {
    if (isNaN(offset)) return 61;
    var stream = SYSCALLS.getStreamFromFD(fd);
    var num = doReadv(stream, iov, iovcnt, offset);
    (growMemViews(), HEAPU64)[((pnum) / 8)] = BigInt(num);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return e.errno;
  }
}

/** @param {number=} offset */ var doWritev = (stream, iov, iovcnt, offset) => {
  var ret = 0;
  for (var i = 0; i < iovcnt; i++) {
    var ptr = Number((growMemViews(), HEAPU64)[((iov) / 8)]);
    var len = Number((growMemViews(), HEAPU64)[(((iov) + (8)) / 8)]);
    iov += 16;
    var curr = FS.write(stream, (growMemViews(), HEAP8), ptr, len, offset);
    if (curr < 0) return -1;
    ret += curr;
    if (curr < len) {
      // No more space to write.
      break;
    }
    if (typeof offset != "undefined") {
      offset += curr;
    }
  }
  return ret;
};

function _fd_pwrite(fd, iov, iovcnt, offset, pnum) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(29, 0, 1, fd, iov, iovcnt, offset, pnum);
  iov = bigintToI53Checked(iov);
  iovcnt = bigintToI53Checked(iovcnt);
  offset = bigintToI53Checked(offset);
  pnum = bigintToI53Checked(pnum);
  try {
    if (isNaN(offset)) return 61;
    var stream = SYSCALLS.getStreamFromFD(fd);
    var num = doWritev(stream, iov, iovcnt, offset);
    (growMemViews(), HEAPU64)[((pnum) / 8)] = BigInt(num);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return e.errno;
  }
}

function _fd_read(fd, iov, iovcnt, pnum) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(30, 0, 1, fd, iov, iovcnt, pnum);
  iov = bigintToI53Checked(iov);
  iovcnt = bigintToI53Checked(iovcnt);
  pnum = bigintToI53Checked(pnum);
  try {
    var stream = SYSCALLS.getStreamFromFD(fd);
    var num = doReadv(stream, iov, iovcnt);
    (growMemViews(), HEAPU64)[((pnum) / 8)] = BigInt(num);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return e.errno;
  }
}

function _fd_seek(fd, offset, whence, newOffset) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(31, 0, 1, fd, offset, whence, newOffset);
  offset = bigintToI53Checked(offset);
  newOffset = bigintToI53Checked(newOffset);
  try {
    if (isNaN(offset)) return 61;
    var stream = SYSCALLS.getStreamFromFD(fd);
    FS.llseek(stream, offset, whence);
    (growMemViews(), HEAP64)[((newOffset) / 8)] = BigInt(stream.position);
    if (stream.getdents && offset === 0 && whence === 0) stream.getdents = null;
    // reset readdir state
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return e.errno;
  }
}

var _fd_sync = function(fd) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(32, 0, 2, fd);
  try {
    var stream = SYSCALLS.getStreamFromFD(fd);
    var rtn = stream.stream_ops?.fsync?.(stream);
    return new Promise(resolve => {
      var mount = stream.node.mount;
      if (mount?.type.syncfs) {
        mount.type.syncfs(mount, false, err => resolve(err ? 29 : 0));
      } else {
        resolve(rtn);
      }
    });
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return e.errno;
  }
};

function _fd_write(fd, iov, iovcnt, pnum) {
  if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(33, 0, 1, fd, iov, iovcnt, pnum);
  iov = bigintToI53Checked(iov);
  iovcnt = bigintToI53Checked(iovcnt);
  pnum = bigintToI53Checked(pnum);
  try {
    var stream = SYSCALLS.getStreamFromFD(fd);
    var num = doWritev(stream, iov, iovcnt);
    (growMemViews(), HEAPU64)[((pnum) / 8)] = BigInt(num);
    return 0;
  } catch (e) {
    if (typeof FS == "undefined" || !(e.name === "ErrnoError")) throw e;
    return e.errno;
  }
}

function _futimes(...args) {
  abort("missing function: futimes");
}

_futimes.stub = true;

var stringToUTF8OnStack = str => {
  var size = lengthBytesUTF8(str) + 1;
  var ret = stackAlloc(size);
  stringToUTF8(str, ret, size);
  return ret;
};

var FS_createPath = (...args) => FS.createPath(...args);

var FS_unlink = (...args) => FS.unlink(...args);

var FS_createLazyFile = (...args) => FS.createLazyFile(...args);

var FS_createDevice = (...args) => FS.createDevice(...args);

PThread.init();

FS.createPreloadedFile = FS_createPreloadedFile;

FS.preloadFile = FS_preloadFile;

FS.staticInit();

// End JS library code
// include: postlibrary.js
// This file is included after the automatically-generated JS library code
// but before the wasm module is created.
{
  // With WASM_ESM_INTEGRATION this has to happen at the top level and not
  // delayed until processModuleArgs.
  initMemory();
  // Begin ATMODULES hooks
  if (Module["noExitRuntime"]) noExitRuntime = Module["noExitRuntime"];
  if (Module["preloadPlugins"]) preloadPlugins = Module["preloadPlugins"];
  if (Module["print"]) out = Module["print"];
  if (Module["printErr"]) err = Module["printErr"];
  if (Module["wasmBinary"]) wasmBinary = Module["wasmBinary"];
  // End ATMODULES hooks
  if (Module["arguments"]) arguments_ = Module["arguments"];
  if (Module["thisProgram"]) thisProgram = Module["thisProgram"];
  if (Module["preInit"]) {
    if (typeof Module["preInit"] == "function") Module["preInit"] = [ Module["preInit"] ];
    while (Module["preInit"].length > 0) {
      Module["preInit"].shift()();
    }
  }
}

// Begin runtime exports
Module["callMain"] = callMain;

Module["addRunDependency"] = addRunDependency;

Module["removeRunDependency"] = removeRunDependency;

Module["FS_preloadFile"] = FS_preloadFile;

Module["FS_unlink"] = FS_unlink;

Module["FS_createPath"] = FS_createPath;

Module["FS_createDevice"] = FS_createDevice;

Module["FS"] = FS;

Module["FS_createDataFile"] = FS_createDataFile;

Module["FS_createLazyFile"] = FS_createLazyFile;

// End runtime exports
// Begin JS library exports
// End JS library exports
// end include: postlibrary.js
// proxiedFunctionTable specifies the list of functions that can be called
// either synchronously or asynchronously from other threads in postMessage()d
// or internally queued events. This way a pthread in a Worker can synchronously
// access e.g. the DOM on the main thread.
var proxiedFunctionTable = [ _proc_exit, exitOnMainThread, pthreadCreateProxied, ___syscall_chmod, ___syscall_connect, ___syscall_fchmod, ___syscall_fcntl64, ___syscall_fstat64, ___syscall_ftruncate64, ___syscall_getpeername, ___syscall_getsockname, ___syscall_getsockopt, ___syscall_ioctl, ___syscall_lstat64, ___syscall_newfstatat, ___syscall_openat, ___syscall_poll, ___syscall_recvfrom, ___syscall_renameat, ___syscall_sendmsg, ___syscall_sendto, ___syscall_socket, ___syscall_stat64, ___syscall_unlinkat, _environ_get, _environ_sizes_get, _fd_close, _fd_fdstat_get, _fd_pread, _fd_pwrite, _fd_read, _fd_seek, _fd_sync, _fd_write ];

function wasmCallFuncPtrTrampoline(pfn, cArgs, pArgs) {
  var func = wasmTable.get(pfn);
  if (!func) {
    err("wasmCallFuncPtrTrampoline: no function at table index " + pfn);
    return -1;
  }
  var idx = Number(pfn);
  var baseIdx = Number(pArgs) >> 3;
  var bigArgs = [];
  for (var i = 0; i < cArgs; i++) {
    bigArgs.push((growMemViews(), HEAP64)[baseIdx + i]);
  }
  if (!Module._typeMaskCache) Module._typeMaskCache = {};
  var cached = Module._typeMaskCache[idx];
  if (cached !== undefined) {
    var args = [];
    for (var i = 0; i < cArgs; i++) {
      args.push((cached & (1 << i)) ? Number(bigArgs[i]) : bigArgs[i]);
    }
    try {
      var result = func(...args);
      return typeof result === "bigint" ? Number(result) : (result | 0);
    } catch (e) {
      delete Module._typeMaskCache[idx];
    }
  }
  var numArgs = bigArgs.map(function(v) {
    return Number(v);
  });
  var combos = 1 << cArgs;
  for (var mask = 0; mask < combos; mask++) {
    var args = [];
    for (var i = 0; i < cArgs; i++) {
      args.push((mask & (1 << i)) ? numArgs[i] : bigArgs[i]);
    }
    try {
      var result = func(...args);
      Module._typeMaskCache[idx] = mask;
      return typeof result === "bigint" ? Number(result) : (result | 0);
    } catch (e) {
      if (!(e instanceof TypeError)) {
        err("wasmCallFuncPtrTrampoline: idx=" + idx + " failed: " + e.message);
        return -1;
      }
    }
  }
  err("wasmCallFuncPtrTrampoline: exhausted all type combos for idx " + idx + " (" + cArgs + " args)");
  return -1;
}

function wasmJitExecBlock(pCpumCtx, pvRAM, maxInsn) {
  if (typeof globalThis.VBoxJIT === "undefined") return 0;
  if (!globalThis.VBoxJIT._initialized) {
    globalThis.VBoxJIT.init(wasmMemory);
    globalThis.VBoxJIT._initialized = true;
  }
  return globalThis.VBoxJIT.execBlock(Number(pCpumCtx), Number(pvRAM), maxInsn);
}

function wasmJitSetRomBuffer(pvROM, cbROM, uGCPhysStart) {
  if (typeof globalThis.VBoxJIT !== "undefined" && globalThis.VBoxJIT.setRomBuffer) globalThis.VBoxJIT.setRomBuffer(Number(pvROM), cbROM, uGCPhysStart);
}

// Imports from the Wasm binary.
var _main, _wasmJitSetGuestRAM, _wasmJitGetGuestRAM, _pthread_self, _wasmDisplayGetFB, _wasmDisplayGetWidth, _wasmDisplayGetHeight, _wasmDisplayCheckDirty, _wasmDisplayGetFBSize, _wasmDisplayRefresh, _wasmDisplayGetRefreshCount, _wasmDisplayGetUpdateRectCount, _malloc, __emscripten_tls_init, __emscripten_proxy_main, __emscripten_thread_init, __emscripten_thread_crashed, _htonl, _htons, _ntohs, __emscripten_run_js_on_main_thread_done, __emscripten_run_js_on_main_thread, __emscripten_thread_free_data, __emscripten_thread_exit, __emscripten_check_mailbox, _setThrew, _emscripten_stack_set_limits, __emscripten_stack_restore, __emscripten_stack_alloc, _emscripten_stack_get_current, __indirect_function_table, wasmTable;

function assignWasmExports(wasmExports) {
  _main = Module["_main"] = wasmExports["__main_argc_argv"];
  _wasmJitSetGuestRAM = Module["_wasmJitSetGuestRAM"] = wasmExports["wasmJitSetGuestRAM"];
  _wasmJitGetGuestRAM = Module["_wasmJitGetGuestRAM"] = wasmExports["wasmJitGetGuestRAM"];
  _pthread_self = wasmExports["pthread_self"];
  _wasmDisplayGetFB = Module["_wasmDisplayGetFB"] = wasmExports["wasmDisplayGetFB"];
  _wasmDisplayGetWidth = Module["_wasmDisplayGetWidth"] = wasmExports["wasmDisplayGetWidth"];
  _wasmDisplayGetHeight = Module["_wasmDisplayGetHeight"] = wasmExports["wasmDisplayGetHeight"];
  _wasmDisplayCheckDirty = Module["_wasmDisplayCheckDirty"] = wasmExports["wasmDisplayCheckDirty"];
  _wasmDisplayGetFBSize = Module["_wasmDisplayGetFBSize"] = wasmExports["wasmDisplayGetFBSize"];
  _wasmDisplayRefresh = Module["_wasmDisplayRefresh"] = wasmExports["wasmDisplayRefresh"];
  _wasmDisplayGetRefreshCount = Module["_wasmDisplayGetRefreshCount"] = wasmExports["wasmDisplayGetRefreshCount"];
  _wasmDisplayGetUpdateRectCount = Module["_wasmDisplayGetUpdateRectCount"] = wasmExports["wasmDisplayGetUpdateRectCount"];
  _malloc = wasmExports["malloc"];
  __emscripten_tls_init = wasmExports["_emscripten_tls_init"];
  __emscripten_proxy_main = Module["__emscripten_proxy_main"] = wasmExports["_emscripten_proxy_main"];
  __emscripten_thread_init = wasmExports["_emscripten_thread_init"];
  __emscripten_thread_crashed = wasmExports["_emscripten_thread_crashed"];
  _htonl = wasmExports["htonl"];
  _htons = wasmExports["htons"];
  _ntohs = wasmExports["ntohs"];
  __emscripten_run_js_on_main_thread_done = wasmExports["_emscripten_run_js_on_main_thread_done"];
  __emscripten_run_js_on_main_thread = wasmExports["_emscripten_run_js_on_main_thread"];
  __emscripten_thread_free_data = wasmExports["_emscripten_thread_free_data"];
  __emscripten_thread_exit = wasmExports["_emscripten_thread_exit"];
  __emscripten_check_mailbox = wasmExports["_emscripten_check_mailbox"];
  _setThrew = wasmExports["setThrew"];
  _emscripten_stack_set_limits = wasmExports["emscripten_stack_set_limits"];
  __emscripten_stack_restore = wasmExports["_emscripten_stack_restore"];
  __emscripten_stack_alloc = wasmExports["_emscripten_stack_alloc"];
  _emscripten_stack_get_current = wasmExports["emscripten_stack_get_current"];
  __indirect_function_table = wasmTable = wasmExports["__indirect_function_table"];
}

var wasmImports;

function assignWasmImports() {
  wasmImports = {
    /** @export */ ATAPIPassthroughParseCdb: _ATAPIPassthroughParseCdb,
    /** @export */ ATAPIPassthroughTrackListClear: _ATAPIPassthroughTrackListClear,
    /** @export */ ATAPIPassthroughTrackListCreateEmpty: _ATAPIPassthroughTrackListCreateEmpty,
    /** @export */ ATAPIPassthroughTrackListDestroy: _ATAPIPassthroughTrackListDestroy,
    /** @export */ ATAPIPassthroughTrackListUpdate: _ATAPIPassthroughTrackListUpdate,
    /** @export */ DBGFR3FlowTraceModAddProbe: _DBGFR3FlowTraceModAddProbe,
    /** @export */ DBGFR3FlowTraceModClear: _DBGFR3FlowTraceModClear,
    /** @export */ DBGFR3FlowTraceModCreate: _DBGFR3FlowTraceModCreate,
    /** @export */ DBGFR3FlowTraceModCreateFromFlowGraph: _DBGFR3FlowTraceModCreateFromFlowGraph,
    /** @export */ DBGFR3FlowTraceModDisable: _DBGFR3FlowTraceModDisable,
    /** @export */ DBGFR3FlowTraceModEnable: _DBGFR3FlowTraceModEnable,
    /** @export */ DBGFR3FlowTraceModQueryReport: _DBGFR3FlowTraceModQueryReport,
    /** @export */ DBGFR3FlowTraceModRelease: _DBGFR3FlowTraceModRelease,
    /** @export */ DBGFR3FlowTraceModRetain: _DBGFR3FlowTraceModRetain,
    /** @export */ DBGFR3FlowTraceProbeCreate: _DBGFR3FlowTraceProbeCreate,
    /** @export */ DBGFR3FlowTraceProbeEntriesAdd: _DBGFR3FlowTraceProbeEntriesAdd,
    /** @export */ DBGFR3FlowTraceProbeRelease: _DBGFR3FlowTraceProbeRelease,
    /** @export */ DBGFR3FlowTraceProbeRetain: _DBGFR3FlowTraceProbeRetain,
    /** @export */ DBGFR3FlowTraceRecordGetAddr: _DBGFR3FlowTraceRecordGetAddr,
    /** @export */ DBGFR3FlowTraceRecordGetCpuId: _DBGFR3FlowTraceRecordGetCpuId,
    /** @export */ DBGFR3FlowTraceRecordGetProbe: _DBGFR3FlowTraceRecordGetProbe,
    /** @export */ DBGFR3FlowTraceRecordGetSeqNo: _DBGFR3FlowTraceRecordGetSeqNo,
    /** @export */ DBGFR3FlowTraceRecordGetTimestamp: _DBGFR3FlowTraceRecordGetTimestamp,
    /** @export */ DBGFR3FlowTraceRecordGetValCount: _DBGFR3FlowTraceRecordGetValCount,
    /** @export */ DBGFR3FlowTraceRecordGetVals: _DBGFR3FlowTraceRecordGetVals,
    /** @export */ DBGFR3FlowTraceRecordGetValsCommon: _DBGFR3FlowTraceRecordGetValsCommon,
    /** @export */ DBGFR3FlowTraceRecordRelease: _DBGFR3FlowTraceRecordRelease,
    /** @export */ DBGFR3FlowTraceRecordRetain: _DBGFR3FlowTraceRecordRetain,
    /** @export */ DBGFR3FlowTraceReportEnumRecords: _DBGFR3FlowTraceReportEnumRecords,
    /** @export */ DBGFR3FlowTraceReportGetRecordCount: _DBGFR3FlowTraceReportGetRecordCount,
    /** @export */ DBGFR3FlowTraceReportQueryFiltered: _DBGFR3FlowTraceReportQueryFiltered,
    /** @export */ DBGFR3FlowTraceReportQueryRecord: _DBGFR3FlowTraceReportQueryRecord,
    /** @export */ DBGFR3FlowTraceReportRelease: _DBGFR3FlowTraceReportRelease,
    /** @export */ DBGFR3FlowTraceReportRetain: _DBGFR3FlowTraceReportRetain,
    /** @export */ HBDMgrDestroy: _HBDMgrDestroy,
    /** @export */ PDMR3AsyncCompletionBwMgrSetMaxForFile: _PDMR3AsyncCompletionBwMgrSetMaxForFile,
    /** @export */ PDMR3AsyncCompletionEpClose: _PDMR3AsyncCompletionEpClose,
    /** @export */ PDMR3AsyncCompletionEpCreateForFile: _PDMR3AsyncCompletionEpCreateForFile,
    /** @export */ PDMR3AsyncCompletionEpFlush: _PDMR3AsyncCompletionEpFlush,
    /** @export */ PDMR3AsyncCompletionEpGetSize: _PDMR3AsyncCompletionEpGetSize,
    /** @export */ PDMR3AsyncCompletionEpRead: _PDMR3AsyncCompletionEpRead,
    /** @export */ PDMR3AsyncCompletionEpSetBwMgr: _PDMR3AsyncCompletionEpSetBwMgr,
    /** @export */ PDMR3AsyncCompletionEpSetSize: _PDMR3AsyncCompletionEpSetSize,
    /** @export */ PDMR3AsyncCompletionEpWrite: _PDMR3AsyncCompletionEpWrite,
    /** @export */ PDMR3AsyncCompletionTemplateDestroy: _PDMR3AsyncCompletionTemplateDestroy,
    /** @export */ PDMR3NsBwGroupSetLimit: _PDMR3NsBwGroupSetLimit,
    /** @export */ PDMR3UsbCreateEmulatedDevice: _PDMR3UsbCreateEmulatedDevice,
    /** @export */ PDMR3UsbCreateProxyDevice: _PDMR3UsbCreateProxyDevice,
    /** @export */ PDMR3UsbDetachDevice: _PDMR3UsbDetachDevice,
    /** @export */ PDMR3UsbDriverAttach: _PDMR3UsbDriverAttach,
    /** @export */ PDMR3UsbDriverDetach: _PDMR3UsbDriverDetach,
    /** @export */ PDMR3UsbHasHub: _PDMR3UsbHasHub,
    /** @export */ PDMR3UsbQueryDeviceLun: _PDMR3UsbQueryDeviceLun,
    /** @export */ PDMR3UsbQueryDriverOnLun: _PDMR3UsbQueryDriverOnLun,
    /** @export */ PDMR3UsbQueryLun: _PDMR3UsbQueryLun,
    /** @export */ RTFileCopyPartCleanup: _RTFileCopyPartCleanup,
    /** @export */ RTFileCopyPartEx: _RTFileCopyPartEx,
    /** @export */ RTFileCopyPartPrep: _RTFileCopyPartPrep,
    /** @export */ RTFileMove: _RTFileMove,
    /** @export */ RTFileQueryFsSizes: _RTFileQueryFsSizes,
    /** @export */ RTFileQuerySectorSize: _RTFileQuerySectorSize,
    /** @export */ RTFileSetAllocationSize: _RTFileSetAllocationSize,
    /** @export */ __call_sighandler: ___call_sighandler,
    /** @export */ __cxa_throw: ___cxa_throw,
    /** @export */ __pthread_create_js: ___pthread_create_js,
    /** @export */ __syscall_chmod: ___syscall_chmod,
    /** @export */ __syscall_connect: ___syscall_connect,
    /** @export */ __syscall_fchmod: ___syscall_fchmod,
    /** @export */ __syscall_fcntl64: ___syscall_fcntl64,
    /** @export */ __syscall_fstat64: ___syscall_fstat64,
    /** @export */ __syscall_ftruncate64: ___syscall_ftruncate64,
    /** @export */ __syscall_getpeername: ___syscall_getpeername,
    /** @export */ __syscall_getsockname: ___syscall_getsockname,
    /** @export */ __syscall_getsockopt: ___syscall_getsockopt,
    /** @export */ __syscall_ioctl: ___syscall_ioctl,
    /** @export */ __syscall_lstat64: ___syscall_lstat64,
    /** @export */ __syscall_newfstatat: ___syscall_newfstatat,
    /** @export */ __syscall_openat: ___syscall_openat,
    /** @export */ __syscall_poll: ___syscall_poll,
    /** @export */ __syscall_recvfrom: ___syscall_recvfrom,
    /** @export */ __syscall_renameat: ___syscall_renameat,
    /** @export */ __syscall_sendmsg: ___syscall_sendmsg,
    /** @export */ __syscall_sendto: ___syscall_sendto,
    /** @export */ __syscall_socket: ___syscall_socket,
    /** @export */ __syscall_stat64: ___syscall_stat64,
    /** @export */ __syscall_unlinkat: ___syscall_unlinkat,
    /** @export */ _abort_js: __abort_js,
    /** @export */ _emscripten_init_main_thread_js: __emscripten_init_main_thread_js,
    /** @export */ _emscripten_lookup_name: __emscripten_lookup_name,
    /** @export */ _emscripten_notify_mailbox_postmessage: __emscripten_notify_mailbox_postmessage,
    /** @export */ _emscripten_receive_on_main_thread_js: __emscripten_receive_on_main_thread_js,
    /** @export */ _emscripten_runtime_keepalive_clear: __emscripten_runtime_keepalive_clear,
    /** @export */ _emscripten_thread_cleanup: __emscripten_thread_cleanup,
    /** @export */ _emscripten_thread_mailbox_await: __emscripten_thread_mailbox_await,
    /** @export */ _emscripten_thread_set_strongref: __emscripten_thread_set_strongref,
    /** @export */ _emscripten_throw_longjmp: __emscripten_throw_longjmp,
    /** @export */ clock_time_get: _clock_time_get,
    /** @export */ emscripten_check_blocking_allowed: _emscripten_check_blocking_allowed,
    /** @export */ emscripten_exit_with_live_runtime: _emscripten_exit_with_live_runtime,
    /** @export */ emscripten_get_now: _emscripten_get_now,
    /** @export */ emscripten_resize_heap: _emscripten_resize_heap,
    /** @export */ emscripten_return_address: _emscripten_return_address,
    /** @export */ emscripten_runtime_keepalive_check: _emscripten_runtime_keepalive_check,
    /** @export */ environ_get: _environ_get,
    /** @export */ environ_sizes_get: _environ_sizes_get,
    /** @export */ exit: _exit,
    /** @export */ fd_close: _fd_close,
    /** @export */ fd_fdstat_get: _fd_fdstat_get,
    /** @export */ fd_pread: _fd_pread,
    /** @export */ fd_pwrite: _fd_pwrite,
    /** @export */ fd_read: _fd_read,
    /** @export */ fd_seek: _fd_seek,
    /** @export */ fd_sync: _fd_sync,
    /** @export */ fd_write: _fd_write,
    /** @export */ futimes: _futimes,
    /** @export */ invoke_i,
    /** @export */ invoke_ii,
    /** @export */ invoke_ij,
    /** @export */ invoke_iji,
    /** @export */ invoke_ijiij,
    /** @export */ invoke_ijijj,
    /** @export */ invoke_ijijjj,
    /** @export */ invoke_ijj,
    /** @export */ invoke_ijji,
    /** @export */ invoke_ijjj,
    /** @export */ invoke_ji,
    /** @export */ invoke_vj,
    /** @export */ invoke_vji,
    /** @export */ invoke_vjiijj,
    /** @export */ invoke_vjijj,
    /** @export */ invoke_vjj,
    /** @export */ invoke_vjji,
    /** @export */ memory: wasmMemory,
    /** @export */ proc_exit: _proc_exit,
    /** @export */ wasmCallFuncPtrTrampoline,
    /** @export */ wasmJitExecBlock,
    /** @export */ wasmJitSetRomBuffer
  };
}

function invoke_ijj(index, a1, a2) {
  var sp = stackSave();
  try {
    return getWasmTableEntry(Number(index))(a1, a2);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_ijji(index, a1, a2, a3) {
  var sp = stackSave();
  try {
    return getWasmTableEntry(Number(index))(a1, a2, a3);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_ijijjj(index, a1, a2, a3, a4, a5) {
  var sp = stackSave();
  try {
    return getWasmTableEntry(Number(index))(a1, a2, a3, a4, a5);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_ji(index, a1) {
  var sp = stackSave();
  try {
    return getWasmTableEntry(Number(index))(a1);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
    return 0n;
  }
}

function invoke_vjiijj(index, a1, a2, a3, a4, a5) {
  var sp = stackSave();
  try {
    getWasmTableEntry(Number(index))(a1, a2, a3, a4, a5);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_ij(index, a1) {
  var sp = stackSave();
  try {
    return getWasmTableEntry(Number(index))(a1);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_vji(index, a1, a2) {
  var sp = stackSave();
  try {
    getWasmTableEntry(Number(index))(a1, a2);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_vjijj(index, a1, a2, a3, a4) {
  var sp = stackSave();
  try {
    getWasmTableEntry(Number(index))(a1, a2, a3, a4);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_i(index) {
  var sp = stackSave();
  try {
    return getWasmTableEntry(Number(index))();
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_ijjj(index, a1, a2, a3) {
  var sp = stackSave();
  try {
    return getWasmTableEntry(Number(index))(a1, a2, a3);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_ijiij(index, a1, a2, a3, a4) {
  var sp = stackSave();
  try {
    return getWasmTableEntry(Number(index))(a1, a2, a3, a4);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_ii(index, a1) {
  var sp = stackSave();
  try {
    return getWasmTableEntry(Number(index))(a1);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_ijijj(index, a1, a2, a3, a4) {
  var sp = stackSave();
  try {
    return getWasmTableEntry(Number(index))(a1, a2, a3, a4);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_vjj(index, a1, a2) {
  var sp = stackSave();
  try {
    getWasmTableEntry(Number(index))(a1, a2);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_vj(index, a1) {
  var sp = stackSave();
  try {
    getWasmTableEntry(Number(index))(a1);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_iji(index, a1, a2) {
  var sp = stackSave();
  try {
    return getWasmTableEntry(Number(index))(a1, a2);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

function invoke_vjji(index, a1, a2, a3) {
  var sp = stackSave();
  try {
    getWasmTableEntry(Number(index))(a1, a2, a3);
  } catch (e) {
    stackRestore(sp);
    if (e !== e + 0) throw e;
    _setThrew(1, 0);
  }
}

// Argument name here must shadow the `wasmExports` global so
// that it is recognised by metadce and minify-import-export-names
// passes.
function applySignatureConversions(wasmExports) {
  // First, make a copy of the incoming exports object
  wasmExports = Object.assign({}, wasmExports);
  var makeWrapper___PP = f => (a0, a1, a2) => f(a0, BigInt(a1 ? a1 : 0), BigInt(a2 ? a2 : 0));
  var makeWrapper_p = f => () => Number(f());
  var makeWrapper_pp = f => a0 => Number(f(BigInt(a0)));
  var makeWrapper__p_____ = f => (a0, a1, a2, a3, a4, a5) => f(BigInt(a0), a1, a2, a3, a4, a5);
  var makeWrapper__pp_ = f => (a0, a1, a2) => f(BigInt(a0), BigInt(a1), a2);
  var makeWrapper___p_p_ = f => (a0, a1, a2, a3, a4) => f(a0, BigInt(a1), a2, BigInt(a3), a4);
  var makeWrapper__p = f => a0 => f(BigInt(a0));
  var makeWrapper__pp = f => (a0, a1) => f(BigInt(a0), BigInt(a1));
  wasmExports["__main_argc_argv"] = makeWrapper___PP(wasmExports["__main_argc_argv"]);
  wasmExports["pthread_self"] = makeWrapper_p(wasmExports["pthread_self"]);
  wasmExports["malloc"] = makeWrapper_pp(wasmExports["malloc"]);
  wasmExports["_emscripten_thread_init"] = makeWrapper__p_____(wasmExports["_emscripten_thread_init"]);
  wasmExports["_emscripten_run_js_on_main_thread_done"] = makeWrapper__pp_(wasmExports["_emscripten_run_js_on_main_thread_done"]);
  wasmExports["_emscripten_run_js_on_main_thread"] = makeWrapper___p_p_(wasmExports["_emscripten_run_js_on_main_thread"]);
  wasmExports["_emscripten_thread_free_data"] = makeWrapper__p(wasmExports["_emscripten_thread_free_data"]);
  wasmExports["_emscripten_thread_exit"] = makeWrapper__p(wasmExports["_emscripten_thread_exit"]);
  wasmExports["setThrew"] = makeWrapper__p(wasmExports["setThrew"]);
  wasmExports["emscripten_stack_set_limits"] = makeWrapper__pp(wasmExports["emscripten_stack_set_limits"]);
  wasmExports["_emscripten_stack_restore"] = makeWrapper__p(wasmExports["_emscripten_stack_restore"]);
  wasmExports["_emscripten_stack_alloc"] = makeWrapper_pp(wasmExports["_emscripten_stack_alloc"]);
  wasmExports["emscripten_stack_get_current"] = makeWrapper_p(wasmExports["emscripten_stack_get_current"]);
  return wasmExports;
}

// include: postamble.js
// === Auto-generated postamble setup entry stuff ===
function callMain(args = []) {
  var entryFunction = __emscripten_proxy_main;
  // With PROXY_TO_PTHREAD make sure we keep the runtime alive until the
  // proxied main calls exit (see exitOnMainThread() for where Pop is called).
  runtimeKeepalivePush();
  args.unshift(thisProgram);
  var argc = args.length;
  var argv = stackAlloc((argc + 1) * 8);
  var argv_ptr = argv;
  for (var arg of args) {
    (growMemViews(), HEAPU64)[((argv_ptr) / 8)] = BigInt(stringToUTF8OnStack(arg));
    argv_ptr += 8;
  }
  (growMemViews(), HEAPU64)[((argv_ptr) / 8)] = 0n;
  try {
    var ret = entryFunction(argc, BigInt(argv));
    // if we're not running an evented main loop, it's time to exit
    exitJS(ret, /* implicit = */ true);
    return ret;
  } catch (e) {
    return handleException(e);
  }
}

function run(args = arguments_) {
  if (runDependencies > 0) {
    dependenciesFulfilled = run;
    return;
  }
  if ((ENVIRONMENT_IS_PTHREAD)) {
    initRuntime();
    return;
  }
  preRun();
  // a preRun added a dependency, run will be called later
  if (runDependencies > 0) {
    dependenciesFulfilled = run;
    return;
  }
  function doRun() {
    // run may have just been called through dependencies being fulfilled just in this very frame,
    // or while the async setStatus time below was happening
    Module["calledRun"] = true;
    if (ABORT) return;
    initRuntime();
    preMain();
    Module["onRuntimeInitialized"]?.();
    var noInitialRun = Module["noInitialRun"] || true;
    if (!noInitialRun) callMain(args);
    postRun();
  }
  if (Module["setStatus"]) {
    Module["setStatus"]("Running...");
    setTimeout(() => {
      setTimeout(() => Module["setStatus"](""), 1);
      doRun();
    }, 1);
  } else {
    doRun();
  }
}

var wasmExports;

if ((!(ENVIRONMENT_IS_PTHREAD))) {
  // Call createWasm on startup if we are the main thread.
  // Worker threads call this once they receive the module via postMessage
  // With async instantation wasmExports is assigned asynchronously when the
  // instance is received.
  createWasm();
  run();
}
