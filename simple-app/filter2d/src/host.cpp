/*
 * Copyright (C) 2019-2022 Xilinx, Inc
 * Copyright (C) 2022-2026 Advanced Micro Devices, Inc.
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
//#define DEBUG_MODE 1 // uncomment to enable debug information

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <queue>
#include <thread>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <cmath>
#include <experimental/xrt_kernel.h>
#include <experimental/xrt_graph.h>
#include <common/xf_aie_sw_utils.hpp>
#include <common/xfcvDataMovers.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include "config.h"
#include "xcl2.hpp"
#include <CL/cl.h>

/* Signal handler for graceful Ctrl+C shutdown */
static std::atomic<bool> g_running{true};
static void signalHandler(int) { g_running = false; }

/* Discards anything written to it; used to mute library stdout chatter. */
class NullBuffer : public std::streambuf {
  public:
    int overflow(int c) override { return c; }
};

/* RAII guard that redirects std::cout to a discarding buffer for its lifetime
   and restores the original buffer on scope exit. Used to hide the per-frame
   "Setting kernel args (Tiler/Stitcher) ..." messages the Vitis Vision data
   movers print from inside host2aie_nb()/aie2host_nb(). */
class ScopedCoutSilencer {
    NullBuffer null_;
    std::streambuf *saved_;
  public:
    ScopedCoutSilencer() : saved_(std::cout.rdbuf(&null_)) {}
    ~ScopedCoutSilencer() { std::cout.rdbuf(saved_); }
    ScopedCoutSilencer(const ScopedCoutSilencer &) = delete;
    ScopedCoutSilencer &operator=(const ScopedCoutSilencer &) = delete;
};

/* Filter coefficients */
float kData[][9] = {
    {-1, -1, -1, 0, 0, 0, 1, 1, 1},     // Horizontal-Gradient
    {-2, -1, 0, -1, 1, 1, 0, 1, 2},     // Emboss
    {0, 1, 0, 1, -4, 1, 0, 1, 0},       // Edge
    {0.0625, 0.125, 0.0625,             // Blur (3x3 Gaussian, sums to 1)
     0.125,  0.25,  0.125,
     0.0625, 0.125, 0.0625},
    {0, 0, 0, 0, 1, 0, 0, 0, 0},        // Identity
    {1, 2, 1, 0, 0, 0, -1, -2, -1}};    // Horizontal-Sobel

const char *filterArgs[] = {
    "Horizontal-Gradient", "Emboss", "Edge", "Blur", "Identity",
    "Horizontal-Sobel"};

enum Filter { HGRAD, EMBOSS, EDGE, BLUR, IDENTITY, HSOBEL, MAX_FILTER_NUM };

/* Chroma handling. Values must match the PL kernel's chroma_mode and the AIE
   kernel's POS_CHROMA_MODE coefficient slot. */
#define CHROMA_PASSTHROUGH 0
#define CHROMA_NEUTRAL 1
#define CHROMA_NEUTRAL_VALUE 128
/* Slot in the 16-entry AIE coefficient buffer that carries the mode. The 3x3
   kernel occupies 0..11, so this needs no change to the graph interface. */
#define POS_CHROMA_MODE 12

/* A zero-sum kernel drives luma to an edge magnitude, and the scene's original
   chroma against that produces a misleading hue -- a yellow subject renders
   red. Neutralise chroma for those and leave it alone for the rest, where flat
   regions keep their luma and the colour is still meaningful. */
int defaultChromaMode(enum Filter Ftype) {
    float sum = 0;
    for (int i = 0; i < 9; i++) sum += kData[(int)Ftype][i];
    return (0.0f == sum) ? CHROMA_NEUTRAL : CHROMA_PASSTHROUGH;
}

/* Set once in main from the preset default or the --chroma override, then read
   by the AIE, PL and reference paths. */
static int g_chromaMode = CHROMA_PASSTHROUGH;

enum Mode { MODE_AIE, MODE_PL };

/* PL filter coefficients (3x3 matrix form needed for PL kernel args) */
#define FILTER_HEIGHT 3
#define FILTER_WIDTH 3
#define FOURCC 0x56595559 /* YUYV Format */

/* PL applies the kernel as integers and right-shifts the accumulator by
   kShift, so fractional coefficients have to be scaled up here first. The AIE
   kernel and run_ref use the float coefficients directly and need no shift. */
const int kShift[] = {0, 0, 0, 4, 0, 0};

