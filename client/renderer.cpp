#include "renderer.hpp"

#include "alamo/assets.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace uaw {

namespace {

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec4 aBone;
layout(location = 5) in vec4 aWeight;
uniform mat4 uViewProj;
uniform mat4 uModel;
uniform mat4 uSkin[64];
uniform int uSkinned;
out vec3 vNormal;
out vec2 vUv;
out vec4 vColor;
out vec3 vWorld;
void main() {
    mat4 skin = mat4(1.0);
    if (uSkinned != 0)
        skin = aWeight.x * uSkin[int(aBone.x + 0.5)] + aWeight.y * uSkin[int(aBone.y + 0.5)] +
               aWeight.z * uSkin[int(aBone.z + 0.5)] + aWeight.w * uSkin[int(aBone.w + 0.5)];
    vec4 world = uModel * skin * vec4(aPos, 1.0);
    vWorld = world.xyz;
    vNormal = mat3(uModel) * mat3(skin) * aNormal;
    vUv = aUv;
    vColor = aColor;
    gl_Position = uViewProj * world;
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec3 vNormal;
in vec2 vUv;
in vec4 vColor;
in vec3 vWorld;
uniform sampler2D uTex;
uniform int uHasTex;
uniform vec4 uTint;
uniform vec3 uLightDir;
uniform vec3 uCamPos;
uniform vec3 uFogColor;
uniform vec4 uFogRange; // x = start, y = end
uniform int uUnlit;
uniform int uAlphaTest;
uniform sampler2D uFow;
uniform vec4 uFowParams; // xy = 1 / map size, z = 1 when fog of war is on
out vec4 fragColor;
void main() {
    vec4 base = vColor * uTint;
    if (uHasTex != 0) base *= texture(uTex, vUv);
    if (uAlphaTest != 0 && base.a < 0.5) discard;
    if (uUnlit != 0) { fragColor = base; return; }
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, -uLightDir), 0.0);
    // Two-sided-ish fill so back faces of thin geometry aren't black.
    float fill = max(dot(n, vec3(0.3, -0.4, 0.5)), 0.0) * 0.15;
    vec3 lit = base.rgb * (0.38 + 0.72 * diffuse + fill);
    if (uFowParams.z > 0.5) lit *= texture(uFow, vWorld.xy * uFowParams.xy).r;
    float dist = length(vWorld - uCamPos);
    float fog = clamp((dist - uFogRange.x) / max(uFogRange.y - uFogRange.x, 1.0), 0.0, 0.85);
    fragColor = vec4(mix(lit, uFogColor, fog), base.a);
}
)";

// Terrain: every triangle carries its three vertices' texture layers (aBone.xyz) and each vertex
// a one-hot weight (aWeight.xyz), so the layers blend smoothly across the triangle. Layers sample a
// texture array, tiled in world units and rotated per layer, and are tinted.
const char* kTerrainVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 4) in vec4 aBone;
layout(location = 5) in vec4 aWeight;
uniform mat4 uViewProj;
out vec3 vNormal;
out vec3 vWorld;
out vec3 vLayers;
out vec3 vWeights;
void main() {
    vWorld = aPos;
    vNormal = aNormal;
    vLayers = aBone.xyz;
    vWeights = aWeight.xyz;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

const char* kTerrainFragmentShader = R"(#version 330 core
in vec3 vNormal;
in vec3 vWorld;
in vec3 vLayers;
in vec3 vWeights;
uniform sampler2DArray uLayers;
uniform vec4 uLayer[64];     // x = texture slot, y = 1 / tiling, z = rotation (radians)
uniform vec4 uLayerTint[64];
uniform vec3 uLightDir;
uniform vec3 uCamPos;
uniform vec3 uFogColor;
uniform vec4 uFogRange;
uniform sampler2D uFow;
uniform vec4 uFowParams;
out vec4 fragColor;
vec3 layerColor(float index) {
    int i = int(index + 0.5);
    vec4 L = uLayer[i];
    float c = cos(L.z), s = sin(L.z);
    vec2 p = vWorld.xy * L.y;
    vec2 uv = vec2(c * p.x - s * p.y, s * p.x + c * p.y);
    return texture(uLayers, vec3(uv, L.x)).rgb * uLayerTint[i].rgb;
}
void main() {
    vec3 w = vWeights / max(vWeights.x + vWeights.y + vWeights.z, 1e-4);
    vec3 base = w.x * layerColor(vLayers.x) + w.y * layerColor(vLayers.y) + w.z * layerColor(vLayers.z);
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, -uLightDir), 0.0);
    vec3 lit = base * (0.42 + 0.7 * diffuse);
    if (uFowParams.z > 0.5) lit *= texture(uFow, vWorld.xy * uFowParams.xy).r;
    float dist = length(vWorld - uCamPos);
    float fog = clamp((dist - uFogRange.x) / max(uFogRange.y - uFogRange.x, 1.0), 0.0, 0.85);
    fragColor = vec4(mix(lit, uFogColor, fog), 1.0);
}
)";

