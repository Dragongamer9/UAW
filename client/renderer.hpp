// OpenGL renderer for the viewer: terrain, ALO models, placeholder boxes.
#pragma once

#include "gl.hpp"
#include "math.hpp"

#include "alamo/ala.hpp"
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
    // Animated models: up to four slots in the mesh's bone palette and their weights.
    float bone[4] = {0, 0, 0, 0};
    float weight[4] = {1, 0, 0, 0};
    // Normal-mapped meshes: the texture's tangent and binormal directions (zero: no normal map).
    float tangent[3] = {0, 0, 0};
    float binormal[3] = {0, 0, 0};
};

struct GpuMesh {
    GLuint vao = 0, vbo = 0, ibo = 0;
    GLsizei indexCount = 0;
    GLenum indexType = GL_UNSIGNED_SHORT;
    GLuint texture = 0;
    GLuint bump = 0; // NormalTexture (tangent space), 0 = none
    // Sun highlights: Specular colour, Shininess, and where the gloss mask is (0 none, 1 the normal
    // map's alpha, 2 the base texture's alpha for *Gloss shaders).
    alamo::Vec4 specular{0, 0, 0, 32};
    int gloss = 0;
    float uvScroll[2] = {0, 0}; // UVScrollRate (texture repeats per second)
    bool scrolls = false;
    alamo::Vec4 tint{1, 1, 1, 1};
    enum class Blend { Opaque, Alpha, Additive } blend = Blend::Opaque;
    bool colorize = false;  // *Colorize shaders: the owner's colour where the texture's alpha is set
    bool alphaTest = false; // foliage (grass, leaves, bushes): cut out where the texture is transparent
    std::vector<uint32_t> palette; // skeleton bone per vertex bone slot (empty: never posed)

    void upload(const std::vector<GpuVertex>& verts, const void* indices, size_t count, bool wide);
    void draw() const;
    void release();
};

struct GpuModel {
    std::vector<GpuMesh> meshes;
    // Particle effects attached to bones (Alamo proxies), at their rest position in model space.
    struct Proxy { std::string name; alamo::Vec3 position; bool hidden; };
    std::vector<Proxy> proxies;
    alamo::Vec3 boundsMin{}, boundsMax{};
    bool loaded = false;
    alamo::Model skeleton;               // bones only, for posing
    std::vector<alamo::Mat34> restInverse; // inverse rest (bind) world transform per bone
};

// Skin matrices for one posed model: per skeleton bone, posed * inverse(rest).
using Pose = std::vector<alamo::Mat34>;

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
    void drawModel(const GpuModel& m, const Mat4& transform, const alamo::Vec4& tint = {1, 1, 1, 1}, const Pose* pose = nullptr);
    // Animations (.ALA), cached; nullptr if missing.
    const alamo::Animation* animation(const std::string& alaName);
    // `m` posed by `anim` at `seconds` (looping).
    Pose pose(const GpuModel& m, const alamo::Animation& anim, float seconds) const;
    void drawBox(const Mat4& transform, const alamo::Vec4& color);
    void endFrame(); // draws queued transparent meshes
    // 2D overlay in pixels (top-left origin), alpha-blended over everything, unlit.
    void drawOverlayRect(float x0, float y0, float x1, float y1, const alamo::Vec4& color);
    void drawOverlayTriangles(const std::vector<GpuVertex>& vertices, GLuint texture); // x/y in pixels
    // The map's sky dome (LandPrimarySkydome): around the camera, behind everything, unlit and unfogged.
    // Call first, right after beginFrame.
    void drawSky(const GpuModel& m);
    // The map's lighting (sun colour and direction, ambient, haze); invalid = the neutral defaults.
    void setEnvironment(const alamo::MapEnvironment& e);
    // The map's water (a sheet at its level over the whole map), drawn by drawWater after the opaque
    // geometry.
    void setWater(const alamo::MapWater& w, float sizeX, float sizeY);
    void drawWater();
    // Models drawn after this are cut off above height z (1e9 = no cut): buildings rising as they are built.
    void setClipHeight(float z);
    // Sun shadows and normal maps on (the default unless UAW_NO_SHADOWS / UAW_NO_BUMPS) or off.
    bool detail() const { return shadowsOn_ || bumpsOn_; }
    void setDetail(bool on) {
        shadowsOn_ = bumpsOn_ = on;
        if (!on) shadowReady_ = false;
    }
    // Seconds of game time, for scrolling textures (UVScrollRate) and the water's drift.
    void setTime(float seconds) { time_ = seconds; }
    // The owner's colour for models drawn after this (their *Colorize meshes); white = none.
    void setTeamColor(const alamo::Vec4& c) { team_ = {c[0], c[1], c[2], 1}; }
    // World-space triangles (camera-facing particle quads), unlit, blended, no depth writes.
    void drawParticles(const std::vector<GpuVertex>& vertices, GLuint texture, bool additive);
    // A game texture by name (cached; 0 if missing).
    GLuint textureNamed(const std::string& name) { return texture(name); }
    const alamo::Vec3& cameraPosition() const { return camPos_; }
    int width() const { return width_; }
    int height() const { return height_; }

    const Mat4& viewProj() const { return viewProj_; }

    // A texture from RGBA rows (top row first), linear filtered, clamped; the caller owns it.
    GLuint uploadTexture(const std::vector<uint8_t>& rgba, int width, int height);

    // Fog of war (World::fogGrid): darkens terrain and models by what the player has seen.
    // nullptr switches it off. The texture is rebuilt only when `version` changes.
    void setFogOfWar(const std::vector<uint8_t>* grid, int w, int h, float cell, uint64_t version);

    // Reads the current back buffer as RGBA rows, top row first.
    std::vector<uint8_t> readPixels(int width, int height);

    size_t modelsLoaded() const { return modelsLoaded_; }
    size_t modelsMissing() const { return modelsMissing_; }
    size_t texturesMissing() const { return texturesMissing_; }

