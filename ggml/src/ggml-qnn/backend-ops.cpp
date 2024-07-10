
#include "backend-ops.hpp"

#include <memory>

#include "graph.hpp"
#include "logger.hpp"
#include "tensor.hpp"
#include "utils.hpp"

namespace {

void print_ggml_tensor(const ggml_tensor *tensor) {
    QNN_LOG_DEBUG("%15s: type = %i (%5s) ne = %5" PRIi64 " x %5" PRIi64 " x %5" PRIi64 ", nb = (%5zi, %5zi, %5zi)\n",
                  tensor->name, tensor->type, ggml_type_name(tensor->type), tensor->ne[0], tensor->ne[1], tensor->ne[2],
                  tensor->nb[0], tensor->nb[1], tensor->nb[2]);
}

bool qnn_is_valid_params(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                         ggml_tensor *dst) {
    if (!ctx || !src0 || !src1 || !dst) {
        QNN_LOG_WARN("invalid params\n");
        return false;
    }

    auto instance = ctx->instance;
    auto *tensor0 = qnn::ggml_qnn_tensor::from_ggml_tensor(src0);
    auto *tensor1 = qnn::ggml_qnn_tensor::from_ggml_tensor(src1);
    auto *tensor2 = qnn::ggml_qnn_tensor::from_ggml_tensor(dst);
    if (!instance || !tensor0 || !tensor1 || !tensor2) {
        QNN_LOG_WARN("invalid tensors\n");
        return false;
    }

    return true;
}

template <size_t _InputSize, size_t _OutputSize>
bool qnn_bind_tensors_to_graph(qnn::ggml_qnn_graph<_InputSize, _OutputSize> *graph, const std::string &op_name,
                               const std::array<const ggml_tensor *, _InputSize> &inputs,
                               const std::array<ggml_tensor *, _OutputSize> &outputs) {
    std::array<Qnn_Tensor_t, _InputSize> qnn_input_tensors;
    for (size_t i = 0; i < inputs.size(); ++i) {
        auto tensor = qnn::ggml_qnn_tensor::from_ggml_tensor(inputs[i]);
        if (!tensor || !tensor->bind_to_graph(*graph)) {
            return false;
        }

        qnn_input_tensors[i] = tensor->get_qnn_tensor();
    }

    std::array<Qnn_Tensor_t, _OutputSize> qnn_output_tensors;
    for (size_t i = 0; i < outputs.size(); ++i) {
        auto tensor = qnn::ggml_qnn_tensor::from_ggml_tensor(outputs[i]);
        if (!tensor || !tensor->bind_to_graph(*graph)) {
            return false;
        }

        qnn_output_tensors[i] = tensor->get_qnn_tensor();
    }

    if (!graph->add_nodes(op_name, qnn_input_tensors, qnn_output_tensors)) {
        return false;
    }

    return true;
}

template <size_t _InputSize, size_t _OutputSize>
bool execute_graph(qnn::ggml_qnn_graph<_InputSize, _OutputSize> *graph,
                   const std::array<const ggml_tensor *, _InputSize> &inputs,
                   const std::array<ggml_tensor *, _OutputSize> &outputs) {

    std::array<Qnn_Tensor_t, _InputSize> qnn_input_tensors;
    for (size_t i = 0; i < inputs.size(); ++i) {
        auto tensor = qnn::ggml_qnn_tensor::from_ggml_tensor(inputs[i]);
        if (!tensor || !tensor->write_to_qnn_tensor()) {
            QNN_LOG_WARN("write_to_qnn_tensor failed\n");
            return false;
        }

        qnn_input_tensors[i] = tensor->get_qnn_tensor();
    }

    std::array<Qnn_Tensor_t, _OutputSize> qnn_output_tensors;
    for (size_t i = 0; i < outputs.size(); ++i) {
        auto tensor = qnn::ggml_qnn_tensor::from_ggml_tensor(outputs[i]);
        if (!tensor) {
            return false;
        }

        qnn_output_tensors[i] = tensor->get_qnn_tensor();
    }

    if (!graph->execute(qnn_input_tensors, qnn_output_tensors)) {
        QNN_LOG_WARN("execute failed\n");
        return false;
    }

    for (auto &output : outputs) {
        auto tensor = qnn::ggml_qnn_tensor::from_ggml_tensor(output);
        if (!tensor || !tensor->read_from_qnn_tensor()) {
            QNN_LOG_WARN("read_from_qnn_tensors failed\n");
            return false;
        }
    }

    return true;
}

template <size_t _InputSize, size_t _OutputSize>
qnn::ggml_qnn_graph_binary *get_qnn_graph_from_cache(ggml_backend_qnn_context *ctx, ggml_op op,
                                                     const std::string &qnn_op,
                                                     const std::array<const ggml_tensor *, _InputSize> &inputs,
                                                     const std::array<ggml_tensor *, _OutputSize> &outputs) {
    const std::string graph_key(ggml_op_name(op));
    auto it = ctx->qnn_binary_graph_cache.find(graph_key);
    qnn::ggml_qnn_graph_binary *graph_ptr = nullptr;
    if (it != ctx->qnn_binary_graph_cache.end()) {
        graph_ptr = it->second.get();
    } else {
        std::string graph_name = graph_key + "_" + std::to_string(ctx->threads);
        for (auto &input : inputs) {
            graph_name += "_";
            graph_name += input->name;
        }
        auto graph = std::make_unique<qnn::ggml_qnn_graph_binary>(graph_name, (QNNBackend)(ctx->device),
                                                                  ctx->instance->get_qnn_context_handle(),
                                                                  ctx->raw_interface, ctx->socinfo.vtcm_size_in_mb);

        if (!graph->is_valid()) {
            return nullptr;
        }

        if (!qnn_bind_tensors_to_graph<2, 1>(graph.get(), qnn_op.c_str(), inputs, outputs)) {
            return nullptr;
        }

        graph_ptr = graph.get();
        ctx->qnn_binary_graph_cache[graph_key] = std::move(graph);
    }

    return graph_ptr;
}

constexpr const char *kGgmlOpToQnnOp[] = {
    nullptr,                 // GGML_OP_NONE
    nullptr,                 // GGML_OP_DUP
    QNN_OP_ELEMENT_WISE_ADD, // GGML_OP_ADD
    nullptr,                 // GGML_OP_ADD1
    nullptr,                 // GGML_OP_ACC
    nullptr,                 // GGML_OP_SUB
    nullptr,                 // GGML_OP_MUL
    nullptr,                 // GGML_OP_DIV
    nullptr,                 // GGML_OP_SQR
    nullptr,                 // GGML_OP_SQRT
    nullptr,                 // GGML_OP_LOG
    nullptr,                 // GGML_OP_SUM
    nullptr,                 // GGML_OP_SUM_ROWS
    nullptr,                 // GGML_OP_MEAN
    nullptr,                 // GGML_OP_ARGMAX
    nullptr,                 // GGML_OP_REPEAT
    nullptr,                 // GGML_OP_REPEAT_BACK
    nullptr,                 // GGML_OP_CONCAT
    nullptr,                 // GGML_OP_SILU_BACK
    nullptr,                 // GGML_OP_NORM
    nullptr,                 // GGML_OP_RMS_NORM
    nullptr,                 // GGML_OP_RMS_NORM_BACK
    nullptr,                 // GGML_OP_GROUP_NORM

    QNN_OP_MAT_MUL, // GGML_OP_MUL_MAT
    nullptr,        // GGML_OP_MUL_MAT_ID
    nullptr,        // GGML_OP_OUT_PROD

    nullptr, // GGML_OP_SCALE
    nullptr, // GGML_OP_SET
    nullptr, // GGML_OP_CPY
    nullptr, // GGML_OP_CONT
    nullptr, // GGML_OP_RESHAPE
    nullptr, // GGML_OP_VIEW
    nullptr, // GGML_OP_PERMUTE
    nullptr, // GGML_OP_TRANSPOSE
    nullptr, // GGML_OP_GET_ROWS
    nullptr, // GGML_OP_GET_ROWS_BACK
    nullptr, // GGML_OP_DIAG
    nullptr, // GGML_OP_DIAG_MASK_INF
    nullptr, // GGML_OP_DIAG_MASK_ZERO
    nullptr, // GGML_OP_SOFT_MAX
    nullptr, // GGML_OP_SOFT_MAX_BACK
    nullptr, // GGML_OP_ROPE
    nullptr, // GGML_OP_ROPE_BACK
    nullptr, // GGML_OP_CLAMP
    nullptr, // GGML_OP_CONV_TRANSPOSE_1D
    nullptr, // GGML_OP_IM2COL
    nullptr, // GGML_OP_CONV_TRANSPOSE_2D
    nullptr, // GGML_OP_POOL_1D
    nullptr, // GGML_OP_POOL_2D
    nullptr, // GGML_OP_UPSCALE
    nullptr, // GGML_OP_PAD
    nullptr, // GGML_OP_ARANGE
    nullptr, // GGML_OP_TIMESTEP_EMBEDDING
    nullptr, // GGML_OP_ARGSORT
    nullptr, // GGML_OP_LEAKY_RELU

    nullptr, // GGML_OP_FLASH_ATTN_EXT
    nullptr, // GGML_OP_FLASH_ATTN_BACK
    nullptr, // GGML_OP_SSM_CONV
    nullptr, // GGML_OP_SSM_SCAN
    nullptr, // GGML_OP_WIN_PART
    nullptr, // GGML_OP_WIN_UNPART
    nullptr, // GGML_OP_GET_REL_POS
    nullptr, // GGML_OP_ADD_REL_POS

    nullptr, // GGML_OP_UNARY

    nullptr, // GGML_OP_MAP_UNARY
    nullptr, // GGML_OP_MAP_BINARY

    nullptr, // GGML_OP_MAP_CUSTOM1_F32
    nullptr, // GGML_OP_MAP_CUSTOM2_F32
    nullptr, // GGML_OP_MAP_CUSTOM3_F32

    nullptr, // GGML_OP_MAP_CUSTOM1
    nullptr, // GGML_OP_MAP_CUSTOM2
    nullptr, // GGML_OP_MAP_CUSTOM3

    nullptr, // GGML_OP_CROSS_ENTROPY_LOSS
    nullptr, // GGML_OP_CROSS_ENTROPY_LOSS_BACK
};

static_assert(sizeof(kGgmlOpToQnnOp) / sizeof(kGgmlOpToQnnOp[0]) == GGML_OP_COUNT,
              "GGML_OP_COUNT does not match the size of the ops table");

template <ggml_op _GgmlOp>
void qnn_binary_op_impl(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                        ggml_tensor *dst) {
    static_assert(kGgmlOpToQnnOp[_GgmlOp] != nullptr, "GGML_OP does not have a corresponding QNN_OP");

    qnn::qnn_perf perf(ggml_op_name(_GgmlOp));
    perf.start();

    bool succeed = false;
    qnn::ggml_qnn_graph_binary *graph_ptr =
        get_qnn_graph_from_cache<2, 1>(ctx, _GgmlOp, kGgmlOpToQnnOp[_GgmlOp], { src0, src1 }, { dst });
    if (graph_ptr) {
        succeed = execute_graph<2, 1>(graph_ptr, { src0, src1 }, { dst });
    }

    if (!succeed) {
        print_ggml_tensor(src0);
        print_ggml_tensor(src1);
        print_ggml_tensor(dst);
    }
}

} // namespace

