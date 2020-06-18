/**
 * \file dnn/src/fallback/conv_bias/conv1x1/algos_conv1x1_gemv.cpp
 * MegEngine is Licensed under the Apache License, Version 2.0 (the "License")
 *
 * Copyright (c) 2014-2020 Megvii Inc. All rights reserved.
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT ARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 */

#include "src/fallback/conv_bias/conv1x1/algos_conv1x1_gemv.h"
#include "src/fallback/conv_bias/conv1x1/conv1x1_utils.h"

#include "src/common/opr_delegate.h"
#include "src/fallback/conv_bias/common.h"
#include "src/fallback/conv_bias/opr_impl.h"

#include "megdnn/opr_param_defs.h"
#include "src/naive/convolution/helper.h"

#include "src/fallback/matrix_mul/gemv.h"
#if MEGDNN_X86
#include "src/x86/conv_bias/postprocess_helper.h"
#elif (MEGDNN_ARMV7 || MEGDNN_AARCH64)
#include "src/arm_common/conv_bias/postprocess_helper.h"
#include "src/arm_common/matrix_mul/fp32/exec_sgemv.h"
#include "src/arm_common/matrix_mul/fp16/hgemv.h"
#include "src/arm_common/matrix_mul/int8/gemv.h"
#endif

#include "midout.h"
MIDOUT_DECL(megdnn_fallback_conv1x1_gemv)

using namespace megdnn;
using namespace fallback;
#if MEGDNN_X86
using namespace x86;
#endif
using namespace conv1x1;

