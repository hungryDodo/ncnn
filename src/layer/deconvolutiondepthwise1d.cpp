// Copyright 2022 Tencent
// SPDX-License-Identifier: BSD-3-Clause

#include "deconvolutiondepthwise1d.h"

#include "fused_activation.h"

namespace ncnn {

DeconvolutionDepthWise1D::DeconvolutionDepthWise1D()
{
    one_blob_only = true;
    support_inplace = false;
}

int DeconvolutionDepthWise1D::load_param(const ParamDict& pd)
{
    num_output = pd.get(0, 0);
    kernel_w = pd.get(1, 0);
    dilation_w = pd.get(2, 1);
    stride_w = pd.get(3, 1);
    pad_left = pd.get(4, 0);
    pad_right = pd.get(15, pad_left);
    output_pad_right = pd.get(18, 0);
    output_w = pd.get(20, 0);
    bias_term = pd.get(5, 0);
    weight_data_size = pd.get(6, 0);
    group = pd.get(7, 1);
    activation_type = pd.get(9, 0);
    activation_params = pd.get(10, Mat());

    dynamic_weight = pd.get(28, 0);

    if (dynamic_weight)
    {
        one_blob_only = false;
    }

    return 0;
}

int DeconvolutionDepthWise1D::load_model(const ModelBin& mb)
{
    if (dynamic_weight)
        return 0;

    weight_data = mb.load(weight_data_size, 0);
    if (weight_data.empty())
        return -100;

    if (bias_term)
    {
        bias_data = mb.load(num_output, 1);
        if (bias_data.empty())
            return -100;
    }

    return 0;
}

static int deconvolutiondepthwise1d(const Mat& bottom_blob, Mat& top_blob, const Mat& weight_data, const Mat& bias_data, int kernel_w, int stride_w, int dilation_w, int group, int activation_type, const Mat& activation_params, const Option& opt)
{
    const int w = bottom_blob.w;
    const int h = bottom_blob.h;

    const int outw = top_blob.w;
    const int outh = top_blob.h;

    const int bias_term = bias_data.empty() ? 0 : 1;

    // depth-wise
    if (h == group && group == outh)
    {
        #pragma omp parallel for num_threads(opt.num_threads)
        for (int g = 0; g < group; g++)
        {
            Mat out = top_blob.row_range(g, 1);

            const float* inptr = bottom_blob.row(g);
            const float* kptr = (const float*)weight_data + kernel_w * g;

            const float bias = bias_term ? bias_data[g] : 0.f;

            out.fill(bias);

            for (int j = 0; j < w; j++)
            {
                float* outptr = (float*)out + j * stride_w;

                const float val = inptr[j];

                for (int k = 0; k < kernel_w; k++)
                {
                    float w = kptr[k];
                    outptr[k * dilation_w] += val * w;
                }
            }

            {
                float* outptr = out;

                for (int i = 0; i < outw; i++)
                {
                    outptr[i] = activation_ss(outptr[i], activation_type, activation_params);
                }
            }
        }
    }
    else
    {
        const int h_g = h / group;
        const int outh_g = outh / group;

#ifdef _WIN32
        #pragma omp parallel for num_threads(opt.num_threads)
#else
        #pragma omp parallel for collapse(2) num_threads(opt.num_threads)
#endif
        for (int g = 0; g < group; g++)
        {
            for (int p = 0; p < outh_g; p++)
            {
                Mat out = top_blob.row_range(g * outh_g + p, 1);

                const float* weight_data_ptr = (const float*)weight_data + kernel_w * h_g * outh_g * g;
                const float bias = bias_term ? bias_data[g * outh_g + p] : 0.f;

                out.fill(bias);

                for (int j = 0; j < w; j++)
                {
                    float* outptr = (float*)out + j * stride_w;

                    const float* kptr = weight_data_ptr + kernel_w * h_g * p;

                    for (int q = 0; q < h_g; q++)
                    {
                        const float val = bottom_blob.row(h_g * g + q)[j];

                        for (int k = 0; k < kernel_w; k++)
                        {
                            outptr[k * dilation_w] += val * kptr[k];
                        }

                        kptr += kernel_w;
                    }
                }

                {
                    float* outptr = out;

                    for (int i = 0; i < outw; i++)
                    {
                        outptr[i] = activation_ss(outptr[i], activation_type, activation_params);
                    }
                }
            }
        }
    }

    return 0;
}

int DeconvolutionDepthWise1D::forward(const Mat& bottom_blob, Mat& top_blob, const Option& opt) const
{
    int w = bottom_blob.w;
    size_t elemsize = bottom_blob.elemsize;

    const int kernel_extent_w = dilation_w * (kernel_w - 1) + 1;

    int outw = (w - 1) * stride_w + kernel_extent_w + output_pad_right;

    Mat top_blob_bordered;
    if (pad_left > 0 || pad_right > 0 || output_w > 0)
    {
        top_blob_bordered.create(outw, num_output, elemsize, opt.workspace_allocator);
    }
    else
    {
        top_blob_bordered = top_blob;
        top_blob_bordered.create(outw, num_output, elemsize, opt.blob_allocator);
    }
    if (top_blob_bordered.empty())
        return -100;

    int ret = deconvolutiondepthwise1d(bottom_blob, top_blob_bordered, weight_data, bias_data, kernel_w, stride_w, dilation_w, group, activation_type, activation_params, opt);
    if (ret != 0)
        return ret;

    cut_padding(top_blob_bordered, top_blob, opt);
    if (top_blob.empty())
        return -100;

    return 0;
}

int DeconvolutionDepthWise1D::forward(const std::vector<Mat>& bottom_blobs, std::vector<Mat>& top_blobs, const Option& opt) const
{
    const Mat& bottom_blob = bottom_blobs[0];
    const Mat& _weight_data = bottom_blobs[1];
    Mat& top_blob = top_blobs[0];

    const int _num_input = bottom_blob.h;
    const int _kernel_w = _weight_data.w;
    const int _num_output = _weight_data.h * group;

    Mat weight_data_flattened;
    flatten(_weight_data, weight_data_flattened, opt);
    if (weight_data_flattened.empty())
        return -100;

    // transpose group-inch/group-outch/group-kw to group-outch/group-inch/group-kw
    Mat weight_data_transposed;
    {
        weight_data_transposed.create(_kernel_w * _num_output * _num_input / group, 4u, opt.workspace_allocator);
        if (weight_data_transposed.empty())
            return -100;

        const int outch_g = _num_output / group;
        const int inch_g = _num_input / group;
        const int maxk = _kernel_w;

        for (int g = 0; g < group; g++)
        {
            // reorder weight from inch-outch to outch-inch
            float* wg2 = (float*)weight_data_transposed + g * outch_g * inch_g * maxk;
            const float* wg = (const float*)weight_data_flattened + g * inch_g * outch_g * maxk;
            for (int i = 0; i < outch_g; i++)
            {
                for (int j = 0; j < inch_g; j++)
                {
                    for (int k = 0; k < maxk; k++)
                    {
                        wg2[(i * inch_g + j) * maxk + k] = wg[(j * outch_g + i) * maxk + k];
                    }
                }
            }
        }
    }

    Mat bias_data_flattened;
    if (bias_term)
    {
        const Mat& _bias_data = bottom_blobs[2];
        flatten(_bias_data, bias_data_flattened, opt);
        if (bias_data_flattened.empty())
            return -100;
    }

    const int w = bottom_blob.w;

    const int kernel_extent_w = dilation_w * (_kernel_w - 1) + 1;

    int outw = (w - 1) * stride_w + kernel_extent_w + output_pad_right;

    Mat top_blob_bordered;
    if (pad_left > 0 || pad_right > 0 || output_w > 0)
    {
        top_blob_bordered.create(outw, _num_output, 4u, opt.workspace_allocator);
    }
    else
    {
        top_blob_bordered = top_blob;
        top_blob_bordered.create(outw, _num_output, 4u, opt.blob_allocator);
    }
    if (top_blob_bordered.empty())
        return -100;

    int ret = deconvolutiondepthwise1d(bottom_blob, top_blob_bordered, weight_data_transposed, bias_data_flattened, _kernel_w, stride_w, dilation_w, group, activation_type, activation_params, opt);
    if (ret != 0)
        return ret;

    cut_padding(top_blob_bordered, top_blob, opt);
    if (top_blob.empty())
        return -100;

    return 0;
}

void DeconvolutionDepthWise1D::cut_padding(const Mat& top_blob_bordered, Mat& top_blob, const Option& opt) const
{
    if (pad_left > 0 || pad_right > 0)
    {
        copy_cut_border(top_blob_bordered, top_blob, 0, 0, pad_left, pad_right, opt);
    }
    else if (output_w > 0)
    {
        int wcut = top_blob_bordered.w - output_w;

        if (pad_left == -233 || pad_right == -233)
        {
            // onnx padding=SAME_UPPER
            copy_cut_border(top_blob_bordered, top_blob, 0, 0, wcut / 2, wcut - wcut / 2, opt);
        }
        else if (pad_left == -234 || pad_right == -234)
        {
            // onnx padding=SAME_LOWER
            copy_cut_border(top_blob_bordered, top_blob, 0, 0, wcut - wcut / 2, wcut / 2, opt);
        }
    }
    else
    {
        top_blob = top_blob_bordered;
    }
}

} // namespace ncnn
