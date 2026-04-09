# AIE Acceleration Example Application on Embedded Plus

This application demonstrates the acceleration of a 2D filter using AMD hardware
accelerators in AIE-ml. The application runs convolution of the input on
the AIE accelerator with a filter configuration chosen by the user. The
application supports three input modes: a single JPG image, a video file, or a
live USB camera feed. In image mode, the results are compared with a SW
implemented reference model for validation. In video and camera modes, frames
are processed continuously through a double-buffered (ping-pong) pipeline using
GStreamer for decoding and encoding; the output can be saved to an MP4 file or
displayed on screen. The application accepts any jpg input image or 1080p video
source; for image mode the given input image is resized to 1080p using OpenCV
APIs. The resized jpg image or decoded video frame is converted to YUYV format,
and the YUYV frame is fed to the hardware accelerator. Tiler and stitcher
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
# Install Xilinx RunTime (XRT) library
$ sudo apt install -y ./xrt_202510.2.19.214_22.04-amd64-xrt.deb

# Install accel firmware binary for AIE filter2D (*xclbin)
$ sudo apt install -y ./filter2d-aie-ve2302_1.2.deb

# Install host app, and OpenCV as dependency.
$ sudo apt install -y ./filter2d-acceleration-application_1.1-0xlnx1_all.deb

```

## Test application

```
$ source /opt/xilinx/xrt/setup.sh

$ export PATH="/opt/xilinx/filter2d-aie:$PATH"

# Filter2d Acceleration Example Application Usage:
# Filter options: Horizontal-Gradient, Emboss, Edge, Blur, Identity, Horizontal-Sobel

# Image mode:
$ <Executable Name> <Filter> -i [path/testimg.jpg] -u [path/user_xclbin]

# Video mode:
$ <Executable Name> <Filter> -v [path/video.mp4] -u [path/user_xclbin]

# Camera mode:
$ <Executable Name> <Filter> -c [camera_device_index] -u [path/user_xclbin]

# Use -d flag to display output on screen instead of saving to file:
$ <Executable Name> <Filter> -v [path/video.mp4] -d
$ <Executable Name> <Filter> -c [camera_device_index] -d

# Use -h for usage help
$ <Executable Name> -h

# Example using default test image and default xclbin
$ filter2D_accel_aie.elf Edge

# Example with custom image
$ filter2D_accel_aie.elf Edge -i path/testimg.jpg

# Example with video file (output saved to hw_out.mp4)
$ filter2D_accel_aie.elf Edge -v path/video.mp4

# Example with video displayed on screen
$ filter2D_accel_aie.elf Edge -v path/video.mp4 -d

# Example with USB camera (output saved to hw_out.mp4)
$ filter2D_accel_aie.elf Edge -c 0

# Example with USB camera displayed on screen
$ filter2D_accel_aie.elf Edge -c 0 -d
```

### Image mode

The application performs a pixel-by-pixel comparison between the output from
the hardware accelerator and the reference image. Both the processed and
reference images are saved in JPG format, allowing users to inspect the
processed image for any artifacts. By default the application will include
 three example \*.jpg files:

- hw_in.jpg - Is an input reference image to both the HW accelerator & SW reference
- sw_ref.jpg - Is an output image as processed by the OpenCV SW libraries
- hw_out.jpg - Is an output image as processed by the AIE HW acceleration library

### Video and Camera modes

In video and camera modes, frames are decoded via GStreamer, processed through
the AIE filter2D accelerator, and either saved to hw_out.mp4 or displayed on
screen (with the `-d` flag). The processing loop uses double-buffered XRT
buffers to overlap frame decoding with AIE computation. Press Ctrl+C during
camera or display mode to gracefully stop processing.

## Compiling F2d application

The application depends on OpenCV and GStreamer library dev packages and
installing them is required before compilation.

```
$ sudo apt install libopencv-dev libboost-all-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
$ cd emb-plus-examples/simple-app/filter2d-aie
$ make
```

Note: This app may not build with the latest commit of Vitis_Libraries.
Please use commit ab0dca2.

# License

(C) Copyright 2024 - 2025, Advanced Micro Devices Inc.\
SPDX-License-Identifier: Apache-2.0