namespace {

#if MEGDNN_X86
template <typename stype, typename btype, param::ConvBias::Format F>
struct GemvLike {
    inline static void do_gemv(const stype* A, const stype* B, btype* C,
                               size_t M, size_t N, size_t K, size_t LDA,
                               size_t LDB, size_t LDC, DType src,
                               DType filter) {
        megdnn_throw("x86 conv1x1 gemv only supports format : NCHW");
    }
};

template <typename stype, typename btype>
struct GemvLike<stype, btype, param::ConvBias::Format::NCHW> {
    inline static void do_gemv(const stype* A, const stype* B, btype* C,
                               size_t M, size_t N, size_t K, size_t LDA,
                               size_t LDB, size_t LDC, DType src,
                               DType filter) {
        MEGDNN_MARK_USED_VAR(src);
        MEGDNN_MARK_USED_VAR(filter);
        megdnn::fallback::gemv_like<stype, btype>(A, B, C, M, N, K, LDA, LDB,
                                                  LDC);
    }
};

#elif MEGDNN_AARCH64 || MEGDNN_ARMV7
template <typename stype, typename btype, param::ConvBias::Format F>
struct GemvLike {
    inline static void do_gemv(const stype* A, const stype* B, btype* C,
                               size_t M, size_t N, size_t K, size_t LDA,
                               size_t LDB, size_t LDC, DType src,
                               DType filter) {
        megdnn_throw("arm conv1x1 gemv only supports format : NCHW");
    }
};

template <typename stype, typename btype>
struct GemvLike<stype, btype, param::ConvBias::Format::NCHW> {
    inline static void do_gemv(const stype* A, const stype* B, btype* C,
                               size_t M, size_t N, size_t K, size_t LDA,
                               size_t LDB, size_t LDC, DType src,
                               DType filter) {
        MEGDNN_MARK_USED_VAR(src);
        MEGDNN_MARK_USED_VAR(filter);
        megdnn::arm_common::gemv_like(A, B, C, M, N, K, LDA, LDB, LDC);
    }
};

template <>
struct GemvLike<dt_int8, dt_int16, param::ConvBias::Format::NCHW> {
    inline static void do_gemv(const dt_int8* A, const dt_int8* B, dt_int16* C,
                               size_t M, size_t N, size_t K, size_t LDA,
                               size_t LDB, size_t LDC, DType src,
                               DType filter) {
        MEGDNN_MARK_USED_VAR(src);
        MEGDNN_MARK_USED_VAR(filter);
        megdnn::fallback::gemv_like<dt_int8, dt_int16>(A, B, C, M, N, K, LDA,
                                                       LDB, LDC);
    }
};

#if __ARM_FEATURE_FP16_VECTOR_ARITHMETIC
template <>
struct GemvLike<dt_float16, dt_float16, param::ConvBias::Format::NCHW> {
    inline static void do_gemv(const dt_float16* A, const dt_float16* B,
                               dt_float16* C, size_t M, size_t N, size_t K,
                               size_t LDA, size_t LDB, size_t LDC, DType src,
                               DType filter) {
        MEGDNN_MARK_USED_VAR(src);
        MEGDNN_MARK_USED_VAR(filter);
        megdnn::arm_common::gemv_like(reinterpret_cast<const __fp16*>(A),
                                      reinterpret_cast<const __fp16*>(B),
                                      reinterpret_cast<__fp16*>(C), M, N, K,
                                      LDA, LDB, LDC);
    }
};
#endif
#endif

template <>
struct GemvLike<dt_uint8, dt_int32, param::ConvBias::Format::NCHW> {
    inline static void do_gemv(const dt_uint8* A, const dt_uint8* B,
                               dt_int32* C, size_t M, size_t N, size_t K,
                               size_t LDA, size_t LDB, size_t LDC, DType src,
                               DType filter) {
        uint8_t zp0 = src.param<dtype::Quantized8Asymm>().zero_point;
        uint8_t zp1 = filter.param<dtype::Quantized8Asymm>().zero_point;
        megdnn::fallback::gemv_like<dt_uint8, dt_int32>(A, B, C, M, N, K, LDA,
                                                        LDB, LDC, zp0, zp1);
    }
};

template <typename src_ctype, typename bias_ctype, typename dst_ctype,
          typename op_ctype, typename op_dtype,
          megdnn::PostprocessMode postprocess_mode,
          param::ConvBias::Format format>
struct Conv1x1GemvWorker {
    static void exec(WorkspaceBundle& whole_bundle,
                     WorkspaceBundle& thread_bundle, size_t oc_tile_size,
                     const ConvBiasImpl::NCBKernSizeParam& param,
                     const ConvBiasImpl::NCBKernParam& ncb_param,
                     const ConvBiasImpl::NCBKernIndex& ncb_index) {
        whole_bundle.set(ncb_param.workspace_ptr);

        size_t OC = param.filter_meta.ocpg;
        size_t IC = param.filter_meta.icpg;

        size_t batch_id = ncb_index.ndrange_id[0];
        size_t group_id = ncb_index.ndrange_id[1];
        size_t oc_tile_id_in_group = ncb_index.ndrange_id[2];
        size_t thread_id = ncb_index.thread_id;

        size_t oc_start = oc_tile_size * oc_tile_id_in_group;
        size_t oc_end = oc_start + oc_tile_size;
        oc_end = (oc_end <= OC ? oc_end : OC);

        size_t numbers_of_ncb_filter_offset =
                oc_tile_size * IC * oc_tile_id_in_group;
        const src_ctype* Aptr = ncb_param.filter<src_ctype>(group_id) +
                                numbers_of_ncb_filter_offset;

        const src_ctype* Bptr = ncb_param.src<src_ctype>(batch_id, group_id);

        size_t thread_offset = thread_bundle.total_size_in_bytes() * thread_id;
        size_t bytes_offset_of_matmul_dst_this_thread =
                thread_offset + thread_bundle.get_size(0);
        bias_ctype* matmul_temp_dst = reinterpret_cast<bias_ctype*>(
                reinterpret_cast<int8_t*>(whole_bundle.get(0)) +
                bytes_offset_of_matmul_dst_this_thread);

        size_t numbers_of_ncb_dst_offset = oc_tile_size * oc_tile_id_in_group;
        dst_ctype* conv_bias_dst =
                ncb_param.dst<dst_ctype>(batch_id, group_id) +
                numbers_of_ncb_dst_offset;

        bool is_dst_8bit =
                (param.src_type.enumv() == DTypeEnum::QuantizedS8 &&
                 param.dst_type.enumv() == DTypeEnum::QuantizedS8) ||
                (param.src_type.enumv() == DTypeEnum::Quantized8Asymm &&
                 param.dst_type.enumv() == DTypeEnum::Quantized8Asymm);
        bias_ctype* gemv_dst =
                is_dst_8bit ? matmul_temp_dst
                            : reinterpret_cast<bias_ctype*>(conv_bias_dst);

        GemvLike<src_ctype, bias_ctype, format>::do_gemv(
                Aptr, Bptr, gemv_dst, oc_end - oc_start, 1, IC, IC, 1, 1,
                ncb_param.filter_type, ncb_param.src_type);

        //! do postprocess
        void* bias_ptr = nullptr;
        if (param.bias_mode == megdnn::BiasMode::BIAS) {
            bias_ptr = static_cast<void*>(const_cast<bias_ctype*>(
                    ncb_param.bias<bias_ctype>(batch_id, group_id) +
                    numbers_of_ncb_dst_offset));
        } else {
            bias_ptr = static_cast<void*>(const_cast<bias_ctype*>(
                    ncb_param.bias<bias_ctype>(batch_id, group_id) + oc_start));
        }

        PostProcess<op_ctype, op_dtype, postprocess_mode>::run(
                gemv_dst, bias_ptr, conv_bias_dst, param.bias_mode,
                param.nonlineMode, param.bias_type, param.dst_type, 1_z,
                oc_end - oc_start, 1, 1, 1);
    }
};

}  // namespace

