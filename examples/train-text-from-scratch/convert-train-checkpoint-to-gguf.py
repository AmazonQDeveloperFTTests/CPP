#!/usr/bin/env python3
# train-text-from-scratch checkpoint --> gguf conversion

import argparse
import gguf
import os
import sys
from pathlib import Path

class Tensor:
    def __init__(self, dtype='f', ne=None):
        if ne is None:
            ne = []
        self.dtype = dtype
        self.ne = ne
        self.nbytes = 0
        if self.dtype == 'f':
            self.nbytes = product(self.ne) * 4
        else:
            raise ValueError(f"Unhandled data type '{self.dtype}'")

    def load(self, data, offset):
        nd = struct.unpack('<I', bytes(data[offset:offset + 4]))[0]; offset += 4
        namelen = struct.unpack('<I', bytes(data[offset:offset + 4]))[0]; offset += 4
        dtype = struct.unpack('<I', bytes(data[offset:offset + 4]))[0]; offset += 4

        assert(nd == len(self.ne))
        ne = []
        for d in range(nd):
            n = struct.unpack('<Q', bytes(data[offset:offset + 8]))[0]; offset += 8
            ne.append(n)

        assert(tuple(ne) == tuple(self.ne))

        if self.dtype == 'f':
            assert(dtype == 0)
        else:
            raise ValueError(f"Unhandled data type '{self.dtype}'")

        self.name = bytes(data[offset:offset+namelen]); offset += namelen
        # 32-byte alignment
        offset += (0 - offset) & 31
        self.data = data[offset:offset+self.nbytes]

        return offset

    def max_storage_size(self):
        result = 0
        result += 4 # nd
        result += 4 # namelen
        result += 4 # dtype
        result += len(self.ne)*8 # ne
        result += 48 # name (maximum as of commit 3b5515bbe0e2224425986ba24f1f5d84aa38dce9)
        result += 31 # 32-byte alignment
        result += self.nbytes
        return result

    def save_gguf(self, gguf_writer, name):
        gguf_writer.add_tensor(
            name=name, 
            tensor=self.data, 
            raw_shape=np.array(list(reversed(self.ne))),
            raw_type=gguf.GGMLQuantizationType.F32)

