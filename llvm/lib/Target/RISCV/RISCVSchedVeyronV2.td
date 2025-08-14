//==- RISCVSchedVeyronV2.td - Veyron V2 Scheduling Definitions --*- tablegen -*-=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//

class VeyronV2GetCyclesDefault<string mx> {
  int c = !cond(
    !eq(mx, "M1") : 1,
    !eq(mx, "M2") : 2,
    !eq(mx, "M4") : 4,
    !eq(mx, "M8") : 8,
    !eq(mx, "MF2") : 1,
    !eq(mx, "MF4") : 1,
    !eq(mx, "MF8") : 1
  );
}

class VeyronV2GetCyclesMergeGather<string mx> {
  int c = !cond(
    !eq(mx, "M1") : 1,
    !eq(mx, "M2") : 2,
    !eq(mx, "M4") : 8,
    !eq(mx, "M8") : 32,
    !eq(mx, "MF2") : 1,
    !eq(mx, "MF4") : 1,
    !eq(mx, "MF8") : 1
  );
}

class VeyronV2GetCyclesVIDivSqrtFactor<int sew> {
  int c = !cond(
    !eq(sew, 8) : 10,
    !eq(sew, 16) : 7,
    !eq(sew, 32) : 10,
    !eq(sew, 64) : 19
  );
}

class VeyronV2GetCyclesVFDivSqrtFactor<int sew> {
  int c = !cond(
    !eq(sew, 16) : 7,
    !eq(sew, 32) : 10,
    !eq(sew, 64) : 17
  );
}



class VeyronV2IsWorstCaseMX<string mx, list<string> MxList> {
  defvar LLMUL = LargestLMUL<MxList>.r;
  bit c = !eq(mx, LLMUL);
}

// TODO: Check if this is indeed the worst case, this assumes that the
// largest LMUL with the smallest SEW is the worst case
// If it's the largest LMUL with largest SEW, need helper function in
// RISCVScheduleV.td, the helper function may also need to be
// adjusted for widening
class VeyronV2IsWorstCaseMXSEW<string mx, int sew, list<string> MxList,
                               bit isF = 0> {
  defvar LLMUL = LargestLMUL<MxList>.r;
  defvar SSEW = SmallestSEW<mx, isF>.r;
  bit c = !and(!eq(mx, LLMUL), !eq(sew, SSEW));
}

def VentanaVeyronV2Model : SchedMachineModel {
  let MicroOpBufferSize = 160;
  let LoopMicroOpBufferSize = 50;
  let IssueWidth = 6;
  let LoadLatency = 6;
  let MispredictPenalty = 20;
  let CompleteModel = 0;
}

