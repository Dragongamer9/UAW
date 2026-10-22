// Tests for ALO, ALA, TED, DDS and glTF using small hand-built files.
#include "alamo/ala.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/techtree.hpp"
#include "alamo/alo.hpp"
#include "alamo/dds.hpp"
#include "alamo/gltf.hpp"
#include "alamo/strings.hpp"
#include "alamo/ted.hpp"

#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace alamo;

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

using Bytes = std::vector<uint8_t>;

static void put32(Bytes& b, uint32_t v) { for (int s = 0; s < 32; s += 8) b.push_back(uint8_t(v >> s)); }
static void putf(Bytes& b, float f) { uint32_t v; std::memcpy(&v, &f, 4); put32(b, v); }
static void putStr(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); b.push_back(0); }

static Bytes chunk(uint32_t id, const Bytes& body, bool container = false) {
    Bytes b;
    put32(b, id);
    put32(b, uint32_t(body.size()) | (container ? 0x80000000u : 0));
    b.insert(b.end(), body.begin(), body.end());
    return b;
}
static Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes o;
    for (const auto& p : parts) o.insert(o.end(), p.begin(), p.end());
    return o;
}
static Bytes mini(uint8_t id, const Bytes& body) {
    Bytes b{id, uint8_t(body.size())};
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

static Bytes boneChunk(const std::string& name, int32_t parent, float tx) {
    Bytes data;
    put32(data, uint32_t(parent));
    put32(data, 1);
    put32(data, 0);
    float m[12] = {1, 0, 0, tx, 0, 1, 0, 0, 0, 0, 1, 0};
    for (float f : m) putf(data, f);
    Bytes nm;
    putStr(nm, name);
    return chunk(0x202, cat({chunk(0x203, nm), chunk(0x206, data)}), true);
}

static Bytes makeAlo() {
    Bytes count(128, 0);
    count[0] = 2;
    Bytes skel = chunk(0x200, cat({chunk(0x201, count), boneChunk("Root", -1, 0), boneChunk("Turret", 0, 5)}), true);

    Bytes name; putStr(name, "Box");
    Bytes info(128, 0); info[0] = 1;
    Bytes shader; putStr(shader, "MeshAlpha.fx");
    Bytes texName; putStr(texName, "BaseTexture");
    Bytes texVal; putStr(texVal, "Test.tga");
    Bytes mat = chunk(0x10100, cat({chunk(0x10101, shader), chunk(0x10105, cat({mini(1, texName), mini(2, texVal)}))}), true);

    Bytes counts(128, 0); counts[0] = 3; counts[4] = 1;
    Bytes fmt; putStr(fmt, "alD3dVertNU2");
    Bytes verts;
    for (int i = 0; i < 3; ++i) {
        Bytes v(144, 0);
        float px = float(i), py = float(i == 2), pz = 0;
        std::memcpy(&v[0], &px, 4);
        std::memcpy(&v[4], &py, 4);
        std::memcpy(&v[8], &pz, 4);
        float nz = 1;
        std::memcpy(&v[20], &nz, 4);
        verts.insert(verts.end(), v.begin(), v.end());
    }
    Bytes idx{0, 0, 1, 0, 2, 0};
    Bytes geo = chunk(0x10000, cat({chunk(0x10001, counts), chunk(0x10002, fmt), chunk(0x10007, verts), chunk(0x10004, idx)}), true);
    Bytes mesh = chunk(0x400, cat({chunk(0x401, name), chunk(0x402, info), mat, geo}), true);

    Bytes conBody;
    Bytes m0; put32(m0, 0);
    Bytes b1; put32(b1, 1);
    Bytes conn = chunk(0x600, chunk(0x602, cat({mini(2, m0), mini(3, b1)})), true);
    return cat({skel, mesh, conn});
}

static void testAlo() {
    Bytes f = makeAlo();
    Model m = loadAlo(f);
    CHECK(m.kind == Model::Kind::Mesh);
    CHECK(m.bones.size() == 2 && m.bones[1].name == "Turret" && m.bones[1].parent == 0);
    CHECK(m.meshes.size() == 1 && m.meshes[0].subMeshes.size() == 1);
    if (!m.meshes.empty() && !m.meshes[0].subMeshes.empty()) {
        const SubMesh& sm = m.meshes[0].subMeshes[0];
        CHECK(sm.vertices.size() == 3 && sm.indices.size() == 3);
        CHECK(sm.vertexFormat == "alD3dVertNU2");
        CHECK(sm.material.shader == "MeshAlpha.fx");
        CHECK(sm.material.texture("BaseTexture") && *sm.material.texture("BaseTexture") == "Test.tga");
        CHECK(sm.vertices[2].position[1] == 1.0f);
    }
    CHECK(m.meshBone(0) == 1);
    Mat34 w = m.boneWorld(1);
    CHECK(w[3] == 5.0f);
    CHECK(m.warnings.empty());

    // Corrupt inputs must throw, not crash.
    Bytes cut(f.begin(), f.begin() + 40);
    bool threw = false;
    try { loadAlo(cut); } catch (const AloError&) { threw = true; }
    CHECK(threw);
    Bytes badIdx = f;
    // find the index buffer (last 6 bytes of the geometry chunk) and point past the vertices
    for (size_t i = 0; i + 8 <= badIdx.size(); ++i) {
        uint32_t id; std::memcpy(&id, &badIdx[i], 4);
        uint32_t sz; std::memcpy(&sz, &badIdx[i + 4], 4);
        if (id == 0x10004 && sz == 6) { badIdx[i + 8] = 9; break; }
    }
    threw = false;
    try { loadAlo(badIdx); } catch (const AloError&) { threw = true; }
    CHECK(threw);
}

static Bytes ddsHeader(uint32_t w, uint32_t h, uint32_t pfFlags, const char* fourcc, uint32_t bits, uint32_t r,
                       uint32_t g, uint32_t b, uint32_t a) {
    Bytes d{'D', 'D', 'S', ' '};
    d.resize(128, 0);
    auto set = [&](size_t off, uint32_t v) { for (int s = 0; s < 4; ++s) d[off + s] = uint8_t(v >> (8 * s)); };
    set(4, 124); set(12, h); set(16, w); set(80, pfFlags);
    if (fourcc) std::memcpy(&d[84], fourcc, 4);
    set(88, bits); set(92, r); set(96, g); set(100, b); set(104, a);
    return d;
}

static void testDds() {
    // DXT1 block: c0 = pure red, c1 = pure blue, all pixels index 0 except pixel 1 = index 1.
    Bytes d = ddsHeader(4, 4, 0x4, "DXT1", 0, 0, 0, 0, 0);
    Bytes blk{0x00, 0xF8, 0x1F, 0x00, 0x04, 0, 0, 0};
    d.insert(d.end(), blk.begin(), blk.end());
    Image img = decodeDds(d);
    CHECK(img.width == 4 && img.height == 4 && img.sourceFormat == "DXT1");
    CHECK(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0 && img.rgba[3] == 255);
    CHECK(img.rgba[4] == 0 && img.rgba[6] == 255);

    // DXT5: alpha endpoints 255/0, all indices 1 -> alpha 0.
    Bytes d5 = ddsHeader(4, 4, 0x4, "DXT5", 0, 0, 0, 0, 0);
    Bytes a{255, 0, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24};
    d5.insert(d5.end(), a.begin(), a.end());
    d5.insert(d5.end(), blk.begin(), blk.end());
    Image i5 = decodeDds(d5);
    CHECK(i5.rgba[3] == 0 && i5.rgba[0] == 255);

    // Uncompressed ARGB32, 1x1, B G R A byte order.
    Bytes dr = ddsHeader(1, 1, 0x41, nullptr, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    Bytes px{10, 20, 30, 40};
    dr.insert(dr.end(), px.begin(), px.end());
    Image ir = decodeDds(dr);
    CHECK(ir.rgba[0] == 30 && ir.rgba[1] == 20 && ir.rgba[2] == 10 && ir.rgba[3] == 40);

    // Truncated data throws.
    bool threw = false;
    try { decodeDds(Bytes(d.begin(), d.begin() + 130)); } catch (const DdsError&) { threw = true; }
    CHECK(threw);

    auto png = encodePng(img);
    CHECK(png.size() > 40 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
}

static void testGltf() {
    Bytes f = makeAlo();
    Model m = loadAlo(f);
    GltfStats st;
    auto glb = exportGlb(m, [](const std::string&) -> std::optional<Image> {
        Image i; i.width = i.height = 1; i.rgba = {255, 0, 0, 255}; return i;
    }, {}, &st);
    CHECK(glb.size() > 20 && std::memcmp(glb.data(), "glTF", 4) == 0);
    uint32_t total; std::memcpy(&total, &glb[8], 4);
    CHECK(total == glb.size());
    CHECK(st.meshes == 1 && st.triangles == 1 && st.textures == 1);
    // The turret bone moves the mesh +5 on X: max X should be 7.
    uint32_t jlen; std::memcpy(&jlen, &glb[12], 4);
    std::string json(reinterpret_cast<const char*>(&glb[20]), jlen);
    CHECK(json.find("\"max\":[7,") != std::string::npos);
}

static Bytes makeAla() {
    auto u32mini = [](uint8_t id, uint32_t v) { Bytes b; put32(b, v); return mini(id, b); };
    Bytes hdrF; putf(hdrF, 30.0f);
    Bytes header = chunk(0x1001, cat({u32mini(1, 2), mini(2, hdrF), u32mini(3, 1), u32mini(0x0B, 4), u32mini(0x0C, 3)}));

    Bytes name; putStr(name, "Turret");
    Bytes off; putf(off, 5); putf(off, 0); putf(off, 0);
    Bytes scl; putf(scl, 1); putf(scl, 1); putf(scl, 1);
    Bytes idx0{0, 0};
    Bytes defRot{0, 0, 0, 0, 0, 0, 0xFF, 0x7F};
    Bytes info = chunk(0x1003, cat({mini(4, name), u32mini(5, 1), mini(6, off), mini(7, scl), mini(0x0E, idx0),
                                    mini(0x10, idx0), mini(0x11, defRot)}));
    Bytes vis{0x01}; // visible on frame 0 only
    Bytes bone = chunk(0x1002, cat({info, chunk(0x1007, vis)}), true);

    // Rotations x,y,z,w: frame 0 identity, frame 1 = 90 degrees about Z.
    auto s16 = [](Bytes& b, int16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(uint16_t(v) >> 8)); };
    Bytes rot;
    for (int16_t v : {0, 0, 0, 32767}) s16(rot, v);
    for (int16_t v : {0, 0, 23170, 23170}) s16(rot, v);
    Bytes trans;
    for (int16_t v : {0, 0, 0, 0, 2, 0}) s16(trans, v);
    return chunk(0x1000, cat({header, bone, chunk(0x1009, rot), chunk(0x100A, trans)}), true);
}

static void testAla() {
    Animation a = loadAla(makeAla());
    CHECK(a.frameCount == 2 && a.fps == 30.0f && a.bones.size() == 1);
    CHECK(a.warnings.empty());
    const AnimBone* b = a.find("turret");
    CHECK(b);
    if (!b) return;
    Vec3 p = transformPoint(a.local(*b, 1), {1, 0, 0});
    CHECK(std::fabs(p[0] - 5) < 1e-3f && std::fabs(p[1] - 3) < 1e-3f && std::fabs(p[2]) < 1e-3f);
    CHECK(a.visible(*b, 0) && !a.visible(*b, 1));

    // Inverse of a rotation+translation undoes it.
    Mat34 m = a.local(*b, 1);
    Vec3 back = transformPoint(inverseRigid(m), transformPoint(m, {0.3f, -2.f, 7.f}));
    CHECK(std::fabs(back[0] - 0.3f) < 1e-3f && std::fabs(back[1] + 2.f) < 1e-3f && std::fabs(back[2] - 7.f) < 1e-3f);

    // Posed export: the mesh rides the Turret bone, rotated 90 degrees and moved to (5, 2, 0).
    Model model = loadAlo(makeAlo());
    GltfOptions opt;
    opt.animation = &a;
    opt.frame = 1;
    opt.includeHidden = true; // frame 1 hides the bone via the visibility track
    auto glb = exportGlb(model, [](const std::string&) { return std::optional<Image>{}; }, opt);
    uint32_t jlen; std::memcpy(&jlen, &glb[12], 4);
    std::string json(reinterpret_cast<const char*>(&glb[20]), jlen);
    CHECK(json.find("\"min\":[4,") != std::string::npos);
    CHECK(json.find("\"max\":[5,") != std::string::npos);
    opt.includeHidden = false;
    GltfStats st;
    exportGlb(model, [](const std::string&) { return std::optional<Image>{}; }, opt, &st);
    CHECK(st.meshes == 0);

    // Data shorter than the header promises must throw.
    Bytes bad = makeAla();
    bad.resize(bad.size() - 4);
    bad[4] = uint8_t(bad.size() - 8); // keep the outer chunk consistent
    bad[5] = uint8_t((bad.size() - 8) >> 8);
    bool threw = false;
    try { loadAla(bad); } catch (const AloError&) { threw = true; }
    CHECK(threw);
}

static Bytes makeTed() {
    auto u32mini = [](uint8_t id, uint32_t v) { Bytes b; put32(b, v); return mini(id, b); };
    auto f32mini = [](uint8_t id, float v) { Bytes b; putf(b, v); return mini(id, b); };
    auto v3mini = [](uint8_t id, float x, float y, float z) { Bytes b; putf(b, x); putf(b, y); putf(b, z); return mini(id, b); };
    Bytes name{'T', 0, 'X', 0, 0, 0};
    Bytes header = chunk(0x00, cat({mini(0x08, name), f32mini(0x0D, 60), f32mini(0x0E, 60)}));

    // 3x3 vertices; height values 0, 512 (=25 units), -1024 (=-50 units); layer 1 in the middle.
    Bytes verts;
    for (int i = 0; i < 9; ++i) {
        int16_t h = i == 4 ? 512 : i == 8 ? -1024 : 0;
        verts.push_back(uint8_t(h));
        verts.push_back(uint8_t(uint16_t(h) >> 8));
        uint8_t layer = i == 4 ? 1 : 0;
        verts.push_back(layer);
        verts.push_back(layer);
    }
    Bytes tex0; putStr(tex0, "wm_sand");
    Bytes tex1; putStr(tex1, "wm_rock");
    Bytes layers = chunk(0x02, cat({chunk(0x03, cat({f32mini(0x07, 64), mini(0x0C, tex0)})),
                                    chunk(0x03, cat({f32mini(0x07, 32), mini(0x0C, tex1), v3mini(0x0A, 0.5f, 0.5f, 0.5f)}))}),
                         true);
    Bytes terrain = chunk(0x101, cat({chunk(0x00, cat({u32mini(0, 3), u32mini(1, 3)})), chunk(0x0D, verts), layers}), true);

    Bytes grpName; putStr(grpName, "Multiplayer");
    Bytes group = chunk(0x17, chunk(0x100, cat({mini(0x00, grpName), u32mini(0x01, 77)}), true), true);
    Bytes props = cat({u32mini(0x00, 77), u32mini(0x01, 0xDEADBEEF), u32mini(0x02, 1), f32mini(0x03, 1.5f),
                       v3mini(0x04, 20, 20, 25), v3mini(0x05, 0, 0, 90)});
    Bytes obj = chunk(0x44C, cat({chunk(0x454, Bytes{}), chunk(0x459, chunk(0x4B0, props), true)}), true);
    Bytes objects = chunk(0x102, cat({group, chunk(0x01, obj, true)}), true);
    Bytes play = chunk(0x103, cat({v3mini(0x05, 10, 10, 0), v3mini(0x06, 50, 50, 0)}));
    return cat({header, chunk(0x01, cat({terrain, objects, play}), true)});
}

static void testTed() {
    Map m = loadTed(makeTed());
    CHECK(m.nameTextId == "TX");
    CHECK(m.width == 3 && m.height == 3 && m.spacing == 20.0f);
    CHECK(m.vertexHeight(1, 1) == 25.0f && m.vertexHeight(2, 2) == -50.0f);
    CHECK(std::fabs(m.heightAt(10, 10) - 6.25f) < 1e-4f); // midway between a 0 corner and the 25 centre
    CHECK(m.layers.size() == 2 && m.layers[1].diffuse == "wm_rock" && m.layers[1].tint[0] == 0.5f);
    CHECK(m.layerIndex[4] == 1 && m.layerIndex[0] == 0);
    CHECK(m.objects.size() == 1);
    if (!m.objects.empty()) {
        const MapObject& o = m.objects[0];
        CHECK(o.typeCrc == 0xDEADBEEF && o.owner == 1 && o.scale == 1.5f && o.position[2] == 25.0f);
        CHECK(o.rotation[2] == 90.0f);
        CHECK(o.groups.size() == 1 && m.groups[o.groups[0]].name == "Multiplayer");
    }
    CHECK(m.playableMax[0] == 50.0f);
    CHECK(m.warnings.empty());

    // Vertex data that doesn't match the declared size must be rejected.
    Bytes bad = makeTed();
    for (size_t i = 0; i + 8 <= bad.size(); ++i) {
        uint32_t id, sz;
        std::memcpy(&id, &bad[i], 4);
        std::memcpy(&sz, &bad[i + 4], 4);
        if (id == 0x00 && sz == 12 && bad[i + 8] == 0 && bad[i + 9] == 4) { bad[i + 10] = 9; break; } // width -> 9
    }
    bool threw = false;
    try { loadTed(bad); } catch (const AloError&) { threw = true; }
    CHECK(threw);
}

static void testStrings() {
    // Two entries: TEXT_A = "Grunt", TEXT_B = "Ré" (non-ASCII survives as UTF-8).
    Bytes d;
    put32(d, 2);
    const std::string ka = "TEXT_A", kb = "TEXT_B";
    put32(d, uint32_t(::crc32(0, reinterpret_cast<const Bytef*>(ka.data()), uInt(ka.size()))));
    put32(d, 5);
    put32(d, uint32_t(ka.size()));
    put32(d, uint32_t(::crc32(0, reinterpret_cast<const Bytef*>(kb.data()), uInt(kb.size()))));
    put32(d, 2);
    put32(d, uint32_t(kb.size()));
    for (char c : std::string("Grunt")) { d.push_back(uint8_t(c)); d.push_back(0); }
    d.insert(d.end(), {'R', 0, 0xE9, 0});
    d.insert(d.end(), ka.begin(), ka.end());
    d.insert(d.end(), kb.begin(), kb.end());
    StringTable t;
    t.parse(d);
    CHECK(t.size() == 2);
    CHECK(t.get("text_a") && *t.get("TEXT_A") == "Grunt");
    CHECK(t.get("TEXT_B") && *t.get("TEXT_B") == "R\xC3\xA9");
    CHECK(!t.get("TEXT_C"));
    Bytes cut(d.begin(), d.begin() + 30);
    bool threw = false;
    try { t.parse(cut); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

static void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// Base game plus two mods: later mods replace objects and tech-tree entries by name,
// everything else from lower layers survives.
static void testModLayers() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "uaw_native_test_mods";
    fs::remove_all(root);
    const fs::path game = root / "Game", modA = root / "Mods" / "A_First", modB = root / "Mods" / "B_Second";
    const std::string megaFiles = "<Mega_Files></Mega_Files>";
    writeFile(game / "Data/MegaFiles.xml", megaFiles);
    writeFile(game / "Data/XML/GameObjectFiles.xml", "<Game_Object_Files><File>Units.xml</File></Game_Object_Files>");
    writeFile(game / "Data/XML/Units.xml",
              "<Units><Unit Name=\"Tank\"><Tactical_Health>100</Tactical_Health></Unit>"
              "<Unit Name=\"Jeep\"><Tactical_Health>50</Tactical_Health></Unit></Units>");
    writeFile(game / "Data/XML/Factions.xml", "<Factions><Faction Name=\"Novus\"><Is_Human_Playable>Yes</Is_Human_Playable></Faction>"
                                               "<Faction Name=\"Military\"><Is_Human_Playable>No</Is_Human_Playable></Faction></Factions>");
    writeFile(game / "Data/XML/ProductionDependencies.xml",
              "<ProductionDependencies><Dependency><Built_Object>Tank</Built_Object><Tactical_Builder>Factory</Tactical_Builder></Dependency>"
              "<Dependency><Built_Object>Jeep</Built_Object><Tactical_Builder>Factory</Tactical_Builder></Dependency></ProductionDependencies>");
    // Mod A: rebalances the tank, adds its own file and unit, makes the Military playable.
    writeFile(modA / "Data/MegaFiles.xml", megaFiles);
    writeFile(modA / "Data/XML/GameObjectFiles.xml",
              "<Game_Object_Files><File>Units.xml</File><File>ModA.xml</File></Game_Object_Files>");
    writeFile(modA / "Data/XML/Units.xml", "<Units><Unit Name=\"Tank\"><Tactical_Health>150</Tactical_Health></Unit></Units>");
    writeFile(modA / "Data/XML/ModA.xml", "<Units><Unit Name=\"Sniper\"><Tactical_Health>40</Tactical_Health></Unit></Units>");
    writeFile(modA / "Data/XML/Factions.xml", "<Factions><Faction Name=\"Military\"><Is_Human_Playable>Yes</Is_Human_Playable></Faction></Factions>");
    writeFile(modA / "Data/XML/ProductionDependencies.xml",
              "<ProductionDependencies><Dependency><Built_Object>Sniper</Built_Object><Tactical_Builder>Barracks</Tactical_Builder></Dependency></ProductionDependencies>");
    // Mod B: different tank stats again (top layer wins) and a new unit in its own file.
    writeFile(modB / "Data/MegaFiles.xml", megaFiles);
    writeFile(modB / "Data/XML/GameObjectFiles.xml", "<Game_Object_Files><File>Units.xml</File><File>ModB.xml</File></Game_Object_Files>");
    writeFile(modB / "Data/XML/Units.xml", "<Units><Unit Name=\"Tank\"><Tactical_Health>200</Tactical_Health></Unit></Units>");
    writeFile(modB / "Data/XML/ModB.xml", "<Units><Unit Name=\"Mech\"><Tactical_Health>300</Tactical_Health></Unit></Units>");
    writeFile(modB / "Data/XML/ProductionDependencies.xml",
              "<ProductionDependencies><Dependency><Built_Object>Tank</Built_Object><Tactical_Builder>Motor_Pool</Tactical_Builder></Dependency></ProductionDependencies>");

    const auto mods = alamo::installedMods(game);
    CHECK(mods.size() == 2 && mods[0].filename() == "A_First" && mods[1].filename() == "B_Second");
    alamo::MegVfs vfs;
    alamo::mountGame(vfs, game, mods);
    CHECK(vfs.layerCount() == 3);
    CHECK(vfs.readAllLayers("Data\\XML\\Units.xml").size() == 3);
    alamo::GameDatabase db;
    db.load(vfs);
    auto health = [&](const char* name) {
        const alamo::GameObjectType* t = db.find(name);
        return t ? t->number("Tactical_Health").value_or(-1) : -2;
    };
    CHECK(health("Tank") == 200);  // top mod wins
    CHECK(health("Jeep") == 50);   // untouched base object survives
    CHECK(health("Sniper") == 40); // mod A's addition survives mod B
    CHECK(health("Mech") == 300);
    CHECK(db.overriddenCount() == 2);
    const alamo::Faction* mil = db.faction("Military");
    CHECK(mil && mil->humanPlayable);
    CHECK(db.faction("Novus") && db.faction("Novus")->humanPlayable);
    const alamo::TechTree tech = alamo::loadTechTree(vfs);
    CHECK(tech.options("Tank").size() == 1 && tech.options("Tank")[0].builders[0] == "Motor_Pool");
    CHECK(tech.options("Jeep").size() == 1 && tech.options("Sniper").size() == 1);

    // --no-mods equivalent: base only.
    alamo::MegVfs plain;
    alamo::mountGame(plain, game, std::vector<fs::path>{});
    alamo::GameDatabase base;
    base.load(plain);
    CHECK(base.find("Tank") && base.find("Tank")->number("Tactical_Health").value_or(0) == 100);
    CHECK(!base.find("Sniper"));
    fs::remove_all(root);
}

int main() {
    testModLayers();
    testStrings();
    testTed();
    testAlo();
    testDds();
    testGltf();
    testAla();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all ALO/DDS/glTF tests passed\n");
    return 0;
}