GLuint link(const char* vsSrc, const char* fsSrc);

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        throw std::runtime_error(std::string("shader compile failed: ") + log);
    }
    return s;
}

GLuint link(const char* vsSrc, const char* fsSrc) {
    GLuint vs = compile(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsSrc);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof log, nullptr, log);
        throw std::runtime_error(std::string("shader link failed: ") + log);
    }
    return prog;
}

// Bilinear resample of an RGBA image to size x size (terrain layers share one texture array).
std::vector<uint8_t> resampled(const alamo::Image& img, int size) {
    std::vector<uint8_t> out(size_t(size) * size * 4);
    const int W = int(img.width), H = int(img.height);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float fx = (float(x) + 0.5f) * float(W) / float(size) - 0.5f, fy = (float(y) + 0.5f) * float(H) / float(size) - 0.5f;
            const int x0 = std::clamp(int(std::floor(fx)), 0, W - 1), y0 = std::clamp(int(std::floor(fy)), 0, H - 1);
            const int x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
            const float tx = std::clamp(fx - float(x0), 0.f, 1.f), ty = std::clamp(fy - float(y0), 0.f, 1.f);
            for (int k = 0; k < 4; ++k) {
                auto at = [&](int xx, int yy) { return float(img.rgba[(size_t(yy) * W + xx) * 4 + k]); };
                const float v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
                out[(size_t(y) * size + x) * 4 + k] = uint8_t(std::clamp(v + 0.5f, 0.f, 255.f));
            }
        }
    return out;
}

GpuVertex vtx(const alamo::Vec3& p, const alamo::Vec3& n, float u, float v, const alamo::Vec4& c) {
    return {p[0], p[1], p[2], n[0], n[1], n[2], u, v, c[0], c[1], c[2], c[3]};
}

float lengthOf(const alamo::Vec3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

} // namespace

// ------------------------------------------------------------------ GpuMesh

