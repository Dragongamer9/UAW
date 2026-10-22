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
uniform vec3 uSun;       // sun colour x intensity
uniform vec3 uAmbient;   // ambient + fill
uniform vec3 uCamPos;
uniform vec3 uFogColor;
uniform vec4 uFogRange; // x = start, y = end
uniform int uUnlit;
uniform int uAlphaTest;
uniform float uClipZ;      // fragments above this height are cut away (structures being built)
uniform vec4 uTeam;        // rgb = the owner's colour, w = 1 for *Colorize shaders (texture alpha = where)
uniform sampler2D uFow;
uniform vec4 uFowParams; // xy = 1 / map size, z = 1 when fog of war is on
out vec4 fragColor;
uniform sampler2DShadow uShadow;
uniform mat4 uShadowVP;
uniform vec4 uShadowParams; // x = 1 when on, y = one texel in shadow-map uv
// How much sunlight reaches this point (1 = all): 3x3 filtered lookups in the sun's depth map, fading
// out towards the map's edge so the shadowed area has no visible border.
float sunlight(vec3 world, vec3 n) {
    if (uShadowParams.x < 0.5) return 1.0;
    vec4 p = uShadowVP * vec4(world + n * 0.8, 1.0);
    vec3 q = p.xyz * 0.5 + 0.5;
    if (q.x <= 0.0 || q.y <= 0.0 || q.x >= 1.0 || q.y >= 1.0 || q.z >= 1.0) return 1.0;
    float s = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) s += texture(uShadow, vec3(q.xy + vec2(x, y) * uShadowParams.y, q.z - 0.0004));
    s /= 9.0;
    vec2 e = abs(q.xy * 2.0 - 1.0);
    return mix(s, 1.0, smoothstep(0.8, 1.0, max(e.x, e.y)));
}
void main() {
    vec4 base = vColor * uTint;
    if (uHasTex != 0) {
        vec4 texel = texture(uTex, vUv);
        if (uTeam.w > 0.5) { // player colour where the texture's alpha says so; the alpha is no transparency
            base.rgb *= mix(texel.rgb, texel.rgb * uTeam.rgb, texel.a);
        } else {
            base *= texel;
        }
    }
    if (uAlphaTest != 0 && base.a < 0.5) discard;
    if (vWorld.z > uClipZ) discard;
    if (uUnlit != 0) { fragColor = base; return; }
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, -uLightDir), 0.0);
    // Two-sided-ish fill so back faces of thin geometry aren't black.
    float fill = max(dot(n, vec3(0.3, -0.4, 0.5)), 0.0) * 0.15;
    if (diffuse > 0.0) diffuse *= sunlight(vWorld, n);
    vec3 lit = base.rgb * (uAmbient + uSun * diffuse + fill);
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
uniform vec3 uSun;
uniform vec3 uAmbient;
uniform vec3 uCamPos;
uniform vec3 uFogColor;
uniform vec4 uFogRange;
uniform sampler2D uFow;
uniform vec4 uFowParams;
out vec4 fragColor;
uniform sampler2DShadow uShadow;
uniform mat4 uShadowVP;
uniform vec4 uShadowParams; // x = 1 when on, y = one texel in shadow-map uv
// How much sunlight reaches this point (1 = all): 3x3 filtered lookups in the sun's depth map, fading
// out towards the map's edge so the shadowed area has no visible border.
float sunlight(vec3 world, vec3 n) {
    if (uShadowParams.x < 0.5) return 1.0;
    vec4 p = uShadowVP * vec4(world + n * 0.8, 1.0);
    vec3 q = p.xyz * 0.5 + 0.5;
    if (q.x <= 0.0 || q.y <= 0.0 || q.x >= 1.0 || q.y >= 1.0 || q.z >= 1.0) return 1.0;
    float s = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) s += texture(uShadow, vec3(q.xy + vec2(x, y) * uShadowParams.y, q.z - 0.0004));
    s /= 9.0;
    vec2 e = abs(q.xy * 2.0 - 1.0);
    return mix(s, 1.0, smoothstep(0.8, 1.0, max(e.x, e.y)));
}
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
    if (diffuse > 0.0) diffuse *= sunlight(vWorld, n);
    vec3 lit = base * (uAmbient + 0.04 + uSun * 0.97 * diffuse);
    if (uFowParams.z > 0.5) lit *= texture(uFow, vWorld.xy * uFowParams.xy).r;
    float dist = length(vWorld - uCamPos);
    float fog = clamp((dist - uFogRange.x) / max(uFogRange.y - uFogRange.x, 1.0), 0.0, 0.85);
    fragColor = vec4(mix(lit, uFogColor, fog), 1.0);
}
)";

