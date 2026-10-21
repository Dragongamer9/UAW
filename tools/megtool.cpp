// megtool: inspect and extract Universe at War .MEG archives.
//   megtool list    <archive.meg>
//   megtool extract <archive.meg> <outdir>
//   megtool cat     <archive.meg> <path-in-archive>
//   megtool verify  <archive.meg>        (checks stored CRCs against filenames)
#include "alamo/meg.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

static int usage() {
    std::cerr << "usage:\n"
                 "  megtool list    <archive.meg>\n"
                 "  megtool extract <archive.meg> <outdir>\n"
                 "  megtool cat     <archive.meg> <path-in-archive>\n"
                 "  megtool verify  <archive.meg>\n";
    return 2;
}

// Turn DATA\XML\FOO.XML into a safe relative path; refuse anything escaping outdir.
static bool safeRelative(const std::string& name, fs::path& out) {
    fs::path p;
    std::string part;
    auto flush = [&]() -> bool {
        if (part.empty() || part == ".") { part.clear(); return true; }
        if (part == "..") return false;
        p /= part;
        part.clear();
        return true;
    };
    for (char c : name) {
        if (c == '\\' || c == '/') { if (!flush()) return false; }
        else if (c == ':' ) return false;
        else part += c;
    }
    if (!flush() || p.empty()) return false;
    out = p;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    try {
        alamo::MegArchive meg(argv[2]);

        if (cmd == "list") {
            for (const auto& e : meg.entries())
                std::printf("%10u  %08X  %s\n", e.size, e.crc, e.name.c_str());
            std::printf("%zu files\n", meg.entries().size());
            return 0;
        }
        if (cmd == "verify") {
            size_t bad = 0;
            for (const auto& e : meg.entries()) {
                if (alamo::crc32(alamo::normalizePath(e.name)) != e.crc) {
                    std::printf("CRC mismatch: %s\n", e.name.c_str());
                    ++bad;
                }
            }
            std::printf("%zu files, %zu CRC mismatches\n", meg.entries().size(), bad);
            return bad ? 1 : 0;
        }
        if (cmd == "cat" && argc == 4) {
            const auto* e = meg.find(argv[3]);
            if (!e) { std::cerr << "not found: " << argv[3] << "\n"; return 1; }
            auto data = meg.read(*e);
            std::fwrite(data.data(), 1, data.size(), stdout);
            return 0;
        }
        if (cmd == "extract" && argc == 4) {
            const fs::path outDir = argv[3];
            size_t n = 0, skipped = 0;
            for (const auto& e : meg.entries()) {
                fs::path rel;
                if (!safeRelative(e.name, rel)) {
                    std::cerr << "skipping unsafe path: " << e.name << "\n";
                    ++skipped;
                    continue;
                }
                const fs::path dst = outDir / rel;
                fs::create_directories(dst.parent_path());
                auto data = meg.read(e);
                std::ofstream o(dst, std::ios::binary);
                o.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
                ++n;
            }
            std::printf("extracted %zu files to %s", n, outDir.string().c_str());
            if (skipped) std::printf(" (%zu skipped)", skipped);
            std::printf("\n");
            return 0;
        }
        return usage();
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
