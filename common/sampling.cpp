#include "sampling.h"

#include "common.h"

struct gpt_sampler {
    gpt_sampler_params params;

    struct llama_constraint * bias;
    struct llama_constraint * pnlt;
    struct llama_constraint * grmr;

    struct llama_sampler * smpl;
};

std::string gpt_sampler_params::print() const {
    char result[1024];

    snprintf(result, sizeof(result),
            "\trepeat_last_n = %d, repeat_penalty = %.3f, frequency_penalty = %.3f, presence_penalty = %.3f\n"
            "\ttop_k = %d, tfs_z = %.3f, top_p = %.3f, min_p = %.3f, typical_p = %.3f, temp = %.3f\n"
            "\tmirostat = %d, mirostat_lr = %.3f, mirostat_ent = %.3f",
            penalty_last_n, penalty_repeat, penalty_freq, penalty_present,
            top_k, tfs_z, top_p, min_p, typ_p, temp,
            mirostat, mirostat_eta, mirostat_tau);

    return std::string(result);
}

std::string gpt_sampler_print(const struct gpt_sampler * gsmpl) {
    std::string result = "\tlogits";

    for (int i = 0; i < llama_sampler_n_constraints(gsmpl->smpl); i++) {
        const auto * cnstr = llama_sampler_constraint_get(gsmpl->smpl, i);
        result += " -> " + std::string(cnstr->iface->name(cnstr)) + " ";
    }

    return result;
}

struct gpt_sampler * gpt_sampler_init(const struct llama_model * model, const struct gpt_sampler_params & params) {
    llama_sampler_params lparams = llama_sampler_default_params();

    lparams.seed   = params.seed;
    lparams.n_prev = params.n_prev;
    lparams.type   = params.temp <= 0.0f ? LLAMA_SAMPLER_TYPE_GREEDY : LLAMA_SAMPLER_TYPE_DIST;

    auto * result = new gpt_sampler {
        /* .params = */ params,
        /* .bias   = */ llama_constraint_init_logit_bias(
            model,
            params.logit_bias.size(),
            params.logit_bias.data()),
        /* .pnlt   = */ llama_constraint_init_penalties(
            model,
            params.penalty_last_n,
            params.penalty_repeat,
            params.penalty_freq,
            params.penalty_present,
            params.penalize_nl,
            params.ignore_eos),
        /* .grmr   = */ llama_constraint_init_grammar(model, params.grammar.c_str(), "root"),
        /* .smpl   = */ llama_sampler_init(model, lparams)
    };

    if (params.temp > 0.0f) {
        if (params.mirostat == 0) {
            for (const auto & cnstr : params.constraints) {
                switch (cnstr) {
                    case GPT_CONSTRAINT_TYPE_TOP_K:
                        llama_sampler_constraint_add(result->smpl, llama_constraint_init_top_k    (params.top_k, params.min_keep));
                        break;
                    case GPT_CONSTRAINT_TYPE_TOP_P:
                        llama_sampler_constraint_add(result->smpl, llama_constraint_init_top_p    (params.top_p, params.min_keep));
                        break;
                    case GPT_CONSTRAINT_TYPE_MIN_P:
                        llama_sampler_constraint_add(result->smpl, llama_constraint_init_min_p    (params.min_p, params.min_keep));
                        break;
                    case GPT_CONSTRAINT_TYPE_TFS_Z:
                        llama_sampler_constraint_add(result->smpl, llama_constraint_init_tail_free(params.tfs_z, params.min_keep));
                        break;
                    case GPT_CONSTRAINT_TYPE_TYPICAL_P:
                        llama_sampler_constraint_add(result->smpl, llama_constraint_init_typical  (params.typ_p, params.min_keep));
                        break;
                    case GPT_CONSTRAINT_TYPE_TEMPERATURE:
                        llama_sampler_constraint_add(result->smpl, llama_constraint_init_temp_ext (params.temp, params.dynatemp_range, params.dynatemp_exponent));
                        break;
                    default:
                        GGML_ASSERT(false && "unknown constraint type");
                }
            }
        } else if (params.mirostat == 1) {
            llama_sampler_constraint_add(result->smpl, llama_constraint_init_temp(params.temp));
            llama_sampler_constraint_add(result->smpl, llama_constraint_init_mirostat(model, params.mirostat_tau, params.mirostat_eta));
        } else if (params.mirostat == 2) {
            llama_sampler_constraint_add(result->smpl, llama_constraint_init_temp(params.temp));
            llama_sampler_constraint_add(result->smpl, llama_constraint_init_mirostat_v2(params.mirostat_tau, params.mirostat_eta));
        } else {
            GGML_ASSERT(false && "unknown mirostat version");
        }
    }

    return result;
}

void gpt_sampler_free(struct gpt_sampler * gsmpl) {
    if (gsmpl) {
        llama_constraint_free(gsmpl->bias);
        llama_constraint_free(gsmpl->pnlt);
        llama_constraint_free(gsmpl->grmr);

        llama_sampler_free(gsmpl->smpl);

        delete gsmpl;
    }
}