GLuint link(const char* vsSrc, const char* fsSrc);

void glUniform4f4(GLint loc, const alamo::Vec4& v) { glUniform4fv(loc, 1, v.data()); }

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
    uTeam_ = glGetUniformLocation(program_, "uTeam");

    terrainProgram_ = link(kTerrainVertexShader, kTerrainFragmentShader);
    shadowsOn_ = std::getenv("UAW_NO_SHADOWS") == nullptr;

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
    if (shadowFbo_) glDeleteFramebuffers(1, &shadowFbo_);
    if (shadowTex_) glDeleteTextures(1, &shadowTex_);
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
        { // average colour (sky domes tint the distance haze with it)
            double sum[3] = {0, 0, 0};
            size_t n = 0;
            for (size_t i = 0; i + 3 < img->rgba.size(); i += 4 * 7, ++n)
                for (int k = 0; k < 3; ++k) sum[k] += img->rgba[i + size_t(k)];
            if (n) texAverage_[tex] = {float(sum[0] / n / 255), float(sum[1] / n / 255), float(sum[2] / n / 255), 1};
        }
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
            // Meshes on hidden bones (effect shells like a walker's arrival fireball) are shown only
            // by animations; drawn in rest they would be permanent.
            if (const uint32_t b = m.meshBone(mi); b < m.bones.size() && !m.bones[b].visible) continue;
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
                // Night-only layers (DayNightEffect: street-lamp light cones, lit windows): the
                // skirmish maps are daytime.
                if (auto it = sm.material.params.find("DayNightEffect"); it != sm.material.params.end()) {
                    const auto* i = std::get_if<int32_t>(&it->second);
                    const auto* f = std::get_if<float>(&it->second);
                    if ((i && *i != 0) || (f && *f != 0)) continue;
                }
                GpuMesh gpu;
                gpu.upload(verts, sm.indices.data(), sm.indices.size(), false);
                gpu.palette = std::move(palette);
                if (const std::string* t = sm.material.texture("BaseTexture"); t && !t->empty()) gpu.texture = texture(*t);
                if (shader.find("Additive") != std::string::npos || shader.find("Glow") != std::string::npos)
                    gpu.blend = GpuMesh::Blend::Additive;
                else if (shader.find("Alpha") != std::string::npos)
                    gpu.blend = GpuMesh::Blend::Alpha;
                gpu.colorize = shader.find("Colorize") != std::string::npos && gpu.blend == GpuMesh::Blend::Opaque;
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
        if (gm->meshes.empty() && m.proxies.empty()) throw std::runtime_error("no visible meshes"); // particle-only models (foo fighters) are fine
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
    camTarget_ = cam.target;
    width_ = std::max(width, 1);
    height_ = std::max(height, 1);
    viewProj_ = Mat4::perspective(cam.fovY, float(width) / float(std::max(height, 1)), 5.f, farPlane) *
                Mat4::lookAt(cam.eye, cam.target, {0, 0, 1});
    glViewport(0, 0, width, height);
    const alamo::Vec3 fog = hazeColor_;
    glClearColor(fog[0], fog[1], fog[2], 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE); // Alamo meshes are frequently single-sided with mixed winding
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE_);
    glUseProgram(program_);
    glUniformMatrix4fv(uViewProj_, 1, GL_FALSE_, viewProj_.data());
    const alamo::Vec3 light = Mat4::normalize(sunDirection_);
    lightDir_ = light;
    fogColor_ = fog;
    glUniform3fv(glGetUniformLocation(program_, "uSun"), 1, sunColor_.data());
    glUniform3fv(glGetUniformLocation(program_, "uAmbient"), 1, ambientColor_.data());
    glUniform3fv(uLightDir_, 1, light.data());
    glUniform3fv(uCamPos_, 1, cam.eye.data());
    glUniform3fv(uFogColor_, 1, fog.data());
    const GLfloat fogRange[4] = {farPlane * 0.45f, farPlane, 0, 0}; // x = fog start, y = fog end
    glUniform4fv(uFogRange_, 1, fogRange);
    glUniform1i(uTex_, 0);
    glUniform1i(uUnlit_, 0);
    glUniform1i(uAlphaTest_, 0);
    glUniform1f(glGetUniformLocation(program_, "uClipZ"), 1e9f);
    clipZ_ = 1e9f;
    glUniform4f4(uTeam_, {0, 0, 0, 0});
    team_ = {1, 1, 1, 1};
    glUniform1i(glGetUniformLocation(program_, "uFow"), 2);
    glUniform4fv(glGetUniformLocation(program_, "uFowParams"), 1, fowParams_);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fowTex_);
    bindShadow(program_);
    glActiveTexture(GL_TEXTURE0);
    transparent_.clear();
    casters_.clear();
    drawTerrainShadow_ = false;
}