void matrixDeconstructor(float matrix[9], short int Darray[], int shift) {
    for (int i = 0; i < FILTER_HEIGHT * FILTER_WIDTH; i++)
        Darray[i] = (short int)lrintf(matrix[i] * (1 << shift));
}

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
        << "Filter2d Acceleration Example Application Usage " << std::endl
        << "=====================================================" << std::endl
        << "<Executable Name> <Filter> [-m aie|pl] -i [input_image_path] -u [user_xclbin]" << std::endl
        << "<Executable Name> <Filter> [-m aie|pl] -v [input_video_path] [-d] -u [user_xclbin]" << std::endl
        << "<Executable Name> <Filter> [-m aie|pl] -c [camera_device_index] [-d] -u [user_xclbin]" << std::endl
        << std::endl
        << "  -m  Set acceleration mode: aie (default) or pl" << std::endl
        << "  -t  Duration in seconds for camera capture (default: 30)" << std::endl
        << "  -d  Display output on screen instead of saving to file" << std::endl
        << "  -k  Chroma handling: auto (default), keep, or grey." << std::endl
        << "      auto greys the chroma for zero-sum kernels, whose luma is an" << std::endl
        << "      edge magnitude, and keeps it for the rest." << std::endl
        << std::endl
        << "Example with default image (AIE):\tfilter2D_accel.elf Edge"
        << std::endl
        << "Example with PL mode:\t\t\tfilter2D_accel.elf Edge -m pl"
        << std::endl
        << "Example with custom image:\t\tfilter2D_accel.elf Edge -i "
           "<path/testimg.jpg>" << std::endl
        << "Example with video file:\t\tfilter2D_accel.elf Edge -v "
           "<path/video.mp4>" << std::endl
        << "Example with video to display:\t\tfilter2D_accel.elf Edge -v "
           "<path/video.mp4> -d" << std::endl
        << "Example PL video:\t\t\tfilter2D_accel.elf Edge -m pl -v "
           "<path/video.mp4>" << std::endl
        << "Example with USB camera:\t\tfilter2D_accel.elf Edge -c 0"
        << std::endl
        << "Example camera to display:\t\tfilter2D_accel.elf Edge -c 0 -d"
        << std::endl
        << std::endl;
    printFilterOptions();
}

/* SW equivalent of the Convolution algorithm implemented on AIE */
void run_ref(uint8_t *srcImageR, uint8_t *dstRefImage, float coeff[9],
             int16_t height, int16_t width, int chromaMode) {
    float window[9];

    width *= 2;

    for (int i = 0; i < height * width; i++) {
        int row = i / width;
        int col = i % width;

        if (col % 2) {
            dstRefImage[i] = (CHROMA_NEUTRAL == chromaMode) ? CHROMA_NEUTRAL_VALUE
                                                            : srcImageR[i];
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

        /* Saturate: a zero-sum kernel gives negative results, and the
         * float->uint8_t conversion wraps them. PL saturates. */
        dstRefImage[i] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, s)));
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

/* Compare image data between the AIE computation and SW reference model.
   The outer ring is excluded: the PL kernel zero-pads the border while the
   AIE kernel and run_ref replicate it, so those pixels differ by up to the
   full range and no magnitude tolerance can cover them. */
