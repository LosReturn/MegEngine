/**
 * \file dnn/src/rocm/indexing_multi_axis_vec/kern_apply_opr_set.cpp.hip
 *
 * MegEngine is Licensed under the Apache License, Version 2.0 (the "License")
 *
 * Copyright (c) 2014-2021 Megvii Inc. All rights reserved.
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT ARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */
#include "hcc_detail/hcc_defs_prologue.h"

#include "hip_header.h"
#include "src/common/indexing_multi_axis_vec_kdef.h"
#define KERN_APPLY_OPR_OPR  ::megdnn::indexing_multi_axis_vec_kdef::OprSet
#include "./kern_apply_opr_impl.hipinl"

// vim: ft=cuda syntax=cpp.doxygen