void Renderer::bindShadow(GLuint program) {
    const GLfloat params[4] = {shadowReady_ ? 1.f : 0.f, 1.f / float(kShadowSize), 0, 0};
    glUniform4fv(glGetUniformLocation(program, "uShadowParams"), 1, params);
    glUniformMatrix4fv(glGetUniformLocation(program, "uShadowVP"), 1, GL_FALSE_, shadowViewProj_.data());
    glUniform1i(glGetUniformLocation(program, "uShadow"), 3);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, shadowReady_ ? shadowTex_ : 0);
}

// The sun's depth map for the next frame, from what this frame drew opaque (one frame behind, which
// nobody sees): an orthographic view along the sun's direction over the ground around the camera
// target, snapped to whole texels so shadows don't shimmer as the camera moves.
void Renderer::renderShadows() {
    shadowReady_ = false;
    if (!shadowsOn_ || (casters_.empty() && !drawTerrainShadow_)) return;
    if (!shadowFbo_) {
        glGenTextures(1, &shadowTex_);
        glBindTexture(GL_TEXTURE_2D, shadowTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(GL_DEPTH_COMPONENT24), kShadowSize, kShadowSize, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GLint(GL_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GLint(GL_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLint(GL_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLint(GL_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GLint(GL_COMPARE_REF_TO_TEXTURE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GLint(GL_LEQUAL));
        glGenFramebuffers(1, &shadowFbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTex_, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "shadows: depth framebuffer incomplete, switched off\n");
            shadowsOn_ = false;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    // Light space: a rotation only (the sun is infinitely far away), then a box around the target.
    alamo::Vec3 L = Mat4::normalize(sunDirection_);
    if (L[2] > -0.15f) L = Mat4::normalize({L[0], L[1], -0.15f}); // keep the sun above the horizon
    const alamo::Vec3 up = std::fabs(L[2]) > 0.98f ? alamo::Vec3{0, 1, 0} : alamo::Vec3{0, 0, 1};
    const Mat4 view = Mat4::lookAt({-L[0], -L[1], -L[2]}, {0, 0, 0}, up);
    const float camDist = lengthOf(Mat4::sub(camPos_, camTarget_));
    const float R = std::clamp(camDist * 1.5f, 500.f, 3000.f), D = 2500.f;
    const float texel = 2 * R / float(kShadowSize);
    float c[3];
    for (int i = 0; i < 3; ++i)
        c[i] = view.at(i, 0) * camTarget_[0] + view.at(i, 1) * camTarget_[1] + view.at(i, 2) * camTarget_[2] + view.at(i, 3);
    c[0] = std::round(c[0] / texel) * texel;
    c[1] = std::round(c[1] / texel) * texel;
    const float l = c[0] - R, r = c[0] + R, b = c[1] - R, t = c[1] + R, n = -c[2] - D, f = -c[2] + D;
    Mat4 ortho;
    ortho.at(0, 0) = 2 / (r - l);
    ortho.at(0, 3) = -(r + l) / (r - l);
    ortho.at(1, 1) = 2 / (t - b);
    ortho.at(1, 3) = -(t + b) / (t - b);
    ortho.at(2, 2) = -2 / (f - n);
    ortho.at(2, 3) = -(f + n) / (f - n);
    shadowViewProj_ = ortho * view;

    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glViewport(0, 0, kShadowSize, kShadowSize);
    glColorMask(GL_FALSE_, GL_FALSE_, GL_FALSE_, GL_FALSE_);
    glDepthMask(GL_TRUE_);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 3.f);
    glUseProgram(program_);
    glUniformMatrix4fv(uViewProj_, 1, GL_FALSE_, shadowViewProj_.data());
    glUniform1i(uUnlit_, 1);
    const GLfloat off[4] = {0, 0, 0, 0};
    glUniform4fv(glGetUniformLocation(program_, "uShadowParams"), 1, off);
    const GLint clip = glGetUniformLocation(program_, "uClipZ");
    if (drawTerrainShadow_ && terrain_.vao) { // hills shade the ground behind them
        glUniform1f(clip, 1e9f);
        glUniform1i(uSkinned_, 0);
        setUniforms(Mat4{}, {1, 1, 1, 1}, 0);
        terrain_.draw();
    }
    for (const Caster& k : casters_) {
        glUniform1f(clip, k.clipZ);
        setUniforms(k.transform, {1, 1, 1, 1}, k.mesh->alphaTest ? k.mesh->texture : 0);
        setSkin(*k.mesh, k.pose.get());
        glUniform1i(uAlphaTest_, k.mesh->alphaTest ? 1 : 0);
        k.mesh->draw();
    }
    glUniform1i(uAlphaTest_, 0);
    glUniform1i(uSkinned_, 0);
    glUniform1f(clip, 1e9f);
    glUniform1i(uUnlit_, 0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE_, GL_TRUE_, GL_TRUE_, GL_TRUE_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glUniformMatrix4fv(uViewProj_, 1, GL_FALSE_, viewProj_.data());
    casters_.clear();
    shadowReady_ = true;
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
    glUniform3fv(loc("uSun"), 1, sunColor_.data());
    glUniform3fv(loc("uAmbient"), 1, ambientColor_.data());
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
    bindShadow(terrainProgram_);
    drawTerrainShadow_ = true;
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
        if (shadowsOn_ && tint[3] > 0.99f) {
            if (pose && !kept) kept = std::make_shared<const Pose>(*pose);
            casters_.push_back({&mesh, transform, kept, clipZ_});
        }
        setUniforms(transform, {mesh.tint[0] * tint[0], mesh.tint[1] * tint[1], mesh.tint[2] * tint[2], 1}, mesh.texture);
        setSkin(mesh, pose);
        if (mesh.alphaTest) glUniform1i(uAlphaTest_, 1);
        if (mesh.colorize) glUniform4f4(uTeam_, team_);
        mesh.draw();
        if (mesh.colorize) glUniform4f4(uTeam_, {0, 0, 0, 0});
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
    renderShadows();
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

void Renderer::setEnvironment(const alamo::MapEnvironment& e) {
    if (!e.valid) {
        sunColor_ = {0.72f, 0.72f, 0.72f};
        ambientColor_ = {0.38f, 0.38f, 0.38f};
        sunDirection_ = {0.45f, 0.35f, -0.82f};
        hazeColor_ = {0.62f, 0.68f, 0.75f};
        return;
    }
    // The map's sun and ambient, kept near the neutral look's overall brightness (the original also
    // has bump, specular and fill lights this renderer doesn't): a third of the colour comes from the map.
    for (int k = 0; k < 3; ++k) {
        const float sun = std::clamp(e.sun[k] * e.sunIntensity, 0.f, 1.5f);
        sunColor_[k] = 0.72f * (0.65f + 0.35f * sun / 0.85f);
        const float amb = e.ambient[k] + 0.5f * (e.fill1[k] + e.fill2[k]) * 0.25f;
        ambientColor_[k] = 0.38f * (0.7f + 0.3f * amb / 0.22f);
        hazeColor_[k] = 0.5f * 0.62f + 0.5f * std::clamp(e.fogColor[k] * 1.1f, 0.f, 1.f);
    }
    sunDirection_ = e.sunDirection;
}

void Renderer::setWater(const alamo::MapWater& w, float sizeX, float sizeY) {
    if (hasWater_) water_.release();
    hasWater_ = w.on;
    if (!w.on) return;
    // A sheet reaching well past the map's edges, texture tiled every 250 units.
    const float m = 4000, z = w.level;
    const alamo::Vec3 n{0, 0, 1};
    const alamo::Vec4 c{1, 1, 1, 1};
    auto at = [&](float x, float y) { return vtx({x, y, z}, n, x / 250.f, y / 250.f, c); };
    std::vector<GpuVertex> v{at(-m, -m), at(sizeX + m, -m), at(sizeX + m, sizeY + m), at(-m, sizeY + m)};
    const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    water_.upload(v, idx, 6, false);
    waterTexture_ = w.texture.empty() ? 0 : texture(w.texture);
    // Murky blue-green, tinted by the map's water colour.
    const alamo::Vec4 base{0.32f, 0.44f, 0.5f, 0.72f};
    for (int k = 0; k < 3; ++k) waterTint_[k] = base[k] * std::clamp(w.color[size_t(k)], 0.2f, 1.f) * (waterTexture_ ? 1.6f : 1.f);
    waterTint_[3] = base[3];
}

void Renderer::drawWater() {
    if (!hasWater_) return;
    glUseProgram(program_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE_);
    setUniforms(Mat4::translate(0, 0, 0), waterTint_, waterTexture_);
    glUniform1i(uSkinned_, 0);
    water_.draw();
    glDepthMask(GL_TRUE_);
    glDisable(GL_BLEND);
}

void Renderer::drawSky(const GpuModel& m) {
    glUseProgram(program_);
    // The haze in the distance takes the sky's colour (the dome's textures, averaged), so far terrain
    // fades into this map's sky rather than a fixed grey-blue.
    {
        float sum[3] = {0, 0, 0}, n = 0;
        for (const GpuMesh& mesh : m.meshes)
            if (auto it = texAverage_.find(mesh.texture); it != texAverage_.end() && mesh.blend != GpuMesh::Blend::Additive) {
                for (int k = 0; k < 3; ++k) sum[k] += it->second[k] * mesh.tint[k];
                n += 1;
            }
        if (n > 0) {
            for (int k = 0; k < 3; ++k) fogColor_[k] = 0.3f * fogColor_[k] + 0.7f * std::clamp(sum[k] / n, 0.f, 1.f);
            glUniform3fv(uFogColor_, 1, fogColor_.data());
        }
    }
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE_);
    glEnable(GL_BLEND);
    glUniform1i(uUnlit_, 1);
    // Centred on the camera so it never comes closer; sized to stay inside the far plane.
    const float ext = std::max({m.boundsMax[0] - m.boundsMin[0], m.boundsMax[1] - m.boundsMin[1], 1.f});
    const float scale = farPlane_ * 0.9f / (ext * 0.5f);
    const Mat4 xf = Mat4::translate(camPos_[0], camPos_[1], camPos_[2] - farPlane_ * 0.05f) * Mat4::scale(scale);
    for (const GpuMesh& mesh : m.meshes) {
        if (mesh.blend == GpuMesh::Blend::Additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        setUniforms(xf, mesh.tint, mesh.texture);
        glUniform1i(uSkinned_, 0);
        mesh.draw();
    }
    glUniform1i(uUnlit_, 0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE_);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::setClipHeight(float z) {
    clipZ_ = z;
    glUseProgram(program_);
    glUniform1f(glGetUniformLocation(program_, "uClipZ"), z);
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
