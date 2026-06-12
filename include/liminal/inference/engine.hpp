#pragma once
// The inference engine. Wraps llama.cpp behind a thread-safe
// request/response queue:
//
//   app thread                       worker thread
//   submit(req) -> id   --queue-->   tokenize, decode, sample
//   poll(id) <-- partial text --     stream pieces as they're sampled
//
// The render loop must NEVER block on token generation, so every public
// call here returns immediately; all llama.cpp work (including the initial
// model load) happens on one dedicated worker thread. llama.h makes no
// thread-safety promises for llama_context, so the context is confined to
// that thread for its entire life.
//
// This layer knows nothing about the app's domain. Strings in, strings out.
// Grammar-constrained sampling (GBNF) is the one structural guarantee: a
// tiny model WILL produce garbage freeform, but a grammar makes the
// structure valid while leaving the content fully hallucinated — which is
// the point.

#ifndef LIMINAL_WITH_INFERENCE
#error "liminal/inference/engine.hpp requires the inference module; configure liminal with LIMINAL_WITH_INFERENCE=ON"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace liminal::inference {

struct EngineConfig {
    std::string modelPath;
    int contextTokens = 2048;
    int threads = 6;
    int gpuLayers = -1; // -1 = offload everything (Metal on macOS)
};

// Per-request sampling. Apps may expose temperature as a live dial, so
// every request carries its own values.
struct SamplingConfig {
    float temperature = 0.7f;
    float topP = 0.9f;
    float repeatPenalty = 1.18f;
    int topK = 40;
    int maxTokens = 256;
    uint32_t seed = 0xFFFFFFFFu; // LLAMA_DEFAULT_SEED — fresh randomness
};

struct PromptRequest {
    std::string system;       // persona
    std::string user;         // context to riff on
    std::string grammarGbnf;  // empty = unconstrained
    SamplingConfig sampling;
};

using RequestId = std::uint64_t; // 0 = invalid

struct Response {
    std::string text;     // partial while streaming; never splits UTF-8
    bool complete = false;
    bool failed = false;
    std::string error;    // set when failed
};

class Engine {
public:
    enum class Status { Stopped, Loading, Ready, Failed };

    Engine() = default;
    ~Engine(); // joins the worker, frees llama context/model/backend

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Spawns the worker thread; the model loads there (it takes a moment,
    // and the app keeps running while the engine wakes up).
    void start(const EngineConfig& config);
    void stop(); // idempotent; safe to call before/without start

    Status status() const;
    std::string statusMessage() const; // model path or load error

    RequestId submit(PromptRequest request); // never blocks
    Response poll(RequestId id) const;       // copies current state
    void cancel(RequestId id);  // drop a queued request / abort an active one
    void forget(RequestId id);  // free a finished response you're done with

    bool busy() const;          // worker mid-generation
    size_t queueDepth() const;

private:
    void workerMain(EngineConfig config);

    struct Pending {
        RequestId id;
        PromptRequest request;
    };

    std::thread m_worker;
    std::atomic<bool> m_stop{false};
    std::atomic<Status> m_status{Status::Stopped};
    std::atomic<RequestId> m_activeId{0};
    std::atomic<RequestId> m_cancelId{0};
    std::atomic<RequestId> m_nextId{1};
    std::atomic<bool> m_busy{false};

    mutable std::mutex m_mutex; // guards queue, responses, status message
    std::condition_variable m_cv;
    std::deque<Pending> m_queue;
    std::unordered_map<RequestId, Response> m_responses;
    std::string m_statusMessage;
};

} // namespace liminal::inference