void compareResult(cv::Mat hwOut, uint8_t *cvRef) {
    const int borderPx = 3 / 2; /* 3x3 kernel */
    std::vector<uint8_t> dstData_vec;
    dstData_vec.assign(hwOut.data, (hwOut.data + hwOut.total() * hwOut.elemSize()));
    int acceptableError = 1;
    int errCount = 0;
    size_t skipped = 0;
    const size_t bytesPerPixel = hwOut.elemSize();
    const size_t bytesPerRow = hwOut.cols * bytesPerPixel;
    uint8_t *dataOut = (uint8_t *)dstData_vec.data();
    for (size_t i = 0; i < hwOut.total() * hwOut.elemSize(); i++) {
        int row = i / bytesPerRow;
        int col = (i % bytesPerRow) / bytesPerPixel;
        if (row < borderPx || row >= hwOut.rows - borderPx || col < borderPx ||
            col >= hwOut.cols - borderPx) {
            skipped++;
            continue;
        }
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
    std::cout << "Comparing interior only, outer " << borderPx
              << " px border excluded (" << skipped
              << " bytes): the PL kernel zero-pads it, the AIE kernel and the "
                 "reference replicate it" << std::endl;
    if (errCount) {
        std::cout << "Test failed, " << errCount << " Bytes unmatched"
                  << std::endl;
    } else {
        std::cout << "Test passed" << std::endl;
    }
}

/* GStreamer Helper Functions */

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

/* Thread-safe bounded queue of decoded host frames. A dedicated decode thread
   (producer) fills it while the compute loop (consumer) drains it, decoupling
   H.264 decode jitter from AIE processing. */
struct FrameQueue {
    std::mutex m;
    std::condition_variable cv_notfull;
    std::condition_variable cv_notempty;
    std::queue<std::vector<uint8_t>> q;
    size_t capacity;
    bool producer_done = false;
    bool aborted = false;

    explicit FrameQueue(size_t cap) : capacity(cap) {}

    /* Producer: blocks while full. Returns false if the consumer aborted. */
    bool push(std::vector<uint8_t> &&frame) {
        std::unique_lock<std::mutex> lk(m);
        cv_notfull.wait(lk, [&] { return q.size() < capacity || aborted; });
        if (aborted) return false;
        q.push(std::move(frame));
        cv_notempty.notify_one();
        return true;
    }

    /* Consumer: blocks while empty. Returns false once drained or aborted. */
    bool pop(std::vector<uint8_t> &out) {
        std::unique_lock<std::mutex> lk(m);
        cv_notempty.wait(lk, [&] { return !q.empty() || producer_done || aborted; });
        if (aborted || q.empty()) return false;
        out = std::move(q.front());
        q.pop();
        cv_notfull.notify_one();
        return true;
    }

    /* Producer signals no more frames will arrive. */
    void mark_producer_done() {
        std::lock_guard<std::mutex> lk(m);
        producer_done = true;
        cv_notempty.notify_all();
    }

    /* Consumer signals early stop; unblocks the producer. */
    void abort() {
        std::lock_guard<std::mutex> lk(m);
        aborted = true;
        cv_notempty.notify_all();
        cv_notfull.notify_all();
    }
};

/* Build the appsrc -> encode/display GStreamer pipeline string shared by both
   the AIE and PL processing paths. */
static std::string build_encode_pipeline(double fps, bool displayMode,
                                         bool isCamera) {
    int fps_int = (int)(fps + 0.5);
    if (fps_int <= 0) fps_int = 30;
    if (displayMode) {
        /* Camera input renders as fast as possible (sync=false); video file
           input paces to its presentation timestamps (sync=true) for
           wall-clock-accurate playback. */
        const char *sinkSync = isCamera ? "false" : "true";
        /* Camera: leaky queue drops frames to stay real-time (never blocks the
           compute loop). Video file: non-leaky queue so the sync=true sink
           backpressures the loop, pacing processing to the source framerate. */
        const char *queueLeak = isCamera ? " leaky=downstream" : "";
        return
            "appsrc name=src is-live=true format=time block=true"
            " caps=video/x-raw,format=YUY2,width=" + std::to_string(RESIZE_WIDTH) +
            ",height=" + std::to_string(RESIZE_HEIGHT) +
            ",framerate=" + std::to_string(fps_int) + "/1"
            " ! queue max-size-buffers=2" + queueLeak +
            " ! videoconvert ! vaapisink sync=" + sinkSync;
    }
    return
        "appsrc name=src is-live=true format=time block=true"
        " caps=video/x-raw,format=YUY2,width=" + std::to_string(RESIZE_WIDTH) +
        ",height=" + std::to_string(RESIZE_HEIGHT) +
        ",framerate=" + std::to_string(fps_int) + "/1"
        " ! queue max-size-buffers=2 ! videoconvert ! video/x-raw,format=NV12"
        " ! vaapih264enc rate-control=cbr bitrate=8000"
        " ! h264parse ! mp4mux"
        " ! filesink location=hw_out.mp4";
}

/* Build and start the encode/display pipeline. On success returns the pipeline
   and sets *enc_src_out / *appsrc_out; on failure returns NULL. */
static GstElement *create_encode_pipeline(double fps, bool displayMode,
                                          bool isCamera,
                                          GstElement **enc_src_out,
                                          GstAppSrc **appsrc_out) {
    std::string encodePipeline = build_encode_pipeline(fps, displayMode, isCamera);
    std::cout << "Encode pipeline: " << encodePipeline << std::endl;
    GstElement *enc_src_elem = nullptr;
    GstElement *enc_pipeline =
        gst_build_pipeline(encodePipeline.c_str(), "src", &enc_src_elem);
    if (!enc_pipeline) return nullptr;
    *enc_src_out = enc_src_elem;
    *appsrc_out = GST_APP_SRC(enc_src_elem);
    return enc_pipeline;
}

/* Copy one decoded GstSample into a host frame buffer and push it onto the
   queue, then release the sample. Used to seed the queue with the first frame
   the caller already pulled. */
static void seed_frame_queue(FrameQueue &frameQueue, GstSample *first_sample,
                             size_t frameBufSize) {
    GstBuffer *buf = gst_sample_get_buffer(first_sample);
    GstMapInfo map;
    std::vector<uint8_t> frame(frameBufSize);
    if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
        memcpy(frame.data(), map.data, std::min(frameBufSize, (size_t)map.size));
        gst_buffer_unmap(buf, &map);
    }
    gst_sample_unref(first_sample);
    frameQueue.push(std::move(frame));
}

/* Launch the producer thread that continuously decodes frames from appsink into
   the bounded queue until EOS, error, or shutdown. */
static std::thread start_decode_thread(GstAppSink *appsink,
                                       FrameQueue &frameQueue,
                                       size_t frameBufSize) {
    return std::thread([appsink, &frameQueue, frameBufSize]() {
        while (g_running) {
            std::vector<uint8_t> frame(frameBufSize);
            if (!gst_pull_frame(appsink, frame.data(), frameBufSize)) break;
            if (!frameQueue.push(std::move(frame))) break;
        }
        frameQueue.mark_producer_done();
    });
}

/* Print a rolling FPS figure every 30 processed frames. */
static void report_batch_fps(int processedFrames,
                             std::chrono::steady_clock::time_point &batchStart) {
    if (processedFrames % 30 != 0) return;
    auto batchEnd = std::chrono::steady_clock::now();
    double batchSec = std::chrono::duration<double>(batchEnd - batchStart).count();
    double batchFps = 30.0 / batchSec;
    char fpsStr[32];
    snprintf(fpsStr, sizeof(fpsStr), "%.2f", batchFps);
    std::cout << "Processed " << processedFrames
              << " frames (" << fpsStr << " FPS)" << std::endl;
    batchStart = batchEnd;
}

/* Signal EOS, wait for the encode pipeline to flush, stop the decode thread,
   tear down both pipelines, and print the overall FPS summary. */
