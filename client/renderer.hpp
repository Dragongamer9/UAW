// OpenGL renderer for the viewer: terrain, ALO models, placeholder boxes.
#pragma once

#include "gl.hpp"
#include "math.hpp"

#include "alamo/alo.hpp"
#include "alamo/gamedata.hpp"
#include "alamo/meg.hpp"
#include "alamo/ted.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace uaw {

// Interleaved vertex used by every mesh: position, normal, uv, colour.
struct GpuVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float r, g, b, a;
};

struct GpuMesh {
    GLuint vao = 0, vbo = 0, ibo = 0;
    GLsizei indexCount = 0;
    GLenum indexType = GL_UNSIGNED_SHORT;
    GLuint texture = 0;
    alamo::Vec4 tint{1, 1, 1, 1};
    enum class Blend { Opaque, Alpha, Additive } blend = Blend::Opaque;

    void upload(const std::vector<GpuVertex>& verts, const void* indices, size_t count, bool wide);
    void draw() const;
    void release();
};

struct GpuModel {
    std::vector<GpuMesh> meshes;
    alamo::Vec3 boundsMin{}, boundsMax{};
    bool loaded = false;
};

class Renderer {
public:
    explicit Renderer(alamo::MegVfs& vfs);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Returns nullptr (once, cached) if the model or its file is missing/broken.
    const GpuModel* model(const std::string& aloName);

    void setTerrain(const alamo::Map& map);

    struct Camera {
        alamo::Vec3 eye, target;
        float fovY = 0.75f;
    };
    void beginFrame(int width, int height, const Camera& cam, float farPlane);
    void drawTerrain();
    void drawModel(const GpuModel& m, const Mat4& transform, const alamo::Vec4& tint = {1, 1, 1, 1});
    void drawBox(const Mat4& transform, const alamo::Vec4& color);
    void endFrame(); // draws queued transparent meshes
    // 2D overlay rectangle in pixels (top-left origin), alpha-blended over everything.
    void drawOverlayRect(float x0, float y0, float x1, float y1, const alamo::Vec4& color);

    const Mat4& viewProj() const { return viewProj_; }

    // Reads the current back buffer as RGBA rows, top row first.
    std::vector<uint8_t> readPixels(int width, int height);

    size_t modelsLoaded() const { return modelsLoaded_; }
    size_t modelsMissing() const { return modelsMissing_; }
    size_t texturesMissing() const { return texturesMissing_; }

private:
    GLuint texture(const std::string& name);
    void setUniforms(const Mat4& model, const alamo::Vec4& tint, GLuint tex);

    alamo::MegVfs& vfs_;
    GLuint program_ = 0;
    GLint uViewProj_ = -1, uModel_ = -1, uTint_ = -1, uHasTex_ = -1, uTex_ = -1, uLightDir_ = -1, uCamPos_ = -1,
          uFogColor_ = -1, uFogRange_ = -1, uUnlit_ = -1;
    Mat4 viewProj_;
    alamo::Vec3 camPos_{};
    int width_ = 1, height_ = 1;
    float farPlane_ = 10000;

    GpuMesh terrain_;
    GpuMesh box_;
    std::unordered_map<std::string, std::unique_ptr<GpuModel>> models_;
    std::unordered_map<std::string, GLuint> textures_;
    size_t modelsLoaded_ = 0, modelsMissing_ = 0, texturesMissing_ = 0;

    struct Deferred {
        const GpuMesh* mesh;
        Mat4 transform;
        alamo::Vec4 tint;
        float depth;
    };
    std::vector<Deferred> transparent_;
};

} // namespace uaw