let SchedModel = VentanaVeyronV2Model in {
def VeyronV2ALU : ProcResource<4>;
def VeyronV2MultiCycleALU : ProcResource<1>;
def VeyronV2LSU : ProcResource<3>;
def VeyronV2FPU : ProcResource<2>;
def VeyronV2FDU : ProcResource<1>;
def VeyronV2UnitFPDivSqrt : ProcResource<1> { let Super = VeyronV2FPU; }
// Vector Units
def VeyronV2VALU : ProcResource<1>;
def VeyronV2VFPU : ProcResource<1>;
def VeyronV2VPerm : ProcResource<1>;
def VeyronV2VMask : ProcResource<1>;
let BufferSize = 0 in {
def VeyronV2VLSU : ProcResource<1>;
def VeyronV2VLoad : ProcResource<1> { let Super = VeyronV2VLSU; }
def VeyronV2VStore : ProcResource<1> { let Super = VeyronV2VLSU; }
def VeyronV2Vwadaccu: ProcResource<1>;
}

// Branching
def : WriteRes<WriteJmp, [VeyronV2ALU]>;
def : WriteRes<WriteJal, [VeyronV2ALU]>;
def : WriteRes<WriteJalr, [VeyronV2ALU]>;

//TODO:
//Short forward branch
let Unsupported = true in {
def : WriteRes<WriteSFB, [VeyronV2ALU]>;
}

// Integer arithmetic and logic
let Latency = 1, ReleaseAtCycles = [1] in {
def : WriteRes<WriteIALU, [VeyronV2ALU]>;
def : WriteRes<WriteIALU32, [VeyronV2ALU]>;
def : WriteRes<WriteShiftImm, [VeyronV2ALU]>;
def : WriteRes<WriteShiftImm32, [VeyronV2ALU]>;
def : WriteRes<WriteShiftReg, [VeyronV2ALU]>;
def : WriteRes<WriteShiftReg32, [VeyronV2ALU]>;
def : WriteRes<WriteSHXADD, [VeyronV2ALU]>;
def : WriteRes<WriteSHXADD32, [VeyronV2ALU]>;
def : WriteRes<WriteRotateReg, [VeyronV2ALU]>;
def : WriteRes<WriteRotateReg32, [VeyronV2ALU]>;
def : WriteRes<WriteRotateImm, [VeyronV2ALU]>;
def : WriteRes<WriteRotateImm32, [VeyronV2ALU]>;

def : WriteRes<WriteSingleBit, [VeyronV2ALU]>;
def : WriteRes<WriteSingleBitImm, [VeyronV2ALU]>;

def : WriteRes<WriteREV8, [VeyronV2ALU]>;
def : WriteRes<WriteORCB, [VeyronV2ALU]>;

def : WriteRes<WriteCLZ, [VeyronV2MultiCycleALU]>;
def : WriteRes<WriteCLZ32, [VeyronV2MultiCycleALU]>;
def : WriteRes<WriteCTZ, [VeyronV2MultiCycleALU]>;
def : WriteRes<WriteCTZ32, [VeyronV2MultiCycleALU]>;

def : WriteRes<WriteBEXT, [VeyronV2ALU]>;
def : WriteRes<WriteBEXTI, [VeyronV2ALU]>;

def : WriteRes<WriteIMinMax, [VeyronV2ALU]>;
}

// Integer multiplication
let Latency = 2, ReleaseAtCycles = [2] in {
def : WriteRes<WriteIMul, [VeyronV2MultiCycleALU]>;
def : WriteRes<WriteIMul32, [VeyronV2MultiCycleALU]>;
def : WriteRes<WriteCLMUL, [VeyronV2MultiCycleALU]>;
}

// Integer division
// Worst case latency is used.
// Formula is: Quotient bits/4 + 3
let Latency = 19, ReleaseAtCycles = [19] in {
def : WriteRes<WriteIDiv, [VeyronV2MultiCycleALU]>;
def : WriteRes<WriteIRem, [VeyronV2MultiCycleALU]>;
}

let Latency = 10, ReleaseAtCycles = [10] in {
def : WriteRes<WriteIDiv32, [VeyronV2MultiCycleALU]>;
def : WriteRes<WriteIRem32, [VeyronV2MultiCycleALU]>;
}

// Multi-cycle ALU instructions
let Latency = 2, ReleaseAtCycles = [2] in {
def : WriteRes<WriteCPOP, [VeyronV2MultiCycleALU]>;
def : WriteRes<WriteCPOP32, [VeyronV2MultiCycleALU]>;
}

// Memory
let Latency = 4, ReleaseAtCycles = [1] in { // TODO: do these use ALU?
def : WriteRes<WriteSTB, [VeyronV2LSU]>;
def : WriteRes<WriteSTH, [VeyronV2LSU]>;
def : WriteRes<WriteSTW, [VeyronV2LSU]>;
def : WriteRes<WriteSTD, [VeyronV2LSU]>;
def : WriteRes<WriteFST32, [VeyronV2LSU]>;
def : WriteRes<WriteFST64, [VeyronV2LSU]>;
}

let Latency = 4, ReleaseAtCycles = [1] in {
def : WriteRes<WriteLDB, [VeyronV2LSU]>;
def : WriteRes<WriteLDH, [VeyronV2LSU]>;
def : WriteRes<WriteLDW, [VeyronV2LSU]>;
def : WriteRes<WriteLDD, [VeyronV2LSU]>;
}

let Latency = 6, ReleaseAtCycles = [1] in {
def : WriteRes<WriteFLD32, [VeyronV2LSU]>;
def : WriteRes<WriteFLD64, [VeyronV2LSU]>;
}

// TODO: Check this. Should these also use the ALU unit?
let Latency = 2, ReleaseAtCycles = [1] in {
def : WriteRes<WriteAtomicW, [VeyronV2LSU]>;
def : WriteRes<WriteAtomicD, [VeyronV2LSU]>;
def : WriteRes<WriteAtomicLDW, [VeyronV2LSU]>;
def : WriteRes<WriteAtomicLDD, [VeyronV2LSU]>;
def : WriteRes<WriteAtomicSTW, [VeyronV2LSU]>;
def : WriteRes<WriteAtomicSTD, [VeyronV2LSU]>;
}

// FP arithmetic
let Latency = 3, ReleaseAtCycles = [1] in {
def : WriteRes<WriteFAdd32, [VeyronV2FPU]>;
def : WriteRes<WriteFMul32, [VeyronV2FPU]>;
def : WriteRes<WriteFAdd64, [VeyronV2FPU]>;
def : WriteRes<WriteFMul64, [VeyronV2FPU]>;
}

let Latency = 2, ReleaseAtCycles = [1] in {
def : WriteRes<WriteFMinMax32, [VeyronV2FPU]>;
def : WriteRes<WriteFMinMax64, [VeyronV2FPU]>;
def : WriteRes<WriteFSGNJ32, [VeyronV2FPU]>;
def : WriteRes<WriteFSGNJ64, [VeyronV2FPU]>;
}

// FPU can issue a new op every cycle but for long latency
// ops, is it iterative and should ReleaseAtCycles be adjusted?
let Latency = 5, ReleaseAtCycles = [1] in {
def : WriteRes<WriteFMA32, [VeyronV2FPU]>;
def : WriteRes<WriteFMA64, [VeyronV2FPU]>;
}

// Use Worst case
// Min = 6, Max = 11
let Latency = 11, ReleaseAtCycles = [11] in {
def : WriteRes<WriteFDiv32, [VeyronV2FPU]>;
def : WriteRes<WriteFSqrt32, [VeyronV2FPU]>;
}

// TODO: check this again.
let Latency = 19, ReleaseAtCycles = [19] in {
def : WriteRes<WriteFDiv64, [VeyronV2FPU]>;
def : WriteRes<WriteFSqrt64, [VeyronV2FPU]>;
}

let Latency = 2, ReleaseAtCycles = [1] in {
def : WriteRes<WriteFCmp32, [VeyronV2FPU]>;
def : WriteRes<WriteFCmp64, [VeyronV2FPU]>;
}

// Conversions
let Latency = 2, ReleaseAtCycles = [1] in {
def : WriteRes<WriteFCvtI32ToF32, [VeyronV2FPU]>;
def : WriteRes<WriteFCvtI32ToF64, [VeyronV2FPU]>;
def : WriteRes<WriteFCvtI64ToF32, [VeyronV2FPU]>;
def : WriteRes<WriteFCvtI64ToF64, [VeyronV2FPU]>;
def : WriteRes<WriteFCvtF32ToI32, [VeyronV2FDU]>;
def : WriteRes<WriteFCvtF32ToI64, [VeyronV2FDU]>;
def : WriteRes<WriteFCvtF32ToF64, [VeyronV2FPU]>;
def : WriteRes<WriteFCvtF64ToI32, [VeyronV2FDU]>;
def : WriteRes<WriteFCvtF64ToI64, [VeyronV2FDU]>;
def : WriteRes<WriteFCvtF64ToF32, [VeyronV2FPU]>;
def : WriteRes<WriteFClass32, [VeyronV2FPU]>;
def : WriteRes<WriteFClass64, [VeyronV2FPU]>;
}

// TODO: check this again.
let Latency = 2, ReleaseAtCycles = [1] in {
def : WriteRes<WriteFMovI32ToF32, [VeyronV2FPU]>;
def : WriteRes<WriteFMovF32ToI32, [VeyronV2FDU]>;
def : WriteRes<WriteFMovI64ToF64, [VeyronV2FPU]>;
def : WriteRes<WriteFMovF64ToI64, [VeyronV2FDU]>;
}
// Others
def : WriteRes<WriteCSR, []>;
def : WriteRes<WriteNop, []>;



// TODO: Why are all bypasses 0?
// Bypasses (none)
def : ReadAdvance<ReadJmp, 0>;
def : ReadAdvance<ReadJalr, 0>;
def : ReadAdvance<ReadCSR, 0>;
def : ReadAdvance<ReadStoreData, 0>;
def : ReadAdvance<ReadMemBase, 0>;
def : ReadAdvance<ReadIALU, 0>;
def : ReadAdvance<ReadIALU32, 0>;
def : ReadAdvance<ReadShiftImm, 0>;
def : ReadAdvance<ReadShiftImm32, 0>;
def : ReadAdvance<ReadShiftReg, 0>;
def : ReadAdvance<ReadShiftReg32, 0>;
def : ReadAdvance<ReadSHXADD, 0>;
def : ReadAdvance<ReadSHXADD32, 0>;
def : ReadAdvance<ReadRotateReg, 0>;
def : ReadAdvance<ReadRotateReg32, 0>;
def : ReadAdvance<ReadRotateImm, 0>;
def : ReadAdvance<ReadRotateImm32, 0>;
def : ReadAdvance<ReadIMinMax, 0>;

def : ReadAdvance<ReadSingleBit, 0>;
def : ReadAdvance<ReadSingleBitImm, 0>;

def : ReadAdvance<ReadREV8, 0>;
def : ReadAdvance<ReadORCB, 0>;
def : ReadAdvance<ReadIDiv, 0>;
def : ReadAdvance<ReadIDiv32, 0>;
def : ReadAdvance<ReadIRem, 0>;
def : ReadAdvance<ReadIRem32, 0>;
def : ReadAdvance<ReadIMul, 0>;
def : ReadAdvance<ReadIMul32, 0>;
def : ReadAdvance<ReadCLMUL, 0>;

def : ReadAdvance<ReadCLZ, 0>;
def : ReadAdvance<ReadCLZ32, 0>;
def : ReadAdvance<ReadCTZ, 0>;
def : ReadAdvance<ReadCTZ32, 0>;
def : ReadAdvance<ReadCPOP, 0>;
def : ReadAdvance<ReadCPOP32, 0>;

def : ReadAdvance<ReadAtomicWA, 0>;
def : ReadAdvance<ReadAtomicWD, 0>;
def : ReadAdvance<ReadAtomicDA, 0>;
def : ReadAdvance<ReadAtomicDD, 0>;
def : ReadAdvance<ReadAtomicLDW, 0>;
def : ReadAdvance<ReadAtomicLDD, 0>;
def : ReadAdvance<ReadAtomicSTW, 0>;
def : ReadAdvance<ReadAtomicSTD, 0>;
def : ReadAdvance<ReadFStoreData, 0>;
def : ReadAdvance<ReadFMemBase, 0>;
def : ReadAdvance<ReadFAdd32, 0>;
def : ReadAdvance<ReadFAdd64, 0>;
def : ReadAdvance<ReadFMul32, 0>;
def : ReadAdvance<ReadFMul64, 0>;
def : ReadAdvance<ReadFMA32, 0>;
def : ReadAdvance<ReadFMA32Addend, 0>;
def : ReadAdvance<ReadFMA64, 0>;
def : ReadAdvance<ReadFMA64Addend, 0>;
def : ReadAdvance<ReadFDiv32, 0>;
def : ReadAdvance<ReadFDiv64, 0>;
def : ReadAdvance<ReadFSqrt32, 0>;
def : ReadAdvance<ReadFSqrt64, 0>;
def : ReadAdvance<ReadFCmp32, 0>;
def : ReadAdvance<ReadFCmp64, 0>;
def : ReadAdvance<ReadFSGNJ32, 0>;
def : ReadAdvance<ReadFSGNJ64, 0>;
def : ReadAdvance<ReadFMinMax32, 0>;
def : ReadAdvance<ReadFMinMax64, 0>;
def : ReadAdvance<ReadFCvtF32ToI32, 0>;
def : ReadAdvance<ReadFCvtF32ToI64, 0>;
def : ReadAdvance<ReadFCvtF64ToI32, 0>;
def : ReadAdvance<ReadFCvtF64ToI64, 0>;
def : ReadAdvance<ReadFCvtI32ToF32, 0>;
def : ReadAdvance<ReadFCvtI32ToF64, 0>;
def : ReadAdvance<ReadFCvtI64ToF32, 0>;
def : ReadAdvance<ReadFCvtI64ToF64, 0>;
def : ReadAdvance<ReadFCvtF32ToF64, 0>;
def : ReadAdvance<ReadFCvtF64ToF32, 0>;
def : ReadAdvance<ReadFMovF32ToI32, 0>;
def : ReadAdvance<ReadFMovI32ToF32, 0>;
def : ReadAdvance<ReadFMovF64ToI64, 0>;
def : ReadAdvance<ReadFMovI64ToF64, 0>;
def : ReadAdvance<ReadFClass32, 0>;
def : ReadAdvance<ReadFClass64, 0>;
def : ReadAdvance<ReadSFBJmp, 0>;
def : ReadAdvance<ReadSFBALU, 0>;

// TODO: Define true latencies.
// 3.6 Vector Byte Length vlenb
def : WriteRes<WriteRdVLENB, []>;

// 6. Configuration-Setting Instructions
let Latency = 2, ReleaseAtCycles = [2] in {
def : WriteRes<WriteVSETVLI, [VeyronV2MultiCycleALU]>;
def : WriteRes<WriteVSETIVLI, [VeyronV2MultiCycleALU]>;
def : WriteRes<WriteVSETVL, [VeyronV2MultiCycleALU]>;
}

// 7. Vector Loads and Stores
// vector ld/st are 6 cycles best case
// TODO: fix and find latencies for indexed, etc.. ld/st
let Latency = 7, ReleaseAtCycles = [1, 1] in {
defm "" : LMULWriteRes<"WriteVLDE", [VeyronV2VLoad, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTE", [VeyronV2VStore, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDM", [VeyronV2VLoad, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTM", [VeyronV2VStore, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDS8", [VeyronV2VLoad, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDS16", [VeyronV2VLoad, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDS32", [VeyronV2VLoad, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDS64", [VeyronV2VLoad, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTS8", [VeyronV2VStore, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTS16", [VeyronV2VStore, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTS32", [VeyronV2VStore, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTS64", [VeyronV2VStore, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDUX8", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDUX16", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDUX32", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDUX64", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDOX8", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDOX16", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDOX32", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDOX64", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTUX8", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTUX16", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTUX32", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTUX64", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTOX8", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTOX16", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTOX32", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVSTOX64", [VeyronV2VLSU, VeyronV2LSU]>;
defm "" : LMULWriteRes<"WriteVLDFF", [VeyronV2VLSU, VeyronV2LSU]>;
}
let Latency = 1, ReleaseAtCycles = [1] in {
// These are already LMUL aware
def : WriteRes<WriteVLD1R, [VeyronV2VPerm]>;
def : WriteRes<WriteVLD2R, [VeyronV2VPerm]>;
def : WriteRes<WriteVLD4R, [VeyronV2VPerm]>;
def : WriteRes<WriteVLD8R, [VeyronV2VPerm]>;
def : WriteRes<WriteVST1R, [VeyronV2VPerm]>;
def : WriteRes<WriteVST2R, [VeyronV2VPerm]>;
def : WriteRes<WriteVST4R, [VeyronV2VPerm]>;
def : WriteRes<WriteVST8R, [VeyronV2VPerm]>;
}

// Vector Segment Loads and Stores
// Latency = 6 + EMUL * NFIELDS^2
// TODO: Fix Latency calculation, EMUL differs for data/idx, just
// setting Latency to nf*nf*LMUL for now, check LSU resource cycles
// for segmented ld/st, should probably be considered complex and so
// is most likely not 1
foreach mx = SchedMxList in {
  foreach eew = [8, 16, 32, 64] in {
    foreach nf=2-8 in {
      defvar Cycles = !mul(!mul(nf, nf), VeyronV2GetCyclesDefault<mx>.c);
      defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxList>.c;
      let Latency = 7, ReleaseAtCycles = [Cycles, 1] in {
        defm "" : LMULWriteResMX <"WriteVLSEG" # nf # "e" # eew, [VeyronV2VLSU, VeyronV2LSU], mx, IsWorstCase>;
        defm "" : LMULWriteResMX <"WriteVLSEGFF" # nf # "e" # eew, [VeyronV2VLSU, VeyronV2LSU], mx, IsWorstCase>;
        defm "" : LMULWriteResMX <"WriteVSSEG" # nf # "e" # eew, [VeyronV2VLSU, VeyronV2LSU], mx, IsWorstCase>;
        defm "" : LMULWriteResMX <"WriteVLSSEG" # nf # "e" # eew, [VeyronV2VLSU, VeyronV2LSU], mx, IsWorstCase>;
        defm "" : LMULWriteResMX <"WriteVSSSEG" # nf # "e" # eew, [VeyronV2VLSU, VeyronV2LSU], mx, IsWorstCase>;
        defm "" : LMULWriteResMX <"WriteVLUXSEG" # nf # "e" # eew, [VeyronV2VLSU, VeyronV2LSU], mx, IsWorstCase>;
        defm "" : LMULWriteResMX <"WriteVLOXSEG" # nf # "e" # eew, [VeyronV2VLSU, VeyronV2LSU], mx, IsWorstCase>;
        defm "" : LMULWriteResMX <"WriteVSUXSEG" # nf # "e" # eew, [VeyronV2VLSU, VeyronV2LSU], mx, IsWorstCase>;
        defm "" : LMULWriteResMX <"WriteVSOXSEG" # nf # "e" # eew, [VeyronV2VLSU, VeyronV2LSU], mx, IsWorstCase>;
      }
    }
  }
}

// 11. Vector Integer Arithmetic Instructions
foreach mx = SchedMxList in {
  defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
  defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxList>.c;
  let Latency = 1, ReleaseAtCycles = [Cycles] in {
    defm "" : LMULWriteResMX<"WriteVIALUV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIALUX", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIALUI", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVICALUV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVICALUX", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVICALUI", [VeyronV2VALU], mx, IsWorstCase>;

    // added due to compile errors:
    // Processor does not define resources for WriteVICALUMI_M1
    // Processor does not define resources for WriteVICALUMV_M1
    // Processor does not define resources for WriteVICALUMX_M1
    defm "" : LMULWriteResMX<"WriteVICALUMI", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVICALUMV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVICALUMX", [VeyronV2VALU], mx, IsWorstCase>;

    defm "" : LMULWriteResMX<"WriteVShiftV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVShiftX", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVShiftI", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIMinMaxV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIMinMaxX", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVICmpV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVICmpX", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVICmpI", [VeyronV2VALU], mx, IsWorstCase>;
  }
}

// TODO: Are cycles different for widening/narrowing?
foreach mx = SchedMxListW in {
  defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
  defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxListW>.c;
  let Latency = 1, ReleaseAtCycles = [Cycles] in {
    defm "" : LMULWriteResMX<"WriteVIWALUV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIWALUX", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIWALUI", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVNShiftV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVNShiftX", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVNShiftI", [VeyronV2VALU], mx, IsWorstCase>;
  }
}

foreach mx = SchedMxList in {
  defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
  defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxList>.c;
  let Latency = 3, ReleaseAtCycles = [Cycles] in {
    defm "" : LMULWriteResMX<"WriteVIMulV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIMulX", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIMulAddV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIMulAddX", [VeyronV2VALU], mx, IsWorstCase>;
  }
}

foreach mx = SchedMxListW in {
  defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
  defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxListW>.c;
  let Latency = 3, ReleaseAtCycles = [Cycles] in {
    defm "" : LMULWriteResMX<"WriteVIWMulV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIWMulX", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIWMulAddV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIWMulAddX", [VeyronV2VALU], mx, IsWorstCase>;
  }
}

foreach mx = SchedMxList in {
  foreach sew = SchedSEWSet<mx>.val in {
    defvar Cycles = !mul(VeyronV2GetCyclesDefault<mx>.c,
                         VeyronV2GetCyclesVIDivSqrtFactor<sew>.c);
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxList>.c;
    let Latency = VeyronV2GetCyclesVIDivSqrtFactor<sew>.c, ReleaseAtCycles = [Cycles] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVIDivV", [VeyronV2VPerm], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVIDivX", [VeyronV2VPerm], mx, sew, IsWorstCase>;
    }
  }
}

// 12. Vector Fixed-Point Arithmetic Instructions
defm "" : LMULWriteRes<"WriteVSALUV", []>;
defm "" : LMULWriteRes<"WriteVSALUX", []>;
defm "" : LMULWriteRes<"WriteVSALUI", []>;
defm "" : LMULWriteRes<"WriteVAALUV", []>;
defm "" : LMULWriteRes<"WriteVAALUX", []>;
defm "" : LMULWriteRes<"WriteVSMulV", []>;
defm "" : LMULWriteRes<"WriteVSMulX", []>;
defm "" : LMULWriteRes<"WriteVSShiftV", []>;
defm "" : LMULWriteRes<"WriteVSShiftX", []>;
defm "" : LMULWriteRes<"WriteVSShiftI", []>;
defm "" : LMULWriteResW<"WriteVNClipV", []>;
defm "" : LMULWriteResW<"WriteVNClipX", []>;
defm "" : LMULWriteResW<"WriteVNClipI", []>;

// 13. Vector Floating-Point Instructions
// 3 cycle latency per vector reg, 1 cycle throughput
// LMUL+2 cycles instruction latency
foreach mx = SchedMxListF in {
  foreach sew = SchedSEWSet<mx, isF=1>.val in {
    defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxListF, isF=1>.c;
    let Latency = 3, ReleaseAtCycles = [!mul(Cycles, 3)] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVFALUV", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFALUF", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFMulV", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFMulF", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFMulAddV", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFMulAddF", [VeyronV2VALU], mx, sew, IsWorstCase>;
    }
  }
}

foreach mx = SchedMxListFW in {
  foreach sew = SchedSEWSet<mx, isF=1, isWidening=1>.val in {
    defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxListFW, isF=1>.c;
    let Latency = 3, ReleaseAtCycles = [!mul(Cycles, 3)] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVFWALUV", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFWALUF", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFWMulV", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFWMulF", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFWMulAddV", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFWMulAddF", [VeyronV2VALU], mx, sew, IsWorstCase>;
    }
  }
}

// Some converts may take 2 cycles
foreach mx = SchedMxListF in {
  foreach sew = SchedSEWSet<mx, isF=1>.val in {
    defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxListF, isF=1>.c;
    let Latency = 1, ReleaseAtCycles = [Cycles] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVFCvtIToFV", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFSgnjV", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFSgnjF", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFMinMaxV", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFMinMaxF", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFRecpV", [VeyronV2VALU], mx, sew, IsWorstCase>;
    }
  }
}

foreach mx = SchedMxListF in {
  defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
  defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxListF>.c;
  let Latency = 1, ReleaseAtCycles = [Cycles] in {
    defm "" : LMULWriteResMX<"WriteVFCvtFToIV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVFCmpV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVFCmpF", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVFClassV", [VeyronV2VALU], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVFMovV", [VeyronV2VALU], mx, IsWorstCase>;
  }
}

foreach mx = SchedMxListW in {
  foreach sew = SchedSEWSet<mx, isF=0, isWidening=1>.val in {
    defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxListW>.c;
    let Latency = 1, ReleaseAtCycles = [Cycles] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVFWCvtIToFV", [VeyronV2VALU], mx, sew, IsWorstCase>;
    }
  }
}

foreach mx = SchedMxListFW in {
  foreach sew = SchedSEWSet<mx, isF=1, isWidening=1>.val in {
    defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxListFW, isF=1>.c;
    let Latency = 1, ReleaseAtCycles = [Cycles] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVFNCvtIToFV", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFNCvtFToFV", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFWCvtFToFV", [VeyronV2VALU], mx, sew, IsWorstCase>;
    }
  }
}

foreach mx = SchedMxListFW in {
  defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
  defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxListFW>.c;
  let Latency = 1, ReleaseAtCycles = [Cycles] in {
    defm "" : LMULWriteResMX<"WriteVFWCvtFToIV", [VeyronV2VALU], mx, IsWorstCase>;
  }
}

// Some converts may take 2 cycles
foreach mx = SchedMxListW in {
  defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
  defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxListW>.c;
  let Latency = 1, ReleaseAtCycles = [Cycles] in {
    defm "" : LMULWriteResMX<"WriteVFNCvtFToIV", [VeyronV2VALU], mx, IsWorstCase>;
  }
}

foreach mx = SchedMxListF in {
  foreach sew = SchedSEWSet<mx, isF=1>.val in {
    defvar Cycles = !mul(VeyronV2GetCyclesDefault<mx>.c,
                         VeyronV2GetCyclesVFDivSqrtFactor<sew>.c);
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxListF, 1>.c;
    let Latency = Cycles, ReleaseAtCycles = [Cycles] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVFDivV", [VeyronV2VPerm], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFDivF", [VeyronV2VPerm], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFSqrtV", [VeyronV2VPerm], mx, sew, IsWorstCase>;
    }
  }
}

