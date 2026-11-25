// YOLO26 NCNN inference example using the NCNN C API.
// The exported YOLO26 detection model returns [x1, y1, x2, y2, score, class].

#include "c_api.h"

#if defined(USE_NCNN_SIMPLEOCV)
#include "simpleocv.h"
#else
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#endif

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/time.h>

struct Detection
{
    float x0;
    float y0;
    float x1;
    float y1;
    float score;
    int label;
};

static int parse_output(const ncnn_mat_t output, int src_w, int src_h,
                        int input_w, int input_h, float scale,
                        int pad_x, int pad_y, float threshold,
                        Detection* detections, int max_detections)
{
    const int w = ncnn_mat_get_w(output);
    const int h = ncnn_mat_get_h(output);
    const int c = ncnn_mat_get_c(output);
    const int dims = ncnn_mat_get_dims(output);

    // Two supported export layouts (see ultralytics yolo26-lite yaml, head
    // DetectDW with end2end: false):
    //   A) end2end NMS-free head:  w=6, h=N, row-major [x1 y1 x2 y2 score label]
    //   B) anchor-wise head:       w=A, h=6, column-major per anchor:
    //      row 0..3 = decoded cx,cy,w,h in letterbox pixels (strides folded
    //      into the graph by pnnx), row 4/5 = per-class sigmoid scores
    //      (nc=2: 4=cat, 5=dog). Needs class-wise greedy NMS.
    if (dims != 2 || c != 1 || (w != 6 && h != 6))
    {
        std::fprintf(stderr, "unexpected YOLO26 output shape: dims=%d w=%d h=%d c=%d\n",
                     dims, w, h, c);
        return 0;
    }

    const float* p = static_cast<const float*>(ncnn_mat_get_data(output));

    std::vector<Detection> cand;
    cand.reserve(static_cast<size_t>(std::max(w, 6)) + 1);

    if (w == 6)
    {
        const int count = std::min(h, max_detections);
        for (int i = 0; i < count; i++)
        {
            const float score = p[i * 6 + 4];
            if (score < threshold)
                continue;
            Detection d;
            d.x0 = p[i * 6 + 0];
            d.y0 = p[i * 6 + 1];
            d.x1 = p[i * 6 + 2];
            d.y1 = p[i * 6 + 3];
            d.score = score;
            d.label = static_cast<int>(p[i * 6 + 5]);
            cand.push_back(d);
        }
    }
    else
    {
        // layout B: h == 6, w == number of anchors (e.g. 432+108=540 for 384x288)
        for (int j = 0; j < w; j++)
        {
            const float s0 = p[4 * w + j];
            const float s1 = p[5 * w + j];
            const float score = std::max(s0, s1);
            if (score < threshold)
                continue;
            Detection d;
            const float cx = p[0 * w + j];
            const float cy = p[1 * w + j];
            const float bw = p[2 * w + j];
            const float bh = p[3 * w + j];
            d.x0 = cx - bw * 0.5f;
            d.y0 = cy - bh * 0.5f;
            d.x1 = cx + bw * 0.5f;
            d.y1 = cy + bh * 0.5f;
            d.score = score;
            d.label = s1 > s0 ? 1 : 0;
            cand.push_back(d);
        }

        // class-wise greedy NMS (IoU 0.7, matching Ultralytics defaults)
        const int ncand = static_cast<int>(cand.size());
        std::vector<char> dead(ncand, 0);
        std::vector<Detection> kept;
        kept.reserve(std::min(cand.size(), static_cast<size_t>(max_detections)));
        for (int iter = 0;; iter++)
        {
            int best = -1;
            float bs = threshold;
            for (int i = 0; i < ncand; i++)
            {
                if (!dead[i] && cand[i].score >= bs)
                {
                    bs = cand[i].score;
                    best = i;
                }
            }
            if (best < 0 || static_cast<int>(kept.size()) >= max_detections)
                break;
            dead[best] = 1;
            kept.push_back(cand[best]);
            for (int i = 0; i < ncand; i++)
            {
                if (dead[i] || cand[i].label != cand[best].label)
                    continue;
                const float ix0 = std::max(cand[i].x0, cand[best].x0);
                const float iy0 = std::max(cand[i].y0, cand[best].y0);
                const float ix1 = std::min(cand[i].x1, cand[best].x1);
                const float iy1 = std::min(cand[i].y1, cand[best].y1);
                const float inter = std::max(0.f, ix1 - ix0) * std::max(0.f, iy1 - iy0);
                const float ua = (cand[i].x1 - cand[i].x0) * (cand[i].y1 - cand[i].y0)
                               + (cand[best].x1 - cand[best].x0) * (cand[best].y1 - cand[best].y0)
                               - inter;
                if (ua > 0.f && inter / ua > 0.7f)
                    dead[i] = 1;
            }
        }
        cand = kept;
    }

    int accepted = 0;
    for (size_t i = 0; i < cand.size() && accepted < max_detections; i++)
    {
        Detection d = cand[i];
        d.x0 = (d.x0 - pad_x) / scale;
        d.y0 = (d.y0 - pad_y) / scale;
        d.x1 = (d.x1 - pad_x) / scale;
        d.y1 = (d.y1 - pad_y) / scale;
        d.x0 = std::max(0.f, std::min(d.x0, static_cast<float>(src_w - 1)));
        d.y0 = std::max(0.f, std::min(d.y0, static_cast<float>(src_h - 1)));
        d.x1 = std::max(0.f, std::min(d.x1, static_cast<float>(src_w - 1)));
        d.y1 = std::max(0.f, std::min(d.y1, static_cast<float>(src_h - 1)));
        detections[accepted++] = d;
    }

    (void)input_w;
    (void)input_h;
    return accepted;
}

