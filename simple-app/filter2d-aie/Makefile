# Copyright 2019-2022 Xilinx, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# vitis makefile-generator v2.0.8

############################## Help Section ##############################
.PHONY: help
help:
	$(ECHO) "Makefile Usage:"
	$(ECHO) "  make host"
	$(ECHO) "      Command to build host application."
	$(ECHO) ""
	$(ECHO) "  make clean"
	$(ECHO) "      Command to remove the generated non-hardware files."
	$(ECHO) ""

############################## Setting up Project Variables ##############################

.PHONY: check_xrt
check_xrt:
ifeq (,$(wildcard $(XILINX_XRT)/lib/libxilinxopencl.so))
	@echo "Cannot locate XRT installation. Please set XILINX_XRT variable." && false
endif

export PATH := $(XILINX_VITIS)/bin:$(XILINX_XRT)/bin:$(PATH)
ifeq ($(HOST_ARCH), x86)
ifeq (,$(LD_LIBRARY_PATH))
LD_LIBRARY_PATH := $(XILINX_XRT)/lib
else
LD_LIBRARY_PATH := $(XILINX_XRT)/lib:$(LD_LIBRARY_PATH)
endif
endif

# Cleaning stuff
RM = rm -f
RMDIR = rm -rf

MV = mv -f
CP = cp -rf
ECHO:= @echo

########################## Setting up Host Variables ##########################

XFLIB_DIR := ../../common/Vitis_Libraries/vision
EXE_FILE := filter2d-aie.elf
HOST_SRCS += ./src/host.cpp ./src/aie_control_xrt.cpp

CXXFLAGS += -fmessage-length=0 -std=c++14 -O3 -Wall -Wno-unknown-pragmas -Wno-unused-label -D __PS_ENABLE_AIE__ -pthread
CXXFLAGS += -I./src -I$(XILINX_XRT)/include
CXXFLAGS += -I$(XILINX_VITIS)/aietools/include -I$(XILINX_VITIS)/aietools/include/drivers/aiengine
CXXFLAGS += -I/usr/include/opencv4 -I$(XFLIB_DIR)/L1/include/aie -I$(XFLIB_DIR)/L1/include/aie-ml -I$(XFLIB_DIR)/ext/xcl2 -I$(XFLIB_DIR)/L1/include

LDFLAGS += -Wl,--as-needed
LDFLAGS += -L$(XILINX_XRT)/lib -L$(XILINX_VITIS)/aietools/lib/lnx64.o -L$(XFLIB_DIR)/L1/lib/sw/aarch64-linux
LDFLAGS += -ladf_api_xrt -lxrt_coreutil -lsmartTilerStitcher -lopencv_imgcodecs -lopencv_core -lopencv_imgproc

############################## Setting Rules for Host (Building Host Executable) ##############################

.PHONY: host
host: check_xrt $(EXE_FILE)
$(EXE_FILE): $(HOST_SRCS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LDFLAGS)

############################## Cleaning Rules ##############################

.PHONY: clean
clean:
	-$(RMDIR) $(EXE_FILE)

