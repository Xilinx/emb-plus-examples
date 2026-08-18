# Filter2D Acceleration Example Application on Embedded Plus

This application demonstrates the acceleration of a 2D filter using AMD hardware
accelerators. It supports two acceleration modes: **AIE** (AIE-ml tiler/stitcher
architecture) and **PL** (PL OpenCL kernel). Use the `-m` flag to select the mode;
the default is AIE.

In AIE mode, the application uses AIE-ml hardware acceleration. The application
runs convolution of the input on the AIE accelerator with a filter configuration
chosen by the user. The application supports three input modes: a single JPG
image, a video file, or a live USB camera feed. In image mode, the results are
compared with a SW implemented reference model for validation. In video and camera
modes, frames are processed continuously through a double-buffered (ping-pong)
pipeline using GStreamer for decoding and encoding; the output can be saved to an
MP4 file or displayed on screen. The application accepts any jpg input image or
1080p video source; for image mode the given input image is resized to 1080p using
OpenCV APIs. The resized jpg image or decoded video frame is converted to YUYV format,
and the YUYV frame is fed to the hardware accelerator. Tiler and stitcher
components in the PL act as data movers to the AIE core, while they manage the
distribution and aggregation of image tiles and metadata across the AIE. The
computation of the f2d algorithm on the luma samples (Y-channel) happens in the
AIE; this operation is repeated over every tile. This is an
example for a typical PLIO AIE-based design as the data movers Tiler/Stitcher
are in the PL and AIE for computation. The F2d on the AIE supports fixed-point
coefficients. Decimal values are left shifted by 10 to obtain the equivalent
fixed-point integer value in the AIE.

