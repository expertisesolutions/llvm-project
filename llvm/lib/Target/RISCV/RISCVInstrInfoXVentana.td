//===-- RISCVInstrInfoXVentana.td --------------------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes the vendor extensions defined by Ventana Micro Systems.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// XVentanaCondOps
// XVentanaMatMul
//===----------------------------------------------------------------------===//

let Predicates = [HasVendorXVentanaCondOps], hasSideEffects = 0,
    mayLoad = 0, mayStore = 0, isCodeGenOnly = 0, DecoderNamespace = "XVentana" in
class VTMaskedMove<bits<3> funct3, string opcodestr>
    : RVInstR<0b0000000, funct3, OPC_CUSTOM_3, (outs GPR:$rd),
              (ins GPR:$rs1, GPR:$rs2), opcodestr,
              "$rd, $rs1, $rs2"> {
}

def VT_MASKC : VTMaskedMove<0b110, "vt.maskc">,
           Sched<[WriteIALU, ReadIALU, ReadIALU]>;

def VT_MASKCN : VTMaskedMove<0b111, "vt.maskcn">,
           Sched<[WriteIALU, ReadIALU, ReadIALU]>;

let Predicates = [HasVendorXVentanaCondOps] in {
def : Pat<(XLenVT (riscv_czero_eqz GPR:$rs1, GPR:$rc)),
          (VT_MASKC GPR:$rs1, GPR:$rc)>;
def : Pat<(XLenVT (riscv_czero_nez GPR:$rs1, GPR:$rc)),
          (VT_MASKCN GPR:$rs1, GPR:$rc)>;

def : Pat<(XLenVT (riscv_czero_eqz GPR:$rs1, (riscv_setne (XLenVT GPR:$rc)))),
          (VT_MASKC GPR:$rs1, GPR:$rc)>;
def : Pat<(XLenVT (riscv_czero_eqz GPR:$rs1, (riscv_seteq (XLenVT GPR:$rc)))),
          (VT_MASKCN GPR:$rs1, GPR:$rc)>;
def : Pat<(XLenVT (riscv_czero_nez GPR:$rs1, (riscv_setne (XLenVT GPR:$rc)))),
          (VT_MASKCN GPR:$rs1, GPR:$rc)>;
def : Pat<(XLenVT (riscv_czero_nez GPR:$rs1, (riscv_seteq (XLenVT GPR:$rc)))),
          (VT_MASKC GPR:$rs1, GPR:$rc)>;
} // Predicates = [HasVendorXVentanaCondOps]

class RVInstVentanaCustom2<bits<6> funct6, bits<3> funct3, dag outs, dag ins,
                      string opcodestr, string argstr>
    : RVInst<outs, ins, opcodestr, argstr, [], InstFormatR> {
  bits<5> rs2;
  bits<5> rs1;
  bits<5> rd;
  bit vm;

  let Inst{31-26} = funct6;
  let Inst{25} = vm;
  let Inst{24-20}  = rs2;
  let Inst{19-15} = rs1;
  let Inst{14-12} = funct3;
  let Inst{11-7} = rd;
  let Inst{6-0} = OPC_CUSTOM_2.Value;

  let Uses = [VTYPE, VL];
  let RVVConstraint = NoConstraint;
}

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
class CustomVentanaVMACC<bits<6> funct6, RISCVVFormat opv, string opcodestr>
    : RVInstVentanaCustom2<funct6, opv.Value, (outs VR:$rd), (ins VR:$rs1, VR:$rs2),
               opcodestr, "$rd, $rs1, $rs2"> {
  let vm = 1;
}
}

class VPseudoTernaryNoMaskNoPolicy<VReg RetClass,
                                     RegisterClass Op1Class,
                                     DAGOperand Op2Class,
                                     string Constraint = "",
                                     bits<2> TargetConstraintType = 1> :
      RISCVVPseudo<(outs RetClass:$rd),
                   (ins RetClass:$rs3, Op1Class:$rs1, Op2Class:$rs2,
                        AVL:$vl, sew:$sew)> {
  let mayLoad = 0;
  let mayStore = 0;
  let hasSideEffects = 0;
  let Constraints = !interleave([Constraint, "$rd = $rs3"], ",");
  let TargetOverlapConstraintType = TargetConstraintType;
  let HasVLOp = 1;
  let HasSEWOp = 1;
}
multiclass VPseudoVentanaVMACC<string mx, VReg vd_type, VReg vs2_type,
                              string Constraint = ""> {
  def "Pseudo" # NAME # "_" # mx
      : VPseudoTernaryNoMaskNoPolicy<vd_type, V_M1.vrclass, vs2_type, Constraint>;
}

multiclass VPseudoVentanaVWMACC<string Constraint = ""> {
  foreach m = [V_M1] in
    let VLMul = m.value in
    defm NAME : VPseudoVentanaVMACC<m.MX, V_M4.vrclass, m.vrclass, Constraint>;
}

multiclass VPatVVentanaVMACC<string intrinsic, string instruction, string kind,
                     list<VTypeInfoToWide> info_pairs, ValueType vec_m1> {
  foreach pair = info_pairs in {
    defvar VdInfo = pair.Wti;
    defvar Vs2Info = pair.Vti;
    let Predicates = [HasVInstructions] in
    def : VPatTernaryNoMask<"int_riscv_ventana_" # intrinsic,
                            "Pseudo" # instruction, kind, VdInfo.Vector,
                             vec_m1, Vs2Info.Vector,
                             Vs2Info.Log2SEW, Vs2Info.LMul,
                             VdInfo.RegClass, VR, Vs2Info.RegClass>;
  }
}

