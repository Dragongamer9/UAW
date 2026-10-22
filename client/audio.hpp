// Sound: plays the game's SFX events (alamo::SfxLibrary) through SDL3's audio stream. The samples
// (.wav, any PCM or ADPCM; .mp3) are found by file name anywhere in the game's archives and loose
// files, decoded once and resampled to the device rate. Events placed in the world fade with
// distance from the camera's focus and pan left/right; unit replies play one at a time.
#pragma once

#include "alamo/meg.hpp"
#include "alamo/sfx.hpp"
#include "alamo/alo.hpp"

#include <SDL3/SDL.h>

#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace uaw {

class Audio {
public:
    Audio(alamo::MegVfs& vfs, const alamo::SfxLibrary& library);
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool ok() const { return stream_ != nullptr; }
    float volume = 0.8f; // master

    // Where the player is listening from: the camera's focus on the ground, and its heading
    // (radians; 0 = looking along +Y) for left/right.
    void setListener(const alamo::Vec3& at, float yaw);
    // Plays an event (by SFX event name); `at` places it in the world (else it is heard as is).
    // Returns false when it can't play (unknown event, no samples found, limit reached).
    bool play(const std::string& event, const alamo::Vec3* at = nullptr);
    // Mixes what is playing into the device's queue; call once a frame.
    void update();

    // Music: plays one of the event's files (streamed), fading out what was playing; the same event
    // again leaves it alone. nullptr stops the music.
    void playMusic(const alamo::MusicEvent* event);
    float musicVolume = 0.6f;

    size_t samplesMissing() const { return missing_; }
    size_t samplesLoaded() const { return clips_.size(); }
    size_t playing() const { return voices_.size(); }

    // Decodes a .wav or .mp3 file to interleaved float frames at `rate` (exposed for tests/tools).
    static bool decode(const std::vector<uint8_t>& bytes, int rate, std::vector<float>& out, int& channels);

private:
    struct Clip {
        std::vector<float> data; // interleaved, at kRate
        int channels = 1;
        size_t frames() const { return data.size() / size_t(channels); }
    };
    struct Voice {
        std::vector<const Clip*> clips; // played in turn (pre-samples, sample, post-samples)
        size_t index = 0;
        double pos = 0;                 // frame in the current clip
        double rate = 1;                // pitch
        float gain = 1, pan = 0;
        long delay = 0;                 // frames before it starts
        const alamo::SfxEvent* event = nullptr;
        bool placed = false;
        alamo::Vec3 at{};
    };
    static constexpr int kRate = 44100;
    const Clip* clip(const std::string& name, bool localize);
    void place(Voice& v) const;

    alamo::MegVfs& vfs_;
    const alamo::SfxLibrary& lib_;
    SDL_AudioStream* stream_ = nullptr;
    std::unordered_map<std::string, std::string> files_; // upper-case file name -> path
    std::unordered_map<std::string, std::unique_ptr<Clip>> clips_;
    std::unordered_map<std::string, size_t> sequence_;   // playSequentially: next sample per event
    std::vector<Voice> voices_;
    std::vector<float> mix_;
    alamo::Vec3 listener_{};
    float yaw_ = 0;
    size_t missing_ = 0;
    std::mt19937 rng_{12345};
    // Streaming music: the file's bytes and an mp3 decoder over them; the previous piece fades out.
    struct Music {
        std::vector<uint8_t> bytes;
        void* decoder = nullptr; // drmp3*
        double step = 1, frac = 1;
        float prev[2] = {0, 0}, next[2] = {0, 0};
        float gain = 1, fade = 1, fadeRate = 0;
        bool loop = true;
        const alamo::MusicEvent* event = nullptr;
    };
    std::unique_ptr<Music> music_, fading_;
    const alamo::MusicEvent* requestedMusic_ = nullptr;
    void mixMusic(Music& m, float* out, int frames);
    static void closeMusic(Music& m);
};

} // namespace uaw