#ifndef NDEBUG
#define CHECK_PARAMS(ctx, src0, src1, dst)                        \
    do {                                                          \
        if (!qnn_is_valid_params((ctx), (src0), (src1), (dst))) { \
            return;                                               \
        }                                                         \
    } while (0)

#else
#define CHECK_PARAMS(ctx, src0, src1, dst)
#endif

static void ggml_qnn_add(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                         ggml_tensor *dst) {
    CHECK_PARAMS(ctx, src0, src1, dst);
    qnn_binary_op_impl<GGML_OP_ADD>(ctx, src0, src1, dst);
}

/*
 * ggml_qnn_mul_mat was re-added as a standalone function because
 * the following comments came from https://github.com/ggerganov/llama.cpp/pull/1632
 * MUL_MAT take most of the compute time (about 95%).
 * So to speed up llama, we have to focus on MUL_MAT.
 *
 * We have three kinds of MUL_MAT to compute:
 * mul_mat_f32:     both src0 and src1 are F32.
 * mul_mat_f16_f32: src0 is F16 and src1 is F32.
 * mul_mat_q_f32:   src0 is quantized (Q4_0, Q4_1, ...), and src1 is F32.
 */
static void ggml_qnn_mul_mat(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                             ggml_tensor *dst) {
    CHECK_PARAMS(ctx, src0, src1, dst);
    qnn_binary_op_impl<GGML_OP_MUL_MAT>(ctx, src0, src1, dst);
}