class OptimizationParamsV0:
    def __init__(self):
        pass

    def load(self, data, offset):
        self.type                 = struct.unpack('<I', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.n_threads            = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.past                 = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.delta                = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.print_forward_graph  = struct.unpack('<?', bytes(data[offset:offset + 4]))[0];  offset += 4 # 32bit-aligned
        self.print_backward_graph = struct.unpack('<?', bytes(data[offset:offset + 4]))[0];  offset += 4 # 32bit-aligned
        self.adam_n_iter          = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.adam_sched           = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.adam_decay           = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.adam_alpha           = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.adam_beta1           = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.adam_beta2           = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.adam_eps             = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.adam_eps_f           = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.adam_eps_g           = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.lbfgs_m              = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.lbfgs_n_iter         = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.lbfgs_max_linesearch = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.lbfgs_eps            = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.lbfgs_ftol           = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.lbfgs_wolfe          = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.lbfgs_min_step       = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.lbfgs_max_step       = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.lbfgs_linesearch     = struct.unpack('<I', bytes(data[offset:offset + 4]))[0];  offset += 4
        return offset

class OptimizationContext:
    def __init__(self):
        pass

    def load(self, data, offset):
        self.version = struct.unpack('<I', bytes(data[offset:offset + 4]))[0]
        offset += 4

        if version == 0:
            params = OptimizationParamsV0()
            offset += params.load(data, offset)
            self.past = params.past
            self.lbfgs_m = params.lbfgs_m
            self.nx = struct.unpack('<Q', bytes(data[offset:offset + 8]))[0];  offset += 8
            self.iter = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
            self.just_initialized = bool(struct.unpack('<i', bytes(data[offset:offset + 4]))[0]);  offset += 4
            self.type = params.type

            self.adam_m  = Tensor('f', [self.nx])
            self.adam_v  = Tensor('f', [self.nx])
            self.adam_pf = Tensor('f', [self.past])

            self.lbfgs_x    = Tensor('f', [self.nx])
            self.lbfgs_xp   = Tensor('f', [self.nx])
            self.lbfgs_g    = Tensor('f', [self.nx])
            self.lbfgs_gp   = Tensor('f', [self.nx])
            self.lbfgs_d    = Tensor('f', [self.nx])
            self.lbfgs_pf   = Tensor('f', [self.past])
            self.lbfgs_lmal = Tensor('f', [self.lbfgs_m])
            self.lbfgs_lmys = Tensor('f', [self.lbfgs_m])
            self.lbfgs_lms  = Tensor('f', [self.nx, self.lbfgs_m])
            self.lbfgs_lmy  = Tensor('f', [self.nx, self.lbfgs_m])

            if self.type == 0:
                # these tensors are stored, but we don't need their data
                x  = Tensor('f', [self.nx])
                g  = Tensor('f', [self.nx])
                g2 = Tensor('f', [self.nx])
                mh = Tensor('f', [self.nx])
                mv = Tensor('f', [self.nx])

                offset += x.load(data, offset)
                offset += g.load(data, offset)
                offset += g2.load(data, offset)
                offset += self.adam_m.load(data, offset)
                offset += self.adam_v.load(data, offset)
                offset += mh.load(data, offset)
                offset += vh.load(data, offset)
                if self.past > 0:
                    offset += self.adam_pf.load(data, offset)

                self.adam_fx_best          = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.adam_fx_prev          = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.adam_n_no_improvement = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4

            elif self.type == 1:
                offset += self.lbfgs_x.load(data, offset)
                offset += self.lbfgs_xp.load(data, offset)
                offset += self.lbfgs_g.load(data, offset)
                offset += self.lbfgs_gp.load(data, offset)
                offset += self.lbfgs_d.load(data, offset)
                if self.past > 0:
                    offset += self.lbfgs_pf.load(data, offset)
                offset += self.lbfgs_lmal.load(data, offset)
                offset += self.lbfgs_lmys.load(data, offset)
                offset += self.lbfgs_lms.load(data, offset)
                offset += self.lbfgs_lmy.load(data, offset)

                self.lbfgs_fx_best          = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.lbfgs_step             = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.lbfgs_j                = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.lbfgs_k                = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.lbfgs_end              = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.lbfgs_n_no_improvement = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4

            else:
                raise ValueError('Unknown optimizer type')


        elif version == 1:
            self.past    = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
            self.lbfgs_m = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
            self.nx      = struct.unpack('<Q', bytes(data[offset:offset + 8]))[0];  offset += 8
            self.iter    = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
            self.just_initialized = bool(struct.unpack('<i', bytes(data[offset:offset + 4]))[0]);  offset += 4

            self.adam_m  = Tensor('f', [self.nx])
            self.adam_w  = Tensor('f', [self.nx])
            self.adam_pf = Tensor('f', [self.past])

            self.lbfgs_x    = Tensor('f', [self.nx])
            self.lbfgs_xp   = Tensor('f', [self.nx])
            self.lbfgs_g    = Tensor('f', [self.nx])
            self.lbfgs_gp   = Tensor('f', [self.nx])
            self.lbfgs_d    = Tensor('f', [self.nx])
            self.lbfgs_pf   = Tensor('f', [self.past])
            self.lbfgs_lmal = Tensor('f', [self.lbfgs_m])
            self.lbfgs_lmys = Tensor('f', [self.lbfgs_m])
            self.lbfgs_lms  = Tensor('f', [self.nx, self.lbfgs_m])
            self.lbfgs_lmy  = Tensor('f', [self.nx, self.lbfgs_m])

            # forgot to save type in version 1:
            # guess self.type from number of remaining bytes
            size_type_0 = 12 + sum([t.max_storage_size() for t in 
                                    [self.adam_m, self.adam_w] 
                                    +[self.adam_pf] if self.past > 0 else []])
            size_type_1 = 24 + sum([t.max_storage_size() for t in 
                                    [self.lbfgs_x, self.lbfgs_xp, self.lbfgs_g,
                                     self.lbfgs_gp, self.lbfgs_d, self.lbfgs_pf,
                                     self.lbfgs_lmal, self.lbfgs_lmys,
                                     self.lbfgs_lms, self.lbfgs_lmy]
                                     +[self.lbfgs_pf] if self.past > 0 else []])
            # due to alignment padding the size might not by exact
            # but the difference in size for both types is significant, 
            # so we can just use whichever is closest
            remaining = len(data) - offset
            if abs(remaining - size_type_0) < abs(remaining - size_type_1):
                self.type = 0
            else:
                self.type = 1

            if self.type == 0:
                offset += self.adam_m.load(data, offset)
                offset += self.adam_w.load(data, offset)
                if self.past > 0:
                    offset += self.adam_pf.load(data,offset)

                self.adam_fx_best          = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.adam_fx_prev          = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.adam_n_no_improvement = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4

            elif self.type == 1:

                offset += self.lbfgs_x.load(data, offset)
                offset += self.lbfgs_xp.load(data, offset)
                offset += self.lbfgs_g.load(data, offset)
                offset += self.lbfgs_gp.load(data, offset)
                offset += self.lbfgs_d.load(data, offset)
                if self.past > 0:
                    offset += self.lbfgs_pf.load(data, offset)
                offset += self.lbfgs_lmal.load(data, offset)
                offset += self.lbfgs_lmys.load(data, offset)
                offset += self.lbfgs_lms.load(data, offset)
                offset += self.lbfgs_lmy.load(data, offset)

                self.lbfgs_fx_best          = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.lbfgs_step             = struct.unpack('<f', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.lbfgs_j                = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.lbfgs_k                = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.lbfgs_end              = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4
                self.lbfgs_n_no_improvement = struct.unpack('<i', bytes(data[offset:offset + 4]))[0];  offset += 4

        else:
            raise ValueError('Invalid version of checkpoint file')

        return offset

    def save_gguf(self, gguf_writer):
        gguf_writer.add_uint32(LLM_KV_OPTIMIZER_FILE_VERSION, 0)
        gguf_writer.add_uint32(LLM_KV_OPTIMIZER_CONVERGENCE_PAST_COUNT, self.past)
        gguf_writer.add_uint32(LLM_KV_OPTIMIZER_PARAMETER_COUNT_LOW, self.nx & 0xffffffff)
        gguf_writer.add_uint32(LLM_KV_OPTIMIZER_PARAMETER_COUNT_HIGH, (self.nx >> 32) & 0xffffffff)
        gguf_writer.add_uint32(LLM_KV_OPTIMIZER_ITERATION_COUNT, self.iter)
        gguf_writer.add_uint32(LLM_KV_OPTIMIZER_JUST_INITIALIZED, self.just_initialized)

        if self.type == 0:
            gguf_writer.add_string(LLM_KV_OPTIMIZER_TYPE, LLM_KV_OPTIMIZER_TYPE_ADAM)
            gguf_writer.add_float32(LLM_KV_OPTIMIZER_ADAM_BEST_LOSS, self.adam_fx_best)
            gguf_writer.add_float32(LLM_KV_OPTIMIZER_ADAM_PREVIOUS_LOSS, self.adam_fx_prev)
            gguf_writer.add_uint32(LLM_KV_OPTIMIZER_ADAM_NO_IMPROVEMENT_COUNT, self.adam_n_no_improvement)

            self.adam_m.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_ADAM_FIRST_MOMENTS)
            self.adam_v.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_ADAM_SECOND_MOMENTS)
            if self.past > 0:
                self.adam_pf.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_ADAM_PAST_LOSS_VALUES)

        elif self.type == 1:
            gguf_writer.add_string(LLM_KV_OPTIMIZER_TYPE, LLM_KV_OPTIMIZER_TYPE_LBFGS)
            gguf_writer.add_uint32(LLM_KV_OPTIMIZER_LBFGS_APPROX_HESSIAN_COUNT, self.lbfgs_m)
            gguf_writer.add_float32(LLM_KV_OPTIMIZER_LBFGS_BEST_LOSS, self.lbfgs_fx_best)
            gguf_writer.add_float32(LLM_KV_OPTIMIZER_LBFGS_LINE_SEARCH_STEP, self.lbfgs_step)
            gguf_writer.add_int32(LLM_KV_OPTIMIZER_LBFGS_LINE_SEARCH_J, self.lbfgs_j)
            gguf_writer.add_int32(LLM_KV_OPTIMIZER_LBFGS_LINE_SEARCH_K, self.lbfgs_k)
            gguf_writer.add_int32(LLM_KV_OPTIMIZER_LBFGS_LINE_SEARCH_END, self.lbfgs_end)
            gguf_writer.add_uint32(LLM_KV_OPTIMIZER_LBFGS_NO_IMPROVEMENT_COUNT, self.lbfgs_n_no_improvement)

            self.lbfgs_x.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_LBFGS_CURRENT_PARAMETERS)
            self.lbfgs_xp.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_LBFGS_PREVIOUS_PARAMETERS)
            self.lbfgs_g.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_LBFGS_CURRENT_GRADIENTS)
            self.lbfgs_gp.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_LBFGS_PREVIOUS_GRADIENTS)
            self.lbfgs_d.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_LBFGS_SEARCH_DIRECTION)
            if self.past > 0:
                self.lbfgs_pf.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_LBFGS_PAST_LOSS_VALUES)
            self.lbfgs_lmal.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_LBFGS_MEMORY_ALPHA)
            self.lbfgs_lmys.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_LBFGS_MEMORY_YS)
            self.lbfgs_lms.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_LBFGS_MEMORY_S)
            self.lbfgs_lmy.save_gguf(gguf_writer, name=LLM_TENSOR_OPTIMIZER_LBFGS_MEMORY_Y)
        else:
            raise ValueError('Unknown optimizer type')

