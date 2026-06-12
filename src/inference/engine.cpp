// engine.cpp — the llama.cpp integration, in one place.
//
// Threading model (the single most important thing here):
//
//   llama.h makes NO thread-safety promises for llama_context. So this file
//   confines the context — and the model, and the backend — to exactly one
//   thread (the worker spawned in start()). Every llama_* call that touches
//   model or context happens between the first and last line of workerMain().
//   The app thread only ever touches std:: primitives: the queue, the
//   response map, and a handful of atomics. That's the whole contract.
//
// Lifetime model:
//
//   workerMain() owns everything llama. It loads the model, builds the
//   context, runs the queue, and frees them in reverse order on the way out.
//   If the model fails to load (wrong path, corrupt gguf, out of memory),
//   the worker does NOT exit — it keeps draining the queue, failing every
//   request instantly, so the app's fallback content path keeps working
//   without special-casing "engine never woke up".

#include <liminal/inference/engine.hpp>

// llama.h pulls in ggml.h, which gives us ggml_log_level / ggml_log_callback
// for the log filter below.
#include <llama.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace liminal::inference {

namespace {

// llama.cpp's logical batch size: the max number of prompt tokens we hand to
// llama_decode in one call. Must match the n_batch we put in the context
// params, or decode rejects the batch.
constexpr int kBatchSize = 512;

// ---------------------------------------------------------------------------
// Log filter.
//
// By default llama.cpp prints its entire life story to stderr: tensor names,
// Metal shader compilation, KV-cache layout, per-layer offload decisions.
// Charming once, unusable in a game. llama_log_set installs a global hook;
// note the header's warning that the logger state is global and NOT thread
// safe — so we install it once, from the worker, before any other llama call,
// and never change it again.
//
// GGML_LOG_LEVEL_CONT means "continuation of the previous message" (llama.cpp
// splits long lines), so we remember the last real level and let CONT inherit
// it — otherwise we'd print the first half of a warning and drop the rest.
void quietLogCallback(enum ggml_log_level level, const char* text, void* /*user*/) {
    static std::atomic<int> lastLevel{GGML_LOG_LEVEL_NONE};
    int effective = level;
    if (level == GGML_LOG_LEVEL_CONT) {
        effective = lastLevel.load(std::memory_order_relaxed);
    } else {
        lastLevel.store(effective, std::memory_order_relaxed);
    }
    if (effective == GGML_LOG_LEVEL_ERROR || effective == GGML_LOG_LEVEL_WARN) {
        std::fputs(text, stderr);
    }
}

// ---------------------------------------------------------------------------
// UTF-8 tail trimming for poll().
//
// Token pieces are byte fragments, not characters: a single é or 〜 can be
// split across two sampled tokens, so a mid-stream snapshot of the response
// text can end half-way through a multibyte sequence. We trim the torn tail
// from the CALLER'S COPY only — the worker keeps appending to the intact
// original, so the bytes reappear (whole) on the next poll.
//
// UTF-8 makes this cheap: continuation bytes look like 0b10xxxxxx and a
// sequence is at most 4 bytes, so we scan back over at most 3 continuation
// bytes, decode the expected length from the lead byte, and cut if short.
void trimIncompleteUtf8(std::string& s) {
    if (s.empty()) return;

    size_t cont = 0; // continuation bytes seen at the very end
    size_t i = s.size();
    while (i > 0 && cont < 3 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) {
        --i;
        ++cont;
    }
    if (i == 0) return; // nothing but continuation bytes; not a torn tail we can fix

    const unsigned char lead = static_cast<unsigned char>(s[i - 1]);
    if ((lead & 0xC0) == 0x80) return; // >3 continuations: malformed, leave it alone

    size_t need = 0; // continuation bytes this lead byte promises
    if ((lead & 0x80) == 0x00)      need = 0; // plain ASCII
    else if ((lead & 0xE0) == 0xC0) need = 1; // 2-byte sequence
    else if ((lead & 0xF0) == 0xE0) need = 2; // 3-byte sequence
    else if ((lead & 0xF8) == 0xF0) need = 3; // 4-byte sequence
    else { s.resize(i - 1); return; }         // invalid lead byte: drop it

    if (cont < need) s.resize(i - 1); // sequence still arriving: cut it for now
}

// Per-request sampler chains are created and destroyed inside one call, but
// there are several early-return failure paths; a tiny RAII guard keeps every
// one of them leak-free. llama_sampler_free on a chain also frees every
// sampler that was added to it (chain_add transfers ownership).
struct SamplerChainGuard {
    llama_sampler* chain = nullptr;
    ~SamplerChainGuard() {
        if (chain) llama_sampler_free(chain);
    }
    SamplerChainGuard() = default;
    SamplerChainGuard(const SamplerChainGuard&) = delete;
    SamplerChainGuard& operator=(const SamplerChainGuard&) = delete;
};

} // namespace