// 14. Vector Reduction Operations
// cycles = LMUL + log2(512/SEW)
// assuming LMUL = 1 and SEW = 64 for now, so latency = 4
foreach mx = SchedMxList in {
  foreach sew = SchedSEWSet<mx>.val in {
    defvar Cycles = !add(VeyronV2GetCyclesDefault<mx>.c, !logtwo(!div(512, sew)));
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxList>.c;
    let Latency = !logtwo(!div(512, sew)), ReleaseAtCycles = [Cycles] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVIRedV_From", [VeyronV2VALU], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVIRedMinMaxV_From", [VeyronV2ALU], mx, sew, IsWorstCase>;
    }
  }
}

foreach mx = SchedMxListWRed in {
  foreach sew = SchedSEWSet<mx, 0, 1>.val in {
    defvar Cycles = !mul(!add(VeyronV2GetCyclesDefault<mx>.c, !logtwo(!div(256, sew))), 2);
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxListWRed>.c;
    let Latency = !logtwo(!div(512, sew)), ReleaseAtCycles = [Cycles] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVIWRedV_From", [VeyronV2VALU], mx, sew, IsWorstCase>;
    }
  }
}

foreach mx = SchedMxListF in {
  foreach sew = SchedSEWSet<mx, 1>.val in {
    defvar Cycles = !add(VeyronV2GetCyclesDefault<mx>.c, !logtwo(!div(512, sew)));
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxListF, 1>.c;
    let Latency = !logtwo(!div(512, sew)), ReleaseAtCycles = [Cycles] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVFRedV_From", [VeyronV2VALU],
                                     mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVFRedMinMaxV_From", [VeyronV2VALU],
                                     mx, sew, IsWorstCase>;
    }
  }
}

