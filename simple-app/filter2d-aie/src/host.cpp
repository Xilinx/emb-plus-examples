/*
 * Copyright (C) 2019-2022 Xilinx, Inc
 * Copyright (C) 2022-2025 Advanced Micro Devices, Inc.
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

#include <atomic>
#include <chrono>
#include <csignal>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <experimental/xrt_kernel.h>
#include <experimental/xrt_graph.h>
#include <common/xf_aie_sw_utils.hpp>
#include <common/xfcvDataMovers.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include "config.h"

// Signal handler for graceful Ctrl+C shutdown
static std::atomic<bool> g_running{true};
static void signalHandler(int) { g_running = false; }

// Filter coefficients
float kData[][9] = {
    {-1, -1, -1, 0, 0, 0, 1, 1, 1},   // Horizontal-Gradient
    {-2, -1, 0, -1, 1, 1, 0, 1, 2},   // Emboss
    {0, 1, 0, 1, -4, 1, 0, 1, 0},     // Edge
    {1, 1, 1, 1, -7, 1, 1, 1, 1},     // Blur
    {0, 0, 0, 0, 1, 0, 0, 0, 0},      // Identity
    {1, 2, 1, 0, 0, 0, -1, -2, -1}};  // Horizontal-Sobel

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

enum Filter getCoeffString(std::string argv) {
    for (int i = 0; i < (int)MAX_FILTER_NUM; i++) {
        if (strcasecmp(filterArgs[i], argv.c_str()) == 0)
            return ((enum Filter)i);
    }
    std::cerr << "Invalid Filter Type Usage: see below options \n";
    printFilterOptions();
    exit(EXIT_FAILURE);
}

/* Helper Function */
void printHelp(void) {
    std::cout
        << "=====================================================" << std::endl
        << "Filter2d AIE Acceleration Example Application Usage " << std::endl
        << "=====================================================" << std::endl
        << "<Executable Name> <Filter> -i [input_image_path] -u [user_xclbin]" << std::endl
        << "<Executable Name> <Filter> -v [input_video_path] [-d] -u [user_xclbin]" << std::endl
        << "<Executable Name> <Filter> -c [camera_device_index] [-d] -u [user_xclbin]" << std::endl
        << std::endl
        << "  -d  Display output on screen instead of saving to file" << std::endl
        << std::endl
        << "Example with default image and xclbin:\tfilter2D_accel_aie.elf Edge"
        << std::endl
        << "Example with custom image:\t\tfilter2D_accel_aie.elf Edge -i "
           "<path/testimg.jpg>" << std::endl
        << "Example with video file:\t\tfilter2D_accel_aie.elf Edge -v "
           "<path/video.mp4>" << std::endl
        << "Example with video to display:\t\tfilter2D_accel_aie.elf Edge -v "
           "<path/video.mp4> -d" << std::endl
        << "Example with USB camera:\t\tfilter2D_accel_aie.elf Edge -c 0"
        << std::endl
        << "Example camera to display:\t\tfilter2D_accel_aie.elf Edge -c 0 -d"
        << std::endl
        << std::endl;
    printFilterOptions();
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
}

/* Color Conversion RBG to YUY2 */
void cvtColor_RGB2YUY2(cv::Mat &src, cv::Mat &dst) {
    cv::Mat temp;
    cv::cvtColor(src, temp, cv::COLOR_BGR2YUV);

    if (dst.rows != src.rows || dst.cols != src.cols || dst.type() != CV_8UC2)
        dst.create(src.rows, src.cols, CV_8UC2);

    for (int i = 0; i < src.rows; i++) {
        const cv::Vec3b *inRow = temp.ptr<cv::Vec3b>(i);
        uint8_t *outRow = dst.ptr<uint8_t>(i);
        for (int j = 0; j < src.cols; j++) {
            outRow[j * 2]     = inRow[j][0];
            outRow[j * 2 + 1] = (j % 2) ? inRow[j][2] : inRow[j][1];
        }
    }
}