void GpuMesh::upload(const std::vector<GpuVertex>& verts, const void* indices, size_t count, bool wide) {
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(GpuVertex)), verts.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(count * (wide ? 4 : 2)), indices, GL_STATIC_DRAW);
    const GLsizei stride = sizeof(GpuVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_, stride, reinterpret_cast<void*>(offsetof(GpuVertex, px)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE_, stride, reinterpret_cast<void*>(offsetof(GpuVertex, nx)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE_, stride, reinterpret_cast<void*>(offsetof(GpuVertex, u)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE_, stride, reinterpret_cast<void*>(offsetof(GpuVertex, r)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE_, stride, reinterpret_cast<void*>(offsetof(GpuVertex, bone)));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE_, stride, reinterpret_cast<void*>(offsetof(GpuVertex, weight)));
    glBindVertexArray(0);
    indexCount = GLsizei(count);
    indexType = wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
}

void GpuMesh::draw() const {
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, indexCount, indexType, nullptr);
}

void GpuMesh::release() {
    if (ibo) glDeleteBuffers(1, &ibo);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    ibo = vbo = vao = 0;
}

// ------------------------------------------------------------------ Renderer

Renderer::Renderer(alamo::MegVfs& vfs) : vfs_(vfs) {
    GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program_, sizeof log, nullptr, log);
        throw std::runtime_error(std::string("shader link failed: ") + log);
    }
    uViewProj_ = glGetUniformLocation(program_, "uViewProj");
    uModel_ = glGetUniformLocation(program_, "uModel");
    uTint_ = glGetUniformLocation(program_, "uTint");
    uHasTex_ = glGetUniformLocation(program_, "uHasTex");
    uTex_ = glGetUniformLocation(program_, "uTex");
    uLightDir_ = glGetUniformLocation(program_, "uLightDir");
    uCamPos_ = glGetUniformLocation(program_, "uCamPos");
    uFogColor_ = glGetUniformLocation(program_, "uFogColor");
    uFogRange_ = glGetUniformLocation(program_, "uFogRange");
    uUnlit_ = glGetUniformLocation(program_, "uUnlit");
    uAlphaTest_ = glGetUniformLocation(program_, "uAlphaTest");
    uSkin_ = glGetUniformLocation(program_, "uSkin");
    uSkinned_ = glGetUniformLocation(program_, "uSkinned");

    terrainProgram_ = link(kTerrainVertexShader, kTerrainFragmentShader);

    // Unit box (-0.5..0.5 in X/Y, 0..1 in Z) for objects whose model isn't available.
    std::vector<GpuVertex> v;
    std::vector<uint16_t> idx;
    const alamo::Vec4 white{1, 1, 1, 1};
    auto face = [&](alamo::Vec3 n, alamo::Vec3 a, alamo::Vec3 b, alamo::Vec3 c, alamo::Vec3 d) {
        uint16_t base = uint16_t(v.size());
        for (auto& p : {a, b, c, d}) v.push_back(vtx(p, n, 0, 0, white));
        idx.insert(idx.end(), {base, uint16_t(base + 1), uint16_t(base + 2), base, uint16_t(base + 2), uint16_t(base + 3)});
    };
    const float h = 0.5f;
    face({0, 0, 1}, {-h, -h, 1}, {h, -h, 1}, {h, h, 1}, {-h, h, 1});
    face({1, 0, 0}, {h, -h, 0}, {h, h, 0}, {h, h, 1}, {h, -h, 1});
    face({-1, 0, 0}, {-h, h, 0}, {-h, -h, 0}, {-h, -h, 1}, {-h, h, 1});
    face({0, 1, 0}, {h, h, 0}, {-h, h, 0}, {-h, h, 1}, {h, h, 1});
    face({0, -1, 0}, {-h, -h, 0}, {h, -h, 0}, {h, -h, 1}, {-h, -h, 1});
    box_.upload(v, idx.data(), idx.size(), false);
}

Renderer::~Renderer() {
    if (overlayVbo_) glDeleteBuffers(1, &overlayVbo_);
    if (overlayVao_) glDeleteVertexArrays(1, &overlayVao_);
    terrain_.release();
    box_.release();
    for (auto& [k, m] : models_)
        if (m)
            for (auto& mesh : m->meshes) mesh.release();
    for (auto& [k, t] : textures_)
        if (t) glDeleteTextures(1, &t);
    if (program_) glDeleteProgram(program_);
    if (terrainProgram_) glDeleteProgram(terrainProgram_);
    if (terrainLayers_) glDeleteTextures(1, &terrainLayers_);
}

GLuint Renderer::texture(const std::string& name) {
    std::string key = alamo::normalizePath(name);
    if (auto it = textures_.find(key); it != textures_.end()) return it->second;
    GLuint tex = 0;
    if (auto img = alamo::loadGameTexture(vfs_, name)) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(img->width), GLsizei(img->height), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, img->rgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    } else {
        ++texturesMissing_;
    }
    textures_[key] = tex;
    return tex;
}

