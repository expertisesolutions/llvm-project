//===- IntrinsicsRISCVXVentana.td - Ventana intrinsics --*- tablegen -*----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines all of the Ventana vendor intrinsics for RISC-V.
//
//===----------------------------------------------------------------------===//

let TargetPrefix = "riscv" in {
  class RISCVVentanaCustomVMACC
      : DefaultAttrsIntrinsic<[llvm_anyvector_ty],
                   [LLVMMatchType<0>, llvm_anyvector_ty, llvm_anyvector_ty,
                    llvm_anyint_ty],
                   [IntrNoMem] >, RISCVVIntrinsic {
    let VLOperand = 3;
  }

  def int_riscv_ventana_vwmatmul_ss : RISCVVentanaCustomVMACC;
  def int_riscv_ventana_vwmatmul_su : RISCVVentanaCustomVMACC;
  def int_riscv_ventana_vwmatmul_uu : RISCVVentanaCustomVMACC;

  def int_riscv_addabsdiff : DefaultAttrsIntrinsic<
      [llvm_anyint_ty], [LLVMMatchType<0>, llvm_anyint_ty, LLVMMatchType<1>, llvm_i64_ty, llvm_i64_ty],
      [IntrNoMem, IntrSpeculatable, IntrWillReturn]>;
}