/* Compare image data between the AIE computation and SW reference model */
void compareResult(cv::Mat hwOut, uint8_t *cvRef) {
    std::vector<uint8_t> dstData_vec;
    dstData_vec.assign(hwOut.data, (hwOut.data + hwOut.total() * hwOut.elemSize()));
    int acceptableError = 1;
    int errCount = 0;
    uint8_t *dataOut = (uint8_t *)dstData_vec.data();
    for (size_t i = 0; i < hwOut.total() * hwOut.elemSize(); i++) {
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

/* ==================== GStreamer Helper Functions ==================== */

/* Pull one YUY2 frame from GStreamer appsink into an XRT buffer.
   Returns false on EOS, error, or shutdown (g_running == false). */
static bool gst_pull_frame(GstAppSink *sink, void *xrt_mapped, size_t size) {
    GstSample *sample = nullptr;
    while (!sample && g_running) {
        sample = gst_app_sink_try_pull_sample(sink, 500 * GST_MSECOND);
        if (!sample && gst_app_sink_is_eos(sink)) return false;
    }
    if (!sample) return false;

    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return false;
    }
    size_t copy_sz = std::min(size, (size_t)map.size);
    memcpy(xrt_mapped, map.data, copy_sz);
    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);
    return true;
}

/* Push one YUY2 frame from an XRT buffer into GStreamer appsrc.
   Returns false on error. */
static bool gst_push_frame(GstAppSrc *src, const void *xrt_mapped, size_t size,
                           GstClockTime pts, GstClockTime duration) {
    GstBuffer *buf = gst_buffer_new_allocate(NULL, size, NULL);
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buf);
        return false;
    }
    memcpy(map.data, xrt_mapped, size);
    gst_buffer_unmap(buf, &map);

    GST_BUFFER_PTS(buf) = pts;
    GST_BUFFER_DURATION(buf) = duration;

    GstFlowReturn ret = gst_app_src_push_buffer(src, buf); // takes ownership
    return (ret == GST_FLOW_OK);
}

/* Build a GStreamer pipeline from a launch string, extract appsink/appsrc,
   and set it to PLAYING. Returns the pipeline (caller owns) or NULL on failure. */
static GstElement *gst_build_pipeline(const char *launch_str,
                                      const char *element_name,
                                      GstElement **out_element) {
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(launch_str, &err);
    if (!pipeline) {
        std::cerr << "GStreamer pipeline error: " << (err ? err->message : "unknown") << std::endl;
        if (err) g_error_free(err);
        return NULL;
    }
    if (err) {
        std::cerr << "GStreamer pipeline warning: " << err->message << std::endl;
        g_error_free(err);
    }
    if (element_name && out_element) {
        *out_element = gst_bin_get_by_name(GST_BIN(pipeline), element_name);
    }
    GstStateChangeReturn sret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Failed to start GStreamer pipeline" << std::endl;
        gst_object_unref(pipeline);
        return NULL;
    }
    return pipeline;
}