const GpuModel* Renderer::model(const std::string& aloName) {
    const std::string key = alamo::normalizePath(aloName);
    if (auto it = models_.find(key); it != models_.end()) return it->second && it->second->loaded ? it->second.get() : nullptr;

    auto gm = std::make_unique<GpuModel>();
    const std::string path = alamo::modelPath(aloName);
    try {
        if (!vfs_.exists(path)) throw std::runtime_error("missing");
        alamo::Model m = alamo::loadAlo(vfs_.read(path));
        if (m.kind != alamo::Model::Kind::Mesh) throw std::runtime_error("particle system");
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t mi = 0; mi < m.meshes.size(); ++mi) {
            const alamo::Mesh& mesh = m.meshes[mi];
            if (mesh.hidden || mesh.collision) continue;
            const alamo::Mat34 xf = m.boneWorld(m.meshBone(mi));
            for (const alamo::SubMesh& sm : mesh.subMeshes) {
                if (sm.vertices.empty() || sm.indices.empty()) continue;
                const std::string& shader = sm.material.shader;
                if (shader.find("Shadow") != std::string::npos) continue;
                std::vector<GpuVertex> verts;
                verts.reserve(sm.vertices.size());
                // Vertices go in rest model space; when posed, each follows its bone with
                // posed(bone) * inverse(rest(bone)): the mesh's own bone for rigid meshes, the
                // vertex's bone-map slot for skinned ones.
                // Weighted formats (alD3dVertB4I4...) blend up to four bones per vertex.
                const bool weighted = sm.vertexFormat.find("B4I4") != std::string::npos && !sm.boneMap.empty();
                std::vector<uint32_t> palette = sm.skinned() || weighted ? sm.boneMap : std::vector<uint32_t>{m.meshBone(mi)};
                if (palette.size() > 64) palette.clear(); // beyond the shader's palette: drawn in rest pose
                for (const alamo::Vertex& v : sm.vertices) {
                    alamo::Vec3 p = sm.skinned() ? v.position : alamo::transformPoint(xf, v.position);
                    alamo::Vec3 n = sm.skinned() ? v.normal : alamo::transformDir(xf, v.normal);
                    verts.push_back(vtx(p, Mat4::normalize(n), v.uv[0][0], v.uv[0][1], {1, 1, 1, 1}));
                    GpuVertex& gv = verts.back();
                    if (sm.skinned() && v.boneIndex[0] < palette.size()) {
                        gv.bone[0] = float(v.boneIndex[0]);
                    } else if (weighted) {
                        float total = 0;
                        for (int k = 0; k < 4; ++k)
                            if (v.boneIndex[k] < palette.size() && v.boneWeight[k] > 0) {
                                gv.bone[k] = float(v.boneIndex[k]);
                                gv.weight[k] = v.boneWeight[k];
                                total += v.boneWeight[k];
                            } else {
                                gv.weight[k] = 0;
                            }
                        if (total <= 0) gv.weight[0] = 1, gv.bone[0] = 0;
                        else for (float& w : gv.weight) w /= total;
                    }
                    for (int k = 0; k < 3; ++k) {
                        mn[k] = std::min(mn[k], p[k]);
                        mx[k] = std::max(mx[k], p[k]);
                    }
                }
                GpuMesh gpu;
                gpu.upload(verts, sm.indices.data(), sm.indices.size(), false);
                gpu.palette = std::move(palette);
                if (const std::string* t = sm.material.texture("BaseTexture"); t && !t->empty()) gpu.texture = texture(*t);
                if (shader.find("Additive") != std::string::npos || shader.find("Glow") != std::string::npos)
                    gpu.blend = GpuMesh::Blend::Additive;
                else if (shader.find("Alpha") != std::string::npos)
                    gpu.blend = GpuMesh::Blend::Alpha;
                for (const char* cut : {"Grass", "Leaves", "Bush", "Foliage", "Alpha_Test", "AlphaTest", "Billboard"})
                    if (shader.find(cut) != std::string::npos) gpu.alphaTest = true;
                if (auto it = sm.material.params.find("Diffuse"); it != sm.material.params.end())
                    if (auto* d = std::get_if<alamo::Vec4>(&it->second)) gpu.tint = {(*d)[0], (*d)[1], (*d)[2], 1};
                if (gpu.tint[0] + gpu.tint[1] + gpu.tint[2] < 0.05f) gpu.tint = {1, 1, 1, 1};
                // Texture named but not available (e.g. Textures.meg not installed): neutral grey, not glaring white.
                if (!gpu.texture && sm.material.texture("BaseTexture")) {
                    // A glow or shield layer without its texture would be an opaque grey shell: skip it.
                    if (gpu.blend != GpuMesh::Blend::Opaque) {
                        gpu.release();
                        continue;
                    }
                    for (int k = 0; k < 3; ++k) gpu.tint[k] *= 0.62f;
                }
                gm->meshes.push_back(gpu);
            }
        }
        if (gm->meshes.empty()) throw std::runtime_error("no visible meshes");
        for (const alamo::Proxy& px : m.proxies) {
            const alamo::Mat34 bw = px.bone < m.bones.size() ? m.boneWorld(px.bone) : alamo::Mat34{};
            gm->proxies.push_back({px.name, alamo::transformPoint(bw, {0, 0, 0}), px.hidden || (px.bone < m.bones.size() && !m.bones[px.bone].visible)});
        }
        gm->skeleton.bones = m.bones;
        gm->restInverse.reserve(m.bones.size());
        for (uint32_t b = 0; b < m.bones.size(); ++b) gm->restInverse.push_back(alamo::inverseRigid(m.boneWorld(b)));
        gm->boundsMin = {mn[0], mn[1], mn[2]};
        gm->boundsMax = {mx[0], mx[1], mx[2]};
        gm->loaded = true;
        ++modelsLoaded_;
    } catch (const std::exception& e) {
        if (std::getenv("UAW_MODEL_ERRORS")) std::fprintf(stderr, "model %s: %s\n", aloName.c_str(), e.what());
        for (auto& mesh : gm->meshes) mesh.release();
        gm.reset();
        ++modelsMissing_;
    }
    const GpuModel* result = gm.get();
    models_[key] = std::move(gm);
    return result;
}

