#include "llm.h"

// https://github.com/ggerganov/ggml/blob/master/examples/replit/main.cpp

// no defaults for now
struct replit_hparams {
  int32_t d_model = 0;
  int32_t max_seq_len = 0;
  int32_t n_heads = 0;
  int32_t n_layers = 0;
  int32_t n_vocab = 0;
  int32_t ftype = 0;
  int32_t n_ctx = 2048;
};

using piece_t = std::pair<std::size_t, float>;
using piece_map_t = std::unordered_map<std::string, piece_t>;

struct replit_tokenizer {
  gpt_vocab raw_vocab;
  piece_map_t piece_map;
  std::vector<std::string> vocab;
};

struct replit_layer {
  // pre normalization
  struct ggml_tensor *norm_1_weight;

  // attention
  struct ggml_tensor *c_attn_wqkv_weight;
  struct ggml_tensor *c_attn_out_proj_weight;

  // post normalization
  struct ggml_tensor *norm_2_weight;

  // ff
  struct ggml_tensor *ffn_up_proj;
  struct ggml_tensor *ffn_down_proj;
};

struct replit_model {
  replit_hparams hparams;

  struct ggml_tensor *wte_weight;     // position embedding
  struct ggml_tensor *norm_f_weight;  // language model head

  std::vector<replit_layer> layers;

  // key + value memory
  struct ggml_tensor *memory_k;
  struct ggml_tensor *memory_v;

  struct ggml_context *ctx;
  std::map<std::string, struct ggml_tensor *> tensors;
};

std::pair<std::vector<gpt_vocab::id>, float> encode_word(
    const std::string &word, const piece_map_t &model) {
  std::vector<int> best_segmentations_starts(word.length() + 1, -1);
  best_segmentations_starts[0] = 0;

  std::vector<float> best_segmentations_scores(
      word.length() + 1, -std::numeric_limits<float>::infinity());
  best_segmentations_scores[0] = 1.0;

  for (int start_idx = 0; start_idx < (int)word.length(); ++start_idx) {
    float best_score_at_start = best_segmentations_scores[start_idx];
    for (int end_idx = start_idx + 1; end_idx <= (int)word.length();
         ++end_idx) {
      std::string token = word.substr(start_idx, end_idx - start_idx);
      if (model.count(token) &&
          best_score_at_start != -std::numeric_limits<float>::infinity()) {
        float token_score = model.at(token).second;
        float score = token_score + best_score_at_start;
        if (best_segmentations_scores[end_idx] ==
                -std::numeric_limits<float>::infinity() ||
            best_segmentations_scores[end_idx] > score) {
          best_segmentations_starts[end_idx] = start_idx;
          best_segmentations_scores[end_idx] = score;
        }
      }
    }
  }

  if (best_segmentations_scores.back() ==
      -std::numeric_limits<float>::infinity()) {
    return std::make_pair(std::vector<gpt_vocab::id>{0}, 0.0f);
  }

  float score = best_segmentations_scores.back();
  int start = best_segmentations_starts.back();
  int end = word.length();
  std::vector<gpt_vocab::id> tokens;
  while (start != 0) {
    const auto token_id = model.at(word.substr(start, end - start)).first;
    tokens.insert(tokens.begin(), token_id);
    int next_start = best_segmentations_starts[start];
    end = start;
    start = next_start;
  }
  const auto token_id = model.at(word.substr(start, end - start)).first;
  tokens.insert(tokens.begin(), token_id);
  return std::make_pair(tokens, score);
}

bool replit_tokenizer_load(replit_tokenizer &tokenizer, std::istream &fin,
                           int max_vocab_size) {
  std::string word;
  std::vector<char> buf(128);

  for (std::size_t i = 0; (int)i < max_vocab_size; i++) {
    uint32_t len;
    fin.read((char *)&len, sizeof(len));

    buf.resize(len);
    fin.read((char *)buf.data(), len);
    word.assign(buf.data(), len);

    float score;
    fin.read((char *)&score, sizeof(score));

    tokenizer.piece_map[word] = std::make_pair(i, -score);
    tokenizer.raw_vocab.id_to_token[i] = word;
  }

  return true;
}

