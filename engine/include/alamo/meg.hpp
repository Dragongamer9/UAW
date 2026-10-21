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

// Virtual file system over several archives plus loose files on disk,
// mirroring the engine's lookup order (loose override > last archive > first).
class MegVfs {
public:
    void mount(const std::filesystem::path& megPath);
    void setLooseRoot(const std::filesystem::path& root) { looseRoot_ = root; }

    bool exists(std::string_view name) const;
    std::vector<uint8_t> read(std::string_view name);

    size_t archiveCount() const { return archives_.size(); }
    size_t fileCount() const { return index_.size(); }

private:
    std::filesystem::path looseRoot_;
    std::vector<std::unique_ptr<MegArchive>> archives_;
    std::unordered_map<std::string, std::pair<MegArchive*, const MegEntry*>> index_;
};

} // namespace alamo