void Renderer::setTerrain(const alamo::Map& map) {
    terrain_.release();
    if (terrainLayers_) glDeleteTextures(1, &terrainLayers_);
    terrainLayers_ = 0;
    // Layer textures, each distinct one once, in a 512x512 texture array; slot 0 is plain white
    // for layers whose texture is missing (they keep their average/guessed colour as the tint).
    constexpr int kSize = 512;
    const std::vector<alamo::Vec3> colors = alamo::terrainLayerColors(vfs_, map);
    std::vector<std::vector<uint8_t>> slots{std::vector<uint8_t>(size_t(kSize) * kSize * 4, 255)};
    std::unordered_map<std::string, int> slotOf;
    layerParams_.assign(64 * 4, 0.f);
    layerTints_.assign(64 * 4, 1.f);
    for (size_t i = 0; i < map.layers.size() && i < 64; ++i) {
        const alamo::TerrainLayer& l = map.layers[i];
        const std::string key = alamo::normalizePath(l.diffuse);
        int slot = 0;
        if (auto it = slotOf.find(key); it != slotOf.end()) {
            slot = it->second;
        } else if (auto img = alamo::loadGameTexture(vfs_, l.diffuse); img && img->width && img->height) {
            slot = int(slots.size());
            slots.push_back(resampled(*img, kSize));
            slotOf[key] = slot;
        } else {
            slotOf[key] = 0;
        }
        layerParams_[i * 4 + 0] = float(slot);
        layerParams_[i * 4 + 1] = 1.f / std::max(l.tiling > 0 ? l.tiling : 100.f, 1.f);
        layerParams_[i * 4 + 2] = l.rotation * 3.14159265f / 180.f;
        const alamo::Vec3 tint = slot ? l.tint : (i < colors.size() ? colors[i] : alamo::Vec3{0.5f, 0.5f, 0.5f});
        for (int k = 0; k < 3; ++k) layerTints_[i * 4 + k] = tint[k];
    }
    glGenTextures(1, &terrainLayers_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, terrainLayers_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GLint(GL_RGBA8), kSize, kSize, GLsizei(slots.size()), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    for (size_t i = 0; i < slots.size(); ++i)
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, GLint(i), kSize, kSize, 1, GL_RGBA, GL_UNSIGNED_BYTE, slots[i].data());
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GLint(GL_LINEAR_MIPMAP_LINEAR));
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GLint(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GLint(GL_REPEAT));
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GLint(GL_REPEAT));
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    // Unshared triangles: each vertex knows its triangle's three layers.
    auto vertexAt = [&](uint32_t x, uint32_t y) {
        const float px = float(x) * map.spacing, py = float(y) * map.spacing;
        const float hl = map.vertexHeight(x > 0 ? x - 1 : x, y), hr = map.vertexHeight(x + 1 < map.width ? x + 1 : x, y);
        const float hd = map.vertexHeight(x, y > 0 ? y - 1 : y), hu = map.vertexHeight(x, y + 1 < map.height ? y + 1 : y);
        const alamo::Vec3 n = Mat4::normalize({hl - hr, hd - hu, 2 * map.spacing});
        return vtx({px, py, map.vertexHeight(x, y)}, n, 0, 0, {1, 1, 1, 1});
    };
    auto layerAt = [&](uint32_t x, uint32_t y) { return float(std::min<uint8_t>(map.layerIndex[size_t(y) * map.width + x], 63)); };
    std::vector<GpuVertex> verts;
    verts.reserve(size_t(map.width - 1) * (map.height - 1) * 6);
    auto tri = [&](uint32_t ax, uint32_t ay, uint32_t bx, uint32_t by, uint32_t cx, uint32_t cy) {
        const float la = layerAt(ax, ay), lb = layerAt(bx, by), lc = layerAt(cx, cy);
        const std::array<std::pair<uint32_t, uint32_t>, 3> corners{{{ax, ay}, {bx, by}, {cx, cy}}};
        for (int k = 0; k < 3; ++k) {
            GpuVertex v = vertexAt(corners[size_t(k)].first, corners[size_t(k)].second);
            v.bone[0] = la, v.bone[1] = lb, v.bone[2] = lc;
            v.weight[0] = k == 0, v.weight[1] = k == 1, v.weight[2] = k == 2, v.weight[3] = 0;
            verts.push_back(v);
        }
    };
    for (uint32_t y = 0; y + 1 < map.height; ++y)
        for (uint32_t x = 0; x + 1 < map.width; ++x) {
            tri(x, y, x + 1, y, x + 1, y + 1);
            tri(x, y, x + 1, y + 1, x, y + 1);
        }
    std::vector<uint32_t> idx(verts.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = uint32_t(i);
    terrain_.upload(verts, idx.data(), idx.size(), true);
}