static void ggml_qnn_repeat(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                            ggml_tensor *dst) {}

static void ggml_qnn_get_rows(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                              ggml_tensor *dst) {}

static void ggml_qnn_acc(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                         ggml_tensor *dst) {}

static void ggml_qnn_div(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                         ggml_tensor *dst) {}

static void ggml_qnn_gelu(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                          ggml_tensor *dst) {}

static void ggml_qnn_silu(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                          ggml_tensor *dst) {}

static void ggml_qnn_gelu_quick(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                                ggml_tensor *dst) {}

static void ggml_qnn_tanh(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                          ggml_tensor *dst) {}

static void ggml_qnn_relu(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                          ggml_tensor *dst) {}

static void ggml_qnn_hardsigmoid(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                                 ggml_tensor *dst) {}

static void ggml_qnn_hardswish(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                               ggml_tensor *dst) {}

static void ggml_qnn_leaky_relu(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                                ggml_tensor *dst) {}

static void ggml_qnn_sqr(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                         ggml_tensor *dst) {}

static void ggml_qnn_norm(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                          ggml_tensor *dst) {}

static void ggml_qnn_group_norm(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                                ggml_tensor *dst) {}

static void ggml_qnn_concat(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                            ggml_tensor *dst) {}

static void ggml_qnn_upscale(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                             ggml_tensor *dst) {}

