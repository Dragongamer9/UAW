#include "alamo/strings.hpp"

#include <cctype>
#include <cstring>

namespace alamo {

namespace {

uint32_t u32(std::span<const uint8_t> d, size_t off) {
    if (off + 4 > d.size()) throw MegError("string table truncated");
    uint32_t v;
    std::memcpy(&v, d.data() + off, 4);
    return v;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}

std::string upper(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = char(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

} // namespace

void StringTable::parse(std::span<const uint8_t> d) {
    strings_.clear();
    const uint32_t n = u32(d, 0);
    if (uint64_t(n) * 12 + 4 > d.size()) throw MegError("string table header larger than file");
    std::vector<std::pair<uint32_t, uint32_t>> lens(n);
    for (uint32_t i = 0; i < n; ++i) lens[i] = {u32(d, 4 + 12 * size_t(i) + 4), u32(d, 4 + 12 * size_t(i) + 8)};
    size_t off = 4 + 12 * size_t(n);
    std::vector<std::string> values(n);
    for (uint32_t i = 0; i < n; ++i) {
        const size_t bytes = size_t(lens[i].first) * 2;
        if (off + bytes > d.size()) throw MegError("string table value past end of file");
        std::string v;
        for (size_t k = 0; k < bytes; k += 2) {
            uint32_t c = uint32_t(d[off + k] | d[off + k + 1] << 8);
            if (c >= 0xD800 && c < 0xDC00 && k + 3 < bytes) { // surrogate pair
                const uint32_t lo = uint32_t(d[off + k + 2] | d[off + k + 3] << 8);
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                k += 2;
            }
            appendUtf8(v, c);
        }
        values[i] = std::move(v);
        off += bytes;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const size_t bytes = lens[i].second;
        if (off + bytes > d.size()) throw MegError("string table key past end of file");
        std::string key(reinterpret_cast<const char*>(d.data() + off), bytes);
        off += bytes;
        strings_[upper(key)] = std::move(values[i]);
    }
}

const std::string* StringTable::get(std::string_view key) const {
    auto it = strings_.find(upper(key));
    return it == strings_.end() ? nullptr : &it->second;
}

std::string StringTable::displayName(const GameObjectType& type) const {
    if (const std::string* id = type.find("Text_ID"))
        if (const std::string* s = get(trim(*id)); s && !s->empty()) return *s;
    // Fallback: "Novus_Robotic_Infantry" -> "Robotic Infantry".
    std::string n = type.name;
    for (const char* prefix : {"Novus_", "Alien_", "Masari_", "Military_"})
        if (n.size() > std::strlen(prefix) && iequals(std::string_view(n).substr(0, std::strlen(prefix)), prefix)) {
            n = n.substr(std::strlen(prefix));
            break;
        }
    for (char& c : n)
        if (c == '_') c = ' ';
    return n;
}

StringTable loadStrings(MegVfs& vfs, const std::string& language) {
    StringTable t;
    const std::string path = "Data\\Text\\MasterTextFile_" + language + ".dat";
    if (vfs.exists(path)) {
        try {
            t.parse(vfs.read(path));
        } catch (const std::exception&) {
            return StringTable{};
        }
    }
    return t;
}

} // namespace alamo
