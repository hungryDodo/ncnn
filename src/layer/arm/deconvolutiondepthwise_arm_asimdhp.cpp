// Copyright 2022 Tencent
// SPDX-License-Identifier: BSD-3-Clause

#include "deconvolutiondepthwise_arm.h"

#if __ARM_NEON
#include <arm_neon.h>
#endif // __ARM_NEON

#include "arm_activation.h"

namespace ncnn {

#if __ARM_FEATURE_FP16_VECTOR_ARITHMETIC
int DeconvolutionDepthWise_arm::create_pipeline_fp16s(const Option& opt)
{
    // create Deconvolution op for each group
    const int maxk = kernel_w * kernel_h;
    int channels = (weight_data_size / group) / maxk / (num_output / group) * group;

    // depth-wise
    if (channels == group && group == num_output)
    {
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

        int elempack = 1;
        if (opt.use_packing_layout)
        {
            elempack = opt.use_fp16_arithmetic && channels % 8 == 0 ? 8 : channels % 4 == 0 ? 4 : 1;
        }

        if (elempack == 8)
        {
            Mat weight_data_r2 = weight_data_transposed.reshape(maxk, group);
            Mat weight_data_r2_packed;
            convert_packing(weight_data_r2, weight_data_r2_packed, 8, opt);

            ncnn::cast_float32_to_float16(weight_data_r2_packed, weight_data_tm, opt);
        }

        if (elempack == 4)
        {
            Mat weight_data_r2 = weight_data_transposed.reshape(maxk, group);
            Mat weight_data_r2_packed;
            convert_packing(weight_data_r2, weight_data_r2_packed, 4, opt);

            ncnn::cast_float32_to_float16(weight_data_r2_packed, weight_data_tm, opt);
        }

        if (elempack == 1)
        {
            ncnn::cast_float32_to_float16(weight_data_transposed, weight_data_tm, opt);
        }

        ncnn::cast_float32_to_float16(bias_data, bias_data_fp16, opt);
    }
    else
    {
        // group deconvolution
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
    }

    if (opt.lightmode)
        weight_data.release();

    return 0;
}

int DeconvolutionDepthWise_arm::forward_fp16s(const Mat& bottom_blob, Mat& top_blob, const Option& opt) const
{
    int w = bottom_blob.w;
    int h = bottom_blob.h;
    int channels = bottom_blob.c;
    size_t elemsize = bottom_blob.elemsize;
    int elempack = bottom_blob.elempack;

    const int kernel_extent_w = dilation_w * (kernel_w - 1) + 1;
    const int kernel_extent_h = dilation_h * (kernel_h - 1) + 1;

    int outw = (w - 1) * stride_w + kernel_extent_w + output_pad_right;
    int outh = (h - 1) * stride_h + kernel_extent_h + output_pad_bottom;
    int out_elempack = (opt.use_packing_layout && num_output % 4 == 0) ? 4 : 1;
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
        if (elempack == 4)
        {
            {
                #pragma omp parallel for num_threads(opt.num_threads)
                for (int g = 0; g < channels; g++)
                {
                    __fp16* outptr = top_blob_bordered.channel(g);
                    const __fp16* kptr = (const __fp16*)weight_data_tm + maxk * g * 4;
                    const Mat m = bottom_blob.channel(g);

                    for (int i = 0; i < outh; i++)
                    {
                        for (int j = 0; j < outw; j++)
                        {
                            float32x4_t _sum = vdupq_n_f32(0.f);

                            if (bias_term)
                            {
                                _sum = vld1q_f32((const float*)bias_data + g * 4);
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

                                    const __fp16* sptr = m.row<const __fp16>(sy) + sx * 4;

                                    float32x4_t _val = vcvt_f32_f16(vld1_f16(sptr));

                                    int k = y * kernel_w + x;

                                    float32x4_t _w = vcvt_f32_f16(vld1_f16(kptr + k * 4));

                                    _sum = vfmaq_f32(_sum, _val, _w);
                                }
                            }

                            _sum = activation_ps(_sum, activation_type, activation_params);

                            vst1_f16(outptr + j * 4, vcvt_f16_f32(_sum));
                        }

                        outptr += outw * 4;
                    }
                }
            }
        }