static int detect(const cv::Mat& bgr, ncnn_net_t net,
                  Detection* detections, int max_detections, float threshold)
{
    // Fixed input shape exported by Ultralytics: height=288, width=384.
    const int input_w = 384;
    const int input_h = 288;

    ncnn_option_t opt = ncnn_net_get_option(net);

    const int src_w = bgr.cols;
    const int src_h = bgr.rows;
    const float scale = std::min(input_w / static_cast<float>(src_w),
                                 input_h / static_cast<float>(src_h));
    const int resized_w = std::max(1, static_cast<int>(src_w * scale));
    const int resized_h = std::max(1, static_cast<int>(src_h * scale));
    const int pad_x = (input_w - resized_w) / 2;
    const int pad_y = (input_h - resized_h) / 2;

    ncnn_allocator_t allocator = ncnn_allocator_create_pool_allocator();
    ncnn_mat_t resized = ncnn_mat_from_pixels_resize(
        bgr.data, NCNN_MAT_PIXEL_X2Y(NCNN_MAT_PIXEL_BGR, NCNN_MAT_PIXEL_RGB),
        src_w, src_h, src_w * 3,
        resized_w, resized_h, allocator);
    ncnn_mat_t input = ncnn_mat_create();
    if (!resized || !ncnn_mat_get_data(resized) || !input)
    {
        std::fprintf(stderr, "failed to allocate input mats: resized=%p input=%p\n",
                     (void*)resized, (void*)input);
        if (input)
            ncnn_mat_destroy(input);
        if (resized)
            ncnn_mat_destroy(resized);
        ncnn_allocator_destroy(allocator);
        return -100;
    }
    ncnn_copy_make_border(resized, input, pad_y, input_h - resized_h - pad_y,
                          pad_x, input_w - resized_w - pad_x,
                          NCNN_BORDER_CONSTANT, 114.f, opt);
    if (!ncnn_mat_get_data(input))
    {
        std::fprintf(stderr, "failed to allocate padded input: %dx%d\n",
                     input_w, input_h);
        ncnn_mat_destroy(input);
        ncnn_mat_destroy(resized);
        ncnn_allocator_destroy(allocator);
        return -100;
    }
    const float norm[3] = {1.f / 255.f, 1.f / 255.f, 1.f / 255.f};
    ncnn_mat_substract_mean_normalize(input, NULL, norm);

    ncnn_extractor_t ex = ncnn_extractor_create(net);
    const int input_count = ncnn_net_get_input_count(net);
    const int output_count = ncnn_net_get_output_count(net);
    if (input_count < 1 || output_count < 1)
    {
        std::fprintf(stderr, "model has no input/output: inputs=%d outputs=%d\n",
                     input_count, output_count);
        ncnn_extractor_destroy(ex);
        ncnn_mat_destroy(input);
        ncnn_mat_destroy(resized);
        ncnn_allocator_destroy(allocator);
        return -1;
    }

    // Match Ultralytics' Python NCNN backend: use the model's blob names
    // (normally in0 and out0), rather than assuming blob index 0 is output.
    const char* input_name = ncnn_net_get_input_name(net, 0);
    const char* output_name = ncnn_net_get_output_name(net, 0);
    int ret = ncnn_extractor_input(ex, input_name, input);
    ncnn_mat_t output = NULL;
    if (ret == 0)
        ret = ncnn_extractor_extract(ex, output_name, &output);

    if (ret != 0)
    {
        std::fprintf(stderr, "NCNN forward failed: ret=%d input=%s output=%s shape=%dx%dx%d\n",
                     ret, input_name, output_name,
                     ncnn_mat_get_w(input), ncnn_mat_get_h(input), ncnn_mat_get_c(input));
    }

    int count = 0;
    if (ret == 0)
        count = parse_output(output, src_w, src_h, input_w, input_h, scale,
                             pad_x, pad_y, threshold, detections, max_detections);
    else
        std::fprintf(stderr, "NCNN inference failed: %d\n", ret);

    if (output)
        ncnn_mat_destroy(output);
    ncnn_extractor_destroy(ex);
    ncnn_mat_destroy(input);
    ncnn_mat_destroy(resized);
    ncnn_allocator_destroy(allocator);
    return ret == 0 ? count : -1;
}