// Ordered reductions require VL dependent add operations
// for example: 128 operations for SEW=32, LMUL=8
// setting latency to "sew" since LMUL=1 for default latency case
foreach mx = SchedMxListF in {
  foreach sew = SchedSEWSet<mx, 1>.val in {
    defvar Cycles = !mul(VeyronV2GetCyclesDefault<mx>.c, sew);
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxListF, 1>.c;
    let Latency = sew, ReleaseAtCycles = [Cycles] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVFRedOV_From", [VeyronV2VALU],
                                      mx, sew, IsWorstCase>;
    }
  }
}

foreach mx = SchedMxListFWRed in {
  foreach sew = SchedSEWSet<mx, 1, 1>.val in {
    defvar Cycles = !mul(!add(VeyronV2GetCyclesDefault<mx>.c, !logtwo(!div(256, sew))), 2);
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxListFWRed, 1>.c;
    let Latency = !logtwo(!div(512, sew)), ReleaseAtCycles = [Cycles] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVFWRedV_From", [VeyronV2VALU], mx, sew, IsWorstCase>;
    }
  }
}

foreach mx = SchedMxListFWRed in {
  foreach sew = SchedSEWSet<mx, 1, 1>.val in {
    defvar Cycles = !mul(VeyronV2GetCyclesDefault<mx>.c, sew);
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxListFWRed, 1>.c;
    let Latency = sew, ReleaseAtCycles = [Cycles] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVFWRedOV_From", [VeyronV2VALU], mx, sew, IsWorstCase>;
    }
  }
}