void Renderer::beginFrame(int width, int height, const Camera& cam, float farPlane) {
    farPlane_ = farPlane;
    camPos_ = cam.eye;
    width_ = std::max(width, 1);
    height_ = std::max(height, 1);
    viewProj_ = Mat4::perspective(cam.fovY, float(width) / float(std::max(height, 1)), 5.f, farPlane) *
                Mat4::lookAt(cam.eye, cam.target, {0, 0, 1});
    glViewport(0, 0, width, height);
    const alamo::Vec3 fog{0.62f, 0.68f, 0.75f};
    glClearColor(fog[0], fog[1], fog[2], 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE); // Alamo meshes are frequently single-sided with mixed winding
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE_);
    glUseProgram(program_);
    glUniformMatrix4fv(uViewProj_, 1, GL_FALSE_, viewProj_.data());
    const alamo::Vec3 light = Mat4::normalize({0.45f, 0.35f, -0.82f});
    lightDir_ = light;
    fogColor_ = fog;
    glUniform3fv(uLightDir_, 1, light.data());
    glUniform3fv(uCamPos_, 1, cam.eye.data());
    glUniform3fv(uFogColor_, 1, fog.data());
    const GLfloat fogRange[4] = {farPlane * 0.45f, farPlane, 0, 0}; // x = fog start, y = fog end
    glUniform4fv(uFogRange_, 1, fogRange);
    glUniform1i(uTex_, 0);
    glUniform1i(uUnlit_, 0);
    glUniform1i(uAlphaTest_, 0);
    glUniform1i(glGetUniformLocation(program_, "uFow"), 2);
    glUniform4fv(glGetUniformLocation(program_, "uFowParams"), 1, fowParams_);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fowTex_);
    glActiveTexture(GL_TEXTURE0);
    transparent_.clear();
}

void Renderer::setFogOfWar(const std::vector<uint8_t>* grid, int w, int h, float cell, uint64_t version) {
    if (!grid || w <= 0 || h <= 0 || grid->size() < size_t(w) * size_t(h)) {
        fowParams_[2] = 0;
        return;
    }
    fowParams_[0] = 1.f / (float(w) * cell);
    fowParams_[1] = 1.f / (float(h) * cell);
    fowParams_[2] = 1;
    if (fowTex_ && version == fowVersion_) return;
    fowVersion_ = version;
    // Unexplored ground nearly black, explored but unseen dimmed, seen at full light.
    std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        const uint8_t v = (*grid)[i] == 2 ? 255 : (*grid)[i] == 1 ? 120 : 28;
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v;
        px[i * 4 + 3] = 255;
    }
    if (fowTex_) glDeleteTextures(1, &fowTex_);
    fowTex_ = uploadTexture(px, w, h);
}

