// Copyright 2021 Tencent
// SPDX-License-Identifier: BSD-3-Clause

static void conv1x1s1_sgemm_packnto1_fp16sa_rvv(const Mat& bottom_blob, Mat& top_blob, const Mat& kernel, const Mat& _bias, const Option& opt)
{
    int w = bottom_blob.w;
    int h = bottom_blob.h;
    const int size = w * h;

    Mat bottom_im2col = bottom_blob;
    bottom_im2col.w = size;
    bottom_im2col.h = 1;

    im2col_sgemm_packnto1_fp16sa_rvv(bottom_im2col, top_blob, kernel, _bias, opt);
}

static void conv1x1s2_sgemm_packnto1_fp16sa_rvv(const Mat& bottom_blob, Mat& top_blob, const Mat& kernel, const Mat& _bias, const Option& opt)
{
    const int packn = csrr_vlenb() / 2;
    const size_t vl = __riscv_vsetvl_e16m1(packn);

    int w = bottom_blob.w;
    int channels = bottom_blob.c;
    size_t elemsize = bottom_blob.elemsize;
    int elempack = bottom_blob.elempack;

    int outw = top_blob.w;
    int outh = top_blob.h;

    const int tailstep = (w - 2 * outw + w) * packn;

    Mat bottom_blob_shrinked;
    bottom_blob_shrinked.create(outw, outh, channels, elemsize, elempack, opt.workspace_allocator);

    #pragma omp parallel for num_threads(opt.num_threads)
    for (int p = 0; p < channels; p++)
    {
        const __fp16* r0 = bottom_blob.channel(p);
        __fp16* outptr = bottom_blob_shrinked.channel(p);

        for (int i = 0; i < outh; i++)
        {
            for (int j = 0; j < outw; j++)
            {
                vfloat16m1_t _val = __riscv_vle16_v_f16m1(r0, vl);
                __riscv_vse16_v_f16m1(outptr, _val, vl);

                r0 += packn * 2;
                outptr += packn;
            }

            r0 += tailstep;
        }
    }

    conv1x1s1_sgemm_packnto1_fp16sa_rvv(bottom_blob_shrinked, top_blob, kernel, _bias, opt);
}