// 15. Vector Mask Instructions
foreach mx = SchedMxList in {
  defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
  defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxList>.c;
  let Latency = 1, ReleaseAtCycles = [Cycles] in {
    defm "" : LMULWriteResMX<"WriteVMALUV", [VeyronV2VMask], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVMFFSV", [VeyronV2VMask], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVMSFSV", [VeyronV2VMask], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIdxV", [VeyronV2VMask], mx, IsWorstCase>;
  }
}

foreach mx = SchedMxList in {
  defvar Cycles = !mul(VeyronV2GetCyclesDefault<mx>.c, 2);
  defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxList>.c;
  let Latency = 2, ReleaseAtCycles = [Cycles] in {
    defm "" : LMULWriteResMX<"WriteVMPopV", [VeyronV2VMask], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIotaV", [VeyronV2VMask], mx, IsWorstCase>;
  }
}

// 16. Vector Permutation Instructions
let Latency = 1, ReleaseAtCycles = [1] in {
def : WriteRes<WriteVMovSX, [VeyronV2VPerm]>;
def : WriteRes<WriteVMovXS, [VeyronV2VPerm]>;
def : WriteRes<WriteVMovSF, [VeyronV2VPerm]>;
def : WriteRes<WriteVMovFS, [VeyronV2VPerm]>;
}