class ModelParams:
    def __init__(self):
        pass

    def load(self, data, offset):
        self.n_vocab = struct.unpack('<I', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.n_embd  = struct.unpack('<I', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.n_mult  = struct.unpack('<I', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.n_head  = struct.unpack('<I', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.n_layer = struct.unpack('<I', bytes(data[offset:offset + 4]))[0];  offset += 4
        self.n_rot   = struct.unpack('<I', bytes(data[offset:offset + 4]))[0];  offset += 4
        return offset

    def get_n_ff(self):
        # struct my_llama_model::get_n_ff in train-text-from-scratch.cpp commit 3b5515bbe0e2224425986ba24f1f5d84aa38dce9
        return ((2*(4*self.n_embd)//3 + self.n_mult - 1)//self.n_mult)*self.n_mult
    
    def save_gguf(self, gguf_writer):
        # self.n_vocab not saved
        gguf_writer.add_embedding_length(self.n_embd)
        gguf_writer.add_head_count(self.n_head)
        gguf_writer.add_block_count(self.n_layer)
        gguf_writer.add_rope_dimension_count(self.n_rot)
        gguf_writer.add_feed_forward_length(self.get_n_ff())

class Layer:
    def __init__(self, params, bid):
        self.bid = bid
        self.att_norm = Tensor('f', [params.n_embd])
        self.wq       = Tensor('f', [params.n_embd, params.n_embd])
        self.wk       = Tensor('f', [params.n_embd, params.n_embd])
        self.wv       = Tensor('f', [params.n_embd, params.n_embd])
        self.wo       = Tensor('f', [params.n_embd, params.n_embd])
        self.ffn_norm = Tensor('f', [params.n_embd])
        self.w1       = Tensor('f', [params.n_embd, params.get_n_ff()])
        self.w2       = Tensor('f', [params.get_n_ff(), params.n_embd])
        self.w3       = Tensor('f', [params.n_embd, params.get_n_ff()])

    def load(self, data, offset):
        offset += self.att_norm.load(data, offset)
        offset += self.wq.load(data, offset)
        offset += self.wk.load(data, offset)
        offset += self.wv.load(data, offset)
        offset += self.wo.load(data, offset)
        offset += self.ffn_norm.load(data, offset)
        offset += self.w1.load(data, offset)
        offset += self.w2.load(data, offset)
        offset += self.w3.load(data, offset)
        return offset

    def save_gguf(self, gguf_writer):
        self.att_norm.save_gguf(gguf_writer, name=gguf.MODEL_TENSOR_NAMES[gguf_writer.arch][gguf.MODEL_TENSOR.ATTN_NORM].format(bid=self.bid))
        self.wq.save_gguf(gguf_writer, name=gguf.MODEL_TENSOR_NAMES[gguf_writer.arch][gguf.MODEL_TENSOR.ATTN_Q].format(bid=self.bid))
        self.wk.save_gguf(gguf_writer, name=gguf.MODEL_TENSOR_NAMES[gguf_writer.arch][gguf.MODEL_TENSOR.ATTN_K].format(bid=self.bid))
        self.wv.save_gguf(gguf_writer, name=gguf.MODEL_TENSOR_NAMES[gguf_writer.arch][gguf.MODEL_TENSOR.ATTN_V].format(bid=self.bid))
        self.wo.save_gguf(gguf_writer, name=gguf.MODEL_TENSOR_NAMES[gguf_writer.arch][gguf.MODEL_TENSOR.ATTN_OUT].format(bid=self.bid))
        self.ffn_norm.save_gguf(gguf_writer, name=gguf.MODEL_TENSOR_NAMES[gguf_writer.arch][gguf.MODEL_TENSOR.FFN_NORM].format(bid=self.bid))
        self.w1.save_gguf(gguf_writer, name=gguf.MODEL_TENSOR_NAMES[gguf_writer.arch][gguf.MODEL_TENSOR.FFN_GATE].format(bid=self.bid))
        self.w2.save_gguf(gguf_writer, name=gguf.MODEL_TENSOR_NAMES[gguf_writer.arch][gguf.MODEL_TENSOR.FFN_DOWN].format(bid=self.bid))
        self.w3.save_gguf(gguf_writer, name=gguf.MODEL_TENSOR_NAMES[gguf_writer.arch][gguf.MODEL_TENSOR.FFN_UP].format(bid=self.bid))

class Model:
    def __init__(self):
        self.params = ModelParams()
        self.layers = []

    def load(self, data, offset):
        offset = self.params.load(data, offset)

        self.tok_embd = Tensor('f', [self.params.n_embd, self.params.n_vocab])
        self.norm     = Tensor('f', [self.params.n_embd])
        self.output   = Tensor('f', [self.params.n_embd, self.params.n_vocab])

        offset += self.tok_embd.load(data, offset)
        offset += self.norm.load(data, offset)
        offset += self.output.load(data, offset)

        self.layers.clear()
        for bid in range(self.n_layer):
            layer = Layer(self.params, bid)
            offset += layer.load(data, offset)
            self.layers.append(layer)

        return offset

    def save_gguf(self, gguf_writer):
        self.params.save_gguf(gguf_writer)

        self.tok_embd.save_gguf(gguf_writer, name=gguf.MODEL_TENSOR_NAMES[gguf_writer.arch][gguf.MODEL_TENSOR.TOKEN_EMBD])
        self.norm.save_gguf(gguf_writer, name=gguf.MODEL_TENSOR_NAMES[gguf_writer.arch][gguf.MODEL_TENSOR.OUTPUT_NORM])
        self.output.save_gguf(gguf_writer, name=gguf.MODEL_TENSOR_NAMES[gguf_writer.arch][gguf.MODEL_TENSOR.OUTPUT])

        for layer in self.layers:
            layer.save_gguf(gguf_writer)

class Checkpoint:
    def __init__(self):
        self.model = Model()
        self.opt_ctx = OptimizationContext()

    def load(self, data, offset):
        magic   = bytes(data[offset:offset + 4]); offset += 4
        if magic != b'ggcp':
            raise ValueError(f"File header magic indicates, that this is no checkpoint file. Expected 'ggcp', Got '{str(magic)}'")

        self.version = struct.unpack('<I', bytes(data[offset:offset + 4]))[0]; offset += 4
        if version != 0:
            raise ValueError('Invalid version of checkpoint file')

        self.train_its     = struct.unpack('<I', bytes(data[offset:offset + 4]))[0]; offset += 4
        self.train_samples = struct.unpack('<I', bytes(data[offset:offset + 4]))[0]; offset += 4
        self.train_tokens  = struct.unpack('<I', bytes(data[offset:offset + 4]))[0]; offset += 4
        
        offset += self.model.load(data, offset)
        offset += self.opt_ctx.load(data, offset)

        return offset

    def save_gguf(self, gguf_writer):
        gguf_writer.add_uint32(LLM_KV_TRAINING_FILE_VERSION,    0)
        gguf_writer.add_uint32(LLM_KV_TRAINING_ITERATION_COUNT, self.train_its)
        gguf_writer.add_uint32(LLM_KV_TRAINING_SAMPLE_COUNT,    self.train_samples)
        gguf_writer.add_uint32(LLM_KV_TRAINING_TOKEN_COUNT,     self.train_tokens)
        self.model.save_gguf(gguf_writer)
        self.opt_ctx.save_gguf(gguf_writer)

def handle_args():
    parser = argparse.ArgumentParser(description = 'Convert train-text-from-scratch checkpoints to GGUF')
    parser.add_argument('--input',  '-i', type = Path, help = 'Input train checkpoint filename')
    parser.add_argument('--output', '-o', type = Path, help ='Output GGUF filename')
    return parser.parse_args()

def main():
    cfg = handle_args()
    data = np.memmap(cfg.input, mode = 'r')
    chk = Checkpoint()
    offset = 0
    offset = chk.load(data, offset)
    # we should have read all available data
    assert(offset == len(data))

    gguf_writer = gguf.GGUFWriter(self.cfg.output, gguf.MODEL_ARCH_NAMES[gguf.MODEL_ARCH.LLAMA], use_temp_file = False)
    chk.save_gguf(gguf_writer)
    print("    gguf: write header")
    gguf_writer.write_header_to_file()
    print("    gguf: write metadata")
    gguf_writer.write_kv_data_to_file()
    print("    gguf: write tensors")
    gguf_writer.write_tensors_to_file()
    gguf_writer.close()

if __name__ == '__main__':
    main()