private:
    GLuint texture(const std::string& name);
    void setUniforms(const Mat4& model, const alamo::Vec4& tint, GLuint tex, GLuint bump = 0);
    void setSkin(const GpuMesh& mesh, const Pose* pose);
    void bindShadow(GLuint program);
    static bool sphereVisible(const Mat4& viewProj, const alamo::Vec3& centre, float radius);
    void renderShadows();

    alamo::MegVfs& vfs_;
    GLuint program_ = 0;
    GLint uViewProj_ = -1, uModel_ = -1, uTint_ = -1, uHasTex_ = -1, uTex_ = -1, uLightDir_ = -1, uCamPos_ = -1,
          uFogColor_ = -1, uFogRange_ = -1, uUnlit_ = -1, uSkin_ = -1, uSkinned_ = -1, uAlphaTest_ = -1, uTeam_ = -1, uHasBump_ = -1, uSpec_ = -1, uGloss_ = -1, uUvOffset_ = -1;
    float time_ = 0;
    alamo::Vec4 team_{1, 1, 1, 1};
    Mat4 viewProj_;
    alamo::Vec3 camPos_{};
    int width_ = 1, height_ = 1;
    float farPlane_ = 10000;

    GLuint overlayVao_ = 0, overlayVbo_ = 0;
    GLuint fowTex_ = 0;
    float fowParams_[4] = {0, 0, 0, 0};
    uint64_t fowVersion_ = ~0ull;
    GLuint terrainProgram_ = 0, terrainLayers_ = 0;
    std::vector<float> layerParams_, layerTints_; // 64 x vec4 each
    alamo::Vec3 lightDir_{0, 0, -1}, fogColor_{0.6f, 0.6f, 0.6f};
    std::unordered_map<GLuint, alamo::Vec4> texAverage_;
    GpuMesh water_;
    GLuint waterTexture_ = 0, waterBump_ = 0, white_ = 0;
    alamo::Vec4 waterTint_{0.3f, 0.42f, 0.5f, 0.72f};
    bool hasWater_ = false;
    alamo::Vec3 sunColor_{0.72f, 0.72f, 0.72f}, ambientColor_{0.38f, 0.38f, 0.38f};
    alamo::Vec3 sunDirection_{0.45f, 0.35f, -0.82f}, hazeColor_{0.62f, 0.68f, 0.75f};
    GpuMesh terrain_;
    GpuMesh box_;
    std::unordered_map<std::string, std::unique_ptr<GpuModel>> models_;
    std::unordered_map<std::string, GLuint> textures_;
    std::unordered_map<std::string, std::unique_ptr<alamo::Animation>> animations_;
    size_t modelsLoaded_ = 0, modelsMissing_ = 0, texturesMissing_ = 0;

    struct Deferred {
        const GpuMesh* mesh;
        Mat4 transform;
        alamo::Vec4 tint;
        float depth;
        std::shared_ptr<const Pose> pose; // animated: kept until the transparent pass
    };
    std::vector<Deferred> transparent_;

    // Sun shadows: opaque meshes drawn this frame cast into a depth map used by the next frame.
    static constexpr int kShadowSize = 2048;
    struct Caster {
        const GpuMesh* mesh;
        Mat4 transform;
        std::shared_ptr<const Pose> pose;
        float clipZ;
    };
    std::vector<Caster> casters_;
    bool bumpsOn_ = true;
    bool shadowsOn_ = true, shadowReady_ = false, drawTerrainShadow_ = false;
    GLuint shadowFbo_ = 0, shadowTex_ = 0;
    Mat4 shadowViewProj_;
    alamo::Vec3 camTarget_{};
    float clipZ_ = 1e9f;
};

} // namespace uaw