foreach mx = SchedMxList in {
  defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
  defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxList>.c;
  let Latency = 1, ReleaseAtCycles = [Cycles] in {
    defm "" : LMULWriteResMX<"WriteVIMovV", [VeyronV2VPerm], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIMovX", [VeyronV2VPerm], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIMovI", [VeyronV2VPerm], mx, IsWorstCase>;
  }
}

// VMov*V is LMUL aware
let Latency = 1, ReleaseAtCycles = [1] in
  def : WriteRes<WriteVMov1V, [VeyronV2VPerm]>;
let Latency = 1, ReleaseAtCycles = [2] in
  def : WriteRes<WriteVMov2V, [VeyronV2VPerm]>;
let Latency = 1, ReleaseAtCycles = [4] in
  def : WriteRes<WriteVMov4V, [VeyronV2VPerm]>;
let Latency = 1, ReleaseAtCycles = [8] in
  def : WriteRes<WriteVMov8V, [VeyronV2VPerm]>;

foreach mx = SchedMxList in {
  defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
  defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxList>.c;
  let Latency = 1, ReleaseAtCycles = [Cycles] in {
    defm "" : LMULWriteResMX<"WriteVSlideUpX", [VeyronV2VPerm], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVSlideDownX", [VeyronV2VPerm], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVSlideI", [VeyronV2VPerm], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVISlide1X", [VeyronV2VPerm], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVFSlide1F", [VeyronV2VPerm], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVExtV", [VeyronV2VPerm], mx, IsWorstCase>;
  }
}

// Commented this section due to the following error:
//  /home/dbarboza/work/llvm-project/llvm/lib/Target/RISCV/RISCVScheduleV.td:64:3: error: WriteType already used in another WriteRes
//  def : WriteRes<!cast<SchedWrite>(name # "_" # mx # "_E" # sew), resources>;
//  ^
// Included from /home/dbarboza/work/llvm-project/llvm/lib/Target/RISCV/RISCV.td:61:
// /home/dbarboza/work/llvm-project/llvm/lib/Target/RISCV/RISCVSchedVeyronV2.td:787:17: note: instantiated from multiclass
//      defm "" : LMULSEWWriteResMXSEW<"WriteVCompressV", [VeyronV2VPerm], mx, sew, IsWorstCase>;
//
// This seems to indicate that WriteVCompressV being declared here
// is being duplicated in the block that follows.

//foreach mx = SchedMxList in {
//  foreach sew = SchedSEWSet<mx>.val in {
//    defvar Cycles = VeyronV2GetCyclesDefault<mx>.c;
//    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxList>.c;
//    let Latency = 1, ReleaseAtCycles = [Cycles] in {
//      defm "" : LMULSEWWriteResMXSEW<"WriteVCompressV", [VeyronV2VPerm], mx, sew, IsWorstCase>;
//    }
//  }
//}

foreach mx = SchedMxList in {
  foreach sew = SchedSEWSet<mx>.val in {
    defvar Cycles = VeyronV2GetCyclesMergeGather<mx>.c;
    defvar IsWorstCase = VeyronV2IsWorstCaseMXSEW<mx, sew, SchedMxList>.c;
    let Latency = 1, ReleaseAtCycles = [Cycles] in {
      defm "" : LMULSEWWriteResMXSEW<"WriteVRGatherVV", [VeyronV2VPerm], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVRGatherEI16VV", [VeyronV2VPerm], mx, sew, IsWorstCase>;
      defm "" : LMULSEWWriteResMXSEW<"WriteVCompressV", [VeyronV2VPerm], mx, sew, IsWorstCase>;
    }
  }
}

foreach mx = SchedMxList in {
  defvar Cycles = VeyronV2GetCyclesMergeGather<mx>.c;
  defvar IsWorstCase = VeyronV2IsWorstCaseMX<mx, SchedMxList>.c;
  let Latency = 1, ReleaseAtCycles = [Cycles] in {
    defm "" : LMULWriteResMX<"WriteVRGatherVX", [VeyronV2VPerm], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVRGatherVI", [VeyronV2VPerm], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIMergeV", [VeyronV2VPerm], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIMergeX", [VeyronV2VPerm], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVIMergeI", [VeyronV2VPerm], mx, IsWorstCase>;
    defm "" : LMULWriteResMX<"WriteVFMergeV", [VeyronV2VPerm], mx, IsWorstCase>;
  }
}

