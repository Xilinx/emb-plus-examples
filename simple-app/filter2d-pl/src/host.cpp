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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <iostream>
#include <sys/stat.h>
#include <CL/cl.h>
#include "xcl2.hpp"
#include <opencv2/opencv.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <string>

#define FILTER_HEIGHT 3
#define FILTER_WIDTH 3
#define RESIZE_HEIGHT 1080
#define RESIZE_WIDTH 1920
#define FOURCC 0x56595559 // YUYV Format
#define DEFAULT_ACCEL "/opt/xilinx/filter2d-pl/binary_container_1.xclbin"

// opencv filter coefficients
float cvkdata[][FILTER_HEIGHT][FILTER_WIDTH] = {
	// opencv hgrad
	{
		{-1, -1, -1},
		{0, 0, 0},
		{1, 1, 1}},
	// opencv emboss
	{
		{-2, -1, 0},
		{-1, 1, 1},
		{0, 1, 2}},
	// opencv edge
	{
		{0, 1, 0},
		{1, -4, 1},
		{0, 1, 0}},
	// opencv blur
	{
		{1, 1, 1},
		{1, -7, 1},
		{1, 1, 1}},
	// opencv identity
	{
		{0, 0, 0},
		{0, 1, 0},
		{0, 0, 0}},
	// opencv hsobel
	{
		{1, 2, 1},
		{0, 0, 0},
		{-1, -2, -1}}};

void matrixDeconstructor(float matrix[][FILTER_HEIGHT], short int Darray[])
{
	int i = 0;
	for (int r = 0; r < FILTER_HEIGHT; r++)
	{
		for (int c = 0; c < FILTER_WIDTH; c++)
		{
			Darray[i++] = (short int)matrix[r][c];
		}
	}
}

const char *filterArgs[] = {
	"Horizontal-Gradient",
	"Emboss",
	"Edge",
	"Blur",
	"Identity",
	"Horizontal-sobel"};

enum Filter
{
	HGRAD,
	EMBOSS,
	EDGE,
	BLUR,
	IDENTITY,
	HSOBEL,
	MAX_FILTER_NUM
};

void print_Filter_Options()
{
	fprintf(stderr, "Available Filter Options:\n");
	for (int i = 0; i < (int)MAX_FILTER_NUM; i++)
		fprintf(stderr, "   %s\n", filterArgs[i]);
}