std::string replace_all(const std::string &str,     // where to work
                        const std::string &find,    // substitute 'find'
                        const std::string &replace  //      by 'replace'
) {
  using namespace std;
  string result;
  size_t find_len = find.size();
  size_t pos, from = 0;
  while (string::npos != (pos = str.find(find, from))) {
    result.append(str, from, pos - from);
    result.append(replace);
    from = pos + find_len;
  }
  result.append(str, from, string::npos);
  return result;
}

std::string ws_symbol = "\342\226\201";
std::vector<gpt_vocab::id> replit_tokenizer_tokenize(
    const replit_tokenizer &tokenizer, const std::string &text) {
  std::vector<gpt_vocab::id> tokens;
  auto normalized_text = replace_all(text, " ", ws_symbol);
  auto tokenized = encode_word(normalized_text, tokenizer.piece_map);

  return tokenized.first;
}

// load the model's weights from a file
bool replit_model_load(const std::string &fname, replit_model &model,
                       replit_tokenizer &tokenizer) {
  auto fin = std::ifstream(fname, std::ios::binary);
  if (!fin) {
    fprintf(stderr, "%s: failed to open '%s'\n", __func__, fname.c_str());
    return false;
  }

  // verify magic
  {
    uint32_t magic;
    fin.read((char *)&magic, sizeof(magic));
    if (magic != GGML_FILE_MAGIC) {
      fprintf(stderr, "%s: invalid model file '%s' (bad magic)\n", __func__,
              fname.c_str());
      return false;
    }
  }

  // load hparams
  {
    auto &hparams = model.hparams;

    fin.read((char *)&hparams.d_model, sizeof(hparams.d_model));
    fin.read((char *)&hparams.max_seq_len, sizeof(hparams.max_seq_len));
    fin.read((char *)&hparams.n_heads, sizeof(hparams.n_heads));
    fin.read((char *)&hparams.n_layers, sizeof(hparams.n_layers));
    fin.read((char *)&hparams.n_vocab, sizeof(hparams.n_vocab));
    fin.read((char *)&hparams.ftype, sizeof(hparams.ftype));

    hparams.n_ctx = std::min(hparams.max_seq_len, hparams.n_ctx);

    const int32_t qntvr = hparams.ftype / GGML_QNT_VERSION_FACTOR;

    hparams.ftype %= GGML_QNT_VERSION_FACTOR;
  }

  // load vocab
  replit_tokenizer_load(tokenizer, fin, model.hparams.n_vocab);

  // for the big tensors, we have the option to store the data in 16-bit
  // floats or quantized in order to save memory and also to speed up the
  // computation
  ggml_type wtype = ggml_ftype_to_ggml_type((ggml_ftype)(model.hparams.ftype));
  if (wtype == GGML_TYPE_COUNT) {
    fprintf(stderr, "%s: invalid model file '%s' (bad ftype value %d)\n",
            __func__, fname.c_str(), model.hparams.ftype);
    return false;
  }

  auto &ctx = model.ctx;

  size_t ctx_size = 0;

  {
    const auto &hparams = model.hparams;

    const int n_embd = hparams.d_model;
    const int n_layer = hparams.n_layers;
    const int n_ctx = hparams.max_seq_len;
    const int n_vocab = hparams.n_vocab;

    ctx_size += n_embd * n_vocab * ggml_type_sizef(wtype);  // wte_weight
    ctx_size += n_embd * ggml_type_sizef(GGML_TYPE_F32);    // ln_f_weight

    ctx_size +=
        n_layer * (n_embd * ggml_type_sizef(GGML_TYPE_F32));  // ln_1_weight
    ctx_size += n_layer * (3 * n_embd * n_embd *
                           ggml_type_sizef(wtype));  // attn_Wqkv_weight
    ctx_size += n_layer * (n_embd * n_embd *
                           ggml_type_sizef(wtype));  // attn_out_proj_weight
    ctx_size +=
        n_layer * (n_embd * ggml_type_sizef(GGML_TYPE_F32));  // ln_2_weight
    ctx_size += n_layer * (4 * n_embd * n_embd *
                           ggml_type_sizef(wtype));  // mlp_mlp_up_weight
    ctx_size += n_layer * (n_embd * n_embd * 4 *
                           ggml_type_sizef(wtype));  // mlp_mlp_down_weight

    ctx_size +=
        n_ctx * n_layer * n_embd * ggml_type_sizef(GGML_TYPE_F16);  // memory_k
    ctx_size +=
        n_ctx * n_layer * n_embd * ggml_type_sizef(GGML_TYPE_F16);  // memory_v

    ctx_size += (1 + 6 * n_layer) * 512;  // object overhead
  }

  // create the ggml context
  {
    struct ggml_init_params params = {
        /*.mem_size   =*/ctx_size,
        /*.mem_buffer =*/NULL,
        /*.no_alloc   =*/false,
    };

    model.ctx = ggml_init(params);
    if (!model.ctx) {
      fprintf(stderr, "%s: ggml_init() failed\n", __func__);
      return false;
    }
  }

  // prepare memory for the weights
  {
    const auto &hparams = model.hparams;

    const size_t n_embd = hparams.d_model;
    const size_t n_layer = hparams.n_layers;
    const size_t n_vocab = hparams.n_vocab;

    model.layers.resize(n_layer);

    model.wte_weight = ggml_new_tensor_2d(ctx, wtype, n_embd, n_vocab);
    model.norm_f_weight = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);

    // map by name
    model.tensors["transformer.wte.weight"] = model.wte_weight;
    model.tensors["transformer.norm_f.weight"] = model.norm_f_weight;

    for (int i = 0; i < (int)n_layer; ++i) {
      auto &layer = model.layers[i];

      layer.norm_1_weight = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);
      layer.c_attn_wqkv_weight =
          ggml_new_tensor_2d(ctx, wtype, n_embd, 3 * n_embd);
      layer.c_attn_out_proj_weight =
          ggml_new_tensor_2d(ctx, wtype, n_embd, n_embd);
      layer.norm_2_weight = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);
      layer.ffn_up_proj = ggml_new_tensor_2d(ctx, wtype, n_embd, 4 * n_embd);
      layer.ffn_down_proj = ggml_new_tensor_2d(ctx, wtype, 4 * n_embd, n_embd);

      // map by name
      model.tensors["transformer.blocks." + std::to_string(i) +
                    ".norm_1.weight"] = layer.norm_1_weight;
      model.tensors["transformer.blocks." + std::to_string(i) +
                    ".attn.Wqkv.weight"] = layer.c_attn_wqkv_weight;
      model.tensors["transformer.blocks." + std::to_string(i) +
                    ".attn.out_proj.weight"] = layer.c_attn_out_proj_weight;
      model.tensors["transformer.blocks." + std::to_string(i) +
                    ".norm_2.weight"] = layer.norm_2_weight;
      model.tensors["transformer.blocks." + std::to_string(i) +
                    ".ffn.up_proj.weight"] = layer.ffn_up_proj;
      model.tensors["transformer.blocks." + std::to_string(i) +
                    ".ffn.down_proj.weight"] = layer.ffn_down_proj;
    }
  }

  // key + value memory
  {
    const auto &hparams = model.hparams;

    const int n_embd = hparams.d_model;
    const int n_layer = hparams.n_layers;
    const int n_ctx = hparams.max_seq_len;

    const int64_t n_mem = n_layer * n_ctx;
    const int64_t n_elements = n_embd * n_mem;

    model.memory_k = ggml_new_tensor_1d(ctx, GGML_TYPE_F16, n_elements);
    model.memory_v = ggml_new_tensor_1d(ctx, GGML_TYPE_F16, n_elements);

    const size_t memory_size =
        ggml_nbytes(model.memory_k) + ggml_nbytes(model.memory_v);
  }

  // load weights
  {
    while (true) {
      int32_t n_dims;
      int32_t length;
      int32_t ttype;

      fin.read(reinterpret_cast<char *>(&n_dims), sizeof(n_dims));
      fin.read(reinterpret_cast<char *>(&length), sizeof(length));
      fin.read(reinterpret_cast<char *>(&ttype), sizeof(ttype));

      if (fin.eof()) {
        break;
      }

      int32_t nelements = 1;
      int32_t ne[2] = {1, 1};
      for (int i = 0; i < n_dims; ++i) {
        fin.read(reinterpret_cast<char *>(&ne[i]), sizeof(ne[i]));
        nelements *= ne[i];
      }

      std::string name(length, 0);
      fin.read(&name[0], length);

      if (model.tensors.find(name.data()) == model.tensors.end()) {
        fprintf(stderr, "%s: unknown tensor '%s' in model file\n", __func__,
                name.data());
        return false;
      }

      auto tensor = model.tensors[name.data()];
      if (ggml_nelements(tensor) != nelements) {
        fprintf(stderr, "%s: tensor '%s' has wrong size in model file\n",
                __func__, name.data());
        return false;
      }

      if (tensor->ne[0] != ne[0] || tensor->ne[1] != ne[1]) {
        fprintf(stderr,
                "%s: tensor '%s' has wrong shape in model file: got [%5d, "
                "%5d], expected [%5d, %5d]\n",
                __func__, name.data(), (int)tensor->ne[0], (int)tensor->ne[1],
                ne[0], ne[1]);
        return false;
      }

      // for debugging
      if (0) {
        printf("%24s - [%5d, %5d], type = %6s, %6.2f MB, %9zu bytes\n",
               name.data(), ne[0], ne[1], ggml_type_name(ggml_type(ttype)),
               ggml_nbytes(tensor) / 1024.0 / 1024.0, ggml_nbytes(tensor));
      }

      const size_t bpe = ggml_type_size(ggml_type(ttype));

      if ((nelements * bpe) / ggml_blck_size(tensor->type) !=
          ggml_nbytes(tensor)) {
        fprintf(stderr,
                "%s: tensor '%s' has wrong size in model file: got %zu, "
                "expected %zu\n",
                __func__, name.data(), ggml_nbytes(tensor), nelements * bpe);
        return false;
      }

      fin.read(reinterpret_cast<char *>(tensor->data), ggml_nbytes(tensor));
    }
  }

  fin.close();

  return true;
}