void Renderer::setUniforms(const Mat4& model, const alamo::Vec4& tint, GLuint tex) {
    glUniformMatrix4fv(uModel_, 1, GL_FALSE_, model.data());
    glUniform4fv(uTint_, 1, tint.data());
    glUniform1i(uHasTex_, tex ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
}

void Renderer::drawTerrain() {
    if (!terrain_.vao) return;
    glUseProgram(terrainProgram_);
    auto loc = [&](const char* n) { return glGetUniformLocation(terrainProgram_, n); };
    glUniformMatrix4fv(loc("uViewProj"), 1, GL_FALSE_, viewProj_.data());
    glUniform3fv(loc("uLightDir"), 1, lightDir_.data());
    glUniform3fv(loc("uCamPos"), 1, camPos_.data());
    glUniform3fv(loc("uFogColor"), 1, fogColor_.data());
    const GLfloat fogRange[4] = {farPlane_ * 0.45f, farPlane_, 0, 0};
    glUniform4fv(loc("uFogRange"), 1, fogRange);
    glUniform4fv(loc("uLayer"), 64, layerParams_.data());
    glUniform4fv(loc("uLayerTint"), 64, layerTints_.data());
    glUniform1i(loc("uLayers"), 1);
    glUniform1i(loc("uFow"), 2);
    glUniform4fv(loc("uFowParams"), 1, fowParams_);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fowTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, terrainLayers_);
    terrain_.draw();
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(program_);
}

void Renderer::setSkin(const GpuMesh& mesh, const Pose* pose) {
    if (!pose || mesh.palette.empty()) {
        glUniform1i(uSkinned_, 0);
        return;
    }
    std::array<float, 64 * 16> mats{};
    for (size_t i = 0; i < mesh.palette.size(); ++i) {
        const uint32_t b = mesh.palette[i];
        const Mat4 m = b < pose->size() ? Mat4::fromAlamo((*pose)[b]) : Mat4{};
        std::copy(m.m.begin(), m.m.end(), mats.begin() + long(i * 16));
    }
    glUniformMatrix4fv(uSkin_, GLsizei(mesh.palette.size()), GL_FALSE_, mats.data());
    glUniform1i(uSkinned_, 1);
}

void Renderer::drawModel(const GpuModel& m, const Mat4& transform, const alamo::Vec4& tint, const Pose* pose) {
    const alamo::Vec3 at{transform.at(0, 3), transform.at(1, 3), transform.at(2, 3)};
    const float depth = lengthOf(Mat4::sub(at, camPos_));
    std::shared_ptr<const Pose> kept;
    for (const GpuMesh& mesh : m.meshes) {
        if (mesh.blend != GpuMesh::Blend::Opaque) {
            if (pose && !kept) kept = std::make_shared<const Pose>(*pose);
            transparent_.push_back({&mesh, transform, {mesh.tint[0] * tint[0], mesh.tint[1] * tint[1], mesh.tint[2] * tint[2], tint[3]}, depth, kept});
            continue;
        }
        setUniforms(transform, {mesh.tint[0] * tint[0], mesh.tint[1] * tint[1], mesh.tint[2] * tint[2], 1}, mesh.texture);
        setSkin(mesh, pose);
        if (mesh.alphaTest) glUniform1i(uAlphaTest_, 1);
        mesh.draw();
        if (mesh.alphaTest) glUniform1i(uAlphaTest_, 0);
    }
    glUniform1i(uSkinned_, 0);
}

const alamo::Animation* Renderer::animation(const std::string& alaName) {
    const std::string key = alamo::normalizePath(alaName);
    if (auto it = animations_.find(key); it != animations_.end()) return it->second.get();
    std::unique_ptr<alamo::Animation> a;
    const std::string path = alamo::modelPath(alaName);
    try {
        if (vfs_.exists(path)) {
            a = std::make_unique<alamo::Animation>(alamo::loadAla(vfs_.read(path)));
            if (a->frameCount == 0) a.reset();
        }
    } catch (const std::exception&) {
        a.reset();
    }
    const alamo::Animation* out = a.get();
    animations_[key] = std::move(a);
    return out;
}

Pose Renderer::pose(const GpuModel& m, const alamo::Animation& anim, float seconds) const {
    const uint32_t frame = anim.frameCount ? uint32_t(std::max(0.f, seconds) * anim.fps) % anim.frameCount : 0;
    Pose out = alamo::poseModel(m.skeleton, &anim, frame);
    for (size_t b = 0; b < out.size() && b < m.restInverse.size(); ++b) {
        const alamo::Mat34& p = out[b];
        const alamo::Mat34& r = m.restInverse[b];
        alamo::Mat34 c{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 4; ++j)
                c[i * 4 + j] = p[i * 4 + 0] * r[0 * 4 + j] + p[i * 4 + 1] * r[1 * 4 + j] + p[i * 4 + 2] * r[2 * 4 + j];
            c[i * 4 + 3] += p[i * 4 + 3];
        }
        out[b] = c;
    }
    return out;
}