int print_Help()
{
	fprintf(stderr, "=================================================\n");
	fprintf(stderr, "Filter2d Accelertation Example Application Usage \n");
	fprintf(stderr, "=================================================\n");
	fprintf(stderr, "<Executable Name> <Filter> [input_image_path]\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "./filter2D_accel_pl.elf Emboss\n");
	fprintf(stderr, "\n");
	print_Filter_Options();
	return -1;
}

enum Filter get_coeff_string(std::string argv)
{

	for (int i = 0; i < (int)MAX_FILTER_NUM; i++)
	{
		if (strcmp(filterArgs[i], argv.c_str()) == 0)
			return (enum Filter)i;
	}
	fprintf(stderr, "Invalid Filter Type Usage: see below options \n");
	print_Filter_Options();
	exit(EXIT_FAILURE);
}

void cvtColor_RGB2YUY2(cv::Mat &src, cv::Mat &dst)
{
	cv::Mat temp;
	cv::cvtColor(src, temp, cv::COLOR_BGR2YUV);

	std::vector<uint8_t> v1;
	for (int i = 0; i < src.rows; i++)
	{
		for (int j = 0; j < src.cols; j++)
		{
			v1.push_back(temp.at<cv::Vec3b>(i, j)[0]);
			j % 2 ? v1.push_back(temp.at<cv::Vec3b>(i, j)[2]) : v1.push_back(temp.at<cv::Vec3b>(i, j)[1]);
		}
	}

	cv::Mat yuy2(src.rows, src.cols, CV_8UC2);
	memcpy(yuy2.data, v1.data(), src.cols * src.rows * 2);
	dst = yuy2;
}

void compareResuts(cv::Mat &out_img, cv::Mat &ref, int rows, int cols)
{
	std::vector<uint8_t> hwData_vec;
	std::vector<uint8_t> cvData_vec;
	hwData_vec.assign(out_img.data, (out_img.data + out_img.total()));
	cvData_vec.assign(ref.data, (ref.data + ref.total()));

	int acceptableError = 1;
	int errCount = 0;
	uint8_t *dataOut = (uint8_t *)hwData_vec.data();
	uint8_t *dataRefOut = (uint8_t *)cvData_vec.data();
	for (int i = 0; i < cols * rows; i++)
	{
		if (abs(dataRefOut[i] - dataOut[i]) > acceptableError)
		{
			std::cout << "err at : i=" << i << " err=" << abs(dataRefOut[i] - dataOut[i]) << "=" << unsigned(dataRefOut[i]) << "-" << unsigned(dataOut[i]) << std::endl;
			errCount++;
		}
	}
	if (errCount)
	{
		std::cout << "Result: Test failed" << std::endl;
	}
	else
		std::cout << "Result: Test Passed" << std::endl;
}

int main(int argc, char **argv)
{
	std::string arg, defaultImg;
	enum Filter Ftype;
	short int Darray[9] = {0};
	defaultImg = "/opt/xilinx/filter2d_pl/test_image/sample_image_4k.jpg";

	if (argc < 2 || argc > 3)
	{
		fprintf(stderr, "Invalid number for arguments\n");
		return print_Help();
	}
	arg = argv[1];
	if (arg == "-h" || arg == "--h" || arg == "help" || arg == "-help" ||
		arg == "--help")
		return print_Help();
	if (argc == 3)
		defaultImg = argv[2];

	Ftype = get_coeff_string(arg);

	////////////////////////// CV START /////////////////////////////////////
	cv::Mat InImage, out_img, ref, resized_img, temp, hwin_img;
	// read Input image & resize to HD (jpg)
	InImage = cv::imread(defaultImg, cv::IMREAD_COLOR);
	if (InImage.data == NULL)
	{
		fprintf(stderr, "Failed to open image at %s\n",
				defaultImg.c_str());
		return -1;
	}
	else
		printf("Input Image: height:%u, width:%u, channels:%u, pixels: %zu, bytes: %zu\n",
			   InImage.rows, InImage.cols, InImage.channels(),
			   InImage.total(), InImage.total() * InImage.elemSize());

	std::cout << "Resizing input image from " << InImage.rows << "x" <<
		InImage.cols << " to " << RESIZE_WIDTH << "x" << RESIZE_HEIGHT <<
		std::endl;
	cv::resize(InImage, resized_img, cv::Size(RESIZE_WIDTH, RESIZE_HEIGHT),
			   0, 0, cv::INTER_LINEAR);

	// convert jpg image to yuv format (hwinput)
	cvtColor_RGB2YUY2(resized_img, temp);
	temp.convertTo(hwin_img, CV_8UC2);

	// dump yuv and jpg (hwin_img)
	cv::cvtColor(hwin_img, temp, cv::COLOR_YUV2BGR_YUYV);
	imwrite("hwin_Img-resized-HD.jpg", temp);

	// creating ocv ref image
	cv::Mat filter(3, 3, CV_32F, cvkdata[(int)Ftype]);
	cv::Point anchor = cv::Point(-1, -1);
	std::vector<cv::Mat> yuvChannels, concat_img;

	cv::split(hwin_img, yuvChannels);
	cv::filter2D(yuvChannels[0], temp, CV_8U, filter, anchor, 0,
				 cv::BORDER_CONSTANT);
	concat_img.push_back(temp);			  // Y channel
	concat_img.push_back(yuvChannels[1]); // UV channel
	cv::merge(concat_img, ref);
	cv::cvtColor(ref, temp, cv::COLOR_YUV2BGR_YUYV);
	imwrite("ocv_ref.jpg", temp); // CV reference image

	////////////////////////// CL START /////////////////////////////////////
	int height = hwin_img.rows;
	int width = hwin_img.cols;
	int chan = hwin_img.channels();
	cl_int err;

	out_img.create(height, width, CV_8UC(chan));

	// Find the versal device
	printf("create device object\n");
	std::vector<cl::Device> devices = xcl::get_xil_devices();
	cl::Device device = devices[0];
	cl::Context context(device);

	// create the command queue
	cl::CommandQueue q(context, device, CL_QUEUE_PROFILING_ENABLE, &err);
	printf("create command queue %i\n", err);

	// Program Kernel
	printf("Programming kernel\n");
	std::string device_name = device.getInfo<CL_DEVICE_NAME>();
	auto binaryFile = xcl::read_binary_file(DEFAULT_ACCEL);
	cl::Program::Binaries bins{{binaryFile.data(), binaryFile.size()}};
	devices.resize(1);
	cl::Program program(context, devices, bins);
	cl::Kernel krnl(program, "filter2d_pl_accel");

	// Allocate Buffer in Global Memory
	printf("Allocate buffer in global memory\n");
	cl::Buffer imageToDevice(context, CL_MEM_READ_ONLY, (height * width * chan), NULL, &err);
	printf("create image to device buffer status:  %i\n", err);

	cl::Buffer imageFromDevice(context, CL_MEM_WRITE_ONLY, (height * width * chan), NULL, &err);
	printf("create image from device status:       %i\n", err);

	cl::Buffer kernelFilterToDevice(context, CL_MEM_READ_ONLY, sizeof(short int) * 9, NULL, &err);
	printf("create kernel filter to device status: %i\n", err);

	// Copying input data to Device buffer from host memory
	printf("1. Copying input data to device buffer\n");
	printf("%i Bytes\n", height * width * chan);
	q.enqueueWriteBuffer(imageToDevice, CL_TRUE, 0, (height * width * chan),
						 (unsigned short *)hwin_img.data);

	printf("2. Copying kernel data to device buffer\n");
	matrixDeconstructor(cvkdata[(int)Ftype], Darray);
	q.enqueueWriteBuffer(kernelFilterToDevice, CL_TRUE, 0, sizeof(short int) * 9, (short int *)Darray);

	printf("set kernel arguments\n");

	// Set the kernel arguments
	krnl.setArg(0, imageToDevice);
	krnl.setArg(1, imageFromDevice);
	krnl.setArg(2, kernelFilterToDevice);
	krnl.setArg(3, height);
	krnl.setArg(4, width);
	krnl.setArg(5, FOURCC); // fourcc in
	krnl.setArg(6, FOURCC); // fourcc out

	// Profiling Objects
	cl_ulong start = 0;
	cl_ulong end = 0;
	double diff_prof = 0.0f;
	cl::Event event_sp;

	// Launch the kernel
	printf("launch the kernel\n");
	q.enqueueTask(krnl, NULL, &event_sp);
	clWaitForEvents(1, (const cl_event *)&event_sp);

	// Profiling
	printf("profiling\n");
	event_sp.getProfilingInfo(CL_PROFILING_COMMAND_START, &start);
	event_sp.getProfilingInfo(CL_PROFILING_COMMAND_END, &end);
	diff_prof = end - start;
	std::cout << (diff_prof / 1000000) << "ms" << std::endl;

	// Copying Device result data to Host memory
	printf("Copying results\n");
	q.enqueueReadBuffer(imageFromDevice, CL_TRUE, 0, (height * width * chan), (unsigned short *)out_img.data);

	q.finish();

	printf("Out Image: height:%u, width:%u, channels:%u, pixels: %zu, bytes: %zu\n",
		   out_img.rows, out_img.cols, out_img.channels(),
		   out_img.total(), out_img.total() * out_img.elemSize());

	cv::cvtColor(out_img, temp, cv::COLOR_YUV2BGR_YUYV);
	imwrite("hw_out.jpg", temp); // reference image

	compareResuts(out_img, ref, hwin_img.rows, hwin_img.cols);

	return 0;
}
