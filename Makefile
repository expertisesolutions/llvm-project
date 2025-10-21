TOP = $(PWD)
SHELL := /bin/bash

prepare:
	rm -rf $(BUILD_DIR)
	rm -rf $(INSTALL_DIR)
	mkdir -p $(BUILD_DIR)
	mkdir -p $(INSTALL_DIR)
	dpkg --extract /mnt/artifacts/gcc/latest_build/ventana-gcc.deb $(INSTALL_DIR)

configure_llvm:
	cd $(BUILD_DIR); \
	cmake $(TOP)/llvm \
	-G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DLLVM_ENABLE_ASSERTIONS=ON \
	-DCMAKE_C_COMPILER=$(CC) \
	-DCMAKE_CXX_COMPILER=$(CXX) \
	-DCMAKE_CXX_COMPILER_LAUNCHER="ccache" \
	-DCMAKE_CXX_FLAGS="-stdlib=libc++" \
	-DLLVM_USE_LINKER=lld \
	-DBUILD_SHARED_LIBS=ON \
	-DLLVM_TARGETS_TO_BUILD="RISCV" \
	-DLLVM_ENABLE_PROJECTS="clang;lld" \
	-DLLVM_OPTIMIZED_TABLEGEN=ON \
	-DLLVM_PARALLEL_LINK_JOBS=1 \
	-DCMAKE_INSTALL_PREFIX=$(INSTALL_DIR) \
	-DLLVM_BINUTILS_INCDIR=$(INSTALL_DIR)/x86_64-pc-linux-gnu/riscv64-linux-gnu/include

configure_llvm_native_riscv64_flang_build:
	rm -rf $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)
	mkdir -p $(INSTALL_DIR)
	cd $(BUILD_DIR); \
	cmake $(TOP)/llvm \
	-G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DLLVM_ENABLE_ASSERTIONS=ON \
	-DLLVM_TARGETS_TO_BUILD="host" \
	-DLLVM_ENABLE_PROJECTS="clang;mlir;flang;openmp" \
	-DCMAKE_C_COMPILER=gcc \
	-DCMAKE_CXX_COMPILER=g++ \
	-DLLVM_PARALLEL_LINK_JOBS=1 \
	-DCMAKE_INSTALL_PREFIX=$(INSTALL_DIR) \
	-DLLVM_ENABLE_RUNTIMES="compiler-rt"

check_llvm:
	cd $(BUILD_DIR); cmake --build . --target check-llvm

install_llvm:
	cd $(BUILD_DIR); cmake --build . --target install

package_llvm:
	mkdir -p $(INSTALL_DIR)/DEBIAN
	echo -e "\
Package: ventanta-llvm \n\
Version: 1.0 \n\
Section: utils \n\
Priority: optional \n\
Architecture: all \n\
Maintainer: Ventana Micro Systems \n\
Description: LLVM, $(BRANCH_NAME) \n\
" > $(INSTALL_DIR)/DEBIAN/control
	dpkg-deb --root-owner-group --build $(INSTALL_DIR)
	mv $(STAGING_DIR)/install.deb $(STAGING_DIR)/ventana-llvm.deb

MCPU=veyron-v2
SPEC_OPTIMIZE_FLAGS="\
  -mcpu=$(MCPU) \
  --sysroot=$(INSTALL_DIR) \
  -O3 \
  -mllvm -riscv-v-slp-prefer-alt-opc-vectorization=true \
  -mllvm -stats \
"
SPEC_LD_FLAGS="\
  -fuse-ld=$(INSTALL_DIR)/riscv64-linux-gnu/bin/ld.bfd \
  -static \
"
# SPEC_DIR is defined in gitlab-ci.yml
# RUN_SCRIPTS_DIR was defined in gitlab-ci.yml
run_spec_test:
	export BRANCH_NAME=$(BRANCH_NAME); \
	export INSTALL_DIR=$(INSTALL_DIR); \
	export RUN_SCRIPTS_DIR=$(RUN_SCRIPTS_DIR); \
	export SPEC_DIR=$(SPEC_DIR); \
	export QEMU=$(QEMU); \
	export MCPU=$(MCPU); \
	export SPEC_OPTIMIZE_FLAGS=$(SPEC_OPTIMIZE_FLAGS); \
	export SPEC_LD_FLAGS=$(SPEC_LD_FLAGS); \
	parallel "$(RUN_SPEC_BENCHMARK)" ::: \
	500.perlbench_r \
	502.gcc_r \
	505.mcf_r \
	508.namd_r \
	510.parest_r \
	511.povray_r \
	519.lbm_r \
	520.omnetpp_r \
	523.xalancbmk_r \
	525.x264_r \
	526.blender_r \
	531.deepsjeng_r \
	538.imagick_r \
	541.leela_r \
	544.nab_r \
	557.xz_r \
	::: test

run_spec_train:
	export BRANCH_NAME=$(BRANCH_NAME); \
	export INSTALL_DIR=$(INSTALL_DIR); \
	export RUN_SCRIPTS_DIR=$(RUN_SCRIPTS_DIR); \
	export SPEC_DIR=$(SPEC_DIR); \
	export QEMU=$(QEMU); \
	export MCPU=$(MCPU); \
	export SPEC_OPTIMIZE_FLAGS=$(SPEC_OPTIMIZE_FLAGS); \
	export SPEC_LD_FLAGS=$(SPEC_LD_FLAGS); \
	parallel "$(RUN_SPEC_BENCHMARK)" ::: \
	500.perlbench_r \
	502.gcc_r \
	505.mcf_r \
	508.namd_r \
	510.parest_r \
	511.povray_r \
	519.lbm_r \
	520.omnetpp_r \
	523.xalancbmk_r \
	525.x264_r \
	526.blender_r \
	531.deepsjeng_r \
	538.imagick_r \
	541.leela_r \
	544.nab_r \
	557.xz_r \
	::: train

clean_spec:
	rm -rf $(SPEC_DIR)/cpu2017/benchspec/C*/*/run
	rm -rf $(SPEC_DIR)/cpu2017/benchspec/C*/*/build
	rm -rf $(SPEC_DIR)/cpu2017/benchspec/C*/*/exe
	rm -rf $(SPEC_DIR)/cpu2017/result/*
	rm -rf $(SPEC_DIR)/cpu2017/tmp/*