void Renderer::drawBox(const Mat4& transform, const alamo::Vec4& color) {
    setUniforms(transform, color, 0);
    box_.draw();
}

void Renderer::endFrame() {
    std::sort(transparent_.begin(), transparent_.end(), [](const Deferred& a, const Deferred& b) { return a.depth > b.depth; });
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE_);
    for (const Deferred& d : transparent_) {
        if (d.mesh->blend == GpuMesh::Blend::Additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        setUniforms(d.transform, d.tint, d.mesh->texture);
        setSkin(*d.mesh, d.pose.get());
        d.mesh->draw();
    }
    glUniform1i(uSkinned_, 0);
    glDepthMask(GL_TRUE_);
    glDisable(GL_BLEND);
}

void Renderer::drawOverlayTriangles(const std::vector<GpuVertex>& v, GLuint texture) {
    if (!overlayVao_) {
        glGenVertexArrays(1, &overlayVao_);
        glBindVertexArray(overlayVao_);
        glGenBuffers(1, &overlayVbo_);
        glBindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
        const GLsizei stride = sizeof(GpuVertex);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_, stride, reinterpret_cast<void*>(offsetof(GpuVertex, px)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE_, stride, reinterpret_cast<void*>(offsetof(GpuVertex, nx)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE_, stride, reinterpret_cast<void*>(offsetof(GpuVertex, u)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE_, stride, reinterpret_cast<void*>(offsetof(GpuVertex, r)));
    }
    if (v.empty()) return;
    glBindVertexArray(overlayVao_);
    glBindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * sizeof(GpuVertex)), v.data(), GL_DYNAMIC_DRAW);

    // Pixel-space orthographic projection, y down.
    Mat4 ortho;
    ortho.at(0, 0) = 2.f / float(width_);
    ortho.at(1, 1) = -2.f / float(height_);
    ortho.at(2, 2) = 0;
    ortho.at(0, 3) = -1;
    ortho.at(1, 3) = 1;
    glUniformMatrix4fv(uViewProj_, 1, GL_FALSE_, ortho.data());
    glUniform1i(uUnlit_, 1);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    setUniforms(Mat4{}, {1, 1, 1, 1}, texture);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size()));
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glUniform1i(uUnlit_, 0);
    glUniformMatrix4fv(uViewProj_, 1, GL_FALSE_, viewProj_.data());
}

void Renderer::drawParticles(const std::vector<GpuVertex>& v, GLuint texture, bool additive) {
    if (v.empty()) return;
    drawOverlayTriangles({}, 0); // makes sure the dynamic buffer exists
    if (!overlayVao_) return;
    glBindVertexArray(overlayVao_);
    glBindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * sizeof(GpuVertex)), v.data(), GL_DYNAMIC_DRAW);
    glUniformMatrix4fv(uViewProj_, 1, GL_FALSE_, viewProj_.data());
    glUniform1i(uUnlit_, 1);
    glUniform1i(uSkinned_, 0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    setUniforms(Mat4{}, {1, 1, 1, 1}, texture);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size()));
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE_);
    glUniform1i(uUnlit_, 0);
}

void Renderer::drawOverlayRect(float x0, float y0, float x1, float y1, const alamo::Vec4& c) {
    const GpuVertex a{x0, y0, 0, 0, 0, 1, 0, 0, c[0], c[1], c[2], c[3]}, b{x1, y0, 0, 0, 0, 1, 0, 0, c[0], c[1], c[2], c[3]},
                    d{x1, y1, 0, 0, 0, 1, 0, 0, c[0], c[1], c[2], c[3]}, e{x0, y1, 0, 0, 0, 1, 0, 0, c[0], c[1], c[2], c[3]};
    drawOverlayTriangles({a, b, d, a, d, e}, 0);
}

GLuint Renderer::uploadTexture(const std::vector<uint8_t>& rgba, int width, int height) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); // GL_CLAMP_TO_EDGE
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

std::vector<uint8_t> Renderer::readPixels(int width, int height) {
    std::vector<uint8_t> px(size_t(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::vector<uint8_t> flipped(px.size());
    const size_t row = size_t(width) * 4;
    for (int y = 0; y < height; ++y) std::copy_n(&px[size_t(height - 1 - y) * row], row, &flipped[size_t(y) * row]);
    return flipped;
}

} // namespace uaw