/* Cleanly shut down a GStreamer pipeline */
static void gst_teardown_pipeline(GstElement *pipeline) {
    if (!pipeline) return;
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

/* Query FPS from a decode pipeline's appsink pad caps.
   Returns 0 if unavailable — caller should default to 30. */
static double gst_query_fps(GstAppSink *sink) {
    GstPad *pad = gst_element_get_static_pad(GST_ELEMENT(sink), "sink");
    if (!pad) return 0;
    GstCaps *caps = gst_pad_get_current_caps(pad);
    gst_object_unref(pad);
    if (!caps) return 0;
    GstStructure *s = gst_caps_get_structure(caps, 0);
    gint num = 0, den = 1;
    gst_structure_get_fraction(s, "framerate", &num, &den);
    gst_caps_unref(caps);
    return (den > 0) ? (double)num / den : 0;
}


/* Run the AIE filter2D processing pipeline.
   Reads decoded frames from appsink, processes through AIE, and pushes results
   to an encode/display pipeline. Owns AIE init, buffer setup, processing loop,
   drain, EOS, and teardown of the encode pipeline. The caller owns the decode
   pipeline and dec_sink_elem. Returns 0 on success, -1 on failure. */
static int run_aie_pipeline(const std::string &userXclbin, enum Filter Ftype,
                            GstAppSink *appsink, GstSample *first_sample,
                            GstElement *dec_pipeline,  GstElement *dec_sink_elem,
                            double fps, bool displayMode) {
    /* Initialize AIE */
    std::cout << "Loading xclbin " << std::endl;
    xF::deviceInit(userXclbin.c_str());

    // Filter Coefficients
    std::array<int16_t, 16> coeffData;
    coeffData = float2fixed_coeff<10, 16>(kData[(int)Ftype]);
    xrt::bo param_buffer = xrt::bo(xF::gpDhdl, 16 * sizeof(short int), 0, 0);
    memcpy(param_buffer.map<short int*>(), &coeffData[0], 16 * sizeof(short int));
    param_buffer.sync(XCL_BO_SYNC_BO_TO_DEVICE);

    /* Prepare double-buffered XRT buffers (ping-pong) */
    size_t frameBufSize = (size_t)RESIZE_WIDTH * RESIZE_HEIGHT * 2;
    xrt::bo src_hndl[2] = {
        xrt::bo(xF::gpDhdl, frameBufSize, 0, 0),
        xrt::bo(xF::gpDhdl, frameBufSize, 0, 0)
    };
    void *srcData[2] = { src_hndl[0].map(), src_hndl[1].map() };
    xrt::bo dst_hndl[2] = {
        xrt::bo(xF::gpDhdl, frameBufSize, 0, 0),
        xrt::bo(xF::gpDhdl, frameBufSize, 0, 0)
    };
    void *dstData[2] = { dst_hndl[0].map(), dst_hndl[1].map() };

    xF::xfcvDataMovers<xF::TILER, int16_t, TILE_HEIGHT, TILE_WIDTH, VECTORIZATION_FACTOR>
        tiler(1, 1, false, 4);
    xF::xfcvDataMovers<xF::STITCHER, int16_t, TILE_HEIGHT, TILE_WIDTH, VECTORIZATION_FACTOR>
        stitcher;

    xrt::kernel krnl_mm2s = xrt::kernel(xF::gpDhdl, xF::xclbin_uuid, "mm2s");
    xrt::run run_krnl_mm2s = xrt::run(krnl_mm2s);
    run_krnl_mm2s.set_arg(0, param_buffer);
    run_krnl_mm2s.set_arg(1, nullptr);
    run_krnl_mm2s.set_arg(2, 16);

    cv::Size frameSize(RESIZE_WIDTH, RESIZE_HEIGHT);
    tiler.compute_metadata(frameSize);

    /* --- Build encode/display pipeline --- */
    int fps_int = (int)(fps + 0.5);
    if (fps_int <= 0) fps_int = 30;
    std::string encodePipeline;
    if (displayMode) {
        encodePipeline =
            "appsrc name=src is-live=true format=time"
            " caps=video/x-raw,format=YUY2,width=" + std::to_string(RESIZE_WIDTH) +
            ",height=" + std::to_string(RESIZE_HEIGHT) +
            ",framerate=" + std::to_string(fps_int) + "/1"
            " ! videoconvert ! vaapisink";
    } else {
        encodePipeline =
            "appsrc name=src is-live=true format=time"
            " caps=video/x-raw,format=YUY2,width=" + std::to_string(RESIZE_WIDTH) +
            ",height=" + std::to_string(RESIZE_HEIGHT) +
            ",framerate=" + std::to_string(fps_int) + "/1"
            " ! videoconvert ! video/x-raw,format=NV12"
            " ! vaapih264enc rate-control=cbr bitrate=8000"
            " ! h264parse ! mp4mux"
            " ! filesink location=hw_out.mp4";
    }
    std::cout << "Encode pipeline: " << encodePipeline << std::endl;

    GstElement *enc_src_elem = nullptr;
    GstElement *enc_pipeline = gst_build_pipeline(
        encodePipeline.c_str(), "src", &enc_src_elem);
    if (!enc_pipeline) {
        std::cerr << "Failed to create encode pipeline" << std::endl;
        gst_object_unref(dec_sink_elem);
        gst_teardown_pipeline(dec_pipeline);
        return -1;
    }
    GstAppSrc *appsrc = GST_APP_SRC(enc_src_elem);

    GstClockTime frame_duration = (GstClockTime)(GST_SECOND / fps);
    int processedFrames = 0;
    auto totalStart = std::chrono::steady_clock::now();
    auto batchStart = std::chrono::steady_clock::now();

    /* --- Pipelined double-buffer loop --- */
    int cur = 0;
    bool aie_active = false;

    /* Load first frame (already pulled by caller) */
    {
        GstBuffer *buf = gst_sample_get_buffer(first_sample);
        GstMapInfo map;
        gst_buffer_map(buf, &map, GST_MAP_READ);
        memcpy(srcData[cur], map.data, std::min(frameBufSize, (size_t)map.size));
        gst_buffer_unmap(buf, &map);
        gst_sample_unref(first_sample);
    }
    src_hndl[cur].sync(XCL_BO_SYNC_BO_TO_DEVICE, frameBufSize, 0);

    /* Launch AIE on first frame */
    auto tiles_sz = tiler.host2aie_nb(&src_hndl[cur], frameSize);
    run_krnl_mm2s.set_arg(3, tiles_sz[0] * tiles_sz[1]);
    run_krnl_mm2s.start();
    stitcher.aie2host_nb(&dst_hndl[cur], frameSize, tiles_sz);
    aie_active = true;
    int aie_buf = cur;
    cur ^= 1;

    while (g_running) {
        /* Decode next frame while AIE processes */
        bool got_frame = gst_pull_frame(appsink, srcData[cur], frameBufSize);
        if (!got_frame) break;
        src_hndl[cur].sync(XCL_BO_SYNC_BO_TO_DEVICE, frameBufSize, 0);

        /* Wait for AIE on previous buffer */
        if (aie_active) {
            run_krnl_mm2s.wait();
            tiler.wait();
            stitcher.wait();
            dst_hndl[aie_buf].sync(XCL_BO_SYNC_BO_FROM_DEVICE, frameBufSize, 0);

            GstClockTime pts = (GstClockTime)processedFrames * frame_duration;
            gst_push_frame(appsrc, dstData[aie_buf], frameBufSize, pts, frame_duration);
            processedFrames++;

            if (processedFrames % 30 == 0) {
                auto batchEnd = std::chrono::steady_clock::now();
                double batchSec = std::chrono::duration<double>(batchEnd - batchStart).count();
                double batchFps = 30.0 / batchSec;
                std::cout << "Processed " << processedFrames
                          << " frames (" << batchFps << " FPS)" << std::endl;
                batchStart = batchEnd;
            }
        }

        /* Launch AIE on newly decoded frame */
        tiles_sz = tiler.host2aie_nb(&src_hndl[cur], frameSize);
        run_krnl_mm2s.set_arg(3, tiles_sz[0] * tiles_sz[1]);
        run_krnl_mm2s.start();
        stitcher.aie2host_nb(&dst_hndl[cur], frameSize, tiles_sz);
        aie_active = true;
        aie_buf = cur;
        cur ^= 1;
    }

    /* Drain: if AIE is still active, collect last result */
    if (aie_active) {
        run_krnl_mm2s.wait();
        tiler.wait();
        stitcher.wait();
        dst_hndl[aie_buf].sync(XCL_BO_SYNC_BO_FROM_DEVICE, frameBufSize, 0);
        GstClockTime pts = (GstClockTime)processedFrames * frame_duration;
        gst_push_frame(appsrc, dstData[aie_buf], frameBufSize, pts, frame_duration);
        processedFrames++;
    }

    /* Signal EOS and wait for pipeline to finish */
    gst_app_src_end_of_stream(appsrc);
    GstBus *bus = gst_element_get_bus(enc_pipeline);
    gst_bus_timed_pop_filtered(bus,
        displayMode ? 1 * GST_SECOND : GST_CLOCK_TIME_NONE,
        (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    gst_object_unref(bus);

    gst_object_unref(dec_sink_elem);
    gst_object_unref(enc_src_elem);
    gst_teardown_pipeline(dec_pipeline);
    gst_teardown_pipeline(enc_pipeline);

    auto totalEnd = std::chrono::steady_clock::now();
    double totalSec = std::chrono::duration<double>(totalEnd - totalStart).count();
    double avgFps = (totalSec > 0) ? processedFrames / totalSec : 0;
    std::cout << "Processing complete. " << processedFrames
              << " frames in " << totalSec << " seconds." << std::endl;
    std::cout << "Average FPS: " << avgFps << std::endl;
    if (!displayMode)
        std::cout << "Output saved to hw_out.mp4" << std::endl;

    return 0;
}
int main(int argc, char **argv) {

    std::string arg, inputImage, inputVideo, userXclbin;
    enum Filter Ftype;
    inputImage = "/opt/xilinx/testimg/HD.jpg";
    userXclbin = "/opt/xilinx/firmware/emb_plus/ve2302_pcie_qdma/base/test/"
                 "filter2d_aie.xclbin";
    bool videoMode = false;
    bool cameraMode = false;
    bool displayMode = false;
    int cameraDevice = 0;

    if (argc < 2 || argc > 8) {
        std::cerr << "Invalid number for arguments passed, calling help menu."
                  << std::endl;
        printHelp();
        return -1;
    }

    arg = argv[1];
    if (arg == "-h" || arg == "--h" || arg == "help" || arg == "-help" ||
        arg == "--help") {
        printHelp();
        return 0;
    }

    for (int i = 2; i < argc; ) {
        if (std::string(argv[i]) == "-d") {
            displayMode = true;
            i += 1;
        } else if (std::string(argv[i]) == "-i" && i + 1 < argc) {
            inputImage = argv[i + 1];
            i += 2;
        } else if (std::string(argv[i]) == "-v" && i + 1 < argc) {
            inputVideo = argv[i + 1];
            videoMode = true;
            i += 2;
        } else if (std::string(argv[i]) == "-c" && i + 1 < argc) {
            cameraDevice = std::atoi(argv[i + 1]);
            cameraMode = true;
            i += 2;
        } else if (std::string(argv[i]) == "-u" && i + 1 < argc) {
            userXclbin = argv[i + 1];
            i += 2;
        } else {
            i += 1;
        }
    }

    Ftype = getCoeffString(arg);

    if (videoMode) {
        /* ==================== Video Mode ==================== */
        if (displayMode) std::signal(SIGINT, signalHandler);
        gst_init(&argc, &argv);

        /* --- Build decode pipeline --- */
        std::string decodePipeline =
            "filesrc location=" + inputVideo +
            " ! qtdemux ! h264parse ! vaapih264dec"
            " ! videoconvert ! video/x-raw,format=YUY2"
            " ! appsink name=sink emit-signals=false sync=false";
        std::cout << "Decode pipeline: " << decodePipeline << std::endl;

        GstElement *dec_sink_elem = nullptr;
        GstElement *dec_pipeline = gst_build_pipeline(
            decodePipeline.c_str(), "sink", &dec_sink_elem);
        if (!dec_pipeline) {
            std::cerr << "Failed to create decode pipeline" << std::endl;
            return -1;
        }

        GstSample *first_sample = gst_app_sink_pull_sample(GST_APP_SINK(dec_sink_elem));
        if (!first_sample) {
            std::cerr << "Failed to pull first frame from decode pipeline" << std::endl;
            gst_teardown_pipeline(dec_pipeline);
            return -1;
        }
        GstAppSink *appsink = GST_APP_SINK(dec_sink_elem);

        double fps = gst_query_fps(appsink);
        if (fps <= 0) fps = 30.0;
        std::cout << "Video: " << inputVideo << ", FPS: " << fps << std::endl;

        /* Validate frame dimensions from caps */
        GstCaps *sample_caps = gst_sample_get_caps(first_sample);
        if (sample_caps) {
            GstStructure *s = gst_caps_get_structure(sample_caps, 0);
            gint w = 0, h = 0;
            gst_structure_get_int(s, "width", &w);
            gst_structure_get_int(s, "height", &h);
            if (w != RESIZE_WIDTH || h != RESIZE_HEIGHT) {
                std::cerr << "Input video resolution " << w << "x" << h
                          << " does not match expected " << RESIZE_WIDTH << "x"
                          << RESIZE_HEIGHT << std::endl;
                gst_sample_unref(first_sample);
                gst_teardown_pipeline(dec_pipeline);
                return -1;
            }
            std::cout << "Frame dimensions: " << w << "x" << h << std::endl;
        }

        return run_aie_pipeline(userXclbin, Ftype, appsink, first_sample,
                                dec_pipeline, dec_sink_elem, fps, displayMode);

    } else if (cameraMode) {
        /* ==================== Camera Mode ==================== */
        std::signal(SIGINT, signalHandler);
        gst_init(&argc, &argv);

        /* --- Build camera decode pipeline --- */
        std::string devPath = "/dev/video" + std::to_string(cameraDevice);
        std::string decodePipeline =
            "v4l2src device=" + devPath +
            " ! image/jpeg,width=1920,height=1080"
            " ! vaapijpegdec"
            " ! videoconvert ! video/x-raw,format=YUY2"
            " ! appsink name=sink emit-signals=false sync=false";
        std::cout << "Camera decode pipeline: " << decodePipeline << std::endl;

        GstElement *dec_sink_elem = nullptr;
        GstElement *dec_pipeline = gst_build_pipeline(
            decodePipeline.c_str(), "sink", &dec_sink_elem);
        if (!dec_pipeline) {
            std::cerr << "Failed to create camera decode pipeline" << std::endl;
            return -1;
        }

        GstSample *first_sample = gst_app_sink_pull_sample(GST_APP_SINK(dec_sink_elem));
        if (!first_sample) {
            std::cerr << "Failed to pull first frame from camera" << std::endl;
            gst_teardown_pipeline(dec_pipeline);
            return -1;
        }
        GstAppSink *appsink = GST_APP_SINK(dec_sink_elem);
        double fps = gst_query_fps(appsink);
        if (fps <= 0) fps = 30.0;
        std::cout << "Camera " << cameraDevice << " opened @ " << fps << " FPS" << std::endl;

        std::cout << "Processing camera frames. Press Ctrl+C to stop..." << std::endl;

        return run_aie_pipeline(userXclbin, Ftype, appsink, first_sample,
                                dec_pipeline, dec_sink_elem, fps, displayMode);

    } else {
        /* ==================== Image Mode ==================== */

        /* Read image and Resize */
        cv::Mat srcImageR, temp1, temp2;
        temp1 = cv::imread(inputImage, 1);
        if (temp1.data == NULL) {
            std::cerr << "Failed to read Image from path " << inputImage
                      << std::endl;
            return -1;
        }
        cv::resize(temp1, temp1, cv::Size(RESIZE_WIDTH, RESIZE_HEIGHT), 0, 0,
                   cv::INTER_LINEAR);
        cvtColor_RGB2YUY2(temp1, temp2);
        temp2.convertTo(srcImageR, CV_8UC2);
        cv::cvtColor(srcImageR, temp2, cv::COLOR_YUV2BGR_YUYV);
        imwrite("hw_in.jpg", temp2);
        std::cout << "Dumping JPG input image consumed by AIE and Reference model" << std::endl;

        std::cout << "Image size" << std::endl;
        std::cout << "Rows : " << srcImageR.rows << std::endl;
        std::cout << "Cols : " << srcImageR.cols << std::endl;
        std::cout << "Channels : " << srcImageR.channels() << std::endl;
        std::cout << "Element size : " << srcImageR.elemSize() << std::endl;
        std::cout << "Total pixels : " << srcImageR.total() << std::endl;
        std::cout << "Type : " << srcImageR.type() << std::endl;
        int width = srcImageR.cols;
        int height = srcImageR.rows;

        /* Run convolution as a reference model */
        std::cout << "Starting Software implemented reference model...\n";
        std::vector<uint8_t> srcData_vec;
        uint8_t *dataRefOut = (uint8_t *)std::malloc(srcImageR.total() * srcImageR.elemSize());
        srcData_vec.assign(height * width * 2, 0);
        memcpy(srcData_vec.data(), srcImageR.data, srcImageR.total() * srcImageR.elemSize());
        run_ref((uint8_t *)srcData_vec.data(), dataRefOut, kData[(int)Ftype], srcImageR.rows, srcImageR.cols);
        cv::Mat ref(srcImageR.rows, srcImageR.cols, srcImageR.type(), dataRefOut);
        cv::cvtColor(ref, temp1, cv::COLOR_YUV2BGR_YUYV);
        imwrite("sw_ref.jpg", temp1);
        std::cout << "Dumping JPG output image from Reference model (software implementation)" << std::endl;

        /* Run convolution on AIE */
        std::cout << "Loading xclbin " << std::endl;
        const char *xclBinName = userXclbin.c_str();
        xF::deviceInit(xclBinName);

        // Filter Coefficients
        std::array<int16_t, 16> coeffData;
        coeffData = float2fixed_coeff<10, 16>(kData[(int)Ftype]);
        xrt::bo param_buffer  = xrt::bo(xF::gpDhdl, 16 * sizeof(short int), 0, 0);
        memcpy(param_buffer.map<short int*>(), &coeffData[0],  16 * sizeof(short int));
        param_buffer.sync(XCL_BO_SYNC_BO_TO_DEVICE);

        void *srcData = nullptr;
        std::cout << "Creating  input buffer..." << std::endl;
        xrt::bo src_hndl = xrt::bo(xF::gpDhdl, (srcImageR.total() * srcImageR.elemSize()), 0, 0);
        srcData = src_hndl.map();
        memcpy(srcData, srcImageR.data, (srcImageR.total() * srcImageR.elemSize()));
        src_hndl.sync(XCL_BO_SYNC_BO_TO_DEVICE, srcImageR.total() * srcImageR.elemSize(), 0);
        std::cout << "Creating  output buffer..." << std::endl;
        void *dstData = nullptr;
        xrt::bo dst_hndl = xrt::bo(xF::gpDhdl, (srcImageR.total() * srcImageR.elemSize()), 0, 0);
        dstData = dst_hndl.map();
        cv::Mat dst(height, width, srcImageR.type(), dstData);
        std::cout << "Initializing Tiler & Stitcher.\n";
        xF::xfcvDataMovers<xF::TILER, int16_t, TILE_HEIGHT, TILE_WIDTH, VECTORIZATION_FACTOR>
            tiler(1, 1, false, 4); // tiler with burst size of 4
        xF::xfcvDataMovers<xF::STITCHER, int16_t, TILE_HEIGHT, TILE_WIDTH, VECTORIZATION_FACTOR>
            stitcher;
        std::cout << "Initializing mm2s kernel.\n";
        xrt::kernel krnl_mm2s = xrt::kernel(xF::gpDhdl, xF::xclbin_uuid, "mm2s");
        xrt::run run_krnl_mm2s = xrt::run(krnl_mm2s);
        run_krnl_mm2s.set_arg(0, param_buffer);
        run_krnl_mm2s.set_arg(1, nullptr);
        run_krnl_mm2s.set_arg(2, 16);

        START_TIMER
        tiler.compute_metadata(srcImageR.size());
        STOP_TIMER("Meta data compute time")

        std::chrono::microseconds tt(0);
        START_TIMER
        auto tiles_sz = tiler.host2aie_nb(&src_hndl, srcImageR.size());
        run_krnl_mm2s.set_arg(3, tiles_sz[0]*tiles_sz[1]);
        run_krnl_mm2s.start();
        stitcher.aie2host_nb(&dst_hndl, dst.size(), tiles_sz);
        run_krnl_mm2s.wait();
        tiler.wait();
        stitcher.wait();
        dst_hndl.sync(XCL_BO_SYNC_BO_FROM_DEVICE, srcImageR.total() * srcImageR.elemSize(), 0);

        STOP_TIMER("yuy2 filter2D function")
        std::cout << "Data transfer complete (Stitcher)\n";
        tt += tdiff;

        cv::cvtColor(dst, temp2, cv::COLOR_YUV2BGR_YUYV);
        imwrite("hw_out.jpg", temp2);
        std::cout << "Dumping JPG output image from AIE implementation" << std::endl;
        compareResult(dst, dataRefOut);

        std::free(dataRefOut);
    }

    return 0;
}
