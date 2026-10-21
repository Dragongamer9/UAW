#include "renderer.hpp"

#include "alamo/assets.hpp"

#include <algorithm>
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
uniform mat4 uViewProj;
uniform mat4 uModel;
out vec3 vNormal;
out vec2 vUv;
out vec4 vColor;
out vec3 vWorld;
void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorld = world.xyz;
    vNormal = mat3(uModel) * aNormal;
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
out vec4 fragColor;
void main() {
    vec4 base = vColor * uTint;
    if (uHasTex != 0) base *= texture(uTex, vUv);
    if (uUnlit != 0) { fragColor = base; return; }
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, -uLightDir), 0.0);
    // Two-sided-ish fill so back faces of thin geometry aren't black.
    float fill = max(dot(n, vec3(0.3, -0.4, 0.5)), 0.0) * 0.15;
    vec3 lit = base.rgb * (0.38 + 0.72 * diffuse + fill);
    float dist = length(vWorld - uCamPos);
    float fog = clamp((dist - uFogRange.x) / max(uFogRange.y - uFogRange.x, 1.0), 0.0, 0.85);
    fragColor = vec4(mix(lit, uFogColor, fog), base.a);
}
)";

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
    terrain_.release();
    box_.release();
    for (auto& [k, m] : models_)
        if (m)
            for (auto& mesh : m->meshes) mesh.release();
    for (auto& [k, t] : textures_)
        if (t) glDeleteTextures(1, &t);
    if (program_) glDeleteProgram(program_);
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
                for (const alamo::Vertex& v : sm.vertices) {
                    alamo::Vec3 p = sm.skinned() ? v.position : alamo::transformPoint(xf, v.position);
                    alamo::Vec3 n = sm.skinned() ? v.normal : alamo::transformDir(xf, v.normal);
                    verts.push_back(vtx(p, Mat4::normalize(n), v.uv[0][0], v.uv[0][1], {1, 1, 1, 1}));
                    for (int k = 0; k < 3; ++k) {
                        mn[k] = std::min(mn[k], p[k]);
                        mx[k] = std::max(mx[k], p[k]);
                    }
                }
                GpuMesh gpu;
                gpu.upload(verts, sm.indices.data(), sm.indices.size(), false);
                if (const std::string* t = sm.material.texture("BaseTexture"); t && !t->empty()) gpu.texture = texture(*t);
                if (shader.find("Additive") != std::string::npos || shader.find("Glow") != std::string::npos)
                    gpu.blend = GpuMesh::Blend::Additive;
                else if (shader.find("Alpha") != std::string::npos)
                    gpu.blend = GpuMesh::Blend::Alpha;
                if (auto it = sm.material.params.find("Diffuse"); it != sm.material.params.end())
                    if (auto* d = std::get_if<alamo::Vec4>(&it->second)) gpu.tint = {(*d)[0], (*d)[1], (*d)[2], 1};
                if (gpu.tint[0] + gpu.tint[1] + gpu.tint[2] < 0.05f) gpu.tint = {1, 1, 1, 1};
                // Texture named but not available (e.g. Textures.meg not installed): neutral grey, not glaring white.
                if (!gpu.texture && sm.material.texture("BaseTexture"))
                    for (int k = 0; k < 3; ++k) gpu.tint[k] *= 0.62f;
                gm->meshes.push_back(gpu);
            }
        }
        if (gm->meshes.empty()) throw std::runtime_error("no visible meshes");
        gm->boundsMin = {mn[0], mn[1], mn[2]};
        gm->boundsMax = {mx[0], mx[1], mx[2]};
        gm->loaded = true;
        ++modelsLoaded_;
    } catch (const std::exception&) {
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
    const std::vector<alamo::Vec3> colors = alamo::terrainLayerColors(vfs_, map);
    std::vector<GpuVertex> verts;
    verts.reserve(size_t(map.width) * map.height);
    for (uint32_t y = 0; y < map.height; ++y) {
        for (uint32_t x = 0; x < map.width; ++x) {
            const float px = float(x) * map.spacing, py = float(y) * map.spacing;
            const float hl = map.vertexHeight(x > 0 ? x - 1 : x, y), hr = map.vertexHeight(x + 1 < map.width ? x + 1 : x, y);
            const float hd = map.vertexHeight(x, y > 0 ? y - 1 : y), hu = map.vertexHeight(x, y + 1 < map.height ? y + 1 : y);
            alamo::Vec3 n = Mat4::normalize({hl - hr, hd - hu, 2 * map.spacing});
            const uint8_t li = map.layerIndex[size_t(y) * map.width + x];
            const alamo::Vec3 c = li < colors.size() ? colors[li] : alamo::Vec3{0.5f, 0.5f, 0.5f};
            verts.push_back(vtx({px, py, map.vertexHeight(x, y)}, n, px / 64.f, py / 64.f, {c[0], c[1], c[2], 1}));
        }
    }
    std::vector<uint32_t> idx;
    idx.reserve(size_t(map.width - 1) * (map.height - 1) * 6);
    for (uint32_t y = 0; y + 1 < map.height; ++y)
        for (uint32_t x = 0; x + 1 < map.width; ++x) {
            const uint32_t a = y * map.width + x, b = a + 1, c = a + map.width, d = c + 1;
            idx.insert(idx.end(), {a, b, d, a, d, c});
        }
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
    glUniform3fv(uLightDir_, 1, light.data());
    glUniform3fv(uCamPos_, 1, cam.eye.data());
    glUniform3fv(uFogColor_, 1, fog.data());
    const GLfloat fogRange[4] = {farPlane * 0.45f, farPlane, 0, 0}; // x = fog start, y = fog end
    glUniform4fv(uFogRange_, 1, fogRange);
    glUniform1i(uTex_, 0);
    glUniform1i(uUnlit_, 0);
    transparent_.clear();
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
    setUniforms(Mat4{}, {1, 1, 1, 1}, 0);
    terrain_.draw();
}

void Renderer::drawModel(const GpuModel& m, const Mat4& transform, const alamo::Vec4& tint) {
    const alamo::Vec3 at{transform.at(0, 3), transform.at(1, 3), transform.at(2, 3)};
    const float depth = lengthOf(Mat4::sub(at, camPos_));
    for (const GpuMesh& mesh : m.meshes) {
        if (mesh.blend != GpuMesh::Blend::Opaque) {
            transparent_.push_back({&mesh, transform, {mesh.tint[0] * tint[0], mesh.tint[1] * tint[1], mesh.tint[2] * tint[2], tint[3]}, depth});
            continue;
        }
        setUniforms(transform, {mesh.tint[0] * tint[0], mesh.tint[1] * tint[1], mesh.tint[2] * tint[2], 1}, mesh.texture);
        mesh.draw();
    }
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
        d.mesh->draw();
    }
    glDepthMask(GL_TRUE_);
    glDisable(GL_BLEND);
}

void Renderer::drawOverlayRect(float x0, float y0, float x1, float y1, const alamo::Vec4& color) {
    // Pixel-space orthographic projection, y down; the box mesh is flattened onto z = 0.
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
    const Mat4 t = Mat4::translate((x0 + x1) / 2, (y0 + y1) / 2, 0) * Mat4::scale3(std::fabs(x1 - x0), std::fabs(y1 - y0), 0);
    // Lighting needs a normal facing the light; the top face's +Z normal is flattened away, so tint covers it.
    setUniforms(t, color, 0);
    box_.draw();
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glUniform1i(uUnlit_, 0);
    glUniformMatrix4fv(uViewProj_, 1, GL_FALSE_, viewProj_.data());
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
