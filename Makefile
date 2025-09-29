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
#
define run_spec_benchmark
	cd $(SPEC_DIR)/cpu2017; \
	(\
	source shrc; \
	runcpu \
	--config=llvm-linux-riscv-ventana.cfg \
	--define label=$(BRANCH_NAME) \
	--define llvm_bin_dir="$(INSTALL_DIR)/bin" \
	--define optimize_flags=$(SPEC_OPTIMIZE_FLAGS) \
	--define ld_flags=$(SPEC_LD_FLAGS) \
	--action=build \
	--size=$(2) \
	$(1); \
	runcpu \
	--config=llvm-linux-riscv-ventana.cfg \
	--define label=$(BRANCH_NAME) \
	--define llvm_bin_dir="$(INSTALL_DIR)/bin" \
	--define optimize_flags=$(SPEC_OPTIMIZE_FLAGS) \
	--define ld_flags=$(SPEC_LD_FLAGS) \
	--action=validate \
	--size=$(2) \
	--fake \
	$(1) \
	> $(RUN_SCRIPTS_DIR)/fake_output_$(1).txt; \
	python3 $(SPEC_DIR)/spec.py \
	--spec_cpu2017_path=$(SPEC_DIR)/cpu2017 \
	--generate_script \
	--run_script \
	--use_libcollect \
	--scripts_dir_path=$(RUN_SCRIPTS_DIR) \
	--exe_dir_path=$(SPEC_DIR)/cpu2017/benchspec/CPU/$(1)/exe/ \
	--run_dir_path=$(SPEC_DIR)/cpu2017/benchspec/CPU/$(1)/run/run_base_$(2)_$(BRANCH_NAME).0000 \
	--fake_run_log=$(RUN_SCRIPTS_DIR)/fake_output_$(1).txt \
	--output_path=$(RUN_SCRIPTS_DIR)/script_$(1).sh \
	--run_under_prefix="$(QEMU) -cpu $(MCPU) -execv-prefix $(QEMU)" \
	) > $(RUN_SCRIPTS_DIR)/$(1).log
endef

run_spec_test:
	$(call run_spec_benchmark,500.perlbench_r,test) & \
	$(call run_spec_benchmark,502.gcc_r,test) & \
	$(call run_spec_benchmark,505.mcf_r,test) & \
	$(call run_spec_benchmark,508.namd_r,test) & \
	$(call run_spec_benchmark,510.parest_r,test) & \
	$(call run_spec_benchmark,511.povray_r,test) & \
	$(call run_spec_benchmark,519.lbm_r,test) & \
	$(call run_spec_benchmark,520.omnetpp_r,test) & \
	$(call run_spec_benchmark,523.xalancbmk_r,test) & \
	$(call run_spec_benchmark,525.x264_r,test) & \
	$(call run_spec_benchmark,526.blender_r,test) & \
	$(call run_spec_benchmark,531.deepsjeng_r,test) & \
	$(call run_spec_benchmark,538.imagick_r,test) & \
	$(call run_spec_benchmark,541.leela_r,test) & \
	$(call run_spec_benchmark,544.nab_r,test) & \
	$(call run_spec_benchmark,557.xz_r,test) & \
	wait

run_spec_train:
	$(call run_spec_benchmark,500.perlbench_r,train) & \
	$(call run_spec_benchmark,502.gcc_r,train) & \
	$(call run_spec_benchmark,505.mcf_r,train) & \
	$(call run_spec_benchmark,508.namd_r,train) & \
	$(call run_spec_benchmark,510.parest_r,train) & \
	$(call run_spec_benchmark,511.povray_r,train) & \
	$(call run_spec_benchmark,519.lbm_r,train) & \
	$(call run_spec_benchmark,520.omnetpp_r,train) & \
	$(call run_spec_benchmark,523.xalancbmk_r,train) & \
	$(call run_spec_benchmark,525.x264_r,train) & \
	$(call run_spec_benchmark,526.blender_r,train) & \
	$(call run_spec_benchmark,531.deepsjeng_r,train) & \
	$(call run_spec_benchmark,538.imagick_r,train) & \
	$(call run_spec_benchmark,541.leela_r,train) & \
	$(call run_spec_benchmark,544.nab_r,train) & \
	$(call run_spec_benchmark,557.xz_r,train) & \
	wait
clean_spec:
	rm -rf $(SPEC_DIR)/cpu2017/benchspec/C*/*/run
	rm -rf $(SPEC_DIR)/cpu2017/benchspec/C*/*/build
	rm -rf $(SPEC_DIR)/cpu2017/benchspec/C*/*/exe
	rm -rf $(SPEC_DIR)/cpu2017/result/*
	rm -rf $(SPEC_DIR)/cpu2017/tmp/*
