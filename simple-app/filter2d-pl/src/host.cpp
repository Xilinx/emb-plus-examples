/**
* Copyright (C) 2019-2021 Xilinx, Inc
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

ssize_t read_file_to_buf(const char *file, uint8_t **buf, size_t *len) {
    ssize_t ret;
    FILE *fp;

    fp = fopen(file, "r");
    if (fp == NULL) {
        return -1;
    }

    ret = fseek(fp, 0, SEEK_END);
    if (ret < 0) {
        return -1;
    }

    *len = (size_t)ftell(fp);
    if ((*len) < 0) {
        return -1;
    }

    rewind(fp);

    *buf = (uint8_t *)malloc(sizeof(uint8_t) * (*len));
    if (*buf == NULL) {
        return -1;
    }

    fread(*buf, (*len), sizeof(char), fp);
    fclose(fp);

    return 0;
}

ssize_t write_buf_to_file(const uint8_t *buf, const size_t len,
                                 const char *file) {
    size_t ret;
    FILE *fp;

    fp = fopen(file, "w");
    if (fp == NULL) {
        return -1;
    }

    ret = fwrite(buf, len, sizeof(char), fp);
    if (ret < 0) {
        return -1;
    }

    fclose(fp);

    return 0;
}

int main(int argc, char** argv)
{
	if (argc != 4)
	{
		fprintf(stderr,"Invalid Number of Arguments!\nUsage:\n");
		fprintf(stderr,"<Executable Name> <input image path> <width> <height>\n");
		return -1;
	}

	uint8_t *img;
	size_t len;


	read_file_to_buf(argv[1], &img, &len);
	printf("image size %zu\n",len);


	////////////////////// OPENCV ////////////////////////////////////////
	cv::Mat in_gray, in_conv_img, out_img, in_img_p, ocv_ref, diff, temp_img;

	cv::Mat in_img(atoi(argv[3]), atoi(argv[2]), CV_8UC2, img);
	write_buf_to_file(in_img.data,len,"input.yuv");
	cv::cvtColor(in_img,temp_img, cv::COLOR_YUV2BGR_YUYV);
	imwrite("input.jpg", temp_img);  // reference image

	in_conv_img = in_img;

	if (in_img.data == NULL)
	{
		fprintf(stderr,"Cannot open image at %s\n",argv[1]);
		return 0;
	}

	printf("Original: height:%u, width:%u, channels:%u, depth: %i, size: %u, pixels: %zu, bytes: %zu\n",
			in_img.rows, in_img.cols, in_img.channels(), in_img.depth(),
			in_img.elemSize(), in_img.total(), in_img.total() * in_img.elemSize());


	printf("filter height: %u, width: %u\n",FILTER_HEIGHT, FILTER_WIDTH);

	// opencv hgrad
	float cvkdata[3][3] = {
		{-1, -1, -1},
		{0, 0, 0},
		{1, 1, 1}

	};

	//xfopencv hgrad
	short int kdata[] = {
		-1, -1, -1,
		0, 0, 0,
		1, 1, 1
	};
/*
	// opencv emboss
	float cvkdata[3][3] = {
		{-2, -1, 0},
		{-1, 1, 1},
		{0, 1, 2}

	};

	//xfopencv emboss
	short int kdata[] = {
		-2, -1, 0,
		-1, 1, 1,
		0, 1, 2
	};
	// opencv edge
	float cvkdata[3][3] = {
		{0, 1, 0},
		{1, -4, 1},
		{0, 1, 0}

	};

	//xfopencv edge
	short int kdata[] = {
		0, 1, 0,
		1, -4, 1,
		0, 1, 0
	};

	// opencv blur
	float cvkdata[3][3] = {
		{1, 1, 1},
		{1, -7, 1},
		{1, 1, 1}

	};

	//xfopencv blur
	short int kdata[] = {
		1, 1, 1,
		1, -7, 1,
		1, 1, 1
	};
	// opencv identity
	float cvkdata[3][3] = {
		{0, 0, 0},
		{0, 1, 0},
		{0, 0, 0}

	};

	//xfopencv identity
	short int kdata[] = {
		0, 0, 0,
		0, 1, 0,
		0, 0, 0
	};

	// opencv hsobel
	float cvkdata[3][3] = {
		{1, 2, 1},
		{0, 0, 0},
		{-1, -2, -1}

	};

	//xfopencv hsobel
	short int kdata[] = {
		1, 2, 1,
		0, 0, 0,
		-1, -2, -1
	};
*/
	cv::Mat filter(3, 3, CV_32F, cvkdata);

	cv::Point anchor = cv::Point( -1, -1 );
	cv::filter2D(temp_img, ocv_ref, CV_8U , filter, anchor, 0, cv::BORDER_CONSTANT);

	imwrite("ocv_ref.jpg", ocv_ref);  // reference image

	printf("Input Conv: height:%u, width:%u, channels:%u, depth: %i, size: %u, pixels: %zu, bytes: %zu\n",
			in_conv_img.rows, in_conv_img.cols, in_conv_img.channels(),
			in_conv_img.depth(), in_conv_img.elemSize(), in_conv_img.total(),
			in_conv_img.total() * in_conv_img.elemSize());

	out_img.create(in_conv_img.rows, in_conv_img.cols, CV_8UC(in_conv_img.channels()));

	printf("Out Image: height:%u, width:%u, channels:%u, depth: %i, size: %u, pixels: %zu, bytes: %zu\n",
			out_img.rows, out_img.cols, out_img.channels(),
			out_img.depth(), out_img.elemSize(), out_img.total(),
			out_img.total() * out_img.elemSize());

	////////////////////////// CL START /////////////////////////////////////

	int height = in_img.rows;
	int width = in_img.cols;
	//int chan = 2;
	int chan = in_img.channels();
	cl_int err;

	// Find the xilinx device
	printf("create xilinx device\n");
	std::vector<cl::Device> devices = xcl::get_xil_devices();
	cl::Device device = devices[0];
	cl::Context context(device);

	//create the command queue
	cl::CommandQueue q(context, device, CL_QUEUE_PROFILING_ENABLE, &err);
	printf("create command queue %i\n", err);

	//Create Program and Kernel
	printf("Create program and kernel\n");
	std::string device_name = device.getInfo<CL_DEVICE_NAME>();
	auto binaryFile = xcl::read_binary_file("filter2d_accel.xclbin");
    cl::Program::Binaries bins{{binaryFile.data(), binaryFile.size()}};
	devices.resize(1);
	cl::Program program(context, devices, bins);
	cl::Kernel krnl(program,"filter2d_pl_accel");

	//Allocate Buffer in Global Memory
	printf("Allocate buffer in global memory\n");
	cl::Buffer imageToDevice(context, CL_MEM_READ_ONLY,(height*width*chan), NULL, &err);
	printf("create image to device buffer status:  %i\n", err);

	cl::Buffer imageFromDevice(context, CL_MEM_WRITE_ONLY,(height*width*chan), NULL, &err);
	printf("create image from device status:       %i\n", err);

	cl::Buffer kernelFilterToDevice(context, CL_MEM_READ_ONLY, sizeof(short int) * 9, NULL, &err);
	printf("create kernel filter to device status: %i\n", err);
	//Copying input data to Device buffer from host memory
	//
	printf("1. Copying input data to device buffer\n");
	printf("%i\n",height*width*chan);
	q.enqueueWriteBuffer(imageToDevice, CL_TRUE, 0, (height*width*chan), (unsigned short *)in_conv_img.data);

	printf("2. Copying kernel data to device buffer\n");
	//q.enqueueWriteBuffer(kernelFilterToDevice, CL_TRUE, 0, (FILTER_SIZE*FILTER_SIZE), (short int *) kdata);
	q.enqueueWriteBuffer(kernelFilterToDevice, CL_TRUE, 0, sizeof(short int) * 9, (short int *)kdata);

	printf("set kernel arguments\n");
	int fourcc = 0x56595559; //YUYV
	unsigned char shift = 0;

	// Set the kernel arguments
	krnl.setArg(0, imageToDevice);
	krnl.setArg(1, imageFromDevice);
	krnl.setArg(2, kernelFilterToDevice);
	krnl.setArg(3, height);
	krnl.setArg(4, width);
	krnl.setArg(5, fourcc); //fourcc in
	krnl.setArg(6, fourcc); //fourcc out

	// Profiling Objects
	cl_ulong start= 0;
	cl_ulong end = 0;
	double diff_prof = 0.0f;
	cl::Event event_sp;

	// Launch the kernel
	printf("launch the kernel\n");
	q.enqueueTask(krnl, NULL, &event_sp);
	clWaitForEvents(1, (const cl_event*) &event_sp);

	// Profiling
	printf("profiling\n");
	event_sp.getProfilingInfo(CL_PROFILING_COMMAND_START,&start);
	event_sp.getProfilingInfo(CL_PROFILING_COMMAND_END,&end);
	diff_prof = end-start;
	std::cout<<(diff_prof/1000000)<<"ms"<<std::endl;

	//Copying Device result data to Host memory
	//
	printf("Copying results\n");
	q.enqueueReadBuffer(imageFromDevice, CL_TRUE, 0, (height*width*chan), (unsigned short *)out_img.data);

	q.finish();

	write_buf_to_file(out_img.data,len,"hw_out.yuv");
	cv::cvtColor(out_img,temp_img, cv::COLOR_YUV2BGR_YUYV);
	imwrite("hw_out.jpg", temp_img);  // reference image


	printf("done\n");

	free(img);
	in_gray.~Mat();
	in_conv_img.~Mat();
	in_img.~Mat();
	out_img.~Mat();
	ocv_ref.~Mat();

	return 0;
}