// 6. Configuration-Setting Instructions
def : ReadAdvance<ReadVSETVLI, 0>;
def : ReadAdvance<ReadVSETVL, 0>;

// 7. Vector Loads and Stores
def : ReadAdvance<ReadVLDX, 0>;
def : ReadAdvance<ReadVSTX, 0>;
defm "" : LMULReadAdvance<"ReadVSTEV", 0>;
defm "" : LMULReadAdvance<"ReadVSTM", 0>;
def : ReadAdvance<ReadVLDSX, 0>;
def : ReadAdvance<ReadVSTSX, 0>;
defm "" : LMULReadAdvance<"ReadVSTS8V", 0>;
defm "" : LMULReadAdvance<"ReadVSTS16V", 0>;
defm "" : LMULReadAdvance<"ReadVSTS32V", 0>;
defm "" : LMULReadAdvance<"ReadVSTS64V", 0>;
defm "" : LMULReadAdvance<"ReadVLDUXV", 0>;
defm "" : LMULReadAdvance<"ReadVLDOXV", 0>;
defm "" : LMULReadAdvance<"ReadVSTUXV", 0>;
defm "" : LMULReadAdvance<"ReadVSTUX8", 0>;
defm "" : LMULReadAdvance<"ReadVSTUX16", 0>;
defm "" : LMULReadAdvance<"ReadVSTUX32", 0>;
defm "" : LMULReadAdvance<"ReadVSTUX64", 0>;
defm "" : LMULReadAdvance<"ReadVSTUX8V", 0>;
defm "" : LMULReadAdvance<"ReadVSTUX16V", 0>;
defm "" : LMULReadAdvance<"ReadVSTUX32V", 0>;
defm "" : LMULReadAdvance<"ReadVSTUX64V", 0>;
defm "" : LMULReadAdvance<"ReadVSTOX8", 0>;
defm "" : LMULReadAdvance<"ReadVSTOX16", 0>;
defm "" : LMULReadAdvance<"ReadVSTOX32", 0>;
defm "" : LMULReadAdvance<"ReadVSTOX64", 0>;
defm "" : LMULReadAdvance<"ReadVSTOXV", 0>;
defm "" : LMULReadAdvance<"ReadVSTOX8V", 0>;
defm "" : LMULReadAdvance<"ReadVSTOX16V", 0>;
defm "" : LMULReadAdvance<"ReadVSTOX32V", 0>;
defm "" : LMULReadAdvance<"ReadVSTOX64V", 0>;
// These are already LMUL aware
def : ReadAdvance<ReadVST1R, 0>;
def : ReadAdvance<ReadVST2R, 0>;
def : ReadAdvance<ReadVST4R, 0>;
def : ReadAdvance<ReadVST8R, 0>;

// 11. Vector Integer Arithmetic Instructions
defm "" : LMULReadAdvance<"ReadVIALUV", 0>;
defm "" : LMULReadAdvance<"ReadVIALUX", 0>;
defm "" : LMULReadAdvanceW<"ReadVIWALUV", 0>;
defm "" : LMULReadAdvanceW<"ReadVIWALUX", 0>;
defm "" : LMULReadAdvance<"ReadVExtV", 0>;
defm "" : LMULReadAdvance<"ReadVICALUV", 0>;
defm "" : LMULReadAdvance<"ReadVICALUX", 0>;
defm "" : LMULReadAdvance<"ReadVShiftV", 0>;
defm "" : LMULReadAdvance<"ReadVShiftX", 0>;
defm "" : LMULReadAdvanceW<"ReadVNShiftV", 0>;
defm "" : LMULReadAdvanceW<"ReadVNShiftX", 0>;
defm "" : LMULReadAdvance<"ReadVICmpV", 0>;
defm "" : LMULReadAdvance<"ReadVICmpX", 0>;
defm "" : LMULReadAdvance<"ReadVIMinMaxV", 0>;
defm "" : LMULReadAdvance<"ReadVIMinMaxX", 0>;
defm "" : LMULReadAdvance<"ReadVIMulV", 0>;
defm "" : LMULReadAdvance<"ReadVIMulX", 0>;
defm "" : LMULSEWReadAdvance<"ReadVIDivV", 0>;
defm "" : LMULSEWReadAdvance<"ReadVIDivX", 0>;
defm "" : LMULReadAdvanceW<"ReadVIWMulV", 0>;
defm "" : LMULReadAdvanceW<"ReadVIWMulX", 0>;
defm "" : LMULReadAdvance<"ReadVIMulAddV", 0>;
defm "" : LMULReadAdvance<"ReadVIMulAddX", 0>;
defm "" : LMULReadAdvanceW<"ReadVIWMulAddV", 0>;
defm "" : LMULReadAdvanceW<"ReadVIWMulAddX", 0>;
defm "" : LMULReadAdvance<"ReadVIMergeV", 0>;
defm "" : LMULReadAdvance<"ReadVIMergeX", 0>;
defm "" : LMULReadAdvance<"ReadVIMovV", 0>;
defm "" : LMULReadAdvance<"ReadVIMovX", 0>;

// 12. Vector Fixed-Point Arithmetic Instructions
defm "" : LMULReadAdvance<"ReadVSALUV", 0>;
defm "" : LMULReadAdvance<"ReadVSALUX", 0>;
defm "" : LMULReadAdvance<"ReadVAALUV", 0>;
defm "" : LMULReadAdvance<"ReadVAALUX", 0>;
defm "" : LMULReadAdvance<"ReadVSMulV", 0>;
defm "" : LMULReadAdvance<"ReadVSMulX", 0>;
defm "" : LMULReadAdvance<"ReadVSShiftV", 0>;
defm "" : LMULReadAdvance<"ReadVSShiftX", 0>;
defm "" : LMULReadAdvanceW<"ReadVNClipV", 0>;
defm "" : LMULReadAdvanceW<"ReadVNClipX", 0>;