static void draw_detections(cv::Mat& image, const Detection* detections, int count)
{
    static const char* class_names[] = {"cat", "dog"};
    static const cv::Scalar colors[] = {
        cv::Scalar(0, 255, 0),
        cv::Scalar(0, 0, 255),
    };

    for (int i = 0; i < count; i++)
    {
        const Detection& d = detections[i];
        const int label = d.label >= 0 && d.label < 2 ? d.label : 0;
        const cv::Scalar& color = colors[label];

        const cv::Point top_left(static_cast<int>(d.x0), static_cast<int>(d.y0));
        const cv::Point bottom_right(static_cast<int>(d.x1), static_cast<int>(d.y1));
        cv::rectangle(image, top_left, bottom_right, color, 2);

        char text[64];
        snprintf(text, sizeof(text), "%s %.1f%%", class_names[label], d.score * 100.f);
        int baseline = 0;
        const cv::Size text_size = cv::getTextSize(
            text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
        const int text_x = std::max(0, static_cast<int>(d.x0));
        const int text_y = std::max(text_size.height + baseline, static_cast<int>(d.y0));
        cv::rectangle(image,
                      cv::Point(text_x, text_y - text_size.height - baseline),
                      cv::Point(text_x + text_size.width, text_y),
                      cv::Scalar(255, 255, 255), -1);
        cv::putText(image, text, cv::Point(text_x, text_y - baseline),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0), 1);

        std::printf("class=%d score=%.4f box=%.1f,%.1f,%.1f,%.1f\n",
                    d.label, d.score, d.x0, d.y0, d.x1, d.y1);
    }
}

