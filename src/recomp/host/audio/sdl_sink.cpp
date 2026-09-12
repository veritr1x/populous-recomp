// sdl_sink.cpp - the mixer's output on the default playback device, through
// an SDL3 audio stream. SDL converts the mixer's rate to the device's.
#include "sink.h"

#include <SDL3/SDL.h>

#include <stdio.h>

#include <vector>
#include <functional>

namespace {
class SdlSink final : public AudioSink {
  public:
    ~SdlSink() override {
        stop();
    }
    bool start(double rate, std::function<void(float *, float *, uint32_t)> render) override {
        if (stream_)
            return true;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "[host] audio: SDL_InitSubSystem failed: %s\n", SDL_GetError());
            return false;
        }
        render_ = std::move(render);
        SDL_AudioSpec spec;
        spec.format = SDL_AUDIO_F32;
        spec.channels = 2;
        spec.freq = int(rate);
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                            &SdlSink::pull, this);
        if (!stream_) {
            fprintf(stderr, "[host] audio: no playback device: %s\n", SDL_GetError());
            return false;
        }
        SDL_AudioSpec device;
        int frames = 0;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device, &frames);
        printf("[host] audio device running: mixer %.0f Hz stereo, device %d Hz %d ch, %d frames\n",
               rate, device.freq, device.channels, frames);
        fflush(stdout);
        period_ms_ = frames > 0 && device.freq > 0 ? 1000.0 * frames / device.freq : 0;
        SDL_ResumeAudioStreamDevice(stream_);
        return true;
    }
    void stop() override {
        if (!stream_)
            return;
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        // Evidence for crackle reports: a pull that arrives later than 1.5
        // device periods after the previous one is a gap the device filled
        // with silence (or stale data) before we could render.
        printf("[host] audio sink: %llu pulls, %llu late (>1.5 periods), max gap %.1f ms, "
               "max render %.1f ms, period %.1f ms\n",
               (unsigned long long)pulls_, (unsigned long long)late_, max_gap_ms_, max_render_ms_,
               period_ms_);
        fflush(stdout);
    }
    bool running() const override {
        return stream_ != nullptr;
    }

  private:
    static void SDLCALL pull(void *userdata, SDL_AudioStream *stream, int additional, int) {
        auto *self = static_cast<SdlSink *>(userdata);
        const uint32_t frames = uint32_t(additional) / (2 * sizeof(float));
        if (!frames)
            return;
        const double now = double(SDL_GetTicksNS()) * 1e-6;
        if (self->last_pull_ms_ > 0) {
            const double gap = now - self->last_pull_ms_;
            if (gap > self->max_gap_ms_)
                self->max_gap_ms_ = gap;
            if (self->period_ms_ > 0 && gap > 1.5 * self->period_ms_)
                ++self->late_;
        }
        self->last_pull_ms_ = now;
        ++self->pulls_;
        struct RenderTimer {
            SdlSink *s;
            double t0;
            ~RenderTimer() {
                const double ms = double(SDL_GetTicksNS()) * 1e-6 - t0;
                if (ms > s->max_render_ms_)
                    s->max_render_ms_ = ms;
            }
        } timer{self, now};
        self->left_.assign(frames, 0.0f);
        self->right_.assign(frames, 0.0f);
        self->render_(self->left_.data(), self->right_.data(), frames);
        self->interleaved_.resize(size_t(frames) * 2);
        for (uint32_t i = 0; i < frames; ++i) {
            self->interleaved_[i * 2] = self->left_[i];
            self->interleaved_[i * 2 + 1] = self->right_[i];
        }
        SDL_PutAudioStreamData(stream, self->interleaved_.data(), int(frames * 2 * sizeof(float)));
    }
    SDL_AudioStream *stream_ = nullptr;
    double period_ms_ = 0, last_pull_ms_ = 0, max_gap_ms_ = 0, max_render_ms_ = 0;
    uint64_t pulls_ = 0, late_ = 0;
    std::function<void(float *, float *, uint32_t)> render_;
    std::vector<float> left_, right_, interleaved_;
};
} // namespace

std::unique_ptr<AudioSink> make_sdl_sink() {
    return std::make_unique<SdlSink>();
}