static void finalize_stream(GstAppSrc *appsrc, GstElement *enc_pipeline,
                            GstElement *enc_src_elem, GstElement *dec_pipeline,
                            GstElement *dec_sink_elem, FrameQueue &frameQueue,
                            std::thread &decodeThread, bool displayMode,
                            int processedFrames,
                            std::chrono::steady_clock::time_point totalStart) {
    /* Signal EOS and wait for pipeline to finish */
    gst_app_src_end_of_stream(appsrc);
    GstBus *bus = gst_element_get_bus(enc_pipeline);
    gst_bus_timed_pop_filtered(bus,
        displayMode ? 1 * GST_SECOND : GST_CLOCK_TIME_NONE,
        (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    gst_object_unref(bus);

    /* Stop and join the decode thread before releasing the decode pipeline. */
    frameQueue.abort();
    if (decodeThread.joinable()) decodeThread.join();

    gst_object_unref(dec_sink_elem);
    gst_object_unref(enc_src_elem);
    gst_teardown_pipeline(dec_pipeline);
    gst_teardown_pipeline(enc_pipeline);

    auto totalEnd = std::chrono::steady_clock::now();
    double totalSec = std::chrono::duration<double>(totalEnd - totalStart).count();
    double avgFps = (totalSec > 0) ? processedFrames / totalSec : 0;
    std::cout << "Processing complete. " << processedFrames
              << " frames in " << totalSec << " seconds." << std::endl;
    char avgFpsStr[32];
    snprintf(avgFpsStr, sizeof(avgFpsStr), "%.2f", avgFps);
    std::cout << "Average FPS: " << avgFpsStr << std::endl;
    if (!displayMode)
        std::cout << "Output saved to hw_out.mp4" << std::endl;
}

/* Run the AIE filter2D processing pipeline.
   Reads decoded frames from appsink, processes through AIE, and pushes results
   to an encode/display pipeline. Owns AIE init, buffer setup, processing loop,
   drain, EOS, and teardown of the encode pipeline. The caller owns the decode
   pipeline and dec_sink_elem. Returns 0 on success, -1 on failure. */
static int run_aie_pipeline(const std::string &userXclbin, enum Filter Ftype,
                            GstAppSink *appsink, GstSample *first_sample,
                            GstElement *dec_pipeline,  GstElement *dec_sink_elem,
                            double fps, bool displayMode, int maxFrames,
                            bool isCamera) {
    /* Initialize AIE */
    std::cout << "Loading xclbin " << std::endl;
    xF::deviceInit(userXclbin.c_str());

    /* Filter Coefficients */
    std::array<int16_t, 16> coeffData;
    coeffData = float2fixed_coeff<10, 16>(kData[(int)Ftype]);
    coeffData[POS_CHROMA_MODE] = (int16_t)g_chromaMode;
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

    /* Build encode/display pipeline */
    GstElement *enc_src_elem = nullptr;
    GstAppSrc *appsrc = nullptr;
    GstElement *enc_pipeline = create_encode_pipeline(
        fps, displayMode, isCamera, &enc_src_elem, &appsrc);
    if (!enc_pipeline) {
        std::cerr << "Failed to create encode pipeline" << std::endl;
        gst_sample_unref(first_sample);
        gst_object_unref(dec_sink_elem);
        gst_teardown_pipeline(dec_pipeline);
        return -1;
    }

    GstClockTime frame_duration = (GstClockTime)(GST_SECOND / fps);
    int processedFrames = 0;
    auto totalStart = std::chrono::steady_clock::now();
    auto batchStart = std::chrono::steady_clock::now();

    /* Decode/compute overlap: a dedicated decode thread fills a bounded
       queue so H.264 decode jitter is absorbed and the AIE never stalls
       waiting on the decoder. */
    FrameQueue frameQueue(16);

    /* Seed the queue with the first frame already pulled by the caller. */
    seed_frame_queue(frameQueue, first_sample, frameBufSize);

    /* Producer: continuously decode subsequent frames into the queue. */
    std::thread decodeThread = start_decode_thread(appsink, frameQueue, frameBufSize);

    /* Pipelined double-buffer loop */
    int cur = 0;
    bool aie_active = false;
    int aie_buf = 0;
    std::vector<uint8_t> hostFrame;

    /* Load first frame from the decode queue and launch AIE on it. */
    if (frameQueue.pop(hostFrame)) {
        memcpy(srcData[cur], hostFrame.data(),
               std::min(frameBufSize, hostFrame.size()));
        src_hndl[cur].sync(XCL_BO_SYNC_BO_TO_DEVICE, frameBufSize, 0);

        {
            ScopedCoutSilencer silence; // mute per-frame data-mover chatter
            auto tiles_sz = tiler.host2aie_nb(&src_hndl[cur], frameSize);
            run_krnl_mm2s.set_arg(3, tiles_sz[0] * tiles_sz[1]);
            run_krnl_mm2s.start();
            stitcher.aie2host_nb(&dst_hndl[cur], frameSize, tiles_sz);
        }
        aie_active = true;
        aie_buf = cur;
        cur ^= 1;
    }

    while (g_running && aie_active && (maxFrames <= 0 || processedFrames < maxFrames)) {
        /* Pop the next decoded frame (produced concurrently by decodeThread) */
        if (!frameQueue.pop(hostFrame)) break;
        memcpy(srcData[cur], hostFrame.data(),
               std::min(frameBufSize, hostFrame.size()));
        src_hndl[cur].sync(XCL_BO_SYNC_BO_TO_DEVICE, frameBufSize, 0);

        /* Wait for AIE on previous buffer */
        run_krnl_mm2s.wait();
        tiler.wait();
        stitcher.wait();
        dst_hndl[aie_buf].sync(XCL_BO_SYNC_BO_FROM_DEVICE, frameBufSize, 0);

        GstClockTime pts = (GstClockTime)processedFrames * frame_duration;
        if (!gst_push_frame(appsrc, dstData[aie_buf], frameBufSize, pts, frame_duration)) {
            std::cerr << "appsrc rejected buffer at frame " << processedFrames
                      << " (downstream error or EOS); stopping." << std::endl;
            break;
        }
        processedFrames++;

        report_batch_fps(processedFrames, batchStart);

        /* Launch AIE on newly decoded frame */
        {
            ScopedCoutSilencer silence; // mute per-frame data-mover chatter
            auto tiles_sz = tiler.host2aie_nb(&src_hndl[cur], frameSize);
            run_krnl_mm2s.set_arg(3, tiles_sz[0] * tiles_sz[1]);
            run_krnl_mm2s.start();
            stitcher.aie2host_nb(&dst_hndl[cur], frameSize, tiles_sz);
        }
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
        if (!gst_push_frame(appsrc, dstData[aie_buf], frameBufSize, pts, frame_duration)) {
            std::cerr << "appsrc rejected final buffer; output may be truncated." << std::endl;
        }
        processedFrames++;
    }

    finalize_stream(appsrc, enc_pipeline, enc_src_elem, dec_pipeline,
                    dec_sink_elem, frameQueue, decodeThread, displayMode,
                    processedFrames, totalStart);

    return 0;
}
/* Run the PL filter2D processing pipeline for video/camera frames.
   Reads decoded frames from appsink, processes through the PL kernel,
   and pushes results to an encode/display pipeline. Returns 0 on success. */
static int run_pl_pipeline(const std::string &userXclbin, enum Filter Ftype,
                           GstAppSink *appsink, GstSample *first_sample,
                           GstElement *dec_pipeline, GstElement *dec_sink_elem,
                           double fps, bool displayMode, int maxFrames,
                           bool isCamera) {
    int height = RESIZE_HEIGHT;
    int width = RESIZE_WIDTH;
    int chan = 2; // YUY2
    size_t frameBufSize = (size_t)height * width * chan;
    short int Darray[9] = {0};
    cl_int err;

    /* Find the versal device */
    std::cout << "create device object" << std::endl;
    std::vector<cl::Device> devices = xcl::get_xil_devices();
    cl::Device device = devices[0];
    cl::Context context(device);

    /* create the command queue */
    cl::CommandQueue q(context, device, CL_QUEUE_PROFILING_ENABLE, &err);
    std::cout << "create command queue " << err << std::endl;

    /* Program Kernel */
    std::cout << "Programming kernel" << std::endl;
    auto binaryFile = xcl::read_binary_file(userXclbin);
    cl::Program::Binaries bins{{binaryFile.data(), binaryFile.size()}};
    devices.resize(1);
    cl::Program program(context, devices, bins);
    cl::Kernel krnl(program, "filter2d_pl_accel", &err);
    if (err) {
        std::cerr << "Failed to program kernel" << std::endl;
        gst_sample_unref(first_sample);
        gst_object_unref(dec_sink_elem);
        gst_teardown_pipeline(dec_pipeline);
        return -1;
    }

    /* Allocate Buffers in Global Memory */
    std::cout << "Allocate buffer in global memory" << std::endl;
    cl::Buffer imageToDevice(context, CL_MEM_READ_ONLY, frameBufSize, NULL, &err);
    cl::Buffer imageFromDevice(context, CL_MEM_WRITE_ONLY, frameBufSize, NULL, &err);
    cl::Buffer kernelFilterToDevice(context, CL_MEM_READ_ONLY,
                                    sizeof(short int) * 9, NULL, &err);

    /* Upload filter coefficients (once) */
    matrixDeconstructor(kData[(int)Ftype], Darray, kShift[(int)Ftype]);
    q.enqueueWriteBuffer(kernelFilterToDevice, CL_TRUE, 0,
                         sizeof(short int) * 9, (short int *)Darray);

    /* Set the kernel arguments that don't change per frame */
    krnl.setArg(0, imageToDevice);
    krnl.setArg(1, imageFromDevice);
    krnl.setArg(2, kernelFilterToDevice);
    krnl.setArg(3, height);
    krnl.setArg(4, width);
    krnl.setArg(5, FOURCC); // fourcc in
    krnl.setArg(6, FOURCC); // fourcc out
    krnl.setArg(7, (uint32_t)g_chromaMode);
    krnl.setArg(8, (uint32_t)kShift[(int)Ftype]);

    /* Host-side frame buffers */
    std::vector<uint8_t> inBuf(frameBufSize);
    std::vector<uint8_t> outBuf(frameBufSize);

    /* Build encode/display pipeline */
    GstElement *enc_src_elem = nullptr;
    GstAppSrc *appsrc = nullptr;
    GstElement *enc_pipeline = create_encode_pipeline(
        fps, displayMode, isCamera, &enc_src_elem, &appsrc);
    if (!enc_pipeline) {
        std::cerr << "Failed to create encode pipeline" << std::endl;
        gst_sample_unref(first_sample);
        gst_object_unref(dec_sink_elem);
        gst_teardown_pipeline(dec_pipeline);
        return -1;
    }

    GstClockTime frame_duration = (GstClockTime)(GST_SECOND / fps);
    int processedFrames = 0;
    auto totalStart = std::chrono::steady_clock::now();
    auto batchStart = std::chrono::steady_clock::now();

    /* Decode/compute overlap: a dedicated decode thread fills a bounded queue
       so H.264 decode jitter is absorbed and the PL kernel never stalls waiting
       on the decoder. */
    FrameQueue frameQueue(16);

    /* Seed the queue with the first frame already pulled by the caller. */
    seed_frame_queue(frameQueue, first_sample, frameBufSize);

    /* Producer: continuously decode subsequent frames into the queue. */
    std::thread decodeThread = start_decode_thread(appsink, frameQueue, frameBufSize);

    /* Process loop */
    std::vector<uint8_t> hostFrame;
    bool have_frame = frameQueue.pop(hostFrame);
    while (have_frame && g_running && (maxFrames <= 0 || processedFrames < maxFrames)) {
        memcpy(inBuf.data(), hostFrame.data(),
               std::min(frameBufSize, hostFrame.size()));

        /* Upload frame to device */
        q.enqueueWriteBuffer(imageToDevice, CL_TRUE, 0, frameBufSize,
                             (unsigned short *)inBuf.data());

        /* Launch the kernel */
        q.enqueueTask(krnl, NULL, NULL);
        q.finish();

        /* Read result back */
        q.enqueueReadBuffer(imageFromDevice, CL_TRUE, 0, frameBufSize,
                            (unsigned short *)outBuf.data());

        /* Push processed frame to encode pipeline */
        GstClockTime pts = (GstClockTime)processedFrames * frame_duration;
        if (!gst_push_frame(appsrc, outBuf.data(), frameBufSize, pts, frame_duration)) {
            std::cerr << "appsrc rejected buffer at frame " << processedFrames
                      << " (downstream error or EOS); stopping." << std::endl;
            break;
        }
        processedFrames++;

        report_batch_fps(processedFrames, batchStart);

        /* Pull next decoded frame (produced concurrently by decodeThread) */
        have_frame = frameQueue.pop(hostFrame);
    }

    q.finish();

    finalize_stream(appsrc, enc_pipeline, enc_src_elem, dec_pipeline,
                    dec_sink_elem, frameQueue, decodeThread, displayMode,
                    processedFrames, totalStart);

    return 0;
}

int main(int argc, char **argv) {

    std::string arg, inputImage, inputVideo, userXclbin;
    enum Filter Ftype;
    enum Mode mode = MODE_AIE;
    inputImage = "/opt/xilinx/testimg/HD.jpg";
    bool userXclbinSet = false;
    bool videoMode = false;
    bool cameraMode = false;
    bool imageMode = false;
    bool displayMode = false;
    int cameraDevice = 0;
    int durationSeconds = 0; // 0 means use default (30s for camera+file, unlimited otherwise)
    std::string chromaArg = "auto";

    if (argc < 2 || argc > 14) {
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
            imageMode = true;
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
            userXclbinSet = true;
            i += 2;
        } else if (std::string(argv[i]) == "-m" && i + 1 < argc) {
            std::string modeStr = argv[i + 1];
            if (modeStr == "aie" || modeStr == "AIE") {
                mode = MODE_AIE;
            } else if (modeStr == "pl" || modeStr == "PL") {
                mode = MODE_PL;
            } else {
                std::cerr << "Invalid mode '" << modeStr
                          << "'. Use 'aie' or 'pl'." << std::endl;
                return -1;
            }
            i += 2;
        } else if (std::string(argv[i]) == "-k" && i + 1 < argc) {
            chromaArg = argv[i + 1];
            if (chromaArg != "auto" && chromaArg != "keep" && chromaArg != "grey") {
                std::cerr << "Invalid chroma option '" << chromaArg
                          << "'. Use 'auto', 'keep' or 'grey'." << std::endl;
                return -1;
            }
            i += 2;
        } else if (std::string(argv[i]) == "-t" && i + 1 < argc) {
            durationSeconds = std::atoi(argv[i + 1]);
            if (durationSeconds <= 0) {
                std::cerr << "Invalid duration '" << argv[i + 1]
                          << "'. Must be a positive number of seconds." << std::endl;
                return -1;
            }
            i += 2;
        } else {
            std::cerr << "Warning: ignoring unrecognized or incomplete argument '"
                      << argv[i] << "'" << std::endl;
            i += 1;
        }
    }

    /* Only one input mode (-i, -v, -c) may be selected at a time */
    if ((static_cast<int>(imageMode) + static_cast<int>(videoMode) +
         static_cast<int>(cameraMode)) > 1) {
        std::cerr << "Error: only one input mode may be specified. "
                     "Choose exactly one of -i (image), -v (video), or "
                     "-c (camera)." << std::endl;
        printHelp();
        return -1;
    }

    Ftype = getCoeffString(arg);
    g_chromaMode = (chromaArg == "auto")   ? defaultChromaMode(Ftype)
                   : (chromaArg == "grey") ? CHROMA_NEUTRAL
                                           : CHROMA_PASSTHROUGH;
    std::cout << "Chroma: "
              << ((CHROMA_NEUTRAL == g_chromaMode) ? "neutral grey" : "passed through")
              << std::endl;

    /* Set default xclbin based on mode if user didn't specify one */
    if (!userXclbinSet) {
        if (mode == MODE_PL) {
            userXclbin = "/opt/xilinx/firmware/emb_plus/ve2302_pcie_qdma/base/test/"
                         "filter2d_pl.xclbin";
        } else {
            userXclbin = "/opt/xilinx/firmware/emb_plus/ve2302_pcie_qdma/base/test/"
                         "filter2d_aie.xclbin";
        }
    }

    std::cout << "Mode: " << (mode == MODE_PL ? "PL" : "AIE") << std::endl;
    std::cout << "Xclbin: " << userXclbin << std::endl;
    if (videoMode) {
        std::cout << "Source: Video (" << inputVideo << ")" << std::endl;
    } else if (cameraMode) {
        std::cout << "Source: Camera (/dev/video" << cameraDevice << ")"
                  << std::endl;
    } else {
        std::cout << "Source: Image (" << inputImage << ")" << std::endl;
    }
    std::cout << "Output: " << (displayMode ? "Display" : "File") << std::endl;

    /* Calculate max frames for camera+file mode */
    int maxFrames = 0; // 0 = unlimited (for video mode and display mode)

    if (videoMode) {
        /* Video Mode */
        std::signal(SIGINT, signalHandler);
        gst_init(&argc, &argv);

        /* Build decode pipeline */
        std::string decodePipeline =
            "filesrc location=" + inputVideo +
            " ! qtdemux ! h264parse ! vaapih264dec"
            " ! videoconvert ! video/x-raw,format=YUY2"
            " ! appsink name=sink emit-signals=false sync=false max-buffers=2 drop=false";
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

        if (mode == MODE_PL) {
            return run_pl_pipeline(userXclbin, Ftype, appsink, first_sample,
                                    dec_pipeline, dec_sink_elem, fps, displayMode, 0, false);
        } else {
            return run_aie_pipeline(userXclbin, Ftype, appsink, first_sample,
                                    dec_pipeline, dec_sink_elem, fps, displayMode, 0, false);
        }

    } else if (cameraMode) {
        /* Camera Mode */
        std::signal(SIGINT, signalHandler);
        gst_init(&argc, &argv);

        /* Build camera decode pipeline */
        std::string devPath = "/dev/video" + std::to_string(cameraDevice);

        /* Verify the camera actually supports the required resolution/format
           before building the capture pipeline, so we fail fast with a clear
           message instead of blocking on a negotiation that can never succeed. */
        {
            GstElement *probe = gst_element_factory_make("v4l2src", "probe");
            if (probe) {
                g_object_set(probe, "device", devPath.c_str(), NULL);
                if (gst_element_set_state(probe, GST_STATE_READY) ==
                        GST_STATE_CHANGE_FAILURE) {
                    std::cerr << "Failed to open camera device " << devPath
                              << std::endl;
                    gst_object_unref(probe);
                    return -1;
                }
                GstPad *pad = gst_element_get_static_pad(probe, "src");
                GstCaps *devCaps = pad ? gst_pad_query_caps(pad, NULL) : NULL;
                GstCaps *want = gst_caps_new_simple(
                    "video/x-raw",
                    "format", G_TYPE_STRING, "YUY2",
                    "width", G_TYPE_INT, RESIZE_WIDTH,
                    "height", G_TYPE_INT, RESIZE_HEIGHT, NULL);
                bool supported = devCaps && gst_caps_can_intersect(devCaps, want);
                if (!supported) {
                    gchar *capsStr = devCaps ? gst_caps_to_string(devCaps)
                                             : g_strdup("<unknown>");
                    std::cerr << "Camera " << devPath << " does not support "
                              << RESIZE_WIDTH << "x" << RESIZE_HEIGHT
                              << " YUY2. Supported formats:\n"
                              << capsStr << std::endl;
                    g_free(capsStr);
                }
                gst_caps_unref(want);
                if (devCaps) gst_caps_unref(devCaps);
                if (pad) gst_object_unref(pad);
                gst_element_set_state(probe, GST_STATE_NULL);
                gst_object_unref(probe);
                if (!supported) return -1;
            }
        }

        std::string decodePipeline =
            "v4l2src device=" + devPath +
            " ! video/x-raw,format=YUY2,width=1920,height=1080,framerate=30/1"
            " ! appsink name=sink emit-signals=false sync=false max-buffers=2 drop=true";
        std::cout << "Camera decode pipeline: " << decodePipeline << std::endl;

        GstElement *dec_sink_elem = nullptr;
        GstElement *dec_pipeline = gst_build_pipeline(
            decodePipeline.c_str(), "sink", &dec_sink_elem);
        if (!dec_pipeline) {
            std::cerr << "Failed to create camera decode pipeline" << std::endl;
            return -1;
        }

        /* Timed pull so an unsatisfiable negotiation (e.g. unsupported
           framerate) times out with a message instead of blocking forever. */
        GstSample *first_sample = gst_app_sink_try_pull_sample(
            GST_APP_SINK(dec_sink_elem), 5 * GST_SECOND);
        if (!first_sample) {
            std::cerr << "Failed to pull first frame from camera " << devPath
                      << " (timed out or device cannot deliver "
                      << RESIZE_WIDTH << "x" << RESIZE_HEIGHT
                      << " YUY2@30)" << std::endl;
            gst_teardown_pipeline(dec_pipeline);
            return -1;
        }
        GstAppSink *appsink = GST_APP_SINK(dec_sink_elem);
        double fps = gst_query_fps(appsink);
        if (fps <= 0) fps = 30.0;
        std::cout << "Camera " << cameraDevice << " opened @ " << fps << " FPS" << std::endl;

        /* For camera with file output, default to 30 seconds if no -t specified */
        if (!displayMode) {
            int captureDuration = (durationSeconds > 0) ? durationSeconds : 30;
            maxFrames = (int)(captureDuration * fps);
            std::cout << "Recording " << captureDuration << " seconds ("
                      << maxFrames << " frames). Press Ctrl+C to stop early..." << std::endl;
        } else {
            maxFrames = (durationSeconds > 0) ? (int)(durationSeconds * fps) : 0;
            std::cout << "Processing camera frames. Press Ctrl+C to stop..." << std::endl;
        }

        if (mode == MODE_PL) {
            return run_pl_pipeline(userXclbin, Ftype, appsink, first_sample,
                                    dec_pipeline, dec_sink_elem, fps, displayMode, maxFrames, true);
        } else {
            return run_aie_pipeline(userXclbin, Ftype, appsink, first_sample,
                                    dec_pipeline, dec_sink_elem, fps, displayMode, maxFrames, true);
        }

    } else {
        /* Image Mode */

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
        run_ref((uint8_t *)srcData_vec.data(), dataRefOut, kData[(int)Ftype], srcImageR.rows, srcImageR.cols,
                g_chromaMode);
        cv::Mat ref(srcImageR.rows, srcImageR.cols, srcImageR.type(), dataRefOut);
        cv::cvtColor(ref, temp1, cv::COLOR_YUV2BGR_YUYV);
        imwrite("sw_ref.jpg", temp1);
        std::cout << "Dumping JPG output image from Reference model (software implementation)" << std::endl;

        if (mode == MODE_PL) {
            /* Run convolution on PL */
            short int Darray[9] = {0};
            cl_int err;

            std::cout << "create device object" << std::endl;
            std::vector<cl::Device> devices = xcl::get_xil_devices();
            cl::Device device = devices[0];
            cl::Context context(device);

            cl::CommandQueue q(context, device, CL_QUEUE_PROFILING_ENABLE, &err);
            std::cout << "create command queue " << err << std::endl;

            std::cout << "Programming kernel" << std::endl;
            auto binaryFile = xcl::read_binary_file(userXclbin);
            cl::Program::Binaries bins{{binaryFile.data(), binaryFile.size()}};
            devices.resize(1);
            cl::Program program(context, devices, bins);
            cl::Kernel krnl(program, "filter2d_pl_accel", &err);
            if (err) {
                std::cerr << "Failed to program kernel" << std::endl;
                std::free(dataRefOut);
                return -1;
            }

            int chan = srcImageR.channels();
            cv::Mat outImg;
            outImg.create(height, width, CV_8UC(chan));

            std::cout << "Allocate buffer in global memory" << std::endl;
            cl::Buffer imageToDevice(context, CL_MEM_READ_ONLY,
                                     (height * width * chan), NULL, &err);
            cl::Buffer imageFromDevice(context, CL_MEM_WRITE_ONLY,
                                       (height * width * chan), NULL, &err);
            cl::Buffer kernelFilterToDevice(context, CL_MEM_READ_ONLY,
                                            sizeof(short int) * 9, NULL, &err);

            q.enqueueWriteBuffer(imageToDevice, CL_TRUE, 0,
                                 (height * width * chan),
                                 (unsigned short *)srcImageR.data);

            matrixDeconstructor(kData[(int)Ftype], Darray, kShift[(int)Ftype]);
            q.enqueueWriteBuffer(kernelFilterToDevice, CL_TRUE, 0,
                                 sizeof(short int) * 9, (short int *)Darray);

            krnl.setArg(0, imageToDevice);
            krnl.setArg(1, imageFromDevice);
            krnl.setArg(2, kernelFilterToDevice);
            krnl.setArg(3, height);
            krnl.setArg(4, width);
            krnl.setArg(5, FOURCC);
            krnl.setArg(6, FOURCC);
            krnl.setArg(7, (uint32_t)g_chromaMode);
            krnl.setArg(8, (uint32_t)kShift[(int)Ftype]);

            cl_ulong start = 0, end = 0;
            cl::Event eventSp;

            std::cout << "launch the kernel" << std::endl;
            q.enqueueTask(krnl, NULL, &eventSp);
            clWaitForEvents(1, (const cl_event *)&eventSp);

            eventSp.getProfilingInfo(CL_PROFILING_COMMAND_START, &start);
            eventSp.getProfilingInfo(CL_PROFILING_COMMAND_END, &end);
            double diffProf = end - start;
            std::cout << (diffProf / 1000000) << "ms" << std::endl;

            q.enqueueReadBuffer(imageFromDevice, CL_TRUE, 0,
                                (height * width * chan),
                                (unsigned short *)outImg.data);
            q.finish();

            cv::cvtColor(outImg, temp2, cv::COLOR_YUV2BGR_YUYV);
            imwrite("hw_out.jpg", temp2);
            std::cout << "Dumping JPG output image from PL implementation" << std::endl;
            compareResult(outImg, dataRefOut);
        } else {
            /* Run convolution on AIE */
            std::cout << "Loading xclbin " << std::endl;
            const char *xclBinName = userXclbin.c_str();
            xF::deviceInit(xclBinName);

            /* Filter Coefficients */
            std::array<int16_t, 16> coeffData;
            coeffData = float2fixed_coeff<10, 16>(kData[(int)Ftype]);
            coeffData[POS_CHROMA_MODE] = (int16_t)g_chromaMode;
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

            cv::cvtColor(dst, temp2, cv::COLOR_YUV2BGR_YUYV);
            imwrite("hw_out.jpg", temp2);
            std::cout << "Dumping JPG output image from AIE implementation" << std::endl;
            compareResult(dst, dataRefOut);
        }

        std::free(dataRefOut);
    }

    return 0;
}
