/**
 * \file dnn/src/naive/check_non_finite/opr_impl.cpp
 * MegEngine is Licensed under the Apache License, Version 2.0 (the "License")
 *
 * Copyright (c) 2014-2021 Megvii Inc. All rights reserved.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * ARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#include "src/naive/check_non_finite/opr_impl.h"

#include "src/common/utils.h"
#include "src/naive/handle.h"

namespace {
using namespace megdnn;

#define wtype dt_int32

void reduce_fwd(const TensorNDArray& srcs, wtype* dptr) {
    dptr[0] = 0;
    for (auto src : srcs) {
        auto sptr = src.ptr<dt_float32>();
        size_t size = src.layout.total_nr_elems();
        std::function<wtype(wtype, wtype)> func;
        func = [&](wtype l, wtype r) -> wtype {
            if (l + 1 < r) {
                wtype mid = l + (r - l) / 2;
                return func(l, mid) | func(mid, r);
            } else {
                auto val = std::isfinite(sptr[l]);
                return static_cast<wtype>(!val);
            }
        };
        dptr[0] |= func(0, size);
    }
}

}  // namespace

namespace megdnn {
namespace naive {

void CheckNonFiniteImpl::exec(
        _megdnn_in const TensorNDArray& srcs, _megdnn_tensor_out dst,
        _megdnn_workspace workspace) {
    check_exec(srcs, dst, workspace.size);

    auto handle = static_cast<HandleImpl*>(this->handle());
    MEGDNN_DISPATCH_CPU_KERN(handle, reduce_fwd(srcs, dst.ptr<dt_int32>()));
}
}  // namespace naive
}  // namespace megdnn

// vim: syntax=cpp.doxygen
