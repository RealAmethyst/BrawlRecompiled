#pragma once

#include "spatial_audio_mix.h"

#include <phonon.h>

#include <memory>
#include <string>
#include <vector>

// Steam Audio (https://github.com/ValveSoftware/steam-audio, Apache-2.0) behind SpatialAudioMix::Binaural. The functions
// come through a table so the runtime can load the library at run time (src/accessibility/spatial_audio.cpp) and the
// offline check can link it directly (accessibility/tools/spatial_mix_check.cpp).
namespace SpatialAudioSteam {

struct Api {
    decltype(&::iplContextCreate) contextCreate = nullptr;
    decltype(&::iplContextRelease) contextRelease = nullptr;
    decltype(&::iplHRTFCreate) hrtfCreate = nullptr;
    decltype(&::iplHRTFRelease) hrtfRelease = nullptr;
    decltype(&::iplBinauralEffectCreate) binauralCreate = nullptr;
    decltype(&::iplBinauralEffectRelease) binauralRelease = nullptr;
    decltype(&::iplBinauralEffectReset) binauralReset = nullptr;
    decltype(&::iplBinauralEffectApply) binauralApply = nullptr;
};

class Voice final : public SpatialAudioMix::Binaural {
public:
    Voice(const Api& api, IPLBinauralEffect effect, IPLHRTF hrtf) : m_api(api), m_effect(effect), m_hrtf(hrtf) {}
    ~Voice() override { m_api.binauralRelease(&m_effect); }
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;

    void Reset() override { m_api.binauralReset(m_effect); }

    void Apply(const SpatialAudioMix::Direction& direction, const float* input, float* left, float* right) override {
        float* in[1] = {const_cast<float*>(input)};
        float* out[2] = {left, right};
        IPLAudioBuffer inBuffer{1, static_cast<IPLint32>(SpatialAudioMix::kRenderFrame), in};
        IPLAudioBuffer outBuffer{2, static_cast<IPLint32>(SpatialAudioMix::kRenderFrame), out};
        IPLBinauralEffectParams params{};
        params.direction = {direction.x, direction.y, direction.z};
        params.interpolation = IPL_HRTFINTERPOLATION_NEAREST;
        params.spatialBlend = 1.0f;
        params.hrtf = m_hrtf;
        m_api.binauralApply(m_effect, &params, &inBuffer, &outBuffer);
    }

private:
    const Api& m_api;
    IPLBinauralEffect m_effect = nullptr;
    IPLHRTF m_hrtf = nullptr;
};

// The context, the default HRTF and a pool of binaural effects at the render rate. `api` must outlive it.
class Engine {
public:
    explicit Engine(const Api& api) : m_api(api) {}
    ~Engine() {
        m_pool.clear();
        if (m_hrtf != nullptr) m_api.hrtfRelease(&m_hrtf);
        if (m_context != nullptr) m_api.contextRelease(&m_context);
    }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Creates everything up front, so the mix thread never allocates. Returns an empty string or what failed.
    std::string Create(size_t voices) {
        IPLContextSettings contextSettings{};
        contextSettings.version = STEAMAUDIO_VERSION;
        if (m_api.contextCreate(&contextSettings, &m_context) != IPL_STATUS_SUCCESS) return "iplContextCreate failed";
        IPLAudioSettings audio{static_cast<IPLint32>(SpatialAudioMix::kRenderRate),
                               static_cast<IPLint32>(SpatialAudioMix::kRenderFrame)};
        IPLHRTFSettings hrtfSettings{};
        hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
        hrtfSettings.volume = 1.0f;
        hrtfSettings.normType = IPL_HRTFNORMTYPE_NONE;
        if (m_api.hrtfCreate(m_context, &audio, &hrtfSettings, &m_hrtf) != IPL_STATUS_SUCCESS) return "iplHRTFCreate failed";
        for (size_t i = 0; i < voices; ++i) {
            IPLBinauralEffectSettings effectSettings{m_hrtf};
            IPLBinauralEffect effect = nullptr;
            if (m_api.binauralCreate(m_context, &audio, &effectSettings, &effect) != IPL_STATUS_SUCCESS) {
                return "iplBinauralEffectCreate failed";
            }
            m_pool.push_back(std::make_unique<Voice>(m_api, effect, m_hrtf));
        }
        return {};
    }

    // Hands the effects to a renderer; the engine keeps the context and HRTF they use.
    std::vector<std::unique_ptr<SpatialAudioMix::Binaural>> TakePool() {
        std::vector<std::unique_ptr<SpatialAudioMix::Binaural>> pool;
        for (auto& voice : m_pool) pool.push_back(std::move(voice));
        m_pool.clear();
        return pool;
    }

private:
    const Api& m_api;
    IPLContext m_context = nullptr;
    IPLHRTF m_hrtf = nullptr;
    std::vector<std::unique_ptr<Voice>> m_pool;
};

}  // namespace SpatialAudioSteam