// ===========================================================================
// Public API — game thread side. Everything here is lock-and-return; nothing
// waits on the model.
// ===========================================================================

Engine::~Engine() {
    stop();
}

void Engine::start(const EngineConfig& config) {
    // Calling start() twice without stop() would assign over a joinable
    // std::thread, which calls std::terminate. Refuse quietly instead.
    if (m_worker.joinable()) return;

    m_stop.store(false);
    // Mark Loading before the thread exists so a poll of status() right after
    // start() never reports Stopped during the spawn window.
    m_status.store(Status::Loading);
    m_worker = std::thread(&Engine::workerMain, this, config);
}

void Engine::stop() {
    m_stop.store(true);
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_status.store(Status::Stopped);
}

Engine::Status Engine::status() const {
    return m_status.load();
}

std::string Engine::statusMessage() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_statusMessage;
}

RequestId Engine::submit(PromptRequest request) {
    const RequestId id = m_nextId.fetch_add(1);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_responses[id] = Response{};

    if (m_status.load() == Status::Failed) {
        // The model never loaded; don't make the caller wait a queue
        // round-trip to learn that. Fail synchronously with the load error.
        Response& r = m_responses[id];
        r.failed = true;
        r.error = m_statusMessage;
        return id;
    }

    m_queue.push_back(Pending{id, std::move(request)});
    m_cv.notify_one();
    return id;
}

Response Engine::poll(RequestId id) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_responses.find(id);
    if (it == m_responses.end()) {
        Response r;
        r.failed = true;
        r.error = "unknown request";
        return r;
    }

    // Copy first, then trim the copy: the worker's master string must keep
    // its partial multibyte tail so the character completes on a later poll.
    Response copy = it->second;
    trimIncompleteUtf8(copy.text);
    return copy;
}

void Engine::cancel(RequestId id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Still queued? Pull it out before the worker ever sees it.
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (it->id == id) {
            m_queue.erase(it);
            auto rit = m_responses.find(id);
            if (rit != m_responses.end()) {
                rit->second.failed = true;
                rit->second.error = "cancelled";
            }
            return;
        }
    }

    // Mid-generation? Flag it; the worker checks this atomic between tokens,
    // so cancellation lands within one token's latency.
    if (m_activeId.load() == id) {
        m_cancelId.store(id);
    }
}

void Engine::forget(RequestId id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Never erase the active response: the worker appends to it through this
    // map and must not have the entry vanish mid-token. (The worker's append
    // helpers also tolerate a missing entry, but don't rely on that.)
    if (m_activeId.load() == id) return;
    m_responses.erase(id);
}

bool Engine::busy() const {
    return m_busy.load();
}

size_t Engine::queueDepth() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

// ===========================================================================
// Worker thread — the only place llama.cpp exists.
// ===========================================================================

