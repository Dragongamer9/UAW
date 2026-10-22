// MEG v1 archive reader (Star Wars: Empire at War, Forces of Corruption,
// Universe at War: Earth Assault).
//
// Layout (all little-endian):
//   uint32 numFilenames
//   uint32 numFiles
//   numFilenames x { uint16 length; char name[length]; }
//   numFiles     x { uint32 crc; uint32 index; uint32 size; uint32 start; uint32 nameIndex; }
//   file data
//
// The file table is sorted by CRC-32 of the (uppercased, backslash) filename.
// The game merges all archives listed in MegaFiles.xml; later archives win.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace alamo {

class MegError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct MegEntry {
    std::string name;   // as stored in the archive, e.g. DATA\XML\GAMEOBJECTFILES.XML
    uint32_t crc = 0;
    uint32_t size = 0;
    uint32_t offset = 0;
};

// Standard CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320).
uint32_t crc32(std::string_view data);

// Canonical archive path: uppercase ASCII, '/' -> '\'.
std::string normalizePath(std::string_view path);

class MegArchive {
public:
    explicit MegArchive(const std::filesystem::path& path);

    const std::filesystem::path& path() const { return path_; }
    const std::vector<MegEntry>& entries() const { return entries_; }

    // Lookup by any spelling of the path (case / slash insensitive).
    const MegEntry* find(std::string_view name) const;

    std::vector<uint8_t> read(const MegEntry& entry);

private:
    std::filesystem::path path_;
    std::ifstream file_;
    uint64_t fileSize_ = 0;
    std::vector<MegEntry> entries_;
    std::unordered_map<std::string, size_t> byName_;
};

// Virtual file system in layers, mirroring the engine's lookup order. Each layer is a
// loose-file folder plus the archives mounted into it; inside a layer loose files beat
// archives and later archives beat earlier ones. Layers stack: the base game is the
// bottom layer and each mod adds one on top, searched first.
class MegVfs {
public:
    // Mounts an archive into the top layer (creating the base layer if needed).
    void mount(const std::filesystem::path& megPath);
    // Sets the base layer's loose-file folder.
    void setLooseRoot(const std::filesystem::path& root);
    // Pushes a new top layer (a mod) with its own loose folder. Returns its index.
    size_t addLayer(const std::filesystem::path& looseRoot, std::string name);

    bool exists(std::string_view name) const;
    std::vector<uint8_t> read(std::string_view name);

    // The file as each layer that has it provides it, bottom (base game) to top.
    struct Version {
        std::string layer;
        std::vector<uint8_t> data;
    };
    std::vector<Version> readAllLayers(std::string_view name);
    // Only layer `layer`'s own copy (loose or archived), or nothing.
    std::optional<std::vector<uint8_t>> readInLayer(size_t layer, std::string_view name);
    // As read(), but ignoring layers above `layer`.
    std::optional<std::vector<uint8_t>> readUpTo(size_t layer, std::string_view name);

    // Path on disk of the loose file a read would use, or empty.
    std::filesystem::path loosePath(std::string_view name) const;

    // Every archived path (normalized), sorted. Loose-only files are not included.
    std::vector<std::string> list() const;
    // Files under a directory (e.g. "Data\\Scripts\\Miscellaneous\\"), from archives and every
    // layer's loose folder, as normalized names (upper case, backslashes), sorted.
    std::vector<std::string> listDirectory(std::string_view dir) const;

    size_t archiveCount() const { return archives_.size(); }
    size_t fileCount() const;
    size_t layerCount() const { return layers_.size(); }
    const std::string& layerName(size_t i) const { return layers_[i].name; }

private:
    struct Layer {
        std::string name;
        std::filesystem::path loose;
        std::unordered_map<std::string, std::pair<MegArchive*, const MegEntry*>> index;
    };
    Layer& top();
    std::vector<Layer> layers_;
    std::vector<std::unique_ptr<MegArchive>> archives_;
};

} // namespace alamo