size_t ConvBiasImpl::AlgoConv1x1Gemv::get_oc_tile_size_heuristic(
        const NCBKernSizeParam& param) const {
    size_t OC = param.filter_meta.ocpg;
    size_t oc_block_size_one_thread = div_ceil(OC, param.nr_threads);
    return round_up<size_t>(oc_block_size_one_thread, 16);
}

size_t ConvBiasImpl::AlgoConv1x1Gemv::get_workspace(
        ConvBiasImpl*, const NCBKernSizeParam& param) const {
    MIDOUT_BEGIN(megdnn_fallback_conv1x1_gemv,
                 midout_iv("AlgoConv1x1Gemv::get_workspace"_hash)) {
        size_t compt_oc_block_size = get_oc_tile_size_heuristic(param);
        auto thread_bundle =
                utils::get_thread_bundle(param, 0, compt_oc_block_size);
        return WorkspaceBundle{
                nullptr,
                {thread_bundle.total_size_in_bytes() * param.nr_threads}}
                .total_size_in_bytes();
    }
    MIDOUT_END();
}

SmallVector<ConvBiasImpl::NCBKern>
ConvBiasImpl::AlgoConv1x1Gemv::dispatch_kerns(
        ConvBiasImpl* opr, const NCBKernSizeParam& param) const {
    SmallVector<ConvBiasImpl::NCBKern> ret_kern;
    size_t OC = param.filter_meta.ocpg;
    size_t compt_oc_block_size = get_oc_tile_size_heuristic(param);
    size_t GROUP = param.filter_meta.group;
    size_t BATCH = param.n;
    size_t oc_blocks_per_group = div_ceil(OC, compt_oc_block_size);

    //! get thread bundle
    auto thread_bundle =
            utils::get_thread_bundle(param, 0, compt_oc_block_size);
    auto whole_bundle = WorkspaceBundle{
            nullptr, {thread_bundle.total_size_in_bytes() * param.nr_threads}};

    using conv1x1_gemv_kern =
            std::function<void(WorkspaceBundle&, WorkspaceBundle&, size_t,
                               const ConvBiasImpl::NCBKernSizeParam&,
                               const ConvBiasImpl::NCBKernParam&,
                               const ConvBiasImpl::NCBKernIndex&)>;
    conv1x1_gemv_kern conv1x1_gemv_worker = nullptr;

#define cb1(_format, _dt, _post_ctype, _postprocess_mode, _midout_tag)         \
    MIDOUT_BEGIN(megdnn_fallback_conv1x1_gemv, midout_iv(_midout_tag)) {       \
        if (param.filter_type.enumv() == DTypeTrait<_dt>::enumv) {             \
            conv1x1_gemv_worker =                                              \
                    Conv1x1GemvWorker<_dt, _dt, _dt, _post_ctype, _post_ctype, \
                                      _postprocess_mode, _format>::exec;       \
        }                                                                      \
    }                                                                          \
    MIDOUT_END()

#define cb2(_format, _i_src_type, _i_bias_type, _i_dst_type, _src_ctype,   \
            _bias_ctype, _dst_ctype, _postprocess_mode, _midout_tag)       \
    MIDOUT_BEGIN(megdnn_fallback_conv1x1_gemv, midout_iv(_midout_tag)) {   \
        if (param.filter_type.enumv() == param.src_type.enumv() &&         \
            param.src_type.enumv() == DTypeTrait<_i_src_type>::enumv &&    \
            param.dst_type.enumv() == DTypeTrait<_i_dst_type>::enumv) {    \
            conv1x1_gemv_worker =                                          \
                    Conv1x1GemvWorker<_src_ctype, _bias_ctype, _dst_ctype, \
                                      DTypeTrait<_i_bias_type>::ctype,     \
                                      DTypeTrait<_i_dst_type>::ctype,      \
                                      _postprocess_mode, _format>::exec;   \
        }                                                                  \
    }                                                                      \
    MIDOUT_END()

    switch (opr->param().format) {
        case param::ConvBias::Format::NCHW:
            cb1(param::ConvBias::Format::NCHW, dt_float32, dt_float32,
                PostprocessMode::FLOAT, "NCHW::GEMV::FLOAT"_hash);
#if __ARM_FEATURE_FP16_VECTOR_ARITHMETIC
            cb1(param::ConvBias::Format::NCHW, dt_float16, __fp16,
                PostprocessMode::FLOAT, "NCHW::GEMV::FLOAT16_FP16"_hash);
#endif
            cb2(param::ConvBias::Format::NCHW, dt_int8, dt_int32, dt_int32,
                dt_int8, dt_int32, dt_int32, PostprocessMode::NO_PROCESS,
                "NCHW::GEMV::INT8x8x32_INT32"_hash);
            cb2(param::ConvBias::Format::NCHW, dt_int8, dt_int16, dt_int16,
                dt_int8, dt_int16, dt_int16, PostprocessMode::NO_PROCESS,
                "NCHW::GEMV::INT8x8x16_INT16"_hash);
            cb2(param::ConvBias::Format::NCHW, dtype::QuantizedS8,
                dtype::QuantizedS32, dtype::QuantizedS32, dt_int8, dt_int32,
                dt_int32, PostprocessMode::NO_PROCESS,
                "NCHW::GEMV::QINT8x8x32_QINT32"_hash);
            cb2(param::ConvBias::Format::NCHW, dtype::QuantizedS8,
                dtype::QuantizedS32, dtype::QuantizedS8, dt_int8, dt_int32,
                dt_int8, PostprocessMode::QUANTIZED,
                "NCHW::GEMV::QINT8x8x32_QINT8"_hash);
            cb2(param::ConvBias::Format::NCHW, dtype::Quantized8Asymm,
                dtype::QuantizedS32, dtype::QuantizedS32, dt_uint8, dt_int32,
                dt_int32, PostprocessMode::NO_PROCESS,
                "NCHW::GEMV::QUINT8x8x32_QINT32"_hash);
            cb2(param::ConvBias::Format::NCHW, dtype::Quantized8Asymm,
                dtype::QuantizedS32, dtype::Quantized8Asymm, dt_uint8, dt_int32,
                dt_uint8, PostprocessMode::QUANTIZED,
                "NCHW::GEMV::QUINT8x8x32_QUINT8"_hash);
            break;

        default:
            megdnn_throw("Invalid Format");
            break;
    }