**No implementation filters chroma.** All three convolve the luma plane only.
Chroma is either passed through unchanged or set to neutral grey, selected with
`-k`; see [Chroma handling](#chroma-handling). This differs from OpenCV's
`filter2D`, which convolves every channel of a multi-channel image.

All three implementations saturate their output to the range 0 to 255.

## Usage

To use this application, follow these steps:

1. Install the application, install XRT tools, and update GPU drivers.

2. Set environment variables.

3. Run the installed executable with command line arguments.

## Installing Application

[Download link to pre-built Application
packages](https://www.sapphiretech.com/en/commercial/edge-plus-vpr_4616#Download)

```
# Install Xilinx RunTime (XRT) library
$ sudo apt install -y ./xrt_202620.2.26.0_22.04-amd64-xrt.deb

# Install accel firmware binary for filter2D AIE (*xclbin)
$ sudo apt install -y ./filter2d-aie-ve2302_2.0.deb

# Install accel firmware binary for filter2D PL (*xclbin)
$ sudo apt install -y ./filter2d-pl-ve2302_2.0.deb

# Install host app, and OpenCV as dependency.
$ sudo apt install -y ./filter2d-acceleration-application_2.0-0xlnx1_all.deb

# Update GPU drivers
$ wget https://repo.radeon.com/amdgpu-install/6.1/ubuntu/jammy/amdgpu-install_6.1.60100-1_all.deb
$ sudo dpkg -i amdgpu-install_6.1.60100-1_all.deb
$ amdgpu-install -y --usecase=graphics
$ sudo reboot

```

## Test application

```
$ source /opt/xilinx/xrt/setup.sh

$ export PATH="/opt/xilinx/filter2d:$PATH"

# Filter2d Acceleration Example Application Usage:
# Filter options: Horizontal-Gradient, Emboss, Edge, Blur, Identity, Horizontal-Sobel

# Image mode:
$ <Executable Name> <Filter> [-m aie|pl] -i [path/testimg.jpg] -u [path/user_xclbin]

# Video mode:
$ <Executable Name> <Filter> [-m aie|pl] -v [path/video.mp4] -u [path/user_xclbin]

# Camera mode:
$ <Executable Name> <Filter> [-m aie|pl] -c [camera_device_index] -t [seconds] -u [path/user_xclbin]

# Use -m flag to select acceleration mode (aie or pl, default: aie):
$ <Executable Name> <Filter> -m pl -i [path/testimg.jpg]

# Use -d flag to display output on screen instead of saving to file:
$ <Executable Name> <Filter> -v [path/video.mp4] -d
$ <Executable Name> <Filter> -c [camera_device_index] -d

# Use -t flag to set camera capture duration in seconds (default: 30):
$ <Executable Name> <Filter> -c [camera_device_index] -t 60

# Use -k to override chroma handling (auto|keep|grey, default: auto):
$ <Executable Name> <Filter> -i [path/testimg.jpg] -k keep

# Use -h for usage help
$ <Executable Name> -h

# Example using default test image and default xclbin (AIE mode)
$ filter2D_accel.elf Edge

# Example with PL mode
$ filter2D_accel.elf Edge -m pl

# Example with custom image
$ filter2D_accel.elf Edge -i path/testimg.jpg

# Example with PL mode and custom image
$ filter2D_accel.elf Edge -m pl -i path/testimg.jpg

# Example with video file (output saved to hw_out.mp4)
$ filter2D_accel.elf Edge -v path/video.mp4

# Example with video displayed on screen
$ filter2D_accel.elf Edge -v path/video.mp4 -d

# Example PL mode with video
$ filter2D_accel.elf Edge -m pl -v path/video.mp4

# Example with USB camera (output saved to hw_out.mp4, default 30 seconds)
$ filter2D_accel.elf Edge -c 0

# Example with USB camera recording for 60 seconds
$ filter2D_accel.elf Edge -c 0 -t 60

# Example with USB camera displayed on screen
$ filter2D_accel.elf Edge -c 0 -d
```

## Filter presets

A kernel that sums to zero is an edge detector: flat areas of the image produce
a luma of zero and the output is an edge magnitude, not a picture. A kernel that
sums to one preserves the luma of flat areas, so the output still looks like the
scene.

| Preset | Kernel sum | Output | Chroma default | PL shift |
|---|---|---|---|---|
| Horizontal-Gradient | 0 | edge magnitude | grey | 0 |
| Emboss | 1 | scene | keep | 0 |
| Edge | 0 | edge magnitude | grey | 0 |
| Blur | 1 | scene, 3x3 Gaussian | keep | 4 |
| Identity | 1 | scene, unchanged | keep | 0 |
| Horizontal-Sobel | 0 | edge magnitude | grey | 0 |

## Chroma handling

U and V are offsets relative to Y, not an independent hue. When a zero-sum
kernel drives luma to near zero, a fixed R-G offset becomes a large *relative*
difference and the original chroma renders as a misleading colour: the yellow
flag in the sample image comes out red.

`-k` selects how chroma is treated:

| `-k` | Behaviour |
|---|---|
| `auto` (default) | grey for zero-sum kernels, keep for the rest |
| `keep` | pass the input chroma through unchanged |
| `grey` | write 128 to both components, giving a monochrome output |

`auto` is derived from the coefficient sum at run time, so a new preset gets the
appropriate default with no table to maintain.

Chroma is never convolved. Filtering it would require the 4:2:2 stream to be
deinterleaved first, and measured against a full-RGB reference it changes a
blurred frame by a mean of 0.66 out of 255 — smaller than the cost of a second
filter instance.

## Border handling

The implementations do not agree at the image border, and this is expected:

| Implementation | Border |
|---|---|
| `run_ref` (software reference) | replicate the edge pixel |
| AIE kernel | replicate the edge pixel |
| PL kernel | zero-pad |

`xf::cv::filter2D` only supports `XF_BORDER_CONSTANT`; its `BORDER_TYPE`
template parameter is accepted but not used, so requesting replicate has no
effect.

Border pixels therefore differ by up to the full 0-255 range, which no
magnitude tolerance can absorb. `compareResult` **excludes the outer
`filter_size / 2` ring** (1 pixel for a 3x3 kernel) and applies its per-pixel
tolerance of 1 to the interior only. On 1920x1080 that skips 11992 of 4147200
bytes, 0.29%, and the count is printed with the result. Interior pixels are
bit-exact across all three implementations.

## Implementation notes

Relevant when changing the kernels or adding a preset.

- **Adding a preset** needs an entry in `kData` and one in `kShift`. PL applies
  coefficients as integers, and `matrixDeconstructor` casts to `short`, so a
  fractional coefficient truncates to zero unless it is scaled by `2^kShift`
  with the kernel right-shifting by the same amount. The AIE kernel and
  `run_ref` take the float coefficients directly and ignore `kShift` — the AIE
  path scales by 1024 and its SRS shifts back by 10. A fractional preset that
  forgets `kShift` therefore breaks on PL, and only on PL, with no diagnostic.
- **AIE coefficient lane 12 is reserved** for `chroma_mode`. The 3x3 kernel
  occupies lanes 0 to 11 of the 16-entry buffer and the kernel reads only those,
  so the mode rides along without a change to the graph interface. Do not reuse
  lanes 12 to 15 for anything else.
- **The PL kernel signature carries `chroma_mode` and `shift`.** Host and
  `.xclbin` must be built and deployed as a pair; a mismatch fails at `setArg`.
- **The PL chroma stage reads its input stream in both modes.** It sits inside a
  dataflow region, where an unconsumed stream stalls the pipeline.

### Image mode

The application performs a pixel-by-pixel comparison between the output from
the hardware accelerator and the reference image. Both the processed and
reference images are saved in JPG format, allowing users to inspect the
processed image for any artifacts. By default the application will include
 three example \*.jpg files:

- hw_in.jpg - Is an input reference image to both the HW accelerator & SW reference
- sw_ref.jpg - Is an output image as processed by the OpenCV SW libraries
- hw_out.jpg - Is an output image as processed by the HW acceleration library (AIE or PL)

### Video and Camera modes

When using camera mode, make sure the camera is plugged into a USB 3.0 port.

In video and camera modes, frames are decoded via GStreamer, processed through
the filter2D accelerator (AIE or PL depending on `-m` flag), and either saved
to hw_out.mp4 or displayed on screen (with the `-d` flag). In AIE mode, the
processing loop uses double-buffered XRT buffers to overlap frame decoding
with AIE computation. In PL mode, frames are processed synchronously via
OpenCL. In camera mode with file output, recording stops automatically after
30 seconds by default to prevent excessively large output files. Use the `-t`
flag to specify a different duration in seconds. Press Ctrl+C at any time
during camera or display mode to gracefully stop processing early.

## Compiling F2d application

The application depends on OpenCV and GStreamer library dev packages and
installing them is required before compilation.

```
$ sudo apt install libopencv-dev libboost-all-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
$ cd emb-plus-examples/simple-app/filter2d
$ make
```

Note: the Vitis_Libraries submodule is pinned to the revision this app is built
and verified against. Use the pinned revision rather than the branch tip.

# License

(C) Copyright 2024 - 2026, Advanced Micro Devices Inc.\
SPDX-License-Identifier: Apache-2.0
