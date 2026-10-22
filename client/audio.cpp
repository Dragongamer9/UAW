#include "audio.hpp"

#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#include "dr_libs/dr_mp3.h"
#include "dr_libs/dr_wav.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace uaw {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

bool Audio::decode(const std::vector<uint8_t>& bytes, int rate, std::vector<float>& out, int& channels) {
    unsigned int ch = 0, srcRate = 0;
    drwav_uint64 frames = 0;
    float* pcm = nullptr;
    bool mp3 = false;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "RIFF", 4) == 0) {
        pcm = drwav_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &ch, &srcRate, &frames, nullptr);
    } else {
        drmp3_config cfg{};
        drmp3_uint64 n = 0;
        pcm = drmp3_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &cfg, &n, nullptr);
        ch = cfg.channels;
        srcRate = cfg.sampleRate;
        frames = n;
        mp3 = true;
    }
    if (!pcm || ch == 0 || srcRate == 0 || frames == 0) {
        if (pcm) mp3 ? drmp3_free(pcm, nullptr) : drwav_free(pcm, nullptr);
        return false;
    }
    channels = int(std::min(ch, 2u));
    // Linear resampling to the device rate (game sounds are mostly 22 or 44 kHz).
    const double step = double(srcRate) / double(rate);
    const size_t outFrames = size_t(double(frames) / step);
    out.assign(outFrames * size_t(channels), 0.f);
    for (size_t i = 0; i < outFrames; ++i) {
        const double src = double(i) * step;
        const size_t a = std::min(size_t(src), size_t(frames - 1)), b = std::min(a + 1, size_t(frames - 1));
        const float t = float(src - double(a));
        for (int c = 0; c < channels; ++c)
            out[i * size_t(channels) + size_t(c)] = pcm[a * ch + size_t(c)] * (1 - t) + pcm[b * ch + size_t(c)] * t;
    }
    mp3 ? drmp3_free(pcm, nullptr) : drwav_free(pcm, nullptr);
    return true;
}