static void ggml_qnn_pad(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                         ggml_tensor *dst) {}

static void ggml_qnn_rms_norm(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                              ggml_tensor *dst) {}

static void ggml_qnn_cpy(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                         ggml_tensor *dst) {}

static void ggml_qnn_dup(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                         ggml_tensor *dst) {
    ggml_qnn_cpy(ctx, src0, dst, nullptr);
    (void)src1;
}

static void ggml_qnn_mul_mat_id(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                                ggml_tensor *dst) {}

static void ggml_qnn_scale(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                           ggml_tensor *dst) {}

static void ggml_qnn_clamp(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                           ggml_tensor *dst) {}

static void ggml_qnn_diag_mask_inf(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                                   ggml_tensor *dst) {}

static void ggml_qnn_soft_max(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                              ggml_tensor *dst) {}

static void ggml_qnn_rope(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                          ggml_tensor *dst) {
    GGML_ASSERT(ggml_is_contiguous(src0));
}

static void ggml_qnn_pool2d(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                            ggml_tensor *dst) {}

static void ggml_qnn_im2col(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                            ggml_tensor *dst) {}

static void ggml_qnn_sum_rows(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                              ggml_tensor *dst) {
    GGML_ASSERT(ggml_is_contiguous(src0));
}

static void ggml_qnn_argsort(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                             ggml_tensor *dst) {
    GGML_ASSERT(ggml_is_contiguous(src0));
}

static void ggml_qnn_nop(ggml_backend_qnn_context *ctx, const ggml_tensor *src0, const ggml_tensor *src1,
                         ggml_tensor *dst) {
    (void)src0;
    (void)src1;
    (void)dst;
}

