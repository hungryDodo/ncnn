// Copyright 2022 yala <zhaojunchao@loongson.cn>;<junchao82@qq.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "deconvolutiondepthwise_loongarch.h"

#include "layer_type.h"

#if __loongarch_sx
#include <lsxintrin.h>
#endif // __loongarch_sx

#include "loongarch_activation.h"
#include "loongarch_usability.h"

namespace ncnn {

DeconvolutionDepthWise_loongarch::DeconvolutionDepthWise_loongarch()
{
#if __loongarch_sx
    support_packing = true;
#endif // __loongarch_sx
}

int DeconvolutionDepthWise_loongarch::create_pipeline(const Option& opt)
{
    if (dynamic_weight)
        return 0;

    const int maxk = kernel_w * kernel_h;
    int channels = (weight_data_size / group) / maxk / (num_output / group) * group;

    // depth-wise
    if (channels == group && group == num_output)
    {
        int elempack = 1;
#if __loongarch_sx
        if (opt.use_packing_layout)
        {
            elempack = channels % 4 == 0 ? 4 : 1;
        }
#endif

        Mat weight_data_transposed(weight_data.w);
        {
            float* pt = weight_data_transposed;
            const float* p = weight_data;

            for (int i = 0; i < (channels / group) * (num_output / group) * group; i++)
            {
                for (int k = 0; k < maxk; k++)
                {
                    pt[maxk - 1 - k] = p[k];
                }

                p += maxk;
                pt += maxk;
            }
        }

#if __loongarch_sx
        // pack4
        if (elempack == 4)
        {
            Mat weight_data_r2 = weight_data_transposed.reshape(maxk, group);
            convert_packing(weight_data_r2, weight_data_tm, 4, opt);
        }
#endif // __loongarch_sx

        if (elempack == 1)
        {
            weight_data_tm = weight_data_transposed;
        }

        if (opt.lightmode)
            weight_data.release();

        return 0;
    }

    // group convolution
    create_group_ops(opt);

    if (opt.lightmode)
        weight_data.release();

    return 0;
}

int DeconvolutionDepthWise_loongarch::create_group_ops(const Option& opt)
{
    // create Deconvolution op for each group
    const int maxk = kernel_w * kernel_h;
    int channels = (weight_data_size / group) / maxk / (num_output / group) * group;

    for (int i = 0; i < (int)group_ops.size(); i++)
        delete group_ops[i];

    group_ops.clear();

    const int channels_g = channels / group;
    const int num_output_g = num_output / group;

    group_ops.resize(group);

    for (int g = 0; g < group; g++)
    {
        Mat weight_data_g = weight_data.range(maxk * channels_g * num_output_g * g, maxk * channels_g * num_output_g).clone();
        Mat bias_data_g;
        if (bias_term)
            bias_data_g = bias_data.range(num_output_g * g, num_output_g);

        ncnn::Layer* op = ncnn::create_layer_cpu(ncnn::LayerType::Deconvolution);

        // set param
        ncnn::ParamDict pd;
        pd.set(0, num_output_g); // num_output
        pd.set(1, kernel_w);
        pd.set(11, kernel_h);
        pd.set(2, dilation_w);
        pd.set(12, dilation_h);
        pd.set(3, stride_w);
        pd.set(13, stride_h);
        pd.set(4, 0);  // pad_w
        pd.set(14, 0); // pad_h
        pd.set(18, output_pad_right);
        pd.set(19, output_pad_bottom);
        pd.set(5, bias_term);
        pd.set(6, maxk * channels_g * num_output_g); // weight_data_size
        pd.set(9, activation_type);
        pd.set(10, activation_params);

        op->load_param(pd);

        // set weights
        if (bias_term)
        {
            ncnn::Mat weights[2];
            weights[0] = weight_data_g;
            weights[1] = bias_data_g;

            op->load_model(ModelBinFromMatArray(weights));
        }
        else
        {
            ncnn::Mat weights[1];
            weights[0] = weight_data_g;

            op->load_model(ModelBinFromMatArray(weights));
        }

        op->create_pipeline(opt);

        group_ops[g] = op;
    }

    return 0;
}

int DeconvolutionDepthWise_loongarch::destroy_pipeline(const Option& opt)
{
    for (int i = 0; i < (int)group_ops.size(); i++)
    {
        group_ops[i]->destroy_pipeline(opt);
        delete group_ops[i];
    }
    group_ops.clear();

    return 0;
}

int DeconvolutionDepthWise_loongarch::forward(const Mat& bottom_blob, Mat& top_blob, const Option& opt) const
{
    // convolv with NxN kernel
    // value = value + bias

    int w = bottom_blob.w;
    int h = bottom_blob.h;
    int channels = bottom_blob.c;
    size_t elemsize = bottom_blob.elemsize;
    int elempack = bottom_blob.elempack;

    const int kernel_extent_w = dilation_w * (kernel_w - 1) + 1;
    const int kernel_extent_h = dilation_h * (kernel_h - 1) + 1;

    int outw = (w - 1) * stride_w + kernel_extent_w + output_pad_right;
    int outh = (h - 1) * stride_h + kernel_extent_h + output_pad_bottom;
    int out_elempack = 1;
#if __loongarch_sx
    if (opt.use_packing_layout)
    {
        out_elempack = num_output % 4 == 0 ? 4 : 1;
    }
#endif
    size_t out_elemsize = elemsize / elempack * out_elempack;

    Mat top_blob_bordered;
    if (pad_left > 0 || pad_right > 0 || pad_top > 0 || pad_bottom > 0 || (output_w > 0 && output_h > 0))
    {
        top_blob_bordered.create(outw, outh, num_output / out_elempack, out_elemsize, out_elempack, opt.workspace_allocator);
    }
    else
    {
        top_blob_bordered = top_blob;
        top_blob_bordered.create(outw, outh, num_output / out_elempack, out_elemsize, out_elempack, opt.blob_allocator);
    }
    if (top_blob_bordered.empty())
        return -100;

    const int maxk = kernel_w * kernel_h;

    // depth-wise
    if (channels * elempack == group && group == num_output)
    {
#if __loongarch_sx
        if (elempack == 4)
        {
            {
                #pragma omp parallel for num_threads(opt.num_threads)
                for (int g = 0; g < channels; g++)
                {
                    float* outptr = top_blob_bordered.channel(g);
                    const float* kptr = (const float*)weight_data_tm + maxk * g * 4;
                    const Mat m = bottom_blob.channel(g);

                    for (int i = 0; i < outh; i++)
                    {
                        for (int j = 0; j < outw; j++)
                        {
                            __m128 _sum = (__m128)__lsx_vreplgr2vr_w(0);

                            if (bias_term)
                            {
                                _sum = (__m128)__lsx_vld((const float*)bias_data + g * 4, 0);
                            }

                            for (int y = 0; y < kernel_h; y++)
                            {
                                int sys = (i + y * dilation_h - (kernel_extent_h - 1));
                                if (sys < 0 || sys % stride_h != 0)
                                    continue;

                                int sy = sys / stride_h;
                                if (sy >= h)
                                    continue;

                                for (int x = 0; x < kernel_w; x++)
                                {
                                    int sxs = (j + x * dilation_w - (kernel_extent_w - 1));
                                    if (sxs < 0 || sxs % stride_w != 0)
                                        continue;

                                    int sx = sxs / stride_w;
                                    if (sx >= w)
                                        continue;

                                    const float* sptr = m.row(sy) + sx * 4;

                                    int k = y * kernel_w + x;

                                    __m128 _val = (__m128)__lsx_vld(sptr, 0);
                                    __m128 _w = (__m128)__lsx_vld(kptr + k * 4, 0);
                                    _sum = __lsx_vfmadd_s(_w, _val, _sum);
                                }
                            }

                            _sum = activation_ps(_sum, activation_type, activation_params);

                            __lsx_vst(_sum, outptr + j * 4, 0);
                        }

                        outptr += outw * 4;
                    }
                }
            }
        }
#endif // __loongarch_sx

        if (elempack == 1)
        {
            #pragma omp parallel for num_threads(opt.num_threads)
            for (int g = 0; g < channels; g++)
            {
                float* outptr = top_blob_bordered.channel(g);
                const float* kptr = (const float*)weight_data_tm + maxk * g;
                const Mat m = bottom_blob.channel(g);

                for (int i = 0; i < outh; i++)
                {
                    for (int j = 0; j < outw; j++)
                    {
                        float sum = 0.f;

                        if (bias_term)
                        {
                            sum = bias_data[g];
                        }

                        for (int y = 0; y < kernel_h; y++)
                        {
                            int sys = (i + y * dilation_h - (kernel_extent_h - 1));
                            if (sys < 0 || sys % stride_h != 0)
                                continue;

                            int sy = sys / stride_h;
                            if (sy >= h)
                                continue;

                            const float* sptr = m.row(sy);

                            for (int x = 0; x < kernel_w; x++)
                            {
                                int sxs = (j + x * dilation_w - (kernel_extent_w - 1));
                                if (sxs < 0 || sxs % stride_w != 0)
                                    continue;

                                int sx = sxs / stride_w;
                                if (sx >= w)
                                    continue;

                                float val = sptr[sx];

                                int k = y * kernel_w + x;

                                float w = kptr[k];

                                sum += val * w;
                            }
                        }

                        sum = activation_ss(sum, activation_type, activation_params);

                        outptr[j] = sum;
                    }

                    outptr += outw;
                }
            }
        }
    }
    else
    {
        // group deconvolution
        const int channels_g = channels * elempack / group;
        const int num_output_g = num_output / group;

        int g_elempack = 1;
        int out_g_elempack = 1;
#if __loongarch_sx
        if (opt.use_packing_layout)
        {
            g_elempack = channels_g % 4 == 0 ? 4 : 1;
            out_g_elempack = num_output_g % 4 == 0 ? 4 : 1;
        }
#endif

        // unpacking
        Mat bottom_blob_unpacked = bottom_blob;
        if (elempack > g_elempack)
        {
            Option opt_p = opt;
            opt_p.blob_allocator = opt.workspace_allocator;
            convert_packing(bottom_blob, bottom_blob_unpacked, 1, opt_p);
        }

        Mat top_blob_bordered_unpacked = top_blob_bordered;
        if (out_g_elempack < out_elempack)
        {
            top_blob_bordered_unpacked.create(outw, outh, num_output, out_elemsize / out_elempack, 1, opt.workspace_allocator);
            if (top_blob_bordered_unpacked.empty())
                return -100;
        }

        for (int g = 0; g < group; g++)
        {
            const Mat bottom_blob_g = bottom_blob_unpacked.channel_range(channels_g * g / g_elempack, channels_g / g_elempack);
            Mat top_blob_bordered_g = top_blob_bordered_unpacked.channel_range(num_output_g * g / out_g_elempack, num_output_g / out_g_elempack);

            const ncnn::Layer* op = group_ops[g];

            Option opt_g = opt;
            opt_g.blob_allocator = top_blob_bordered_unpacked.allocator;

            // forward
            op->forward(bottom_blob_g, top_blob_bordered_g, opt_g);
        }

        // packing
        if (out_g_elempack < out_elempack)
        {
            convert_packing(top_blob_bordered_unpacked, top_blob_bordered, 4, opt);
        }
        else
        {
            top_blob_bordered = top_blob_bordered_unpacked;
        }
    }

    cut_padding(top_blob_bordered, top_blob, opt);
    if (top_blob.empty())
        return -100;

    return 0;
}

int DeconvolutionDepthWise_loongarch::forward(const std::vector<Mat>& bottom_blobs, std::vector<Mat>& top_blobs, const Option& opt) const
{
    const Mat& bottom_blob = bottom_blobs[0];
    const Mat& _weight_data = bottom_blobs[1];
    Mat& top_blob = top_blobs[0];

    const int _num_input = bottom_blob.c * bottom_blob.elempack;
    const int _kernel_w = _weight_data.w;
    const int _kernel_h = _weight_data.h;
    const int _num_output = _weight_data.d * group;

    Mat weight_data_flattened;
    flatten(_weight_data, weight_data_flattened, opt);
    if (weight_data_flattened.empty())
        return -100;

    // weight_data_flattened as pack1
    weight_data_flattened.w *= weight_data_flattened.elempack;
    weight_data_flattened.elemsize /= weight_data_flattened.elempack;
    weight_data_flattened.elempack = 1;

    // transpose group-inch/group-outch/group-kh-kw to group-outch/group-inch/group-kh-kw
    Mat weight_data_transposed;
    {
        weight_data_transposed.create(_kernel_w * _kernel_h * _num_output * _num_input / group, 4u, opt.workspace_allocator);
        if (weight_data_transposed.empty())
            return -100;

        const int outch_g = _num_output / group;
        const int inch_g = _num_input / group;
        const int maxk = _kernel_h * _kernel_w;

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

        // bias_data_flattened as pack1
        bias_data_flattened.w *= bias_data_flattened.elempack;
        bias_data_flattened.elemsize /= bias_data_flattened.elempack;
        bias_data_flattened.elempack = 1;
    }

    ncnn::Layer* op = ncnn::create_layer_cpu(ncnn::LayerType::DeconvolutionDepthWise);

    ncnn::ParamDict pd;
    pd.set(0, _num_output);
    pd.set(1, _kernel_w);
    pd.set(11, _kernel_h);
    pd.set(2, dilation_w);
    pd.set(12, dilation_h);
    pd.set(3, stride_w);
    pd.set(13, stride_h);
    pd.set(4, pad_left);
    pd.set(15, pad_right);
    pd.set(14, pad_top);
    pd.set(16, pad_bottom);
    pd.set(18, output_pad_right);
    pd.set(19, output_pad_bottom);
    pd.set(20, output_w);
    pd.set(21, output_h);
    pd.set(5, bias_term);
    pd.set(6, weight_data_transposed.w);
    pd.set(7, group);
    pd.set(9, activation_type);
    pd.set(10, activation_params);

    op->load_param(pd);

    ncnn::Mat weights[2];
    weights[0] = weight_data_transposed;
    weights[1] = bias_data_flattened;

    op->load_model(ncnn::ModelBinFromMatArray(weights));

    op->create_pipeline(opt);

    op->forward(bottom_blob, top_blob, opt);

    op->destroy_pipeline(opt);

    delete op;

    return 0;
}

} // namespace ncnn
