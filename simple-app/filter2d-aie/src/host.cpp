/*
 * Copyright (C) 2019-2022 Xilinx, Inc
 * Copyright (C) 2022-2024 Advance Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define PROFILE
#define int64 INT164
#define uint64 UINT164
//#define DEBUG_MODE 1 // uncomment to enable debug information

#include <chrono>
#include <common/xf_aie_sw_utils.hpp>
#include <common/xfcvDataMovers.h>
#include <fstream>
#include <iostream>

static constexpr int RESIZE_HEIGHT = 1080;
static constexpr int RESIZE_WIDTH = 1920;
/* Graph specific configuration */
static constexpr int VECTORIZATION_FACTOR = 16;
static constexpr int TILE_WIDTH = 128;
static constexpr int TILE_HEIGHT = 16;

/* Fixed filter Coefficients for edge filter */
float kData[9] = {0, 1, 0, 1, -4, 1, 0, 1, 0};

/* Helper Function */
void printHelp(void) {
    std::cout
        << "=====================================================" << std::endl
        << "Filter2d AIE Acceleration Example Application Usage " << std::endl
        << "=====================================================" << std::endl
        << "<Executable Name> -i [input_image_path] -u [user_xclbin]"
        << std::endl
        << std::endl
        << "Example with default image and xclbin:\tfilter2D_accel_aie.elf "
        << std::endl
        << "Example with custom image:\t\tfilter2D_accel_aie.elf -i "
           "<path/testimg.jpg>"
        << std::endl
        << std::endl
        << "Note: Fixed coefficients are used for convolution resulting in "
           "an Edge-Filter"
        << std::endl;
}

/* SW equivalent of the Convolution algorithm implemented on AIE */
void run_ref(uint8_t *srcImageR, uint8_t *dstRefImage, float coeff[9],
             int16_t height, int16_t width) {
    float window[9];

    width *= 2;

    for (int i = 0; i < height * width; i++) {
        int row = i / width;
        int col = i % width;

        if (col % 2) {
            dstRefImage[i] = srcImageR[i];
            continue;
        }

        int w = 0;
        for (int j = -1; j <= 1; j++) {
            for (int k = -2; k <= 2; k += 2) {
                int r = std::max(row + j, 0);
                int c = std::max(col + k, 0);
                r = std::min(r, height - 1);
                c = std::min(c, width - 2);
                window[w++] = srcImageR[r * width + c];
            }
        }

        float s = 0;
        for (int j = 0; j < 9; j++)
            s += window[j] * coeff[j];
        dstRefImage[i] = s;
    }
    return;
}

