# Embedded Plus Platform Applications

The Embedded Plus platform is a combination of the Rayzen x86 and Versal Edge device. This
repository provides examples and source code to help you get started with development on
this platform.

The x86 device is the Ryzen R2314, and the Versal Edge device is the VE2302.

## Examples

Below is a table listing all the examples available in this repository:

| Example Name       | Description                              |
|--------------------|------------------------------------------|
| filter2d-pl        | Accelerator in PL logic                  |
| filter2d-aie       | Accelerator in AIE                       |

Each subfolder contains a README file that provides instructions for testing the
corresponding sub-application on this platform.

## Getting Started

To get started, follow these steps:

1. Clone this repository to your local machine:

```
git clone --recursive https://github.com/Xilinx/emb-plus-examples
```

2. Navigate to the desired sub-application folder:
```
cd <sub_application_folder>
```

3. Follow the instructions provided in the README file of the subfolder to test the
application on the Embedded Plus platform.

4. Download the prebuilt debian packages mentioned in the respective application
README file.   
[Download link to pre-built Application
packages](https://www.sapphiretech.com/en/commercial/edge-plus-vpr_4616#Download)

## Create Debian Package

A step-by-step guide to create a Debian package (deb) for emb-plus-examples
from its source code on the embedded-plus target.

The below steps are recommended to be executed on the emb-plus target using
Ubuntu 22.04 OS.

Dependency packages required for debian package generation on emb-plus target.

```
sudo apt install -y dkms
sudo apt install -y libopencv-dev
sudo apt install -y ./xrt_202410.2.17.326_22.04-amd64-xrt.deb
sudo apt install dh-make dpkg-dev devscripts build-essential debhelper
# Note: xrt is a local package
```
Debian package generation
```
git clone --recursive https://github.com/Xilinx/emb-plus-examples
cd emb-plus-examples
rm -rf .git common/Vitis_Libraries/.git
tar czvf ../emb-plus-examples_0.6.orig.tar.gz .
dpkg-buildpackage -us -uc -sa -F
```
The output will be a deb package named
'filter2d-acceleration-application_0.6-0xlnx1_all.deb', to install run
```
sudo apt install ./filter2d-acceleration-application_0.6-0xlnx1_all.deb
```
# License
(C) Copyright 2024, Advanced Micro Devices Inc.\
SPDX-License-Identifier: Apache-2.0