// 13. Vector Floating-Point Instructions
defm "" : LMULSEWReadAdvance<"ReadVFALUV", 0>;
defm "" : LMULSEWReadAdvance<"ReadVFALUF", 0>;
defm "" : LMULSEWReadAdvanceFW<"ReadVFWALUV", 0>;
defm "" : LMULSEWReadAdvanceFW<"ReadVFWALUF", 0>;
defm "" : LMULSEWReadAdvance<"ReadVFMulV", 0>;
defm "" : LMULSEWReadAdvance<"ReadVFMulF", 0>;
defm "" : LMULSEWReadAdvanceF<"ReadVFDivV", 0>;
defm "" : LMULSEWReadAdvanceF<"ReadVFDivF", 0>;
defm "" : LMULSEWReadAdvanceFW<"ReadVFWMulV", 0>;
defm "" : LMULSEWReadAdvanceFW<"ReadVFWMulF", 0>;
defm "" : LMULSEWReadAdvance<"ReadVFMulAddV", 0>;
defm "" : LMULSEWReadAdvance<"ReadVFMulAddF", 0>;
defm "" : LMULSEWReadAdvanceFW<"ReadVFWMulAddV", 0>;
defm "" : LMULSEWReadAdvanceFW<"ReadVFWMulAddF", 0>;
defm "" : LMULSEWReadAdvanceF<"ReadVFSqrtV", 0>;
defm "" : LMULSEWReadAdvance<"ReadVFRecpV", 0>;
defm "" : LMULReadAdvance<"ReadVFCmpV", 0>;
defm "" : LMULReadAdvance<"ReadVFCmpF", 0>;
defm "" : LMULSEWReadAdvance<"ReadVFMinMaxV", 0>;
defm "" : LMULSEWReadAdvance<"ReadVFMinMaxF", 0>;
defm "" : LMULSEWReadAdvance<"ReadVFSgnjV", 0>;
defm "" : LMULSEWReadAdvance<"ReadVFSgnjF", 0>;
defm "" : LMULReadAdvance<"ReadVFClassV", 0>;
defm "" : LMULReadAdvance<"ReadVFMergeV", 0>;
defm "" : LMULReadAdvance<"ReadVFMergeF", 0>;
defm "" : LMULReadAdvance<"ReadVFMovF", 0>;
defm "" : LMULSEWReadAdvance<"ReadVFCvtIToFV", 0>;
defm "" : LMULReadAdvance<"ReadVFCvtFToIV", 0>;
defm "" : LMULSEWReadAdvanceW<"ReadVFWCvtIToFV", 0>;
defm "" : LMULReadAdvanceFW<"ReadVFWCvtFToIV", 0>;
defm "" : LMULSEWReadAdvanceFW<"ReadVFWCvtFToFV", 0>;
defm "" : LMULSEWReadAdvanceFW<"ReadVFNCvtIToFV", 0>;
defm "" : LMULReadAdvanceW<"ReadVFNCvtFToIV", 0>;
defm "" : LMULSEWReadAdvanceFW<"ReadVFNCvtFToFV", 0>;

// 14. Vector Reduction Operations
def : ReadAdvance<ReadVIRedV, 0>;
def : ReadAdvance<ReadVIRedV0, 0>;
def : ReadAdvance<ReadVIWRedV, 0>;
def : ReadAdvance<ReadVIWRedV0, 0>;
def : ReadAdvance<ReadVFRedV, 0>;
def : ReadAdvance<ReadVFRedV0, 0>;
def : ReadAdvance<ReadVFRedOV, 0>;
def : ReadAdvance<ReadVFRedOV0, 0>;
def : ReadAdvance<ReadVFWRedV, 0>;
def : ReadAdvance<ReadVFWRedV0, 0>;
def : ReadAdvance<ReadVFWRedOV, 0>;
def : ReadAdvance<ReadVFWRedOV0, 0>;

// 15. Vector Mask Instructions
defm "" : LMULReadAdvance<"ReadVMALUV", 0>;
defm "" : LMULReadAdvance<"ReadVMPopV", 0>;
defm "" : LMULReadAdvance<"ReadVMFFSV", 0>;
defm "" : LMULReadAdvance<"ReadVMSFSV", 0>;
defm "" : LMULReadAdvance<"ReadVIotaV", 0>;

// 16. Vector Permutation Instructions
def : ReadAdvance<ReadVMovXS, 0>;
def : ReadAdvance<ReadVMovSX_V, 0>;
def : ReadAdvance<ReadVMovSX_X, 0>;
def : ReadAdvance<ReadVMovFS, 0>;
def : ReadAdvance<ReadVMovSF_V, 0>;
def : ReadAdvance<ReadVMovSF_F, 0>;
defm "" : LMULReadAdvance<"ReadVISlideV", 0>;
defm "" : LMULReadAdvance<"ReadVISlideX", 0>;
defm "" : LMULReadAdvance<"ReadVFSlideV", 0>;
defm "" : LMULReadAdvance<"ReadVFSlideF", 0>;
defm "" : LMULSEWReadAdvance<"ReadVRGatherVV_data", 0>;
defm "" : LMULSEWReadAdvance<"ReadVRGatherVV_index", 0>;
defm "" : LMULSEWReadAdvance<"ReadVRGatherEI16VV_data", 0>;
defm "" : LMULSEWReadAdvance<"ReadVRGatherEI16VV_index", 0>;
defm "" : LMULReadAdvance<"ReadVRGatherVX_data", 0>;
defm "" : LMULReadAdvance<"ReadVRGatherVX_index", 0>;
defm "" : LMULReadAdvance<"ReadVRGatherVI_data", 0>;
defm "" : LMULReadAdvance<"ReadVGatherV", 0>;
defm "" : LMULSEWReadAdvance<"ReadVCompressV", 0>;
// These are already LMUL aware
def : ReadAdvance<ReadVMov1V, 0>;
def : ReadAdvance<ReadVMov2V, 0>;
def : ReadAdvance<ReadVMov4V, 0>;
def : ReadAdvance<ReadVMov8V, 0>;

// Others
def : ReadAdvance<ReadVMask, 0>;
def : ReadAdvance<ReadVPassthru_WorstCase, 0>;
foreach mx = SchedMxList in {
  def : ReadAdvance<!cast<SchedRead>("ReadVPassthru_" # mx), 0>;
  foreach sew = SchedSEWSet<mx>.val in
    def : ReadAdvance<!cast<SchedRead>("ReadVPassthru_" # mx  # "_E" # sew), 0>;
}

//===----------------------------------------------------------------------===//
// Unsupported extensions
defm : UnsupportedSchedZbkb;
defm : UnsupportedSchedZbkx;
//TODO: V2 is expected to have zfa
defm : UnsupportedSchedZfa;
defm : UnsupportedSchedZfhmin;
defm : UnsupportedSchedZabha;
defm : UnsupportedSchedZvk;
defm : UnsupportedSchedXsf;
defm : UnsupportedSchedQ;
}