/* Color Conversion RBG to YUY2 */
void cvtColor_RGB2YUY2(cv::Mat &src, cv::Mat &dst) {
    cv::Mat temp;
    std::vector<uint8_t> v1;

    cv::cvtColor(src, temp, cv::COLOR_BGR2YUV);
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

/* Compare image data between the AIE computation and SW reference model */
void compareResult(cv::Mat hwOut, uint8_t *cvRef, int rows, int cols) {
    std::vector<uint8_t> dstData_vec;
    dstData_vec.assign(hwOut.data, (hwOut.data + hwOut.total()));
    int acceptableError = 1;
    int errCount = 0;
    uint8_t *dataOut = (uint8_t *)dstData_vec.data();
    for (int i = 0; i < cols * rows; i++) {
        if (abs(cvRef[i] - dataOut[i]) > acceptableError) {
#ifdef DEBUG_MODE
            std::cout << "err at : i=" << i
                      << " err=" << abs(cvRef[i] - dataOut[i]) << "="
                      << unsigned(cvRef[i]) << "-" << unsigned(dataOut[i])
                      << std::endl;
#endif
            errCount++;
        }
    }
    if (errCount) {
        std::cout << "Test failed, " << errCount << " Bytes unmatched"
                  << std::endl;
    } else {
        std::cout << "Test passed" << std::endl;
    }
}

int main(int argc, char **argv) {

    std::string arg, inputImage, userXclbin;
    inputImage = "/opt/xilinx/testimg/HD.jpg";
    userXclbin = "/opt/xilinx/firmware/emb_plus/ve2302_pcie_qdma/base/test/"
                 "filter2d_aie.xclbin";

    if (argc > 5) {
        std::cerr << "Invalid number for arguments passed, calling help menu."
                  << std::endl;
        printHelp();
        return -1;
    }

    arg = (argc > 1) ? argv[1] : "";
    if (arg == "-h" || arg == "--h" || arg == "help" || arg == "-help" ||
        arg == "--help") {
        printHelp();
        return 0;
    }

    for (int i = 1; i < argc; i += 2) {
        if (std::string(argv[i]) == "-i" && i + 1 < argc) {
            inputImage = argv[i + 1];
        } else if (std::string(argv[i]) == "-u" && i + 1 < argc) {
            userXclbin = argv[i + 1];
        } else {
            std::cerr << "Invalid arguments passed, calling help menu."
                      << std::endl;
            printHelp();
            return -1;
        }
    }

    /* Read image and Resize */
    cv::Mat srcImageR, temp1, temp2;
    temp1 = cv::imread(inputImage, 1);
    if (temp1.data == NULL) {
        std::cout << "Failed to read Image from path " << inputImage
                  << std::endl;
    }
    cv::resize(temp1, temp1, cv::Size(RESIZE_WIDTH, RESIZE_HEIGHT), 0, 0,
               cv::INTER_LINEAR);
    cvtColor_RGB2YUY2(temp1, temp2);
    temp2.convertTo(srcImageR, CV_8UC2);
    cv::cvtColor(srcImageR, temp2, cv::COLOR_YUV2BGR_YUYV);
    imwrite("hw_in.jpg", temp2);
    std::cout << "Dumping JPG input image consumed by AIE and Reference model"
              << std::endl;

    std::cout << "Image size" << std::endl;
    std::cout << "Rows : " << srcImageR.rows << std::endl;
    std::cout << "Cols : " << srcImageR.cols << std::endl;
    std::cout << "Channels : " << srcImageR.channels() << std::endl;
    std::cout << "Element size : " << srcImageR.elemSize() << std::endl;
    std::cout << "Total pixels : " << srcImageR.total() << std::endl;
    std::cout << "Type : " << srcImageR.type() << std::endl;
    int width = srcImageR.cols;
    int height = srcImageR.rows;

    /* Run convolution as a reference model  */
    std::cout << "Starting Software implemented reference model...\n";
    std::vector<uint8_t> srcData_vec;
    uint8_t *dataRefOut =
        (uint8_t *)std::malloc(srcImageR.total() * srcImageR.elemSize());
    srcData_vec.assign(height * width * 2, 0);
    memcpy(srcData_vec.data(), srcImageR.data,
           srcImageR.total() * srcImageR.elemSize());
    run_ref((uint8_t *)srcData_vec.data(), dataRefOut, kData, srcImageR.rows,
            srcImageR.cols);
    cv::Mat ref(srcImageR.rows, srcImageR.cols, srcImageR.type(), dataRefOut);
    cv::cvtColor(ref, temp1, cv::COLOR_YUV2BGR_YUYV);
    imwrite("sw_ref.jpg", temp1);
    std::cout << "Dumping JPG output image from Reference model (software "
                 "implementation)"
              << std::endl;

    /* Run convolution on AIE   */
    std::cout << "Loading xclbin " << std::endl;
    const char *xclBinName = userXclbin.c_str();
    xF::deviceInit(xclBinName);
    void *srcData = nullptr;
    std::cout << "Creating  input buffer..." << std::endl;
    xrt::bo src_hndl =
        xrt::bo(xF::gpDhdl, (srcImageR.total() * srcImageR.elemSize()), 0, 0);
    srcData = src_hndl.map();
    memcpy(srcData, srcImageR.data, (srcImageR.total() * srcImageR.elemSize()));
    src_hndl.sync(XCL_BO_SYNC_BO_TO_DEVICE,
                  srcImageR.total() * srcImageR.elemSize(), 0);
    std::cout << "Creating  output buffer..." << std::endl;
    void *dstData = nullptr;
    xrt::bo dst_hndl =
        xrt::bo(xF::gpDhdl, (srcImageR.total() * srcImageR.elemSize()), 0, 0);
    dstData = dst_hndl.map();
    cv::Mat dst(height, width, srcImageR.type(), dstData);
    std::cout << "Initializing Tiler & Stitcher.\n";
    xF::xfcvDataMovers<xF::TILER, int16_t, TILE_HEIGHT, TILE_WIDTH,
                       VECTORIZATION_FACTOR>
        tiler(1, 1);
    xF::xfcvDataMovers<xF::STITCHER, int16_t, TILE_HEIGHT, TILE_WIDTH,
                       VECTORIZATION_FACTOR>
        stitcher;

    START_TIMER
    tiler.compute_metadata(srcImageR.size());
    STOP_TIMER("Meta data compute time")

    std::chrono::microseconds tt(0);
    START_TIMER
    auto tiles_sz = tiler.host2aie_nb(&src_hndl, srcImageR.size());
    stitcher.aie2host_nb(&dst_hndl, dst.size(), tiles_sz);
    tiler.wait();
    stitcher.wait();
    dst_hndl.sync(XCL_BO_SYNC_BO_FROM_DEVICE,
                  srcImageR.total() * srcImageR.elemSize(), 0);

    STOP_TIMER("yuy2 filter2D function")
    std::cout << "Data transfer complete (Stitcher)\n";
    tt += tdiff;

    cv::cvtColor(dst, temp2, cv::COLOR_YUV2BGR_YUYV);
    imwrite("hw_out.jpg", temp2);
    std::cout << "Dumping JPG output image from AIE implementation"
              << std::endl;
    compareResult(dst, dataRefOut, height, width);

    std::free(dataRefOut);

    return 0;
}
