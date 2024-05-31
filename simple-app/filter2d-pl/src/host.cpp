/**
 * Copyright (C) 2019-2022 Xilinx, Inc
 * Copyright (C) 2022-2024 Advance Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include "xcl2.hpp"
#include <CL/cl.h>
#include <iostream>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>

// #define DEBUG_MODE 1 // uncomment to enable debug information
#define FILTER_HEIGHT 3
#define FILTER_WIDTH 3
#define RESIZE_HEIGHT 1080
#define RESIZE_WIDTH 1920
#define FOURCC 0x56595559 // YUYV Format

// opencv filter coefficients
float cvkdata[][FILTER_HEIGHT][FILTER_WIDTH] = {
    // opencv hgrad
    {{-1, -1, -1}, {0, 0, 0}, {1, 1, 1}},
    // opencv emboss
    {{-2, -1, 0}, {-1, 1, 1}, {0, 1, 2}},
    // opencv edge
    {{0, 1, 0}, {1, -4, 1}, {0, 1, 0}},
    // opencv blur
    {{1, 1, 1}, {1, -7, 1}, {1, 1, 1}},
    // opencv identity
    {{0, 0, 0}, {0, 1, 0}, {0, 0, 0}},
    // opencv hsobel
    {{1, 2, 1}, {0, 0, 0}, {-1, -2, -1}}};

void matrixDeconstructor(float matrix[][FILTER_WIDTH], short int Darray[]) {
    int i;

    i = 0;
    for (int r = 0; r < FILTER_HEIGHT; r++) {
        for (int c = 0; c < FILTER_WIDTH; c++) {
            Darray[i++] = (short int)matrix[r][c];
        }
    }
}

const char *filterArgs[] = {
    "Horizontal-Gradient", "Emboss", "Edge", "Blur", "Identity",
    "Horizontal-Sobel"};

enum Filter { HGRAD, EMBOSS, EDGE, BLUR, IDENTITY, HSOBEL, MAX_FILTER_NUM };

void printFilterOptions(void) {
    std::cout << "Available Filter Options:\n"
              << "------------------------" << std::endl;
    for (int i = 0; i < (int)MAX_FILTER_NUM; i++)
        std::cout << filterArgs[i] << std::endl;
}

void printHelp(void) {
    std::cout
        << "=================================================" << std::endl
        << "Filter2d Accelertation Example Application Usage " << std::endl
        << "=================================================" << std::endl
        << "<Executable Name> <Filter> -i [input_image_path] -u [user_xclbin]"
        << std::endl
        << std::endl
        << "Example: filter2D_accel_pl.elf Emboss" << std::endl
        << std::endl;
    printFilterOptions();
}

enum Filter getCoeffString(std::string argv) {
    for (int i = 0; i < (int)MAX_FILTER_NUM; i++) {
        if (strcmp(filterArgs[i], argv.c_str()) == 0)
            return ((enum Filter)i);
    }
    std::cerr << "Invalid Filter Type Usage: see below options \n";
    printFilterOptions();
    exit(EXIT_FAILURE);
}

void cvtColorRGB2YUY2(cv::Mat &src, cv::Mat &dst) {
    cv::Mat temp;
    cv::cvtColor(src, temp, cv::COLOR_BGR2YUV);
    std::vector<uint8_t> v1;
    for (int i = 0; i < src.rows; i++) {
        for (int j = 0; j < src.cols; j++) {
            v1.push_back(temp.at<cv::Vec3b>(i, j)[0]);
            j % 2 ? v1.push_back(temp.at<cv::Vec3b>(i, j)[2])
                  : v1.push_back(temp.at<cv::Vec3b>(i, j)[1]);
        }
    }
    cv::Mat yuy2(src.rows, src.cols, CV_8UC2);
    memcpy(yuy2.data, v1.data(), src.cols * src.rows * 2);
    dst = yuy2;
}

void compareResuts(cv::Mat &outImg, cv::Mat &ref, int rows, int cols) {
    int acceptableError;
    int errCount;
    uint8_t *dataOut;
    uint8_t *dataRefOut;

    std::vector<uint8_t> hwDataVec;
    std::vector<uint8_t> cvDataVec;
    hwDataVec.assign(outImg.data, (outImg.data + outImg.total()));
    cvDataVec.assign(ref.data, (ref.data + ref.total()));
    acceptableError = 1;
    errCount = 0;
    dataOut = (uint8_t *)hwDataVec.data();
    dataRefOut = (uint8_t *)cvDataVec.data();
    for (int i = 0; i < cols * rows; i++) {
        if (abs(dataRefOut[i] - dataOut[i]) > acceptableError) {
#ifdef DEBUG_MODE
            std::cout << "err at : i=" << i
                      << " err=" << abs(dataRefOut[i] - dataOut[i]) << "="
                      << unsigned(dataRefOut[i]) << "-" << unsigned(dataOut[i])
                      << std::endl;
#endif
            errCount++;
        }
    }
    if (errCount) {
        std::cout << "Result: Test failed " << errCount << "/" << cols * rows
                  << " unmatched Bytes" << std::endl;
    } else
        std::cout << "Result: Test Passed" << std::endl;
}

int main(int argc, char **argv) {
    enum Filter Ftype;
    short int Darray[9] = {0};
    int height;
    int width;
    int chan;
    cl_int err;
    cl_ulong start;
    cl_ulong end;
    double diffProf;

    std::string arg, inputImage, userXclbin;
    inputImage = "/opt/xilinx/filter2d-pl/test_image/sample_image_4k.jpg";
    userXclbin = "/opt/xilinx/firmware/emb_plus/ve2302_pcie_qdma/base/test/"
                 "filter2d_pl.xclbin";

    if (argc < 2 || argc > 6) {
        std::cerr << "Invalid number for arguments passed" << std::endl;
        printHelp();
        return -1;
    }

    arg = argv[1];
    if (arg == "-h" || arg == "--h" || arg == "help" || arg == "-help" ||
        arg == "--help") {
        printHelp();
        return 0;
    }

    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "-i" && i + 1 < argc) {
            inputImage = argv[i + 1];
        } else if (std::string(argv[i]) == "-u" && i + 1 < argc) {
            userXclbin = argv[i + 1];
        }
    }

    Ftype = getCoeffString(arg);

    ////////////////////////// CV START /////////////////////////////////////
    cv::Mat InImage, outImg, ref, resizedImg, temp, hwinImg;

    // read Input image & resize to HD (jpg)
    InImage = cv::imread(inputImage, cv::IMREAD_COLOR);
    if (InImage.data == NULL) {
        std::cerr << "Failed to open image at PATH: " << inputImage
                  << std::endl;
        return (-1);
    } else
        std::cout << "Input Image: height:" << InImage.rows
                  << ", width:" << InImage.cols
                  << ", channels:" << InImage.channels()
                  << ", pixels:" << InImage.total()
                  << ", bytes:" << InImage.total() * InImage.elemSize()
                  << ", Input image path:" << inputImage << std::endl;

    std::cout << "Resizing input image from " << InImage.rows << "x"
              << InImage.cols << " to " << RESIZE_WIDTH << "x" << RESIZE_HEIGHT
              << std::endl;
    cv::resize(InImage, resizedImg, cv::Size(RESIZE_WIDTH, RESIZE_HEIGHT), 0, 0,
               cv::INTER_LINEAR);

    // convert jpg image to yuv format (hwinput)
    cvtColorRGB2YUY2(resizedImg, temp);
    temp.convertTo(hwinImg, CV_8UC2);

    // dump yuv and jpg (hwinImg)
    cv::cvtColor(hwinImg, temp, cv::COLOR_YUV2BGR_YUYV);
    imwrite("hwin_HD.jpg", temp);

    // creating ocv ref image
    cv::Mat filter(3, 3, CV_32F, cvkdata[(int)Ftype]);
    cv::Point anchor = cv::Point(-1, -1);
    std::vector<cv::Mat> yuvChannels, concatImg;
    cv::split(hwinImg, yuvChannels);
    cv::filter2D(yuvChannels[0], temp, CV_8U, filter, anchor, 0,
                 cv::BORDER_CONSTANT);
    concatImg.push_back(temp);           // Y channel
    concatImg.push_back(yuvChannels[1]); // UV channel
    cv::merge(concatImg, ref);
    cv::cvtColor(ref, temp, cv::COLOR_YUV2BGR_YUYV);
    imwrite("ocv_ref.jpg", temp); // CV reference image

    ////////////////////////// CL START /////////////////////////////////////
    height = hwinImg.rows;
    width = hwinImg.cols;
    chan = hwinImg.channels();
    outImg.create(height, width, CV_8UC(chan));

    // Find the versal device
    std::cout << "create device object" << std::endl;
    std::vector<cl::Device> devices = xcl::get_xil_devices();
    cl::Device device = devices[0];
    cl::Context context(device);

    // create the command queue
    cl::CommandQueue q(context, device, CL_QUEUE_PROFILING_ENABLE, &err);
    std::cout << "create command queue " << err << std::endl;

    // Program Kernel
    std::cout << "Programming kernel" << std::endl;
    std::string device_name = device.getInfo<CL_DEVICE_NAME>();
    auto binaryFile = xcl::read_binary_file(userXclbin);
    cl::Program::Binaries bins{{binaryFile.data(), binaryFile.size()}};
    devices.resize(1);
    cl::Program program(context, devices, bins);
    cl::Kernel krnl(program, "filter2d_pl_accel", &err);
    if (err) {
        std::cerr << "Failed to program kernel" << std::endl;
        return (-1);
    }
    // Allocate Buffer in Global Memory
    std::cout << "Allocate buffer in global memory" << std::endl;
    cl::Buffer imageToDevice(context, CL_MEM_READ_ONLY, (height * width * chan),
                             NULL, &err);
    std::cout << "create image to device buffer status:  " << err << std::endl;
    cl::Buffer imageFromDevice(context, CL_MEM_WRITE_ONLY,
                               (height * width * chan), NULL, &err);
    std::cout << "create image from device status:       " << err << std::endl;
    cl::Buffer kernelFilterToDevice(context, CL_MEM_READ_ONLY,
                                    sizeof(short int) * 9, NULL, &err);
    std::cout << "create kernel filter to device status: " << err << std::endl;

    // Copying input data to Device buffer from host memory
    std::cout << "1. Copying input data to device buffer"
              << height * width * chan << " Bytes" << std::endl;

    q.enqueueWriteBuffer(imageToDevice, CL_TRUE, 0, (height * width * chan),
                         (unsigned short *)hwinImg.data);
    std::cout << "2. Copying kernel data to device buffer" << std::endl;

    matrixDeconstructor(cvkdata[(int)Ftype], Darray);
    q.enqueueWriteBuffer(kernelFilterToDevice, CL_TRUE, 0,
                         sizeof(short int) * 9, (short int *)Darray);
    std::cout << "set kernel arguments" << std::endl;

    // Set the kernel arguments
    krnl.setArg(0, imageToDevice);
    krnl.setArg(1, imageFromDevice);
    krnl.setArg(2, kernelFilterToDevice);
    krnl.setArg(3, height);
    krnl.setArg(4, width);
    krnl.setArg(5, FOURCC); // fourcc in
    krnl.setArg(6, FOURCC); // fourcc out

    // Profiling Objects
    start = 0;
    end = 0;
    diffProf = 0.0f;
    cl::Event eventSp;

    // Launch the kernel
    std::cout << "launch the kernel" << std::endl;
    q.enqueueTask(krnl, NULL, &eventSp);
    clWaitForEvents(1, (const cl_event *)&eventSp);

    // Profiling
    std::cout << "profiling" << std::endl;
    eventSp.getProfilingInfo(CL_PROFILING_COMMAND_START, &start);
    eventSp.getProfilingInfo(CL_PROFILING_COMMAND_END, &end);
    diffProf = end - start;
    std::cout << (diffProf / 1000000) << "ms" << std::endl;

    // Copying Device result data to Host memory
    std::cout << "Copying results" << std::endl;
    q.enqueueReadBuffer(imageFromDevice, CL_TRUE, 0, (height * width * chan),
                        (unsigned short *)outImg.data);
    q.finish();

    std::cout << "Out Image: height:" << outImg.rows
              << ", width:" << outImg.cols << ", channels:" << outImg.channels()
              << ", pixels:" << outImg.total()
              << ", bytes:" << outImg.total() * outImg.elemSize() << std::endl;

    cv::cvtColor(outImg, temp, cv::COLOR_YUV2BGR_YUYV);
    imwrite("hw_out.jpg", temp); // reference image
    compareResuts(outImg, ref, hwinImg.rows, hwinImg.cols);
    return (0);
}
