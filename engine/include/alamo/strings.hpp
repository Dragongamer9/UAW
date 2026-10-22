// Localized strings: Data/Text/MasterTextFile_<LANGUAGE>.dat
//
//   uint32 count
//   count x { uint32 crc32(key); uint32 valueLength (UTF-16 units); uint32 keyLength (bytes) }
//   all values, UTF-16LE, back to back
//   all keys, ASCII, back to back
// (Verified on the retail English file: 7,945 strings, every key matches its CRC.)
#pragma once

#include "alamo/gamedata.hpp"
#include "alamo/meg.hpp"

#include <span>
#include <string>
#include <unordered_map>

namespace alamo {

class StringTable {
public:
    void parse(std::span<const uint8_t> data); // adds/replaces strings; throws MegError on malformed input
    const std::string* get(std::string_view key) const; // UTF-8, nullptr if missing
    size_t size() const { return strings_.size(); }

    // Display name for an object type: its Text_ID if translated, otherwise a tidied type name.
    std::string displayName(const GameObjectType& type) const;

private:
    std::unordered_map<std::string, std::string> strings_; // uppercase key -> UTF-8
};

// Loads MasterTextFile_<language>.dat if present (empty table otherwise).
StringTable loadStrings(MegVfs& vfs, const std::string& language = "ENGLISH");

} // namespace alamo