qnn::ggml_qnn_op_array_t qnn::ggml_qnn_op_array() {
    static constexpr const qnn::ggml_qnn_op_t kQnnOpsTable[] = {
        nullptr,      // GGML_OP_NONE
        nullptr,      // GGML_OP_DUP
        ggml_qnn_add, // GGML_OP_ADD
        nullptr,      // GGML_OP_ADD1
        nullptr,      // GGML_OP_ACC
        nullptr,      // GGML_OP_SUB
        nullptr,      // GGML_OP_MUL
        nullptr,      // GGML_OP_DIV
        nullptr,      // GGML_OP_SQR
        nullptr,      // GGML_OP_SQRT
        nullptr,      // GGML_OP_LOG
        nullptr,      // GGML_OP_SUM
        nullptr,      // GGML_OP_SUM_ROWS
        nullptr,      // GGML_OP_MEAN
        nullptr,      // GGML_OP_ARGMAX
        nullptr,      // GGML_OP_REPEAT
        nullptr,      // GGML_OP_REPEAT_BACK
        nullptr,      // GGML_OP_CONCAT
        nullptr,      // GGML_OP_SILU_BACK
        nullptr,      // GGML_OP_NORM
        nullptr,      // GGML_OP_RMS_NORM
        nullptr,      // GGML_OP_RMS_NORM_BACK
        nullptr,      // GGML_OP_GROUP_NORM

        ggml_qnn_mul_mat, // GGML_OP_MUL_MAT
        nullptr,          // GGML_OP_MUL_MAT_ID
        nullptr,          // GGML_OP_OUT_PROD

        nullptr, // GGML_OP_SCALE
        nullptr, // GGML_OP_SET
        nullptr, // GGML_OP_CPY
        nullptr, // GGML_OP_CONT
        nullptr, // GGML_OP_RESHAPE
        nullptr, // GGML_OP_VIEW
        nullptr, // GGML_OP_PERMUTE
        nullptr, // GGML_OP_TRANSPOSE
        nullptr, // GGML_OP_GET_ROWS
        nullptr, // GGML_OP_GET_ROWS_BACK
        nullptr, // GGML_OP_DIAG
        nullptr, // GGML_OP_DIAG_MASK_INF
        nullptr, // GGML_OP_DIAG_MASK_ZERO
        nullptr, // GGML_OP_SOFT_MAX
        nullptr, // GGML_OP_SOFT_MAX_BACK
        nullptr, // GGML_OP_ROPE
        nullptr, // GGML_OP_ROPE_BACK
        nullptr, // GGML_OP_CLAMP
        nullptr, // GGML_OP_CONV_TRANSPOSE_1D
        nullptr, // GGML_OP_IM2COL
        nullptr, // GGML_OP_CONV_TRANSPOSE_2D
        nullptr, // GGML_OP_POOL_1D
        nullptr, // GGML_OP_POOL_2D
        nullptr, // GGML_OP_UPSCALE
        nullptr, // GGML_OP_PAD
        nullptr, // GGML_OP_ARANGE
        nullptr, // GGML_OP_TIMESTEP_EMBEDDING
        nullptr, // GGML_OP_ARGSORT
        nullptr, // GGML_OP_LEAKY_RELU

        nullptr, // GGML_OP_FLASH_ATTN_EXT
        nullptr, // GGML_OP_FLASH_ATTN_BACK
        nullptr, // GGML_OP_SSM_CONV
        nullptr, // GGML_OP_SSM_SCAN
        nullptr, // GGML_OP_WIN_PART
        nullptr, // GGML_OP_WIN_UNPART
        nullptr, // GGML_OP_GET_REL_POS
        nullptr, // GGML_OP_ADD_REL_POS

        nullptr, // GGML_OP_UNARY

        nullptr, // GGML_OP_MAP_UNARY
        nullptr, // GGML_OP_MAP_BINARY

        nullptr, // GGML_OP_MAP_CUSTOM1_F32
        nullptr, // GGML_OP_MAP_CUSTOM2_F32
        nullptr, // GGML_OP_MAP_CUSTOM3_F32

        nullptr, // GGML_OP_MAP_CUSTOM1
        nullptr, // GGML_OP_MAP_CUSTOM2
        nullptr, // GGML_OP_MAP_CUSTOM3

        nullptr, // GGML_OP_CROSS_ENTROPY_LOSS
        nullptr, // GGML_OP_CROSS_ENTROPY_LOSS_BACK
    };

    static_assert(sizeof(kQnnOpsTable) / sizeof(kQnnOpsTable[0]) == GGML_OP_COUNT,
                  "GGML_OP_COUNT does not match the size of the ops table");
    return kQnnOpsTable;
}