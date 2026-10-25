#include "speech.h"

#if defined(MKW_SCREEN_READER_SPEECH)

#include "runtime_config.h"
#include "runtime_log.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include <dlfcn.h>
#include <prism.h>

namespace Speech {
namespace {

// Screen-reader backends only, most preferred first. Prism's synthesiser backends (Speech Dispatcher, SAPI, OneCore
// and the like) are deliberately absent: speech is for players who already run a screen reader.
constexpr PrismBackendId kScreenReaderBackends[] = {PRISM_BACKEND_ORCA};

// A screen reader started after the game is picked up on a later line, but not looked for on every line.
constexpr auto kReconnectInterval = std::chrono::seconds(5);

// libprism is loaded with dlopen from the executable's folder rather than linked: its Orca backend links glibmm,
// and a machine without it must still run the game, silently.
struct PrismApi {
    void* handle = nullptr;
    decltype(&::prism_config_init) config_init = nullptr;
    decltype(&::prism_init) init = nullptr;
    decltype(&::prism_registry_exists) registry_exists = nullptr;
    decltype(&::prism_registry_create) registry_create = nullptr;
    decltype(&::prism_backend_initialize) backend_initialize = nullptr;
    decltype(&::prism_backend_name) backend_name = nullptr;
    decltype(&::prism_backend_get_features) backend_get_features = nullptr;
    decltype(&::prism_backend_output) backend_output = nullptr;
    decltype(&::prism_backend_speak) backend_speak = nullptr;
    decltype(&::prism_backend_stop) backend_stop = nullptr;
    decltype(&::prism_backend_free) backend_free = nullptr;
    decltype(&::prism_error_string) error_string = nullptr;

    template <typename Fn>
    bool Bind(Fn& fn, const char* symbol) noexcept {
        fn = reinterpret_cast<Fn>(dlsym(handle, symbol));
        return fn != nullptr;
    }

