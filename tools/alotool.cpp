// alotool: inspect and export Universe at War models.
//   alotool <game-root> check                     load every .ALO in the mounted archives
//   alotool <game-root> info   <MODEL.ALO>        bones, meshes, materials, textures
//   alotool <game-root> export <MODEL.ALO> <out.glb> [--all] [--anim X.ALA [--frame N|last]]
//   alotool <game-root> anim   <X.ALA>                frame count, fps, animated bones
//   alotool <game-root> unit   <ObjectName> <out.glb> [--all]   export a game object's land model
//   alotool <game-root> texture <NAME.tga|.dds> <out.png>
#include "alamo/ala.hpp"
#include "alamo/alo.hpp"
#include "alamo/dds.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/gltf.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>

using namespace alamo;

static int usage() {
    std::cerr << "usage:\n"
                 "  alotool <game-root> check\n"
                 "  alotool <game-root> info    <MODEL.ALO>\n"
                 "  alotool <game-root> export  <MODEL.ALO> <out.glb> [--all] [--anim X.ALA [--frame N|last]]\n"
                 "  alotool <game-root> anim    <X.ALA>\n"
                 "  alotool <game-root> unit    <ObjectName> <out.glb> [--all]\n"
                 "  alotool <game-root> texture <NAME.tga> <out.png>\n";
    return 2;
}

static std::string modelPath(std::string name) {
    if (name.find('\\') == std::string::npos && name.find('/') == std::string::npos) name = "Data\\Art\\Models\\" + name;
    return name;
}

// Materials name textures as .tga; the shipped files are usually .dds with the same stem.
static std::optional<Image> loadTexture(MegVfs& vfs, const std::string& name) {
    std::string stem = name;
    if (size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
    for (const char* ext : {".dds", ".tga"}) {
        std::string p = "Data\\Art\\Textures\\" + stem + ext;
        if (!vfs.exists(p)) continue;
        try {
            auto bytes = vfs.read(p);
            return std::string(ext) == ".dds" ? decodeDds(bytes) : decodeTga(bytes);
        } catch (const std::exception& e) {
            std::cerr << "warning: " << p << ": " << e.what() << "\n";
        }
    }
    return std::nullopt;
}

static void printParam(const ShaderParam& p) {
    if (auto* i = std::get_if<int32_t>(&p)) std::printf("%d", *i);
    else if (auto* f = std::get_if<float>(&p)) std::printf("%g", *f);
    else if (auto* v3 = std::get_if<Vec3>(&p)) std::printf("(%g, %g, %g)", (*v3)[0], (*v3)[1], (*v3)[2]);
    else if (auto* v4 = std::get_if<Vec4>(&p)) std::printf("(%g, %g, %g, %g)", (*v4)[0], (*v4)[1], (*v4)[2], (*v4)[3]);
    else if (auto* s = std::get_if<std::string>(&p)) std::printf("\"%s\"", s->c_str());
}

struct ExportFlags {
    bool all = false;
    std::string anim;
    uint32_t frame = 0;
    bool lastFrame = false;
};

static ExportFlags parseFlags(int argc, char** argv, int from) {
    ExportFlags f;
    for (int i = from; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--all") f.all = true;
        else if (a == "--anim" && i + 1 < argc) f.anim = argv[++i];
        else if (a == "--frame" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "last") f.lastFrame = true;
            else f.frame = uint32_t(std::stoul(v));
        }
    }
    return f;
}