Audio::Audio(alamo::MegVfs& vfs, const alamo::SfxLibrary& library) : vfs_(vfs), lib_(library) {
    // Every sound file the game has, by name: samples are named without folders.
    for (const std::string& p : vfs_.list()) {
        const std::string u = upper(p);
        if (u.size() > 4 && (u.compare(u.size() - 4, 4, ".WAV") == 0 || u.compare(u.size() - 4, 4, ".MP3") == 0))
            files_.emplace(upper(baseName(p)), p);
    }
    if (std::getenv("UAW_NO_AUDIO")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "audio: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        std::fprintf(stderr, "audio: %s\n", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(stream_);
}

Audio::~Audio() {
    if (music_) closeMusic(*music_);
    if (fading_) closeMusic(*fading_);
    if (stream_) SDL_DestroyAudioStream(stream_);
}

void Audio::closeMusic(Music& m) {
    if (m.decoder) {
        drmp3_uninit(static_cast<drmp3*>(m.decoder));
        delete static_cast<drmp3*>(m.decoder);
        m.decoder = nullptr;
    }
}

void Audio::playMusic(const alamo::MusicEvent* e) {
    if (e == requestedMusic_) return; // already playing it (or it can't be played)
    requestedMusic_ = e;
    if (fading_) closeMusic(*fading_);
    fading_ = std::move(music_);
    if (fading_) fading_->fadeRate = 1.f / (2.f * kRate); // two seconds
    if (!e || e->files.empty()) return;
    std::uniform_int_distribution<size_t> pick(0, e->files.size() - 1);
    const std::string name = upper(e->files[pick(rng_)]);
    auto f = files_.find(name);
    if (f == files_.end()) {
        ++missing_;
        if (std::getenv("UAW_AUDIO_LOG")) std::fprintf(stderr, "audio: no music %s\n", name.c_str());
        return;
    }
    auto m = std::make_unique<Music>();
    try { m->bytes = vfs_.read(f->second); } catch (const std::exception&) { return; }
    auto* dec = new drmp3;
    if (!drmp3_init_memory(dec, m->bytes.data(), m->bytes.size(), nullptr)) {
        delete dec;
        return;
    }
    m->decoder = dec;
    m->step = double(dec->sampleRate) / kRate;
    m->gain = e->volume / 100.f;
    m->loop = e->loop;
    m->event = e;
    music_ = std::move(m);
}

void Audio::mixMusic(Music& m, float* out, int frames) {
    auto* dec = static_cast<drmp3*>(m.decoder);
    if (!dec) return;
    const int ch = int(dec->channels);
    for (int i = 0; i < frames; ++i) {
        // Linear resampling from the file's rate: decode a frame each time we pass one.
        while (m.frac >= 1) {
            m.prev[0] = m.next[0], m.prev[1] = m.next[1];
            float f[8] = {0};
            if (drmp3_read_pcm_frames_f32(dec, 1, f) == 0) {
                if (!m.loop || !drmp3_seek_to_pcm_frame(dec, 0) || drmp3_read_pcm_frames_f32(dec, 1, f) == 0) {
                    closeMusic(m);
                    return;
                }
            }
            m.next[0] = f[0];
            m.next[1] = ch > 1 ? f[1] : f[0];
            m.frac -= 1;
        }
        const float t = float(m.frac), g = m.gain * m.fade * musicVolume;
        out[i * 2] += (m.prev[0] * (1 - t) + m.next[0] * t) * g;
        out[i * 2 + 1] += (m.prev[1] * (1 - t) + m.next[1] * t) * g;
        m.frac += m.step;
        if (m.fadeRate > 0 && (m.fade -= m.fadeRate) <= 0) {
            closeMusic(m);
            return;
        }
    }
}

void Audio::setListener(const alamo::Vec3& at, float yaw) {
    listener_ = at;
    yaw_ = yaw;
}

const Audio::Clip* Audio::clip(const std::string& name, bool localize) {
    const std::string key = upper(name);
    if (auto it = clips_.find(key); it != clips_.end()) return it->second.get();
    // The name as given, then (speech) the English recording of it, as .wav or .mp3.
    std::vector<std::string> tries{key};
    const size_t dot = key.rfind('.');
    const std::string stem = dot == std::string::npos ? key : key.substr(0, dot);
    for (const char* ext : {".WAV", ".MP3"}) tries.push_back(stem + ext);
    if (localize)
        for (const char* ext : {".MP3", ".WAV"}) tries.push_back(stem + "_ENG" + ext);
    std::unique_ptr<Clip> c;
    for (const std::string& t : tries) {
        auto f = files_.find(t);
        if (f == files_.end()) continue;
        try {
            auto made = std::make_unique<Clip>();
            if (decode(vfs_.read(f->second), kRate, made->data, made->channels)) { c = std::move(made); break; }
        } catch (const std::exception&) {
        }
    }
    if (!c) {
        ++missing_;
        static const bool log = std::getenv("UAW_AUDIO_LOG") != nullptr;
        if (log) std::fprintf(stderr, "audio: no sample %s\n", name.c_str());
    }
    return (clips_[key] = std::move(c)).get();
}

void Audio::place(Voice& v) const {
    if (!v.placed) return;
    // Volume falls off past the event's saturation distance from the listener; pan by which side
    // of the view it is on.
    const float dx = v.at[0] - listener_[0], dy = v.at[1] - listener_[1];
    const float d = std::hypot(dx, dy) + 60.f; // the listener sits a little above the ground
    const float sat = v.event && v.event->saturationDistance > 0 ? v.event->saturationDistance : 200.f;
    float g = d <= sat ? 1.f : std::pow(sat / d, 1.2f);
    if (g < 0.02f) g = 0;
    v.gain *= g;
    const float rx = std::cos(yaw_), ry = -std::sin(yaw_); // the camera's right
    v.pan = std::clamp((dx * rx + dy * ry) / std::max(d, 1.f), -1.f, 1.f) * 0.75f;
}

bool Audio::play(const std::string& eventName, const alamo::Vec3* at) {
    const alamo::SfxEvent* e = lib_.find(eventName);
    if (!e || e->samples.empty()) return false;
    std::uniform_real_distribution<float> u01(0.f, 1.f);
    if (e->probability < 100 && u01(rng_) * 100 >= e->probability) return false;
    // Instances: a unit reply cuts off the one before; other events are capped.
    int same = 0;
    for (auto it = voices_.begin(); it != voices_.end();) {
        if (e->unitResponse && it->event && it->event->unitResponse) { it = voices_.erase(it); continue; }
        same += it->event == e;
        ++it;
    }
    if (e->maxInstances > 0 && same >= e->maxInstances) return false;
    if (voices_.size() >= 48) return false;

    Voice v;
    v.event = e;
    for (const std::string& s : e->preSamples)
        if (const Clip* c = clip(s, e->localize)) v.clips.push_back(c);
    size_t pick = 0;
    if (e->playSequentially) pick = sequence_[e->name]++ % e->samples.size();
    else pick = size_t(u01(rng_) * float(e->samples.size())) % e->samples.size();
    const Clip* main = clip(e->samples[pick], e->localize);
    if (!main) return false;
    v.clips.push_back(main);
    for (const std::string& s : e->postSamples)
        if (const Clip* c = clip(s, e->localize)) v.clips.push_back(c);
    v.gain = (e->minVolume + (e->maxVolume - e->minVolume) * u01(rng_)) / 100.f;
    v.rate = (e->minPitch + (e->maxPitch - e->minPitch) * u01(rng_)) / 100.f;
    v.delay = long((e->minPredelay + (e->maxPredelay - e->minPredelay) * u01(rng_)) * kRate);
    if (at && e->is3D) {
        v.placed = true;
        v.at = *at;
        place(v);
        if (v.gain <= 0) return false;
    }
    voices_.push_back(std::move(v));
    return true;
}

void Audio::update() {
    if (!stream_) {
        voices_.clear();
        return;
    }
    if (fading_ && !fading_->decoder) fading_.reset();
    // Keep about a tenth of a second queued.
    const int queued = SDL_GetAudioStreamQueued(stream_) / int(sizeof(float) * 2);
    const int want = kRate / 10 - queued;
    if (want <= 0) return;
    mix_.assign(size_t(want) * 2, 0.f);
    for (auto it = voices_.begin(); it != voices_.end();) {
        Voice& v = *it;
        const float l = v.gain * std::min(1.f, 1 - v.pan) * volume, r = v.gain * std::min(1.f, 1 + v.pan) * volume;
        bool done = false;
        for (int i = 0; i < want && !done; ++i) {
            if (v.delay > 0) { --v.delay; continue; }
            const Clip* c = v.clips[v.index];
            const size_t n = c->frames();
            const size_t a = size_t(v.pos);
            if (a >= n) {
                if (++v.index >= v.clips.size()) { done = true; break; }
                v.pos = 0;
                --i;
                continue;
            }
            const size_t b = std::min(a + 1, n - 1);
            const float t = float(v.pos - double(a));
            if (c->channels == 1) {
                const float s = c->data[a] * (1 - t) + c->data[b] * t;
                mix_[size_t(i) * 2] += s * l;
                mix_[size_t(i) * 2 + 1] += s * r;
            } else {
                mix_[size_t(i) * 2] += (c->data[a * 2] * (1 - t) + c->data[b * 2] * t) * l;
                mix_[size_t(i) * 2 + 1] += (c->data[a * 2 + 1] * (1 - t) + c->data[b * 2 + 1] * t) * r;
            }
            v.pos += v.rate;
        }
        it = done ? voices_.erase(it) : std::next(it);
    }
    if (music_) mixMusic(*music_, mix_.data(), want);
    if (fading_) mixMusic(*fading_, mix_.data(), want);
    for (float& s : mix_) s = std::clamp(s, -1.f, 1.f);
    SDL_PutAudioStreamData(stream_, mix_.data(), int(mix_.size() * sizeof(float)));
}

} // namespace uaw