        if (elempack == 1)
        {
            {
                #pragma omp parallel for num_threads(opt.num_threads)
                for (int g = 0; g < channels; g++)
                {
                    __fp16* outptr = top_blob_bordered.channel(g);
                    const __fp16* kptr = (const __fp16*)weight_data_tm + maxk * g;
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

                                const __fp16* sptr = m.row<const __fp16>(sy);

                                for (int x = 0; x < kernel_w; x++)
                                {
                                    int sxs = (j + x * dilation_w - (kernel_extent_w - 1));
                                    if (sxs < 0 || sxs % stride_w != 0)
                                        continue;

                                    int sx = sxs / stride_w;
                                    if (sx >= w)
                                        continue;

                                    float val = (float)sptr[sx];

                                    int k = y * kernel_w + x;

                                    float w = (float)kptr[k];

                                    sum += val * w;
                                }
                            }

                            sum = activation_ss(sum, activation_type, activation_params);

                            outptr[j] = (__fp16)sum;
                        }

                        outptr += outw;
                    }
                }
            }
        }
    }
    else
    {
        // group deconvolution
        const int channels_g = channels * elempack / group;
        const int num_output_g = num_output / group;

        int g_elempack = (opt.use_packing_layout && channels_g % 4 == 0) ? 4 : 1;
        int out_g_elempack = (opt.use_packing_layout && num_output_g % 4 == 0) ? 4 : 1;

        // unpacking
        Mat bottom_blob_unpacked = bottom_blob;
        if (elempack == 4 && g_elempack == 1)
        {
            Option opt_p = opt;
            opt_p.blob_allocator = opt.workspace_allocator;
            convert_packing(bottom_blob, bottom_blob_unpacked, 1, opt_p);
            if (bottom_blob_unpacked.empty())
                return -100;
        }

        Mat top_blob_bordered_unpacked = top_blob_bordered;
        if (out_g_elempack == 1 && out_elempack == 4)
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
            int ret = op->forward(bottom_blob_g, top_blob_bordered_g, opt_g);
            if (ret != 0)
                return ret;
        }

        // packing
        if (out_g_elempack == 1 && out_elempack == 4)
        {
            convert_packing(top_blob_bordered_unpacked, top_blob_bordered, 4, opt);
            if (top_blob_bordered.empty())
                return -100;
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

int DeconvolutionDepthWise_arm::forward_fp16sa(const Mat& bottom_blob, Mat& top_blob, const Option& opt) const
{
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
    if (opt.use_packing_layout)
    {
        out_elempack = opt.use_fp16_arithmetic && num_output % 8 == 0 ? 8 : num_output % 4 == 0 ? 4 : 1;
    }
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
        if (elempack == 8)
        {
            {
                #pragma omp parallel for num_threads(opt.num_threads)
                for (int g = 0; g < channels; g++)
                {
                    __fp16* outptr = top_blob_bordered.channel(g);
                    const __fp16* kptr = (const __fp16*)weight_data_tm + maxk * g * 8;
                    const Mat m = bottom_blob.channel(g);

                    for (int i = 0; i < outh; i++)
                    {
                        for (int j = 0; j < outw; j++)
                        {
                            float16x8_t _sum = vdupq_n_f16((__fp16)0.f);

                            if (bias_term)
                            {
                                _sum = vld1q_f16((const __fp16*)bias_data_fp16 + g * 8);
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

                                    const __fp16* sptr = m.row<const __fp16>(sy) + sx * 8;

                                    float16x8_t _val = vld1q_f16(sptr);

                                    int k = y * kernel_w + x;

                                    float16x8_t _w = vld1q_f16(kptr + k * 8);

                                    _sum = vfmaq_f16(_sum, _val, _w);
                                }
                            }

                            _sum = activation_ps_f16(_sum, activation_type, activation_params);

                            vst1q_f16(outptr + j * 8, _sum);
                        }

                        outptr += outw * 8;
                    }
                }
            }
        }

        if (elempack == 4)
        {
            {
                #pragma omp parallel for num_threads(opt.num_threads)
                for (int g = 0; g < channels; g++)
                {
                    __fp16* outptr = top_blob_bordered.channel(g);
                    const __fp16* kptr = (const __fp16*)weight_data_tm + maxk * g * 4;
                    const Mat m = bottom_blob.channel(g);

                    for (int i = 0; i < outh; i++)
                    {
                        for (int j = 0; j < outw; j++)
                        {
                            float16x4_t _sum = vdup_n_f16((__fp16)0.f);

                            if (bias_term)
                            {
                                _sum = vld1_f16((const __fp16*)bias_data_fp16 + g * 4);
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

                                    const __fp16* sptr = m.row<const __fp16>(sy) + sx * 4;

                                    float16x4_t _val = vld1_f16(sptr);

                                    int k = y * kernel_w + x;

                                    float16x4_t _w = vld1_f16(kptr + k * 4);

                                    _sum = vfma_f16(_sum, _val, _w);
                                }
                            }

                            _sum = activation_ps_f16(_sum, activation_type, activation_params);

                            vst1_f16(outptr + j * 4, _sum);
                        }

                        outptr += outw * 4;
                    }
                }
            }
        }

        if (elempack == 1)
        {
            {
                #pragma omp parallel for num_threads(opt.num_threads)
                for (int g = 0; g < channels; g++)
                {
                    __fp16* outptr = top_blob_bordered.channel(g);
                    const __fp16* kptr = (const __fp16*)weight_data_tm + maxk * g;
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

                                const __fp16* sptr = m.row<const __fp16>(sy);

                                for (int x = 0; x < kernel_w; x++)
                                {
                                    int sxs = (j + x * dilation_w - (kernel_extent_w - 1));
                                    if (sxs < 0 || sxs % stride_w != 0)
                                        continue;

                                    int sx = sxs / stride_w;
                                    if (sx >= w)
                                        continue;

                                    __fp16 val = sptr[sx];

                                    int k = y * kernel_w + x;

                                    __fp16 w = kptr[k];

                                    sum += val * w;
                                }
                            }

                            sum = activation_ss_f16(sum, activation_type, activation_params);

                            outptr[j] = (__fp16)sum;
                        }

                        outptr += outw;
                    }
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
        if (opt.use_packing_layout)
        {
            g_elempack = opt.use_fp16_arithmetic && channels_g % 8 == 0 ? 8 : channels_g % 4 == 0 ? 4 : 1;
            out_g_elempack = opt.use_fp16_arithmetic && num_output_g % 8 == 0 ? 8 : num_output_g % 4 == 0 ? 4 : 1;
        }

        // unpacking
        Mat bottom_blob_unpacked = bottom_blob;
        if (elempack > g_elempack)
        {
            Option opt_p = opt;
            opt_p.blob_allocator = opt.workspace_allocator;
            convert_packing(bottom_blob, bottom_blob_unpacked, g_elempack, opt_p);
            if (bottom_blob_unpacked.empty())
                return -100;
        }

        Mat top_blob_bordered_unpacked = top_blob_bordered;
        if (out_g_elempack < out_elempack)
        {
            top_blob_bordered_unpacked.create(outw, outh, num_output / out_g_elempack, out_elemsize / out_elempack * out_g_elempack, out_g_elempack, opt.workspace_allocator);
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
            int ret = op->forward(bottom_blob_g, top_blob_bordered_g, opt_g);
            if (ret != 0)
                return ret;
        }

        // packing
        if (out_g_elempack < out_elempack)
        {
            convert_packing(top_blob_bordered_unpacked, top_blob_bordered, out_elempack, opt);
            if (top_blob_bordered.empty())
                return -100;
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
#endif // __ARM_FEATURE_FP16_VECTOR_ARITHMETIC

} // namespace ncnn