// evaluate the transformer
//
//   - model:     the model
//   - n_threads: number of threads to use
//   - n_past:    the context size so far
//   - embd_inp:  the embeddings of the tokens in the context
//   - embd_w:    the predicted logits for the next token
//
bool replit_eval(const replit_model &model, const int n_threads,
                 const int n_past, const std::vector<gpt_vocab::id> &embd_inp,
                 std::vector<float> &embd_w, size_t &mem_per_token) {
  const bool logits_all = false;
  const int N = embd_inp.size();

  const auto &hparams = model.hparams;

  const int n_embd = hparams.d_model;
  const int n_layer = hparams.n_layers;
  const int n_head = hparams.n_heads;
  const int n_vocab = hparams.n_vocab;
  const int n_ctx = hparams.max_seq_len;

  static size_t buf_size = 256u * 1024 * 1024;
  static void *buf = malloc(buf_size);

  if (mem_per_token > 0 && mem_per_token * N > buf_size) {
    const size_t buf_size_new =
        1.1 *
        (mem_per_token * N);  // add 10% to account for ggml object overhead
    // printf("\n%s: reallocating buffer from %zu to %zu bytes\n", __func__,
    // buf_size, buf_size_new);

    // reallocate
    buf_size = buf_size_new;
    buf = realloc(buf, buf_size);
    if (buf == nullptr) {
      fprintf(stderr, "%s: failed to allocate %zu bytes\n", __func__, buf_size);
      return false;
    }
  }

  struct ggml_init_params params = {
      /*.mem_size   =*/buf_size,
      /*.mem_buffer =*/buf,
      /*.no_alloc   =*/false,
  };

  struct ggml_context *ctx0 = ggml_init(params);
  struct ggml_cgraph gf = {};

  struct ggml_tensor *embd = ggml_new_tensor_1d(ctx0, GGML_TYPE_I32, N);
  memcpy(embd->data, embd_inp.data(), N * ggml_element_size(embd));

  struct ggml_tensor *inpL = ggml_get_rows(ctx0, model.wte_weight, embd);

  for (int il = 0; il < n_layer; ++il) {
    struct ggml_tensor *cur;

    // a = self.ln_1(x)
    {
      cur = ggml_norm(ctx0, inpL);

      cur = ggml_mul(
          ctx0, ggml_repeat(ctx0, model.layers[il].norm_1_weight, cur), cur);
    }

    // self-attention
    //  b, _, past_key_value = self.attn(a, past_key_value=past_key_value,
    //  attn_bias=attn_bias, attention_mask=attention_mask,
    //  is_causal=is_causal)
    {
      // compute QKV
      cur = ggml_mul_mat(ctx0, model.layers[il].c_attn_wqkv_weight, cur);

      struct ggml_tensor *Qcur = ggml_view_2d(ctx0, cur, n_embd, N, cur->nb[1],
                                              0 * sizeof(float) * n_embd);
      struct ggml_tensor *Kcur = ggml_view_2d(ctx0, cur, n_embd, N, cur->nb[1],
                                              1 * sizeof(float) * n_embd);
      struct ggml_tensor *Vcur = ggml_view_2d(ctx0, cur, n_embd, N, cur->nb[1],
                                              2 * sizeof(float) * n_embd);

      // store key and value to memory
      {
        struct ggml_tensor *k =
            ggml_view_1d(ctx0, model.memory_k, N * n_embd,
                         (ggml_element_size(model.memory_k) * n_embd) *
                             (il * n_ctx + n_past));
        struct ggml_tensor *v =
            ggml_view_1d(ctx0, model.memory_v, N * n_embd,
                         (ggml_element_size(model.memory_v) * n_embd) *
                             (il * n_ctx + n_past));

        ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Kcur, k));
        ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Vcur, v));
      }

      // Q = Qcur.contiguous().view(n_embd/n_head, n_head, N).permute(0,
      // 2, 1, 3) [64, N, 12]
      struct ggml_tensor *Q =
          ggml_permute(ctx0,
                       ggml_cpy(ctx0, Qcur,
                                ggml_new_tensor_3d(ctx0, GGML_TYPE_F32,
                                                   n_embd / n_head, n_head, N)),
                       0, 2, 1, 3);

      // K = Kmem.view(n_embd/n_head, n_head, n_past + N).permute(0, 2, 1,
      // 3) [64, n_past + N, 12]
      struct ggml_tensor *K = ggml_permute(
          ctx0,
          ggml_reshape_3d(
              ctx0,
              ggml_view_1d(
                  ctx0, model.memory_k, (n_past + N) * n_embd,
                  il * n_ctx * ggml_element_size(model.memory_k) * n_embd),
              n_embd / n_head, n_head, n_past + N),
          0, 2, 1, 3);
      // K * Q
      struct ggml_tensor *KQ = ggml_mul_mat(ctx0, K, Q);

      // KQ_scaled = KQ / sqrt(n_embd/n_head)
      struct ggml_tensor *KQ_scaled = ggml_scale(
          ctx0, KQ, ggml_new_f32(ctx0, 1.0f / sqrt(float(n_embd) / n_head)));

      struct ggml_tensor *KQ_scaled_alibi =
          ggml_alibi(ctx0, KQ_scaled, n_past, n_head, 8.0f);

      // KQ_masked = mask_past(KQ_scaled)
      struct ggml_tensor *KQ_masked =
          ggml_diag_mask_inf(ctx0, KQ_scaled_alibi, n_past);

      // KQ = soft_max(KQ_masked)
      struct ggml_tensor *KQ_soft_max = ggml_soft_max(ctx0, KQ_masked);

      // V_trans = Vmem.view(n_embd/n_head, n_head, n_past + N).permute(1,
      // 2, 0, 3).contiguous() [n_past + N, 64, 12]
      struct ggml_tensor *V_trans = ggml_cpy(
          ctx0,
          ggml_permute(
              ctx0,
              ggml_reshape_3d(
                  ctx0,
                  ggml_view_1d(
                      ctx0, model.memory_v, (n_past + N) * n_embd,
                      il * n_ctx * ggml_element_size(model.memory_v) * n_embd),
                  n_embd / n_head, n_head, n_past + N),
              1, 2, 0, 3),
          ggml_new_tensor_3d(ctx0, model.memory_v->type, n_past + N,
                             n_embd / n_head, n_head));

      // KQV = transpose(V) * KQ_soft_max
      struct ggml_tensor *KQV = ggml_mul_mat(ctx0, V_trans, KQ_soft_max);

      // KQV_merged = KQV.permute(0, 2, 1, 3)
      struct ggml_tensor *KQV_merged = ggml_permute(ctx0, KQV, 0, 2, 1, 3);

      // cur = KQV_merged.contiguous().view(n_embd, N)
      cur = ggml_cpy(ctx0, KQV_merged,
                     ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, n_embd, N));

      // projection
      {
        cur = ggml_mul_mat(ctx0, model.layers[il].c_attn_out_proj_weight, cur);
      }
    }

    inpL = ggml_add(ctx0, inpL, cur);

    // m = self.ln_2(x)
    {
      cur = ggml_norm(ctx0, inpL);

      cur = ggml_mul(
          ctx0, ggml_repeat(ctx0, model.layers[il].norm_2_weight, cur), cur);
    }

    // n = self.mlp(m)
    {
      cur = ggml_mul_mat(ctx0, model.layers[il].ffn_up_proj, cur);

      // GELU activation
      cur = ggml_gelu(ctx0, cur);

      // projection
      // cur = proj_w*cur + proj_b
      cur = ggml_mul_mat(ctx0, model.layers[il].ffn_down_proj, cur);
    }

    // x = x + n
    inpL = ggml_add(ctx0, inpL, cur);
  }

  // norm
  {
    inpL = ggml_norm(ctx0, inpL);
    // inpL = ln_f_g*inpL
    inpL = ggml_mul(ctx0, ggml_repeat(ctx0, model.norm_f_weight, inpL), inpL);
  }

  // output embedding weight tied to input embedding
  inpL = ggml_mul_mat(ctx0, model.wte_weight, inpL);

  // run the computation
  ggml_build_forward_expand(&gf, inpL);
  ggml_graph_compute_with_ctx(ctx0, &gf, n_threads);

  if (logits_all) {
    // return result for all tokens
    embd_w.resize(n_vocab * N);
    memcpy(embd_w.data(), (float *)ggml_get_data(inpL),
           sizeof(float) * n_vocab * N);
  } else {
    // return result for just the last token
    embd_w.resize(n_vocab);
    memcpy(embd_w.data(), (float *)ggml_get_data(inpL) + (n_vocab * (N - 1)),
           sizeof(float) * n_vocab);
  }

  if (mem_per_token == 0) {
    mem_per_token = ggml_used_mem(ctx0) / N;
  }
  // printf("used_mem = %zu\n", ggml_used_mem(ctx0));

  ggml_free(ctx0);

  return true;
}