    bool Load() {
        const auto directory = RuntimeConfigFile::ExecutableDirectory();
        if (!directory) {
            RT_LOG(RT_TAG_ACCESSIBILITY) << "speech off: the executable's folder is unknown" << std::endl;
            return false;
        }
        const std::string path = (*directory / "libprism.so").string();
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            RT_LOG(RT_TAG_ACCESSIBILITY) << "speech off: " << dlerror() << std::endl;
            return false;
        }
        const bool ok = Bind(config_init, "prism_config_init") && Bind(init, "prism_init") &&
                        Bind(registry_exists, "prism_registry_exists") &&
                        Bind(registry_create, "prism_registry_create") &&
                        Bind(backend_initialize, "prism_backend_initialize") &&
                        Bind(backend_name, "prism_backend_name") &&
                        Bind(backend_get_features, "prism_backend_get_features") &&
                        Bind(backend_output, "prism_backend_output") && Bind(backend_speak, "prism_backend_speak") &&
                        Bind(backend_stop, "prism_backend_stop") && Bind(backend_free, "prism_backend_free") &&
                        Bind(error_string, "prism_error_string");
        if (!ok) {
            RT_LOG(RT_TAG_ACCESSIBILITY) << "speech off: " << path << " lacks a Prism function" << std::endl;
            dlclose(handle);
            handle = nullptr;
        }
        return ok;
    }
};

struct Utterance {
    std::string text;
    bool interrupt = false;
};

// Shared between the game's threads and the worker. Allocated once and never destroyed: the detached worker may be
// waiting on it when std::exit runs static destructors (OSResetSystem, fatal paths), and destroying a condition
// variable with a waiter blocks forever.
struct SpeechState {
    std::once_flag start;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Utterance> queue;
    bool stopRequested = false;
    bool unavailable = false;  // Prism failed to load or the worker could not start: Say discards everything
};

SpeechState& State() {
    static SpeechState& state = *new SpeechState;
    return state;
}

// The first allowed screen reader that is running, initialized; nullptr when none is.
PrismBackend* OpenScreenReader(const PrismApi& api, PrismContext* context) {
    for (const PrismBackendId id : kScreenReaderBackends) {
        if (!api.registry_exists(context, id)) continue;
        PrismBackend* backend = api.registry_create(context, id);
        if (backend == nullptr) continue;
        const PrismError error = api.backend_initialize(backend);
        if (error == PRISM_OK || error == PRISM_ERROR_ALREADY_INITIALIZED) {
            RT_LOG(RT_TAG_ACCESSIBILITY) << "speaking through " << api.backend_name(backend) << std::endl;
            return backend;
        }
        api.backend_free(backend);
    }
    return nullptr;
}

// Owns the Prism context and backend: a Prism backend must only be used from one thread, and its calls can block
// (the Orca backend makes synchronous D-Bus calls), which must never stall the game thread.
void SpeechWorker() {
    PrismApi api;
    PrismContext* context = nullptr;
    if (api.Load()) {
        PrismConfig config = api.config_init();
        context = api.init(&config);
        if (context == nullptr) RT_LOG(RT_TAG_ACCESSIBILITY) << "speech off: prism_init failed" << std::endl;
    }
    SpeechState& state = State();
    if (context == nullptr) {
        std::lock_guard lock(state.mutex);
        state.unavailable = true;
        state.queue.clear();
        return;
    }

    PrismBackend* backend = nullptr;
    bool useOutput = false;
    auto nextConnectAttempt = std::chrono::steady_clock::time_point{};
    bool reportedNoScreenReader = false;
    for (;;) {
        Utterance utterance;
        bool stop = false;
        {
            std::unique_lock lock(state.mutex);
            state.wake.wait(lock, [&] { return state.stopRequested || !state.queue.empty(); });
            stop = std::exchange(state.stopRequested, false);
            if (!stop) {
                utterance = std::move(state.queue.front());
                state.queue.pop_front();
            }
        }

        try {
            if (backend == nullptr && std::chrono::steady_clock::now() >= nextConnectAttempt) {
                backend = OpenScreenReader(api, context);
                nextConnectAttempt = std::chrono::steady_clock::now() + kReconnectInterval;
                if (backend != nullptr) {
                    useOutput = (api.backend_get_features(backend) & PRISM_BACKEND_SUPPORTS_OUTPUT) != 0;
                    reportedNoScreenReader = false;
                } else if (!reportedNoScreenReader) {
                    RT_LOG(RT_TAG_ACCESSIBILITY) << "no screen reader running: speech is silent" << std::endl;
                    reportedNoScreenReader = true;
                }
            }
            if (backend == nullptr) continue;

            if (stop) {
                (void)api.backend_stop(backend);
                continue;
            }
            const char* text = utterance.text.c_str();
            const PrismError error = useOutput ? api.backend_output(backend, text, utterance.interrupt)
                                               : api.backend_speak(backend, text, utterance.interrupt);
            if (error == PRISM_ERROR_BACKEND_NOT_AVAILABLE || error == PRISM_ERROR_SPEAK_FAILURE) {
                // The screen reader went away or restarted (the Orca backend reports a failed D-Bus call as a speak
                // failure); connect afresh on a later line.
                RT_LOG(RT_TAG_ACCESSIBILITY) << "screen reader lost: " << api.error_string(error) << std::endl;
                api.backend_free(backend);
                backend = nullptr;
            } else if (error != PRISM_OK) {
                RT_LOG(RT_TAG_ACCESSIBILITY) << "speech failed: " << api.error_string(error) << std::endl;
            }
        } catch (const std::exception& e) {
            // Prism (and the C++ libraries under it) can throw; speech then starts afresh on a later line rather than
            // taking the game down.
            RT_LOG(RT_TAG_ACCESSIBILITY) << "speech error: " << e.what() << std::endl;
            if (backend != nullptr) api.backend_free(backend);
            backend = nullptr;
        }
    }
}

void StartSpeechWorker() {
    SpeechState& state = State();
    // Detached for the life of the process: nothing at exit may wait on a screen reader's reply.
    std::call_once(state.start, [&] {
        try {
            std::thread(SpeechWorker).detach();
        } catch (const std::system_error& e) {
            RT_LOG(RT_TAG_ACCESSIBILITY) << "speech off: " << e.what() << std::endl;
            std::lock_guard lock(state.mutex);
            state.unavailable = true;
        }
    });
}

} // namespace

void Say(std::string text, bool interrupt) {
    if (text.empty()) return;
    StartSpeechWorker();
    SpeechState& state = State();
    {
        std::lock_guard lock(state.mutex);
        if (state.unavailable) return;
        // Prism's backends differ in whether an interrupt also discards speech they already queued, so lines are
        // queued here and handed over one at a time.
        if (interrupt) state.queue.clear();
        state.queue.push_back({std::move(text), interrupt});
    }
    state.wake.notify_one();
}

void Silence() {
    StartSpeechWorker();
    SpeechState& state = State();
    {
        std::lock_guard lock(state.mutex);
        if (state.unavailable) return;
        state.queue.clear();
        state.stopRequested = true;
    }
    state.wake.notify_one();
}

} // namespace Speech

#else

// Built without screen-reader speech (MKW_SCREEN_READER_SPEECH off, or a platform it is not wired up for yet).
namespace Speech {
void Say(std::string, bool) {}
void Silence() {}
} // namespace Speech

#endif
