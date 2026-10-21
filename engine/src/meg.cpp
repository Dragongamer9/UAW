#include "alamo/meg.hpp"

#include <array>
#include <cctype>
#include <cstring>

namespace alamo {

namespace {

constexpr std::array<uint32_t, 256> makeCrcTable() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
constexpr auto kCrcTable = makeCrcTable();

uint32_t readU32(std::istream& in) {
    uint8_t b[4];
    if (!in.read(reinterpret_cast<char*>(b), 4)) throw MegError("unexpected end of file");
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}

uint16_t readU16(std::istream& in) {
    uint8_t b[2];
    if (!in.read(reinterpret_cast<char*>(b), 2)) throw MegError("unexpected end of file");
    return uint16_t(b[0] | b[1] << 8);
}

} // namespace

uint32_t crc32(std::string_view data) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : data) c = kCrcTable[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string normalizePath(std::string_view path) {
    std::string out(path);
    for (char& ch : out) {
        if (ch == '/') ch = '\\';
        else ch = char(std::toupper(static_cast<unsigned char>(ch)));
    }
    return out;
}

MegArchive::MegArchive(const std::filesystem::path& path) : path_(path) {
    file_.open(path, std::ios::binary);
    if (!file_) throw MegError("cannot open " + path.string());
    fileSize_ = std::filesystem::file_size(path);

    const uint32_t numFilenames = readU32(file_);
    const uint32_t numFiles = readU32(file_);

    // v2/v3 archives (Grey Goo and later) start with a 0xFFFFFFFF / 0x8FFFFFFF marker.
    if (numFilenames == 0xFFFFFFFFu || numFilenames == 0x8FFFFFFFu)
        throw MegError(path.string() + ": MEG v2/v3 archive, not used by Universe at War");

    // Each filename needs >= 2 bytes, each record 20 bytes: cheap sanity bound.
    if (uint64_t(numFilenames) * 2 + uint64_t(numFiles) * 20 + 8 > fileSize_)
        throw MegError(path.string() + ": header counts exceed file size (not a MEG v1?)");

    std::vector<std::string> names;
    names.reserve(numFilenames);
    for (uint32_t i = 0; i < numFilenames; ++i) {
        const uint16_t len = readU16(file_);
        std::string name(len, '\0');
        if (len && !file_.read(name.data(), len)) throw MegError("truncated filename table");
        names.push_back(std::move(name));
    }

    entries_.reserve(numFiles);
    for (uint32_t i = 0; i < numFiles; ++i) {
        MegEntry e;
        e.crc = readU32(file_);
        (void)readU32(file_); // record index
        e.size = readU32(file_);
        e.offset = readU32(file_);
        const uint32_t nameIndex = readU32(file_);
        if (nameIndex >= names.size()) throw MegError("file record references missing filename");
        if (uint64_t(e.offset) + e.size > fileSize_) throw MegError("file record points past end of archive");
        e.name = names[nameIndex];
        byName_[normalizePath(e.name)] = entries_.size();
        entries_.push_back(std::move(e));
    }
}

const MegEntry* MegArchive::find(std::string_view name) const {
    auto it = byName_.find(normalizePath(name));
    return it == byName_.end() ? nullptr : &entries_[it->second];
}

std::vector<uint8_t> MegArchive::read(const MegEntry& entry) {
    std::vector<uint8_t> data(entry.size);
    file_.clear();
    file_.seekg(entry.offset);
    if (entry.size && !file_.read(reinterpret_cast<char*>(data.data()), entry.size))
        throw MegError("failed to read " + entry.name);
    return data;
}

void MegVfs::mount(const std::filesystem::path& megPath) {
    archives_.push_back(std::make_unique<MegArchive>(megPath));
    MegArchive* a = archives_.back().get();
    for (const auto& e : a->entries()) index_[normalizePath(e.name)] = {a, &e};
}

namespace {
std::filesystem::path findLoose(const std::filesystem::path& root, std::string_view name) {
    // Loose files on Linux: walk components case-insensitively.
    std::filesystem::path cur = root;
    std::string norm = normalizePath(name);
    size_t start = 0;
    while (start <= norm.size()) {
        size_t end = norm.find('\\', start);
        std::string part = norm.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? norm.size() + 1 : end + 1;
        if (part.empty()) continue;
        std::error_code ec;
        bool found = false;
        for (const auto& d : std::filesystem::directory_iterator(cur, ec)) {
            if (normalizePath(d.path().filename().string()) == part) {
                cur = d.path();
                found = true;
                break;
            }
        }
        if (!found) return {};
    }
    return cur;
}
} // namespace

bool MegVfs::exists(std::string_view name) const {
    if (!looseRoot_.empty() && !findLoose(looseRoot_, name).empty()) return true;
    return index_.count(normalizePath(name)) != 0;
}

std::vector<uint8_t> MegVfs::read(std::string_view name) {
    if (!looseRoot_.empty()) {
        auto p = findLoose(looseRoot_, name);
        if (!p.empty() && std::filesystem::is_regular_file(p)) {
            std::ifstream in(p, std::ios::binary);
            return {std::istreambuf_iterator<char>(in), {}};
        }
    }
    auto it = index_.find(normalizePath(name));
    if (it == index_.end()) throw MegError("file not found: " + std::string(name));
    return it->second.first->read(*it->second.second);
}

} // namespace alamo