defset list<VTypeInfoToWide> VentanaInfoPairs = {
  def : VTypeInfoToWide<VI8M1, VI32M4>;
}

let Predicates = [IsRV64, HasVendorXVentanaMatmul] in {
  def XVWMATMUL_SS_VV : CustomVentanaVMACC<0b000000, OPIVV, "vwmatmulss.vv">;
  def XVWMATMUL_SU_VV : CustomVentanaVMACC<0b000001, OPIVV, "vwmatmulsu.vv">;
  def XVWMATMUL_UU_VV : CustomVentanaVMACC<0b000011, OPIVV, "vwmatmuluu.vv">;

  defm XVWMATMUL_SS_VV : VPseudoVentanaVWMACC;
  defm XVWMATMUL_SU_VV : VPseudoVentanaVWMACC;
  defm XVWMATMUL_UU_VV : VPseudoVentanaVWMACC;

  defm : VPatVVentanaVMACC<"vwmatmul_ss", "XVWMATMUL_SS", "VV", VentanaInfoPairs, vint8m1_t>;
  defm : VPatVVentanaVMACC<"vwmatmul_su", "XVWMATMUL_SU", "VV", VentanaInfoPairs, vint8m1_t>;
  defm : VPatVVentanaVMACC<"vwmatmul_uu", "XVWMATMUL_UU", "VV", VentanaInfoPairs, vint8m1_t>;
}

// Vector absolute difference accumulate
// TODO: add all possible variants.
// TODO: use multiclass.
def VWADACCU_VV : RVInstVV<
  0b010000,
  OPIVV,
  (outs VR:$vd),
  (ins VR: $vs0, VR:$vs1, VR:$vs2),
  "vwadaccu.vv",
  "$vd, $vs1, $vs2"
> {
  let Inst{6-0} = OPC_CUSTOM_2.Value;
  let vm = 1;
  let hasSideEffects = 0;
  let mayLoad = 0;
  let mayStore = 0;
  let Uses = [VTYPE, VL];
  let Constraints = "$vd = $vs0";
}

let Predicates = [IsRV64, HasVInstructionsI64, HasVendorXVentanaVwadaccu], hasSideEffects = 0, mayLoad = 0, mayStore = 0, isCodeGenOnly = 1, SEW  = 8 in {
  def PseudoVWADACCU_VV_MF8:
  RISCVVPseudo<
    (outs VR:$vd),
    (ins VR:$vs0, VR:$vs1, VR:$vs2, ixlenimm:$vl, ixlenimm:$sew, ixlenimm:$policy),
    []
  > {
    let mayLoad = 0;
    let mayStore = 0;
    let hasSideEffects = 0;
    let HasVLOp = 1;
    let HasSEWOp = 1;
    let HasVecPolicyOp = 1;
    let VLMul = 0b101;
    let Constraints = "$vd = $vs0";
    let isConvertibleToThreeAddress = 1;
    let IsTiedPseudo = 1;
  }

  def PseudoVWADACCU_VV_MF4:
  RISCVVPseudo<
    (outs VR:$vd),
    (ins VR:$vs0, VR:$vs1, VR:$vs2, ixlenimm:$vl, ixlenimm:$sew, ixlenimm:$policy),
    []
  > {
    let mayLoad = 0;
    let mayStore = 0;
    let hasSideEffects = 0;
    let HasVLOp = 1;
    let HasSEWOp = 1;
    let HasVecPolicyOp = 1;
    let VLMul = 0b110;
    let Constraints = "$vd = $vs0";
    let isConvertibleToThreeAddress = 1;
    let IsTiedPseudo = 1;
  }
}

def nxv1i8_vr:  PatLeaf<(nxv1i8   VR: $R)>;
def nxv1i32_vr:  PatLeaf<(nxv1i32   VR: $R)>;
def nxv2i8_vr:  PatLeaf<(nxv2i8   VR: $R)>;
def nxv2i32_vr:  PatLeaf<(nxv2i32   VR: $R)>;

def : Pat<
  (nxv1i32 (int_riscv_addabsdiff nxv1i32_vr: $vs0, nxv1i8_vr: $vs1, nxv1i8_vr: $vs2, (XLenVT imm:$vl), (XLenVT imm:$policy))),
  (PseudoVWADACCU_VV_MF8 nxv1i32_vr: $vs0, nxv1i8_vr: $vs1, nxv1i8_vr: $vs2, (XLenVT imm:$vl), 3, (XLenVT imm:$policy))
>;

def : Pat<
  (nxv2i32 (int_riscv_addabsdiff nxv2i32_vr: $vs0, nxv2i8_vr: $vs1, nxv2i8_vr: $vs2, (XLenVT imm:$vl), (XLenVT imm:$policy))),
  (PseudoVWADACCU_VV_MF4 nxv2i32_vr: $vs0, nxv2i8_vr: $vs1, nxv2i8_vr: $vs2, (XLenVT imm:$vl), 3, (XLenVT imm:$policy))
>;