void Engine::workerMain(EngineConfig config) {
    // Silence the logger BEFORE backend init; init itself logs (Metal device
    // discovery, etc.) and we want the filter in place for all of it.
    llama_log_set(quietLogCallback, nullptr);

    // One-time global setup for ggml's backends (CPU feature detection,
    // Metal registration). Paired with llama_backend_free at the bottom.
    llama_backend_init();

    m_status.store(Status::Loading);

    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;
    std::string loadError;

    // ---- Model load -------------------------------------------------------
    // NEVER brace-initialize llama param structs: their layout changes
    // between llama.cpp versions, and *_default_params() is the only way to
    // get every field (including ones added after this code was written)
    // into a sane state. Start from defaults, override what we care about.
    {
        llama_model_params mparams = llama_model_default_params();
        // n_gpu_layers: how many transformer layers live on the GPU. On Apple
        // Silicon, Metal + unified memory means "all of them" is the only
        // sensible answer for a 0.5B model — 999 comfortably exceeds any
        // layer count this game will ever see.
        mparams.n_gpu_layers = (config.gpuLayers < 0) ? 999 : config.gpuLayers;

        // This is the slow call — seconds, not milliseconds. It mmaps the
        // gguf and uploads weights to Metal. It's exactly why model loading
        // lives on this thread and not in start().
        model = llama_model_load_from_file(config.modelPath.c_str(), mparams);
        if (!model) {
            loadError = "failed to load model: " + config.modelPath;
        }
    }

    // ---- Context creation -------------------------------------------------
    if (model) {
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = static_cast<uint32_t>(config.contextTokens);
        // n_batch caps how many prompt tokens one llama_decode call may
        // carry; our prompt-feeding loop below chunks to the same constant.
        cparams.n_batch = kBatchSize;
        // Two thread counts: n_threads for one-token-at-a-time generation,
        // n_threads_batch for the big prompt-ingestion matmuls. We use the
        // same value; the game tuned it to leave cores free for rendering.
        cparams.n_threads = config.threads;
        cparams.n_threads_batch = config.threads;
        // Flash attention: fuses the attention matmuls + softmax into one
        // pass, cutting per-token decode latency (and KV-cache memory) on
        // Metal. The pinned llama tag (b9585) replaced the old bool
        // cparams.flash_attn with this enum — ENABLED is explicit and safe
        // (AUTO may already turn it on for Metal, but we don't leave it to
        // chance for the dominant decode cost). Verified against the fetched
        // header build/_deps/llama-src/include/llama.h.
        cparams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;

        ctx = llama_init_from_model(model, cparams);
        if (!ctx) {
            loadError = "failed to create llama context";
        } else {
            // The vocab is owned by the model — a borrowed pointer, valid as
            // long as the model is. Tokenization, detokenization, grammar and
            // end-of-generation checks all go through it.
            vocab = llama_model_get_vocab(model);
        }
    }

    if (loadError.empty()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_statusMessage = config.modelPath;
        }
        m_status.store(Status::Ready);
    } else {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_statusMessage = loadError;
        }
        m_status.store(Status::Failed);
        // Deliberately NOT returning: the queue loop below keeps running and
        // fails each request immediately, so the game's fallback content path
        // works the same whether the oracle is slow or dead.
    }

    // ---- Small helpers (all response-map writes go through these) ---------

    auto failRequest = [this](RequestId id, std::string error) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_responses.find(id);
        if (it == m_responses.end()) return; // caller forgot it mid-flight
        it->second.failed = true;
        it->second.error = std::move(error);
    };

    auto completeRequest = [this](RequestId id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_responses.find(id);
        if (it == m_responses.end()) return;
        it->second.complete = true;
    };

    // Streaming: each sampled token's bytes are appended under the mutex so
    // poll() on the game thread always sees a consistent (if torn-tailed —
    // see trimIncompleteUtf8) snapshot.
    auto appendText = [this](RequestId id, const char* data, size_t n) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_responses.find(id);
        if (it == m_responses.end()) return;
        it->second.text.append(data, n);
    };

    auto cancelled = [this](RequestId id) {
        return m_stop.load() || m_cancelId.load() == id;
    };

    // ---- The actual request processing -------------------------------------

    auto processRequest = [&](const Pending& job) {
        const PromptRequest& req = job.request;
        const SamplingConfig& sampling = req.sampling;

        // -- 1) Build the prompt string the model was trained to expect. ----
        // Chat models are fine-tuned on a specific turn format; feed them raw
        // text and the tiny ones especially fall apart. The gguf usually
        // embeds its template, and llama_chat_apply_template renders it
        // (it's a fixed list of known templates, not a real Jinja engine).
        std::string prompt;
        bool templated = false;

        const char* tmpl = llama_model_chat_template(model, nullptr);
        if (tmpl) {
            llama_chat_message msgs[2] = {
                {"system", req.system.c_str()},
                {"user", req.user.c_str()},
            };
            // Standard llama.cpp size dance: the call returns the length it
            // NEEDED; if that exceeds our buffer, grow and call again.
            std::vector<char> buf(2 * (req.system.size() + req.user.size()) + 256);
            int32_t needed = llama_chat_apply_template(
                tmpl, msgs, 2, /*add_ass=*/true, buf.data(), static_cast<int32_t>(buf.size()));
            if (needed > static_cast<int32_t>(buf.size())) {
                buf.resize(static_cast<size_t>(needed));
                needed = llama_chat_apply_template(
                    tmpl, msgs, 2, true, buf.data(), static_cast<int32_t>(buf.size()));
            }
            if (needed >= 0) {
                prompt.assign(buf.data(), static_cast<size_t>(needed));
                templated = true;
            }
        }
        if (!templated) {
            // No embedded template (or the renderer balked). Qwen speaks
            // ChatML, so hand-rolling it is safe for the model this game
            // ships with. The trailing "assistant\n" cues generation.
            prompt = "<|im_start|>system\n" + req.system +
                     "<|im_end|>\n<|im_start|>user\n" + req.user +
                     "<|im_end|>\n<|im_start|>assistant\n";
        }

        // -- 2) Tokenize. -----------------------------------------------------
        // parse_special=true so the <|im_start|> markers become their single
        // control tokens instead of being shredded into plaintext pieces.
        // Negative return = "buffer too small, you need -n tokens": resize
        // and retry once.
        std::vector<llama_token> tokens(prompt.size() + 8);
        int32_t nTokens = llama_tokenize(
            vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()),
            tokens.data(), static_cast<int32_t>(tokens.size()),
            /*add_special=*/true, /*parse_special=*/true);
        if (nTokens == INT32_MIN) {
            failRequest(job.id, "tokenization overflow");
            return;
        }
        if (nTokens < 0) {
            tokens.resize(static_cast<size_t>(-nTokens));
            nTokens = llama_tokenize(
                vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()),
                tokens.data(), static_cast<int32_t>(tokens.size()), true, true);
        }
        if (nTokens <= 0) {
            failRequest(job.id, "tokenization failed");
            return;
        }
        tokens.resize(static_cast<size_t>(nTokens));

        const int nCtx = config.contextTokens;
        if (nTokens + 1 >= nCtx) {
            failRequest(job.id, "prompt too long for context");
            return;
        }

        // -- 3) Wipe the KV cache. --------------------------------------------
        // Every request is an independent dream fragment; nothing carries
        // over. Clearing the memory (the KV cache lives behind llama_memory_t
        // now — the old llama_kv_cache_* API is gone) resets all sequence
        // state so positions start at 0 again.
        llama_memory_clear(llama_get_memory(ctx), /*data=*/true);

        // -- 4) Build the per-request sampler chain. ----------------------------
        // Samplers are stateful (penalties remember recent tokens, the
        // grammar tracks its parse position), so reuse across requests would
        // bleed state. Building per request is cheap; the chain frees its
        // members when freed.
        SamplerChainGuard guard;
        guard.chain = llama_sampler_chain_init(llama_sampler_chain_default_params());

        if (!req.grammarGbnf.empty()) {
            // The grammar sampler goes FIRST, mirroring llama.cpp's own
            // common/sampling.cpp: it masks structurally invalid tokens to
            // -inf before top-k/top-p truncate the distribution. Put it after
            // them and a truncation step could discard every grammar-legal
            // token, leaving nothing to sample.
            llama_sampler* grammar =
                llama_sampler_init_grammar(vocab, req.grammarGbnf.c_str(), "root");
            if (!grammar) {
                // NULL means the GBNF text itself didn't parse — a bug in our
                // grammar file, not a model mood. No point generating
                // unconstrained garbage the parser downstream will reject.
                failRequest(job.id, "grammar parse failed");
                return;
            }
            llama_sampler_chain_add(guard.chain, grammar); // chain owns it now
        }
        // Order matters from here too: penalties reshape logits, top-k then
        // top-p truncate, temperature rescales, and dist — the only sampler
        // that actually PICKS — must sit at the end of the chain.
        llama_sampler_chain_add(guard.chain,
            llama_sampler_init_penalties(64, sampling.repeatPenalty, 0.0f, 0.0f));
        llama_sampler_chain_add(guard.chain, llama_sampler_init_top_k(sampling.topK));
        llama_sampler_chain_add(guard.chain, llama_sampler_init_top_p(sampling.topP, 1));
        llama_sampler_chain_add(guard.chain, llama_sampler_init_temp(sampling.temperature));
        llama_sampler_chain_add(guard.chain, llama_sampler_init_dist(sampling.seed));

        // -- 5) Feed the prompt through the model. ------------------------------
        // llama_batch_get_one wraps a span of tokens as a single-sequence
        // batch; positions and sequence ids are tracked automatically. We
        // chunk to n_batch because that's the hard cap we set on the context.
        int nPast = 0;
        for (int off = 0; off < nTokens; off += kBatchSize) {
            if (cancelled(job.id)) {
                failRequest(job.id, "cancelled");
                return;
            }
            const int count = std::min(kBatchSize, nTokens - off);
            if (llama_decode(ctx, llama_batch_get_one(tokens.data() + off, count)) != 0) {
                failRequest(job.id, "prompt decode failed");
                return;
            }
            nPast += count;
        }

        // -- 6) Generate, one token at a time, streaming as we go. -------------
        int generated = 0;
        while (true) {
            // Cancellation is checked between tokens, so worst-case latency
            // to abort is one token (~tens of ms on this model).
            if (cancelled(job.id)) {
                failRequest(job.id, "cancelled");
                return;
            }
            if (generated >= sampling.maxTokens) break; // budget spent: done

            // idx=-1 means "sample from the logits of the last token in the
            // previous decode". The chain runs grammar -> penalties -> top-k
            // -> top-p -> temp and dist picks; it also auto-accepts the token
            // so the grammar and penalty state advance.
            // Non-const because llama_batch_get_one wants a mutable pointer.
            llama_token tok = llama_sampler_sample(guard.chain, ctx, -1);

            // End-of-generation covers eos AND chat end-of-turn tokens
            // (<|im_end|> for Qwen) — exactly the "model chose to stop" set.
            if (llama_vocab_is_eog(vocab, tok)) break;

            // Token -> bytes. NOT necessarily whole characters — multibyte
            // UTF-8 can straddle tokens, which is why poll() trims the tail.
            // special=false: if a control token leaks past the eog check we'd
            // rather render nothing than "<|im_start|>" in a dream.
            char piece[128];
            const int32_t n = llama_token_to_piece(
                vocab, tok, piece, static_cast<int32_t>(sizeof(piece)), 0, /*special=*/false);
            if (n > 0) {
                appendText(job.id, piece, static_cast<size_t>(n));
            }
            ++generated;

            // The next decode would place this token at position nPast; stop
            // while there's still room rather than let decode fail at the rim
            // of the context window.
            if (nPast + 1 >= nCtx) break;

            // Feed the sampled token back in to get logits for the next one.
            if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0) {
                failRequest(job.id, "decode failed during generation");
                return;
            }
            ++nPast;
        }

        completeRequest(job.id);
        // guard's destructor frees the chain (and every sampler in it) here,
        // on success and on every early return above alike.
    };

    // ---- Queue loop ---------------------------------------------------------

    for (;;) {
        Pending job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop.load() || !m_queue.empty(); });
            if (m_stop.load()) break;

            job = std::move(m_queue.front());
            m_queue.pop_front();
            // Publish "this id is active" BEFORE unlocking, so cancel() and
            // forget() racing with us classify the id correctly.
            m_activeId.store(job.id);
            m_busy.store(true);
        }

        if (ctx) {
            processRequest(job);
        } else {
            // Dead oracle, live queue: anything submitted during the Loading
            // window (before m_status flipped to Failed) lands here.
            failRequest(job.id, loadError);
        }

        m_busy.store(false);
        m_activeId.store(0);
        // Clear a cancel flag aimed at the request we just finished, so it
        // can't linger. (Ids are never reused, so this is belt-and-braces.)
        RequestId expected = job.id;
        m_cancelId.compare_exchange_strong(expected, 0);
    }

    // ---- Teardown — strictly here, on the worker, in reverse order. ----------
    // The context references the model and the model references backend
    // buffers, so: context, then model, then backend. Doing this anywhere
    // else (e.g. the destructor on the game thread) would violate the
    // single-thread confinement this whole file is built on.
    if (ctx) llama_free(ctx);
    if (model) llama_model_free(model);
    llama_backend_free();
}

} // namespace liminal::inference