class replit_llm : public LLM {
 public:
  virtual ~replit_llm() {
    if (model_.ctx != nullptr) {
      ggml_free(model_.ctx);
    }
  }

  std::vector<gpt_vocab::id> Tokenize(const std::string &text) const override {
    // tokenize the prompt
    std::vector<gpt_vocab::id> embd_inp =
        replit_tokenizer_tokenize(replit_tokenizer_, text);

    return embd_inp;
  }

  const std::string &Detokenize(const gpt_vocab::id id) const override {
    const auto it = vocab_.id_to_token.find(id);
    if (it == vocab_.id_to_token.end()) {
      return kEmptyString;
    }

    detokenized_text_ = replace_all(vocab_.id_to_token.at(id), ws_symbol, " ");
    return detokenized_text_;
  }

 protected:
  replit_tokenizer replit_tokenizer_;
  bool Load(const std::string &filename, const int context_length,
            const int gpu_layers) override {
    if (context_length > 0) {
      model_.hparams.n_ctx = context_length;
    }
    if (!replit_model_load(filename, model_, replit_tokenizer_)) {
      return false;
    }
    n_ctx_ = model_.hparams.n_ctx;
    vocab_ = replit_tokenizer_.raw_vocab;
    return true;
  }

  bool Eval(const std::vector<gpt_vocab::id> &tokens, const int threads,
            const int n_past) override {
    return replit_eval(model_, threads, n_past, tokens, logits_,
                       mem_per_token_);
  }

 private:
  replit_model model_;
  mutable std::string detokenized_text_;
};