#undef cb1
#undef cb2

    megdnn_assert(conv1x1_gemv_worker, "No suitable gemv worker");

    auto kern_compt =
            [compt_oc_block_size, param, conv1x1_gemv_worker, whole_bundle,
             thread_bundle](
                    const ConvBiasImpl::NCBKernParam& ncb_param,
                    const ConvBiasImpl::NCBKernIndex& ncb_index) mutable {
                conv1x1_gemv_worker(whole_bundle, thread_bundle,
                                    compt_oc_block_size, param, ncb_param,
                                    std::move(ncb_index));
            };
    ret_kern.push_back({kern_compt, {BATCH, GROUP, oc_blocks_per_group}});
    return ret_kern;
}

bool ConvBiasImpl::AlgoConv1x1Gemv::usable(ConvBiasImpl* opr,
                                           const NCBKernSizeParam& param,
                                           AlgoSelectionStrategy) const {
    MIDOUT_BEGIN(megdnn_fallback_conv1x1_gemv,
                 midout_iv("AlgoConv1x1Gemv::usable"_hash)) {
        //! whether 1x1
        size_t FH = param.filter_meta.spatial[0],
               FW = param.filter_meta.spatial[1];
        size_t PH = param.filter_meta.padding[0],
               PW = param.filter_meta.padding[1];
        size_t SH = param.filter_meta.stride[0],
               SW = param.filter_meta.stride[1];

        if (FH != 1 || FW != 1 || PH || PW || SH != 1 || SW != 1) {
            return false;
        }

        //! whether gemv
        size_t OH = param.osz[0];
        size_t OW = param.osz[1];
        if (OH * OW != 1) {
            return false;
        }

        //! even no naive support in gemv
        if ((param.src_type.enumv() == param.filter_type.enumv() &&
             param.src_type.enumv() == DTypeEnum::Int16) &&
            param.dst_type.enumv() == DTypeEnum::Int32) {
            return false;
        }

        //! make sure 8x8x16 and 8x8x32 biasmode is nobias and nonlineMode
        //! is identity otherwise return false mean that 8x8x32 and 8x8x16
        //! not support PostProcess
        if (param.dst_type.enumv() == DTypeEnum::Int16 ||
            param.dst_type.enumv() == DTypeEnum::Int32 ||
            param.dst_type.enumv() == DTypeEnum::QuantizedS32) {
            if (param.bias_mode != megdnn::BiasMode::NO_BIAS ||
                param.nonlineMode != megdnn::NonlineMode::IDENTITY) {
                return false;
            }
        }

        //! supports a few dtypes
        if (param.src_type.enumv() != param.filter_type.enumv()) {
            return false;
        }

        if (param.src_type.enumv() != DTypeEnum::Int8 &&
            param.src_type.enumv() != DTypeEnum::QuantizedS8 &&
            param.src_type.enumv() != DTypeEnum::Quantized8Asymm &&
#if !MEGDNN_DISABLE_FLOAT16
            param.src_type.enumv() != DTypeEnum::Float16 &&
#endif
            param.src_type.enumv() != DTypeEnum::Float32) {
            return false;
        }

        bool is_param_ok =
                (param.filter_meta.dilation[0] ==
                         param.filter_meta.dilation[1] &&
                 param.filter_meta.dilation[0] == 1) &&
                param.compute_mode == param::ConvBias::ComputeMode::DEFAULT;

        bool is_format_and_dtype_ok = false;
#if MEGDNN_X86
        if (opr->param().format == param::ConvBias::Format::NCHW) {
            //! x86 supports all dtypes in NCHW
            is_format_and_dtype_ok = true;
        }
#elif MEGDNN_AARCH64 || MEGDNN_ARMV7
        //! add NCHW44 and NCHW44_DOT support in the future
        if (opr->param().format == param::ConvBias::Format::NCHW) {
            //! NCHW format supports all dtype
            is_format_and_dtype_ok = true;
        }
#endif
        return is_param_ok && is_format_and_dtype_ok;
    }
    MIDOUT_END();
    return false;
}