static int exportModel(MegVfs& vfs, const std::string& path, const std::string& out, const ExportFlags& flags) {
    Model m = loadAlo(vfs.read(path));
    if (m.kind == Model::Kind::ParticleSystem) {
        std::cerr << path << " is a particle system, not a mesh\n";
        return 1;
    }
    GltfOptions opt;
    opt.includeHidden = flags.all;
    Animation anim;
    if (!flags.anim.empty()) {
        anim = loadAla(vfs.read(modelPath(flags.anim)));
        opt.animation = &anim;
        opt.frame = flags.lastFrame && anim.frameCount ? anim.frameCount - 1 : flags.frame;
        size_t matched = 0;
        for (const auto& b : m.bones) matched += anim.find(b.name) != nullptr;
        std::printf("animation %s: frame %u of %u, %zu/%zu bones matched\n", flags.anim.c_str(), opt.frame,
                    anim.frameCount, matched, m.bones.size());
    }
    GltfStats st;
    auto glb = exportGlb(m, [&](const std::string& n) { return loadTexture(vfs, n); }, opt, &st);
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), std::streamsize(glb.size()));
    std::printf("%s -> %s: %zu meshes, %zu primitives, %zu triangles, %zu textures\n", path.c_str(), out.c_str(),
                st.meshes, st.primitives, st.triangles, st.textures);
    for (const auto& t : st.missingTextures) std::printf("  missing texture: %s\n", t.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[2];
    try {
        MegVfs vfs;
        mountGame(vfs, argv[1]);

        if (cmd == "check") {
            size_t meshes = 0, particles = 0, failed = 0, warned = 0, tris = 0, anims = 0, frames = 0;
            std::map<std::string, size_t> formats;
            for (const auto& p : vfs.list()) {
                if (p.size() > 4 && p.compare(p.size() - 4, 4, ".ALA") == 0) {
                    try {
                        Animation a = loadAla(vfs.read(p));
                        ++anims;
                        frames += a.frameCount;
                        for (const auto& w : a.warnings) std::printf("warning: %s: %s\n", p.c_str(), w.c_str());
                        warned += !a.warnings.empty();
                    } catch (const std::exception& e) {
                        ++failed;
                        std::printf("FAILED: %s: %s\n", p.c_str(), e.what());
                    }
                    continue;
                }
                if (p.size() < 4 || p.compare(p.size() - 4, 4, ".ALO") != 0) continue;
                try {
                    Model m = loadAlo(vfs.read(p));
                    if (m.kind == Model::Kind::ParticleSystem) { ++particles; continue; }
                    ++meshes;
                    for (const auto& me : m.meshes)
                        for (const auto& sm : me.subMeshes) { ++formats[sm.vertexFormat]; tris += sm.indices.size() / 3; }
                    if (!m.warnings.empty()) {
                        ++warned;
                        for (const auto& w : m.warnings) std::printf("warning: %s: %s\n", p.c_str(), w.c_str());
                    }
                } catch (const std::exception& e) {
                    ++failed;
                    std::printf("FAILED: %s: %s\n", p.c_str(), e.what());
                }
            }
            std::printf("mesh models: %zu, animations: %zu (%zu frames), particle systems: %zu, with warnings: %zu, "
                        "failed: %zu, triangles: %zu\n",
                        meshes, anims, frames, particles, warned, failed, tris);
            for (const auto& [f, n] : formats) std::printf("  %6zu  %s\n", n, f.c_str());
            return failed ? 1 : 0;
        }
        if (cmd == "info" && argc > 3) {
            const std::string path = modelPath(argv[3]);
            Model m = loadAlo(vfs.read(path));
            if (m.kind == Model::Kind::ParticleSystem) { std::printf("%s: particle system\n", path.c_str()); return 0; }
            std::printf("%s\nbones: %zu\n", path.c_str(), m.bones.size());
            for (size_t i = 0; i < m.bones.size(); ++i) {
                const auto& b = m.bones[i];
                std::printf("  [%2zu] %-28s parent %3d  pos (%g, %g, %g)%s\n", i, b.name.c_str(), b.parent, b.local[3],
                            b.local[7], b.local[11], b.visible ? "" : "  hidden");
            }
            std::printf("meshes: %zu\n", m.meshes.size());
            for (size_t i = 0; i < m.meshes.size(); ++i) {
                const auto& me = m.meshes[i];
                const uint32_t bone = m.meshBone(uint32_t(i));
                std::printf("  [%2zu] %-28s on bone %-20s%s%s\n", i, me.name.c_str(),
                            bone < m.bones.size() ? m.bones[bone].name.c_str() : "?", me.hidden ? " hidden" : "",
                            me.collision ? " collision" : "");
                for (const auto& sm : me.subMeshes) {
                    std::printf("       %s: %zu verts, %zu tris, shader %s%s\n", sm.vertexFormat.c_str(),
                                sm.vertices.size(), sm.indices.size() / 3, sm.material.shader.c_str(),
                                sm.boneMap.empty() ? "" : (", " + std::to_string(sm.boneMap.size()) + " skin bones").c_str());
                    for (const auto& [k, v] : sm.material.params) {
                        std::printf("         %s = ", k.c_str());
                        printParam(v);
                        std::printf("\n");
                    }
                }
            }
            std::printf("proxies: %zu", m.proxies.size());
            for (const auto& p : m.proxies) std::printf("\n  %s on bone %u%s", p.name.c_str(), p.bone, p.hidden ? " (hidden)" : "");
            std::printf("\nlights: %zu, dazzles: %zu\n", m.lightCount, m.dazzleCount);
            for (const auto& w : m.warnings) std::printf("warning: %s\n", w.c_str());
            return 0;
        }
        if (cmd == "export" && argc > 4) return exportModel(vfs, modelPath(argv[3]), argv[4], parseFlags(argc, argv, 5));
        if (cmd == "anim" && argc > 3) {
            Animation a = loadAla(vfs.read(modelPath(argv[3])));
            std::printf("%s: %u frames at %g fps (%.2fs), %zu bones, rotation block %u, translation block %u\n", argv[3],
                        a.frameCount, a.fps, a.duration(), a.bones.size(), a.rotationBlock, a.translationBlock);
            for (const auto& b : a.bones)
                std::printf("  [%2u] %-28s rot %s  pos %s%s\n", b.index, b.name.c_str(),
                            b.rotationIndex >= 0 ? "animated" : "fixed   ", b.translationIndex >= 0 ? "animated" : "fixed   ",
                            b.visibility.empty() ? "" : "  visibility track");
            for (const auto& w : a.warnings) std::printf("warning: %s\n", w.c_str());
            return 0;
        }
        if (cmd == "unit" && argc > 4) {
            GameDatabase db;
            db.load(vfs);
            const GameObjectType* o = db.find(argv[3]);
            if (!o) { std::cerr << "no object named " << argv[3] << "\n"; return 1; }
            const std::string* model = o->find("Land_Model_Name");
            if (!model) { std::cerr << o->name << " has no Land_Model_Name\n"; return 1; }
            const std::string path = modelPath(trim(*model));
            if (!vfs.exists(path)) {
                std::cerr << o->name << " uses " << trim(*model) << ", which isn't in the mounted archives\n";
                return 1;
            }
            return exportModel(vfs, path, argv[4], parseFlags(argc, argv, 5));
        }
        if (cmd == "texture" && argc > 4) {
            auto img = loadTexture(vfs, argv[3]);
            if (!img) { std::cerr << "texture not found: " << argv[3] << "\n"; return 1; }
            auto png = encodePng(*img);
            std::ofstream(argv[4], std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
            std::printf("%s: %ux%u %s\n", argv[3], img->width, img->height, img->sourceFormat.c_str());
            return 0;
        }
        return usage();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
