// Sound effect events (Data/XML/SFXEventFiles.xml and the files it lists): what a named event
// such as "Unit_Select_Alien_Saucer" plays. Presets (Use_Preset) are copied first and the event's
// own fields override them. Sample files are looked up by name anywhere in the game's files.
#pragma once

#include "alamo/meg.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {

struct SfxEvent {
    std::string name;
    std::vector<std::string> samples;     // one is picked (in turn when playSequentially)
    std::vector<std::string> preSamples;  // played before / after the chosen sample
    std::vector<std::string> postSamples;
    float minVolume = 100, maxVolume = 100; // percent
    float minPitch = 100, maxPitch = 100;   // percent
    float probability = 100;                // percent chance that a play request sounds at all
    int maxInstances = 0;                   // playing at once (0 = no limit)
    int priority = 1;
    bool is3D = false;                      // placed in the world (otherwise heard as is)
    bool isGUI = false;
    bool unitResponse = false;              // a unit's voice reply (one at a time)
    bool playSequentially = false;
    bool localize = false;                  // speech: the file is per language
    float saturationDistance = 0;           // full volume within this distance (3D)
    float minPredelay = 0, maxPredelay = 0; // seconds
};

class SfxLibrary {
public:
    // Every layer's SFXEventFiles.xml list, each file read layer by layer; later definitions
    // replace earlier ones by name.
    void load(MegVfs& vfs);
    // Reads <SFXEvent> elements from one file's text.
    void parse(std::string_view xml);
    const SfxEvent* find(std::string_view name) const;
    size_t size() const { return events_.size(); }

private:
    std::map<std::string, SfxEvent> events_; // by upper-case name (presets included)
};

} // namespace alamo
