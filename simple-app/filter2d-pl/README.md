Filter2D Acceleration Example PL Application
=========================================

This application demonstrates the acceleration of a 2D filter using AMD hardware
accelerators in programmable logic. The application runs convolution of the input image on
the hardware accelerator with a filter configuration chosen by the user and compares the
results with OpenCV's implementation for validation. There are multiple preset filter
configurations to choose from, provided in the application help menu. The application is
capable to receive any jpg input image, for simplicity the given input image is resized to
1080p using OpenCV apis. The resized jpg image is converted to YUYV format, the YUYV frame
is fed to the hardware accelerator. The hardware accelerator performs 2D convolution on
luma samples (Y-channel) of the input frame. Final image is written out keeping the chroma
samples (UV-channels) untouched.

Usage
-----

To use this application, follow these steps:

1. Install the application and XRT tools.

2. Set environment variables.

2. Run the installed executable with command line arguments.


Installing Application
-------------

```
# Install 2.17 Xilinx RunTime (XRT) library
$ sudo apt install -y ./xrt_202410.2.17.240_22.04-amd64-xrt.deb

# Install accel firmware binary for filter2D (*xclbin)
$ sudo apt install ./filter2d-pl-ve2302_1.0.deb

# Install host app, and OpenCV as dependency.
$ sudo apt install -y ./filter2d-acceleration-application_0.1-0xlnx1_all.deb

```


Test application
----------------
```
$ source /opt/xilinx/xrt/setup.sh

$ export PATH="/opt/xilinx/filter2d-pl:$PATH"

# Filter2d Accelertation Example Application Usage:
$ <Executable Name> <Filter> -i [input_image_path] -u [user_xclbin]

# Use -h to find available filter options
$ <Executable Name> -h

# Example Test with default image
$ filter2D_accel_pl.elf Blur

# Example Test with custom image
$ filter2D_accel_pl.elf Blur -i /abs_path_jpg_image_file -u /abs_path_user_xclbin
```

The application performs a pixel-by-pixel comparison between the output from the
hardware accelerator and the reference image. Both the processed and reference
images are saved in JPG format, allowing users to inspect the processed image
for any artifacts.

By default the application will include three example *.jpg files:
* hwin_HD.jpg - Is an input reference image to both the HW accelerator & SW reference
* ocv_ref.jpg - Is an output image as processed by the OpenCV SW libraries
* hw_out.jpg - Is an output image as processed by the PL HW acceleration library

Compiling F2d application
-------------------------

The application depends on OpenCV library dev package and installing it is required before compilaiton.

```
$ sudo apt install libopencv-dev
$ cd emb-plus-examples/simple-app/filter2d-pl
$ make
```

# License
(C) Copyright 2024, Advanced Micro Devices Inc.\
SPDX-License-Identifier: Apache-2.0
