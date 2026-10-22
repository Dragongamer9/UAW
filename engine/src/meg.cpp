#include "alamo/meg.hpp"

#include <algorithm>
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

MegVfs::Layer& MegVfs::top() {
    if (layers_.empty()) layers_.push_back({"base", {}, {}});
    return layers_.back();
}

void MegVfs::mount(const std::filesystem::path& megPath) {
    archives_.push_back(std::make_unique<MegArchive>(megPath));
    MegArchive* a = archives_.back().get();
    Layer& l = top();
    for (const auto& e : a->entries()) l.index[normalizePath(e.name)] = {a, &e};
}

void MegVfs::setLooseRoot(const std::filesystem::path& root) {
    if (layers_.empty()) layers_.push_back({"base", {}, {}});
    layers_.front().loose = root;
}

size_t MegVfs::addLayer(const std::filesystem::path& looseRoot, std::string name) {
    layers_.push_back({std::move(name), looseRoot, {}});
    return layers_.size() - 1;
}

size_t MegVfs::fileCount() const {
    size_t n = 0;
    for (const Layer& l : layers_) n += l.index.size();
    return n;
}

namespace {
// Loose files on Linux: walk components case-insensitively. A folder copied from Windows
// can hold the same directory twice in different case (DATA and Data); every match is
// followed, so `all` collects each existing path in directory order.
void findLooseAll(const std::filesystem::path& cur, const std::vector<std::string>& parts, size_t i,
                  std::vector<std::filesystem::path>& all) {
    if (i == parts.size()) { all.push_back(cur); return; }
    std::error_code ec;
    std::vector<std::filesystem::path> hits;
    for (const auto& d : std::filesystem::directory_iterator(cur, ec))
        if (normalizePath(d.path().filename().string()) == parts[i]) hits.push_back(d.path());
    std::sort(hits.begin(), hits.end());
    for (const auto& h : hits) findLooseAll(h, parts, i + 1, all);
}
std::vector<std::filesystem::path> findLooseAll(const std::filesystem::path& root, std::string_view name) {
    std::vector<std::string> parts;
    const std::string norm = normalizePath(name);
    size_t start = 0;
    while (start <= norm.size()) {
        size_t end = norm.find('\\', start);
        std::string part = norm.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? norm.size() + 1 : end + 1;
        if (!part.empty()) parts.push_back(std::move(part));
    }
    std::vector<std::filesystem::path> all;
    findLooseAll(root, parts, 0, all);
    return all;
}
} // namespace

std::vector<std::string> MegVfs::list() const {
    std::vector<std::string> out;
    for (const Layer& l : layers_)
        for (const auto& [k, v] : l.index) out.push_back(k);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> MegVfs::listDirectory(std::string_view dir) const {
    std::string prefix = normalizePath(dir);
    if (!prefix.empty() && prefix.back() != '\\') prefix += '\\';
    std::vector<std::string> out;
    for (const Layer& l : layers_) {
        for (const auto& [k, v] : l.index)
            if (k.rfind(prefix, 0) == 0) out.push_back(k);
        if (l.loose.empty()) continue;
        for (const std::filesystem::path& d : findLooseAll(l.loose, prefix.substr(0, prefix.size() - 1))) {
        std::error_code ec;
        if (!std::filesystem::is_directory(d, ec)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(d, ec); !ec && it != std::filesystem::recursive_directory_iterator();
             it.increment(ec))
            if (it->is_regular_file(ec)) out.push_back(prefix + normalizePath(std::filesystem::relative(it->path(), d, ec).generic_string()));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

namespace {
std::filesystem::path looseIn(const std::filesystem::path& root, std::string_view name) {
    if (root.empty()) return {};
    std::error_code ec;
    for (const auto& p : findLooseAll(root, name))
        if (std::filesystem::is_regular_file(p, ec)) return p;
    return {};
}
std::vector<uint8_t> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
} // namespace

std::filesystem::path MegVfs::loosePath(std::string_view name) const {
    for (auto l = layers_.rbegin(); l != layers_.rend(); ++l)
        if (auto p = looseIn(l->loose, name); !p.empty()) return p;
    return {};
}

bool MegVfs::exists(std::string_view name) const {
    const std::string key = normalizePath(name);
    for (auto l = layers_.rbegin(); l != layers_.rend(); ++l)
        if (!looseIn(l->loose, name).empty() || l->index.count(key)) return true;
    return false;
}

std::vector<uint8_t> MegVfs::read(std::string_view name) {
    const std::string key = normalizePath(name);
    for (auto l = layers_.rbegin(); l != layers_.rend(); ++l) {
        if (auto p = looseIn(l->loose, name); !p.empty()) return readFile(p);
        if (auto it = l->index.find(key); it != l->index.end()) return it->second.first->read(*it->second.second);
    }
    throw MegError("file not found: " + std::string(name));
}

std::optional<std::vector<uint8_t>> MegVfs::readInLayer(size_t layer, std::string_view name) {
    if (layer >= layers_.size()) return std::nullopt;
    Layer& l = layers_[layer];
    if (auto p = looseIn(l.loose, name); !p.empty()) return readFile(p);
    if (auto it = l.index.find(normalizePath(name)); it != l.index.end()) return it->second.first->read(*it->second.second);
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> MegVfs::readUpTo(size_t layer, std::string_view name) {
    for (size_t i = std::min(layer + 1, layers_.size()); i-- > 0;)
        if (auto d = readInLayer(i, name)) return d;
    return std::nullopt;
}

std::vector<MegVfs::Version> MegVfs::readAllLayers(std::string_view name) {
    std::vector<Version> out;
    for (size_t i = 0; i < layers_.size(); ++i)
        if (auto d = readInLayer(i, name)) out.push_back({layers_[i].name, std::move(*d)});
    return out;
}

} // namespace alamo