int main(int argc, char** argv)
{
    if (argc != 4 && argc != 5)
    {
        std::fprintf(stderr, "Usage: %s image model.ncnn.param model.ncnn.bin [conf=0.25]\n", argv[0]);
        return 1;
    }
    const float conf = (argc == 5) ? strtof(argv[4], NULL) : 0.25f;

    cv::Mat image = cv::imread(argv[1], 1);
    if (image.empty())
    {
        std::fprintf(stderr, "failed to read image: %s\n", argv[1]);
        return 1;
    }

    ncnn_net_t net = ncnn_net_create();
    if (!net)
    {
        std::fprintf(stderr, "failed to create NCNN net\n");
        return 1;
    }

    // Initialize and load the model once, before the benchmark loop.
    ncnn_option_t opt = ncnn_net_get_option(net);
    ncnn_option_set_num_threads(opt, 1);
    // Runtime switches, overridable for on-device A/B experiments:
    //   NCNN_WINOGRAD=1  enable winograd F(2x2/4x4/6x6,3x3) for stride-1 3x3 convs
    //   NCNN_SGEMM=1     enable im2col+sgemm path
    //   NCNN_BF16=1      bf16 storage (half memory traffic, conversion overhead)
    //   NCNN_PACKING=1   packed layout (only helps when MSA/SIMD is available)
    //   NCNN_THREADS=n   worker threads (default 1)
    const char* e_wino = getenv("NCNN_WINOGRAD");
    const char* e_sgemm = getenv("NCNN_SGEMM");
    const char* e_bf16 = getenv("NCNN_BF16");
    const char* e_pack = getenv("NCNN_PACKING");
    const char* e_thr = getenv("NCNN_THREADS");
    ncnn_option_set_use_packing_layout(opt, e_pack ? atoi(e_pack) : 0);
    ncnn_option_set_use_winograd_convolution(opt, e_wino ? atoi(e_wino) : 0);
    ncnn_option_set_use_sgemm_convolution(opt, e_sgemm ? atoi(e_sgemm) : 0);
    ncnn_option_set_use_bf16_storage(opt, e_bf16 ? atoi(e_bf16) : 0);
    if (e_thr)
        ncnn_option_set_num_threads(opt, atoi(e_thr));

    if (ncnn_net_load_param(net, argv[2]) != 0 ||
        ncnn_net_load_model(net, argv[3]) != 0)
    {
        std::fprintf(stderr, "failed to load NCNN model\n");
        ncnn_net_destroy(net);
        return 1;
    }

    const int num_runs = 20;
    Detection detections[300];
    int count = 0;
    double total_time = 0.0;
    double min_time = DBL_MAX;
    double max_time = 0.0;
    int timed_runs = 0;

    for (int i = 0; i < num_runs; i++)
    {
        struct timeval start, end;
        gettimeofday(&start, NULL);
        count = detect(image, net, detections, 300, conf);
        gettimeofday(&end, NULL);

        if (count < 0)
        {
            ncnn_net_destroy(net);
            return 1;
        }

        const double elapsed =
            (end.tv_sec - start.tv_sec) * 1000.0 +
            (end.tv_usec - start.tv_usec) / 1000.0;
        // Run 1 is a warm-up (first-touch page faults etc.) and is excluded.
        if (i > 0)
        {
            total_time += elapsed;
            timed_runs++;
            if (elapsed < min_time)
                min_time = elapsed;
            if (elapsed > max_time)
                max_time = elapsed;
        }

        std::fprintf(stderr, "Run %3d: %.2f ms%s\n", i + 1, elapsed,
                     i == 0 ? " (warm-up, excluded)" : "");
    }

    std::fprintf(stderr, "\n=== Statistics ===\n");
    std::fprintf(stderr, "Total runs: %d (timed: %d, run 1 warm-up excluded)\n",
                 num_runs, timed_runs);
    std::fprintf(stderr, "Average time: %.2f ms\n", total_time / timed_runs);
    std::fprintf(stderr, "Min time: %.2f ms\n", min_time);
    std::fprintf(stderr, "Max time: %.2f ms\n", max_time);
    std::fprintf(stderr, "Total time: %.2f ms\n", total_time);

    draw_detections(image, detections, count);
    if (!cv::imwrite("output.png", image))
    {
        std::fprintf(stderr, "failed to write output.png\n");
        ncnn_net_destroy(net);
        return 1;
    }

    std::fprintf(stderr, "Saved result to output.png\n");
    ncnn_net_destroy(net);
    return 0;
}
