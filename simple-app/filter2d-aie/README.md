# AIE Acceleration Example Application on Embedded Plus

This application demonstrates the acceleration of a 2D filter using AMD hardware
accelerators in AIE-ml. The application runs convolution of the input image on
the AIE accelerator with a fixed filter configuration and compares the results
with a SW implemented reference model for validation. The application is
capable to receive any jpg input image, for simplicity the given input image is
resized to 1080p using OpenCV APIs. The resized jpg image is converted to YUYV
format, the YUYV frame is fed to the hardware accelerator. Tiler and stitcher
components in the PL act as data movers to the AIE core, while they manage the
distribution and aggregation of image tiles and metadata across the AIE. The
computation of the f2d algorithm on the luma samples (Y-channel) happens in the
AIE; this operation is repeated over every tile. The final output preserves the
untouched chroma samples (UV-channels) of the processed image. This is an
example for a typical PLIO AIE-based design as the data movers Tiler/Stitcher
are in the PL and AIE for computation. The F2d on the AIE supports fixed-point
coefficients. Decimal values are left shifted by 10 to obtain the equivalent
fixed-point integer value in the AIE.

## Usage

To use this application, follow these steps:

1. Install the application and XRT tools.

2. Set environment variables.

3. Run the installed executable with command line arguments.

## Installing Application

[Download link to pre-built Application
packages](https://www.sapphiretech.com/en/commercial/edge-plus-vpr_4616#Download)

```
# Install 2.17 Xilinx RunTime (XRT) library
$ sudo apt install -y ./xrt_202410.2.17.326_22.04-amd64-xrt.deb

# Install accel firmware binary for AIE filter2D (*xclbin)
$ sudo apt install ./filter2d-aie-ve2302_0.6.deb

# Install host app, and OpenCV as dependency.
$ sudo apt install -y ./filter2d-acceleration-application_0.6-0xlnx1_all.deb

```

## Test application

```
$ source /opt/xilinx/xrt/setup.sh

$ export PATH="/opt/xilinx/filter2d-aie:$PATH"

# Filter2d Acceleration Example Application Usage:
$ <Executable Name> -i [path/testimg.jpg] -u [path/user_xclbin]

# Use -h for usage help
$ <Executable Name> -h

# Example using default test image and default xclbin
$ filter2D_accel_aie.elf
```

The application performs a pixel-by-pixel comparison between the output from
the hardware accelerator and the reference image. Both the processed and
reference images are saved in JPG format, allowing users to inspect the
processed image for any artifacts. By default the application will include
 three example \*.jpg files:

- hw_in.jpg - Is an input reference image to both the HW accelerator & SW reference
- sw_ref.jpg - Is an output image as processed by the OpenCV SW libraries
- hw_out.jpg - Is an output image as processed by the AIE HW acceleration library

## Compiling F2d application

The application depends on OpenCV library dev package and installing it is
required before compilaiton.

```
$ sudo apt install libopencv-dev
$ cd emb-plus-examples/simple-app/filter2d-aie
$ make
```

# License

(C) Copyright 2024, Advanced Micro Devices Inc.\
SPDX-License-Identifier: Apache-2.0
