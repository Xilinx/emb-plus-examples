# Copyright (C) 2019-2022 Xilinx, Inc.
# Copyright (C) 2022-2024 Advance Micro Devices, Inc.
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

############################## Help Section ##############################
.PHONY: help
help:
	$(ECHO) "Makefile Usage:"
	$(ECHO) "  make "
	$(ECHO) "      Command to build host application."
	$(ECHO) ""
	$(ECHO) "  make clean"
	$(ECHO) "      Command to remove the generated non-hardware files."
	$(ECHO) ""

############################## Setting up Project Variables ##############################

# Cleaning stuff
RM = rm -f
RMDIR = rm -rf

ECHO:= @echo

########################## Setting up Host Variables ##########################

XFLIB_DIR = ../../common/Vitis_Libraries/vision
EXE_FILE = filter2D_accel_aie.elf
HOST_SRCS +=  ./src/host.cpp
HOST_OBJ += host.o

CXXFLAGS += -I$(XILINX_XRT)/include -I./src -I/usr/include/opencv4 -I$(XFLIB_DIR)/L1/include/aie
CXXFLAGS += -fmessage-length=0 -Wall -O0 -g -std=c++1y -pthread

LDFLAGS += -L$(XILINX_XRT)/lib -L$(XFLIB_DIR)/L1/lib/sw/x86/
LDFLAGS += -lstdc++ -lsmartTilerStitcher -lxrt_core -lxrt_coreutil -luuid -lOpenCL -lopencv_core -lopencv_imgproc -lopencv_imgcodecs

############################## Setting Rules for Host (Building Host Executable) ##############################
.DEFAULT_GOAL := all

all: check_xrt $(EXE_FILE)

%.o: ./src/%.cpp
	$(CXX) $(CXXFLAGS) -o $@ -c $<

$(EXE_FILE): $(HOST_OBJ)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LDFLAGS)

check_xrt:
ifeq (,$(wildcard $(XILINX_XRT)/lib/libxilinxopencl.so))
	@echo "Cannot locate XRT installation. Please set XILINX_XRT variable." && false
endif

# Install target
install: $(EXE_FILE)
	install -d $(ELFDIR)
	install -m 0755 $(EXE_FILE) $(ELFDIR)

############################## Cleaning Rules ##############################

.PHONY: clean
clean:
	-$(RMDIR) $(EXE_FILE) $(HOST_OBJ) *jpg