struct gpt_sampler * gpt_sampler_cp(gpt_sampler * gsmpl) {
    return new gpt_sampler {
        /* .params = */ gsmpl->params,
        /* .bias   = */ llama_constraint_cp(gsmpl->bias),
        /* .pnlt   = */ llama_constraint_cp(gsmpl->pnlt),
        /* .grmr   = */ llama_constraint_cp(gsmpl->grmr),
        /* .smpl   = */ llama_sampler_cp   (gsmpl->smpl)
    };
}

void gpt_sampler_accept(struct gpt_sampler * gsmpl, llama_token token, bool apply_grammar) {
    if (apply_grammar) {
        llama_constraint_accept(gsmpl->grmr, token);
    }

    llama_sampler_accept(gsmpl->smpl, token);
}

void gpt_sampler_reset(struct gpt_sampler * gsmpl) {
    llama_constraint_reset(gsmpl->grmr);

    llama_sampler_reset(gsmpl->smpl);
}

void gpt_sampler_set_logits(struct gpt_sampler * gsmpl, const float * logits) {
    llama_sampler_set_logits(gsmpl->smpl, logits);
}

llama_token_data_array * gpt_sampler_get_candidates(struct gpt_sampler * gsmpl) {
    return llama_sampler_get_candidates(gsmpl->smpl);
}

llama_token gpt_sampler_last(const struct gpt_sampler * gsmpl) {
    return llama_sampler_last(gsmpl->smpl);
}

void gpt_print_timings(struct llama_context * ctx, struct gpt_sampler * gsmpl) {
    llama_print_timings(ctx, gsmpl ? gsmpl->smpl : nullptr);
}

llama_token gpt_sampler_sample(struct gpt_sampler * gsmpl, struct llama_token_data_array * cur_p) {
    return llama_sampler_sample(gsmpl->smpl, cur_p);
}

llama_token gpt_sampler_sample(struct gpt_sampler * gsmpl, struct llama_context * ctx, int idx, bool grammar_first) {
    auto & bias = gsmpl->bias;
    auto & pnlt = gsmpl->pnlt;
    auto & grmr = gsmpl->grmr;
    auto & smpl = gsmpl->smpl;

    const auto * logits = llama_get_logits_ith(ctx, idx);

    llama_sampler_set_logits(smpl, logits);

    auto * cur_p = llama_sampler_get_candidates(smpl);

    llama_constraint_apply(bias, cur_p);
    llama_constraint_apply(pnlt, cur_p);

    if (grammar_first) {
        llama_constraint_apply(grmr, cur_p);
    }

    llama_sampler_apply(smpl, cur_p);

    const llama_token id = llama_sampler_sample(smpl, cur_p);

    if (grammar_first) {
        return id;
    }

    // check if it the sampled token fits the grammar
    {
        llama_token_data       single_token_data       = { id, 1.0f, 0.0f };
        llama_token_data_array single_token_data_array = { &single_token_data, 1, false };

        llama_constraint_apply(grmr, &single_token_data_array);

        // check if the token is valid according to the grammar by seeing if its logit has been set to -INFINITY
        const bool is_valid = single_token_data_array.data[0].logit != -INFINITY;
        if (is_valid) {
            return id;
        }
    }

    // if the token is not valid, sample again, first apply the grammar constraints and then sample
    llama_sampler_set_logits(smpl, logits);

    llama_constraint_apply(bias, cur_p);
    llama_constraint_apply(pnlt, cur_p);
    llama_constraint_apply(grmr, cur_p);

    llama_sampler_apply(smpl, cur_p);

    return llama_sampler_sample(smpl, cur_p);
}

void gpt_sampler_apply_grammar(struct gpt_sampler * gsmpl, llama_token_data_array * cur_p) {
    GGML_ASSERT(cur_p != nullptr);

    llama_constraint_apply(gsmpl->grmr, cur_p);
}

std::string gpt_sampler_prev_str(gpt_sampler * gsmpl, llama_context * ctx_main, int n) {
    auto & smpl = gsmpl->smpl;

    n = std::min(n, llama_sampler_n_prev(smpl));

    if (n <= 0) {
        return "";
    }

    std::string result;
    result.reserve(8*n); // 8 is the average length of a token [citation needed], TODO: compute this from the vocab

    for (int i = n - 1; i >= 0; i--) {
        const llama_token id = llama_sampler_prev(smpl, i);

        GGML_ASSERT(id != LLAMA_TOKEN_NULL && "null token in the sampling history - should not happen");

        result += llama_token_to_piece(ctx_main, id);
    }

    return result;
}