bool ConvBiasImpl::AlgoConv1x1Gemv::is_preferred(
        ConvBiasImpl*, const NCBKernSizeParam& param) const {
    size_t OC = param.filter_meta.ocpg;
    if (OC <= 2 && param.src_type.enumv() != DTypeEnum::Float32)
        return true;
#if (MEGDNN_ARMV7 || MEGDNN_AARCH64)
    //! maybe add support for QuantizedAsym in the future
    return (param.src_type.enumv() == DTypeEnum::Int8 &&
            param.filter_type.enumv() == DTypeEnum::Int8 &&
            param.dst_type.enumv() == DTypeEnum::Int32) ||
           (param.src_type.enumv() == DTypeEnum::QuantizedS8 &&
            param.filter_type.enumv() == DTypeEnum::QuantizedS8 &&
            param.dst_type.enumv() == DTypeEnum::QuantizedS8) ||
           (param.src_type.enumv() == DTypeEnum::QuantizedS8 &&
            param.filter_type.enumv() == DTypeEnum::QuantizedS8 &&
            param.dst_type.enumv() == DTypeEnum::QuantizedS32) ||
#if !MEGDNN_DISABLE_FLOAT16
           (param.src_type.enumv() == DTypeEnum::Float16 &&
            param.filter_type.enumv() == DTypeEnum::Float16 &&
            param.dst_type.enumv() == DTypeEnum::Float16) ||
#endif
           (param.src_type.enumv() == DTypeEnum::Float32 &&
            param.filter_type.enumv() == DTypeEnum::Float32 &&
            param.dst_type.enumv() == DTypeEnum::Float32);
#else
    return false;
#endif
}

// vim: syntax=cpp.doxygen