char gpt_constraint_type_to_chr(enum gpt_constraint_type cnstr) {
    switch (cnstr) {
        case GPT_CONSTRAINT_TYPE_TOP_K:       return 'k';
        case GPT_CONSTRAINT_TYPE_TFS_Z:       return 'f';
        case GPT_CONSTRAINT_TYPE_TYPICAL_P:   return 'y';
        case GPT_CONSTRAINT_TYPE_TOP_P:       return 'p';
        case GPT_CONSTRAINT_TYPE_MIN_P:       return 'm';
        case GPT_CONSTRAINT_TYPE_TEMPERATURE: return 't';
        default : return '?';
    }
}

std::string gpt_constraint_type_to_str(enum gpt_constraint_type cnstr) {
    switch (cnstr) {
        case GPT_CONSTRAINT_TYPE_TOP_K:       return "top_k";
        case GPT_CONSTRAINT_TYPE_TFS_Z:       return "tfs_z";
        case GPT_CONSTRAINT_TYPE_TYPICAL_P:   return "typ_p";
        case GPT_CONSTRAINT_TYPE_TOP_P:       return "top_p";
        case GPT_CONSTRAINT_TYPE_MIN_P:       return "min_p";
        case GPT_CONSTRAINT_TYPE_TEMPERATURE: return "temperature";
        default : return "";
    }
}

std::vector<gpt_constraint_type> gpt_constraint_types_from_names(const std::vector<std::string> & names, bool allow_alt_names) {
    std::unordered_map<std::string, gpt_constraint_type> constraint_canonical_name_map {
        { "top_k",       GPT_CONSTRAINT_TYPE_TOP_K },
        { "top_p",       GPT_CONSTRAINT_TYPE_TOP_P },
        { "typ_p",       GPT_CONSTRAINT_TYPE_TYPICAL_P },
        { "min_p",       GPT_CONSTRAINT_TYPE_MIN_P },
        { "tfs_z",       GPT_CONSTRAINT_TYPE_TFS_Z },
        { "temperature", GPT_CONSTRAINT_TYPE_TEMPERATURE },
    };

    // since constraints names are written multiple ways
    // make it ready for both system names and input names
    std::unordered_map<std::string, gpt_constraint_type> constraint_alt_name_map {
        { "top-k",       GPT_CONSTRAINT_TYPE_TOP_K },
        { "top-p",       GPT_CONSTRAINT_TYPE_TOP_P },
        { "nucleus",     GPT_CONSTRAINT_TYPE_TOP_P },
        { "typical-p",   GPT_CONSTRAINT_TYPE_TYPICAL_P },
        { "typical",     GPT_CONSTRAINT_TYPE_TYPICAL_P },
        { "typ-p",       GPT_CONSTRAINT_TYPE_TYPICAL_P },
        { "typ",         GPT_CONSTRAINT_TYPE_TYPICAL_P },
        { "min-p",       GPT_CONSTRAINT_TYPE_MIN_P },
        { "tfs-z",       GPT_CONSTRAINT_TYPE_TFS_Z },
        { "tfs",         GPT_CONSTRAINT_TYPE_TFS_Z },
        { "temp",        GPT_CONSTRAINT_TYPE_TEMPERATURE },
    };

    std::vector<gpt_constraint_type> constraints;
    constraints.reserve(names.size());

    for (const auto & name : names) {
        auto constraint = constraint_canonical_name_map.find(name);
        if (constraint != constraint_canonical_name_map.end()) {
            constraints.push_back(constraint->second);
        } else {
            if (allow_alt_names) {
                constraint = constraint_alt_name_map.find(name);
                if (constraint != constraint_alt_name_map.end()) {
                    constraints.push_back(constraint->second);
                }
            }
        }
    }

    return constraints;
}

std::vector<gpt_constraint_type> gpt_constraint_types_from_chars(const std::string & chars) {
    std::unordered_map<char, gpt_constraint_type> constraint_name_map {
        { gpt_constraint_type_to_chr(GPT_CONSTRAINT_TYPE_TOP_K),       GPT_CONSTRAINT_TYPE_TOP_K },
        { gpt_constraint_type_to_chr(GPT_CONSTRAINT_TYPE_TFS_Z),       GPT_CONSTRAINT_TYPE_TFS_Z },
        { gpt_constraint_type_to_chr(GPT_CONSTRAINT_TYPE_TYPICAL_P),   GPT_CONSTRAINT_TYPE_TYPICAL_P },
        { gpt_constraint_type_to_chr(GPT_CONSTRAINT_TYPE_TOP_P),       GPT_CONSTRAINT_TYPE_TOP_P },
        { gpt_constraint_type_to_chr(GPT_CONSTRAINT_TYPE_MIN_P),       GPT_CONSTRAINT_TYPE_MIN_P },
        { gpt_constraint_type_to_chr(GPT_CONSTRAINT_TYPE_TEMPERATURE), GPT_CONSTRAINT_TYPE_TEMPERATURE }
    };

    std::vector<gpt_constraint_type> constraints;
    constraints.reserve(chars.size());

    for (const auto & c : chars) {
        const auto constraint = constraint_name_map.find(c);
        if (constraint != constraint_name_map.end()) {
            constraints.push_back(constraint->second);
        }
    }

    return constraints;
}
