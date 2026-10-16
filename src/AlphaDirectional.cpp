// AlphaDirectional (AlphaShadows.dll): real-time directional sun/moon shadows for
// Cube World Alpha (32-bit, Direct3D 9).
//
// How it works, per frame:
//   1. DrawIndexedPrimitive hook: every opaque world mesh drawn with Alpha's native
//      world shader is recorded (buffers, transforms, point lights). At night the
//      native light constants are temporarily replaced with blue moonlight.
//   2. Clear hook: when Alpha clears only depth (just before its HUD), the recorded
//      meshes are replayed into a light-space depth map ("caster" pass) and then
//      again over the scene ("receiver" pass) to blend shadow and sun warmth.
//      Optional cloud meshes get their own small shadow map.
//   3. Present hook: per-frame state is reset; F7 toggles the effect.
//   4. Reset hook: all GPU resources are released.
//
// Static terrain chunks are kept in a cache so shadows from terrain just outside
// the camera view still render. Optional companion DLLs (AlphaWorldMotion,
// AlphaTAA) are discovered at runtime and used when present.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "AlphaMoonRules.h"

// ===========================================================================
// Constants
// ===========================================================================

// IDirect3DDevice9 vtable slots that are hooked.
static constexpr int SLOT_RESET = 16, SLOT_PRESENT = 17, SLOT_CLEAR = 43,
                     SLOT_DRAW_INDEXED_PRIMITIVE = 82;

// Vertex-shader constant registers of Alpha's native world shader.
static constexpr UINT NATIVE_POINT_LIGHTS = 0;     // 20 registers.
static constexpr UINT NATIVE_WORLD_VIEW_PROJ = 20; // 4 registers.
static constexpr UINT NATIVE_WORLD = 27;           // 3 registers (3x4 row-major).
static constexpr UINT NATIVE_LIGHTING = 33;        // 6 registers; c33 is the sun direction.
static constexpr UINT NATIVE_CAMERA = 36;          // Camera position.
static constexpr UINT REPLAY_POINT_LIGHTS = 40;    // Where our receiver shader expects them.
static constexpr UINT LIGHTING_REGISTER_COUNT = 6;

// Cube World Alpha memory layout (32-bit). The model array holds pointers to
// loaded voxel models; each model owns its vertex buffer and dimensions.
static constexpr DWORD CLOUD_MODEL_INDEX = 0xa08; // Cloud model slot in the model array.
static constexpr DWORD PROBE_MODEL_INDEX = 0x988; // A 16x16x25 model used to find the array.
static constexpr DWORD MODEL_VERTEX_BUFFER = 0x34;
static constexpr DWORD MODEL_DIMENSIONS = 0x44; // DWORD[3]
static constexpr DWORD MODEL_DEVICE = 0x58;     // IDirect3DDevice9*

static constexpr size_t MAX_CAPTURED_DRAWS = 6000;

// ===========================================================================
// State
// ===========================================================================

static HMODULE self;
static IDirect3DDevice9 *dev;
static void **vt; // Device vtable.
// `internal` is true while this mod issues its own draws, so the hooks ignore them.
static bool internal = false, enabled = true, rendered = false, ready = false;
// Requests from the exported control functions, consumed on the render thread.
static volatile LONG stopRequest = 0, captureRequest = 0, toggleRequest = 0;
static bool softShadows = false, filterEdges = false, failed = false;
static DWORD modelArray = 0;

// Settings (see AlphaDirectional.ini).
static UINT resolution = 4096;
static float radius = 100, strength = .48f, bias = .008f / 256, warmth = .03f;
static float sunTint[3] = {1, .93f, .76f};
static bool moonEnabled = true, visibleMoon = true;
static float nightBrightness = .04f, moonShadowStrength = .60f;

// Per-frame lighting state.
static bool sceneProjectionKnown = false;
static float nightWeight = 0, activeStrength = 0, sceneProjection[16]{};
static alphaMoon::Phase moonPhase;
static IDirect3DPixelShader9 *moonShader = nullptr;
static unsigned frames = 0, successful = 0;
static double renderTotalMs = 0;
static float camera[4]{}, sun[4]{}; // Native camera position and current shadow light.

static bool read(DWORD address, void *buffer, size_t size);

template <class T> static void release(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Folder for the INI/log: "<game>\graphics-rebuild" when loaded from "<game>\Mods".
static std::wstring computeDirectory() {
    wchar_t p[32768];
    GetModuleFileNameW(self, p, 32768);
    std::wstring s = p;
    s.resize(s.find_last_of(L"\\"));
    if (s.substr(s.find_last_of(L"\\") + 1) == L"Mods")
        s = s.substr(0, s.find_last_of(L"\\")) + L"\\graphics-rebuild";
    return s;
}
static const std::wstring &directory() {
    static const std::wstring cached = computeDirectory();
    return cached;
}

static void log(const char *s) {
    FILE *f = nullptr;
    _wfopen_s(&f, (directory() + L"\\AlphaDirectional.log").c_str(), L"a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
}

// ===========================================================================
// Hook types and optional companion-mod entry points
// ===========================================================================

using Dip = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using Clear = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float,
                                DWORD);
using Present = HRESULT(WINAPI *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND,
                                  const RGNDATA *);
using Reset = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static Dip oldDip;
static Clear oldClear;
static Present oldPresent;
static Reset oldReset;

// AlphaWorldMotion.dll: replays a draw with its leaf-sway applied, and reports the
// cloud drift origin so cloud shadows move with the clouds.
using MotionReplay = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT,
                                       UINT, Dip);
using MotionOrigin = BOOL(WINAPI *)(double *);
static MotionReplay motionReplay;
static MotionOrigin motionOrigin;
static bool motionChecked;

// Cloud shadows: a small light-space depth map of the nearest cloud meshes.
static bool cloudShadows = true, cloudMapKnown = false;
static float cloudStrength = .24f, cloudMatrix[16]{}, cloudDimensions[3]{16, 16, 16};
static double cloudOrigin[2]{};
static ULONGLONG cloudUpdated = 0, cloudAttempted = 0;
static IDirect3DTexture9 *cloudTexture = nullptr;
static IDirect3DSurface9 *cloudSurface = nullptr, *cloudDepth = nullptr;
static constexpr UINT cloudResolution = 512, cloudDrawLimit = 16;
static constexpr ULONGLONG cloudInterval = 100; // Minimum ms between cloud-map refreshes.

// ===========================================================================
// Captured draws and the static-terrain cache
// ===========================================================================

// One recorded DrawIndexedPrimitive call with everything needed to replay it.
// Holds a reference on vb/ib/decl.
struct Draw {
    bool casts = true;  // Wrote depth natively, so it can cast shadows.
    bool cloud = false; // Uses the cloud model's vertex buffer.
    IDirect3DVertexBuffer9 *vb = nullptr;
    IDirect3DIndexBuffer9 *ib = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    UINT offset = 0, stride = 0, min = 0, count = 0, start = 0, primitives = 0;
    INT base = 0;
    D3DPRIMITIVETYPE type;
    float wvp[16]{}, world[12]{}, points[80]{};
};
static std::vector<Draw> draws; // Draws captured this frame.

struct Cached {
    Draw draw;
    ULONGLONG seen; // Last tick the chunk was drawn natively.
    UINT bytes;     // Vertex + index buffer size, for the memory budget.
};
static std::vector<Cached> terrainCache;
static UINT cachedBytes = 0;
static constexpr size_t TERRAIN_CACHE_MAX_ENTRIES = 512;
static constexpr UINT TERRAIN_CACHE_MAX_BYTES = 96 * 1024 * 1024;
static constexpr ULONGLONG TERRAIN_CACHE_MAX_AGE_MS = 30000;

static void dropDraw(Draw &x) {
    release(x.vb);
    release(x.ib);
    release(x.decl);
}
static void clearCache() {
    for (auto &c : terrainCache)
        dropDraw(c.draw);
    terrainCache.clear();
    cachedBytes = 0;
}

// Terrain chunks are 8-byte-vertex triangle lists with an unscaled, unrotated transform.
static bool staticTerrain(const Draw &x) {
    return x.casts && !x.cloud && x.stride == 8 && x.type == D3DPT_TRIANGLELIST &&
           fabsf(x.world[0] - 1) < .00001f && fabsf(x.world[5] - 1) < .00001f &&
           fabsf(x.world[10] - 1) < .00001f &&
           fabsf(x.world[1]) + fabsf(x.world[2]) + fabsf(x.world[4]) + fabsf(x.world[6]) +
                   fabsf(x.world[8]) + fabsf(x.world[9]) <
               .00001f;
}

// Identity of the geometry a draw submits (independent of its transform).
struct MeshKey {
    IDirect3DVertexBuffer9 *vb;
    IDirect3DIndexBuffer9 *ib;
    INT base;
    UINT start, count, offset, primitives;
    bool operator==(const MeshKey &o) const {
        return vb == o.vb && ib == o.ib && base == o.base && start == o.start && count == o.count &&
               offset == o.offset && primitives == o.primitives;
    }
};
struct MeshKeyHash {
    size_t operator()(const MeshKey &k) const {
        size_t h = std::hash<const void *>()(k.vb);
        for (size_t v : {(size_t)k.ib, (size_t)k.base, (size_t)k.start, (size_t)k.count,
                         (size_t)k.offset, (size_t)k.primitives})
            h = h * 31 + v;
        return h;
    }
};
static MeshKey meshKey(const Draw &x) {
    return {x.vb, x.ib, x.base, x.start, x.count, x.offset, x.primitives};
}
static bool sameMesh(const Draw &a, const Draw &b) {
    return meshKey(a) == meshKey(b);
}

static void dropCacheEntry(std::vector<Cached>::iterator &it) {
    cachedBytes -= it->bytes;
    dropDraw(it->draw);
    it = terrainCache.erase(it);
}

// Brings the terrain cache up to date with this frame's draws. Alpha moves the
// world origin as the player travels, so cached chunk transforms are shifted by
// the offset observed on chunks drawn this frame. Returns false (and empties the
// cache) when the visible chunks disagree on that offset.
static bool updateCache() {
    ULONGLONG now = GetTickCount64();
    bool aligned = terrainCache.empty();

    // 1. Measure the floating-origin shift from chunks present both here and in the cache.
    std::unordered_map<MeshKey, size_t, MeshKeyHash> cacheIndex;
    for (size_t i = 0; i < terrainCache.size(); ++i)
        cacheIndex.emplace(meshKey(terrainCache[i].draw), i);
    float shift[3]{};
    unsigned anchors = 0;
    for (const auto &x : draws) {
        if (!staticTerrain(x))
            continue;
        auto match = cacheIndex.find(meshKey(x));
        if (match == cacheIndex.end())
            continue;
        const Draw &cached = terrainCache[match->second].draw;
        float delta[3] = {x.world[3] - cached.world[3], x.world[7] - cached.world[7],
                          x.world[11] - cached.world[11]};
        if (!anchors)
            memcpy(shift, delta, sizeof(shift));
        else if (fabsf(delta[0] - shift[0]) + fabsf(delta[1] - shift[1]) +
                     fabsf(delta[2] - shift[2]) >
                 .01f) {
            clearCache();
            return false;
        }
        anchors++;
    }
    if (anchors) {
        aligned = true;
        for (auto &c : terrainCache) {
            c.draw.world[3] += shift[0];
            c.draw.world[7] += shift[1];
            c.draw.world[11] += shift[2];
        }
    }
    if (!aligned) {
        clearCache();
        aligned = true;
    }

    // 2. Evict stale chunks and chunks outside the shadow radius.
    for (auto it = terrainCache.begin(); it != terrainCache.end();) {
        float dx = it->draw.world[3] - camera[0], dy = it->draw.world[7] - camera[1];
        if (now - it->seen > TERRAIN_CACHE_MAX_AGE_MS || fabsf(dx) > radius + 64 ||
            fabsf(dy) > radius + 64)
            dropCacheEntry(it);
        else
            ++it;
    }

    // 3. Add or refresh chunks drawn exactly once this frame.
    std::unordered_map<MeshKey, unsigned, MeshKeyHash> instanceCounts;
    for (const auto &y : draws)
        if (staticTerrain(y))
            instanceCounts[meshKey(y)]++;
    for (const auto &x : draws) {
        if (!staticTerrain(x) || instanceCounts[meshKey(x)] != 1)
            continue;
        auto found = std::find_if(terrainCache.begin(), terrainCache.end(),
                                  [&](const Cached &c) { return sameMesh(x, c.draw); });
        if (found != terrainCache.end()) {
            if (found->draw.decl != x.decl) {
                x.decl->AddRef();
                release(found->draw.decl);
            }
            found->draw = x;
            found->seen = now;
            continue;
        }
        // Replace remeshed geometry occupying the same chunk transform.
        for (auto it = terrainCache.begin(); it != terrainCache.end();)
            if (fabsf(it->draw.world[3] - x.world[3]) + fabsf(it->draw.world[7] - x.world[7]) +
                        fabsf(it->draw.world[11] - x.world[11]) <
                    .001f &&
                it->draw.vb != x.vb)
                dropCacheEntry(it);
            else
                ++it;
        D3DVERTEXBUFFER_DESC vd{};
        D3DINDEXBUFFER_DESC id{};
        if (FAILED(x.vb->GetDesc(&vd)) || FAILED(x.ib->GetDesc(&id)))
            continue;
        UINT bytes = vd.Size + id.Size;
        if (terrainCache.size() >= TERRAIN_CACHE_MAX_ENTRIES ||
            bytes > TERRAIN_CACHE_MAX_BYTES - cachedBytes)
            continue;
        Draw kept = x;
        kept.vb->AddRef();
        kept.ib->AddRef();
        kept.decl->AddRef();
        terrainCache.push_back({kept, now, bytes});
        cachedBytes += bytes;
    }
    return aligned;
}

// ===========================================================================
// GPU resources
// ===========================================================================

static IDirect3DTexture9 *mapTexture = nullptr; // R32F light-space depth.
static IDirect3DSurface9 *mapSurface = nullptr, *mapDepth = nullptr;
static IDirect3DVertexShader9 *casterVS = nullptr, *receiverVS = nullptr;
static IDirect3DPixelShader9 *casterPS = nullptr, *receiverPS = nullptr;

static void clearDraws() {
    for (auto &x : draws)
        dropDraw(x);
    draws.clear();
}

// Releases every GPU resource and per-frame cache (device reset / detach).
static void cleanup() {
    clearCache();
    clearDraws();
    release(mapTexture);
    release(mapSurface);
    release(mapDepth);
    release(casterVS);
    release(receiverVS);
    release(casterPS);
    release(receiverPS);
    release(moonShader);
    release(cloudSurface);
    release(cloudDepth);
    release(cloudTexture);
    cloudMapKnown = false;
    cloudAttempted = 0;
    sceneProjectionKnown = false;
    ready = false;
    failed = false;
}

// RAII guard around one native world draw. It decides tonight's shadow light
// (sun by day, moon at night) and, at night, swaps the native lighting
// constants for blue moonlight, restoring the originals when it goes out of scope.
struct NightLighting {
    IDirect3DDevice9 *d;
    bool changed = false;
    float saved[24]{};
    NightLighting(IDirect3DDevice9 *device, bool native) : d(device) {
        if (!native || internal)
            return;
        float direction[4]{};
        if (FAILED(d->GetVertexShaderConstantF(NATIVE_LIGHTING, direction, 1)))
            return;
        moonPhase =
            moonEnabled ? alphaMoon::phase(direction, moonShadowStrength) : alphaMoon::Phase{};
        nightWeight = moonPhase.weight;
        if (nightWeight > 0) {
            for (int i = 0; i < 3; i++)
                sun[i] = moonPhase.direction[i];
            activeStrength = moonPhase.strength;
            if (SUCCEEDED(
                    d->GetVertexShaderConstantF(NATIVE_LIGHTING, saved, LIGHTING_REGISTER_COUNT))) {
                float values[24]{};
                alphaMoon::lighting(values, saved, moonPhase, nightBrightness);
                changed = SUCCEEDED(
                    d->SetVertexShaderConstantF(NATIVE_LIGHTING, values, LIGHTING_REGISTER_COUNT));
            }
        } else {
            memcpy(sun, direction, 16);
            activeStrength = strength;
        }
    }
    ~NightLighting() {
        if (changed)
            d->SetVertexShaderConstantF(NATIVE_LIGHTING, saved, LIGHTING_REGISTER_COUNT);
    }
};

// ===========================================================================
// Light-space math
// ===========================================================================

struct Vec {
    float x, y, z;
};
static float dot(Vec a, Vec b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static Vec cross(Vec a, Vec b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static Vec normalize(Vec a) {
    float n = sqrtf(dot(a, a));
    return n > 1e-6f ? Vec{a.x / n, a.y / n, a.z / n} : Vec{0, 0, 1};
}

// Orthographic light matrix (4x4, row-major) centred on the camera. It covers
// `radius` units sideways and 256 units of depth, and is snapped to whole
// shadow-map texels so shadow edges do not shimmer as the camera moves.
static void lightMatrix(float *m) {
    Vec l = normalize({sun[0], sun[1], sun[2]}), f{-l.x, -l.y, -l.z};
    Vec r = normalize(cross(f, fabsf(f.z) > .95f ? Vec{0, 1, 0} : Vec{0, 0, 1}));
    Vec u = cross(r, f);
    Vec c{camera[0], camera[1], camera[2]};
    float snap = 2 * radius / resolution;
    float cx = floorf(dot(r, c) / snap + .5f) * snap, cy = floorf(dot(u, c) / snap + .5f) * snap;
    // clang-format off
    float a[16] = {
        r.x / radius, r.y / radius, r.z / radius, -cx / radius,          // right
        u.x / radius, u.y / radius, u.z / radius, -cy / radius,          // up
        f.x / 256,    f.y / 256,    f.z / 256,    .5f - dot(f, c) / 256, // depth
        0,            0,            0,            1,
    };
    // clang-format on
    memcpy(m, a, sizeof(a));
}

// ===========================================================================
// Shaders
// ===========================================================================

// caster/depth:      render casters into the light-space depth map.
// receiver/visibility: draw the scene again, blending shadow and sun warmth.
// Registers are documented next to each constant; the C++ side sets them in render().
static const char *source = R"HLSL(
// Caster vertex shader: c0-c3 light matrix.
float4 light0:register(c0),light1:register(c1),light2:register(c2),light3:register(c3);
// Receiver vertex shader: native transforms and point lights.
float4 worldViewProjMatrix[4]:register(c20);
float4 worldMatrix[3]:register(c27);
float4 pointPositions[10]:register(c40);
float4 pointColors[10]:register(c50);

struct V {
    float4 p:POSITION;
    float3 world:TEXCOORD0;
    float depth:TEXCOORD1;
    float3 normal:TEXCOORD2;
    float local:TEXCOORD3;
};

V caster(float4 p:POSITION){
    V o;
    o.normal=0;
    o.local=0;
    p.w=1;
    float4 w=float4(dot(worldMatrix[0],p),dot(worldMatrix[1],p),dot(worldMatrix[2],p),1);
    o.p=float4(dot(light0,w),dot(light1,w),dot(light2,w),1);
    o.world=w.xyz;
    o.depth=o.p.z;
    return o;
}

V receiver(float4 p:POSITION){
    V o;
    // Alpha packs the voxel face id (0..5) into position.w.
    float3 n=p.w==0?float3(1,0,0):p.w==1?float3(-1,0,0):p.w==2?float3(0,1,0):p.w==3?float3(0,-1,0):p.w==4?float3(0,0,1):float3(0,0,-1);
    o.normal=normalize(float3(dot(worldMatrix[0].xyz,n),dot(worldMatrix[1].xyz,n),dot(worldMatrix[2].xyz,n)));
    p.w=1;
    o.p=float4(dot(worldViewProjMatrix[0],p),dot(worldViewProjMatrix[1],p),dot(worldViewProjMatrix[2],p),dot(worldViewProjMatrix[3],p));
    o.world=float3(dot(worldMatrix[0],p),dot(worldMatrix[1],p),dot(worldMatrix[2],p));
    o.depth=0;
    o.local=0;
    // Accumulate nearby point-light influence; lit areas are protected from shadow.
    [loop]for(int j=0;j<10;j++)
        if(pointPositions[j].w>0){
            float3 delta=o.world-pointPositions[j].xyz;
            float d2=dot(delta,delta);
            float falloff=saturate(1-d2/(pointPositions[j].w*pointPositions[j].w));
            float facing=clamp(-4*dot(o.normal,delta*rsqrt(max(d2,.0001))),.3,1);
            o.local+=falloff*falloff*facing*max(pointColors[j].r,max(pointColors[j].g,pointColors[j].b));
        }
    o.local=saturate(o.local);
    return o;
}

float4 depth(V i):COLOR0{return i.depth;}

sampler2D shadowMap:register(s0);
sampler2D cloudMap:register(s1);
// Cloud map: c9-c11 projection, c12 = (texel size, strength, -, -).
float4 cloudProjection0:register(c9),cloudProjection1:register(c10),cloudProjection2:register(c11),cloudConfig:register(c12);

float cloudCompare(float2 uv,float z){return z<=tex2Dlod(cloudMap,float4(uv,0,0)).r?1:0;}

// Bilinear-filtered cloud occlusion (0 = clear sky, 1 = fully under cloud).
float cloudOcclusion(float4 w){
    [branch]if(cloudConfig.y<=0)return 0;
    float3 p=float3(dot(cloudProjection0,w),dot(cloudProjection1,w),dot(cloudProjection2,w));
    float2 uv=float2(p.x,-p.y)*.5+.5;
    [branch]if(any(uv<cloudConfig.x)||any(uv>1-cloudConfig.x)||p.z<0||p.z>1)return 0;
    float2 pixel=uv/cloudConfig.x-.5;
    float2 fraction=frac(pixel);
    float2 base=(floor(pixel)+.5)*cloudConfig.x;
    float z=p.z-.5/4096;
    float a=cloudCompare(base,z),b=cloudCompare(base+float2(cloudConfig.x,0),z),c=cloudCompare(base+float2(0,cloudConfig.x),z),d=cloudCompare(base+cloudConfig.xx,z);
    return 1-lerp(lerp(a,b,fraction.x),lerp(c,d,fraction.x),fraction.y);
}

// Visibility pass constants:
//   c0-c2 light projection, c3 config = (texel size, strength, bias, radius),
//   c4 eye, c5 sunlight = (direction, soft), c6 filtering = (filter edges, -, -, -),
//   c7 sunTone = (tint, warmth), c8 detail = (small entity, -, -, -).
float4 projection0:register(c0),projection1:register(c1),projection2:register(c2);
float4 config:register(c3);
float4 eye:register(c4);
float4 sunlight:register(c5);
float4 filtering:register(c6);
float4 sunTone:register(c7);
float4 detail:register(c8);

float compareDepth(float2 uv,float z){return z<=tex2Dlod(shadowMap,float4(uv,0,0)).r?1:0;}

float filteredDepth(float2 uv,float z){
    if(filtering.x<.5)return compareDepth(uv,z);
    float2 pixel=uv/config.x-.5;
    float2 fraction=frac(pixel);
    float2 base=(floor(pixel)+.5)*config.x;
    float a=compareDepth(base,z),b=compareDepth(base+float2(config.x,0),z),c=compareDepth(base+float2(0,config.x),z),d=compareDepth(base+config.xx,z);
    return lerp(lerp(a,b,fraction.x),lerp(c,d,fraction.x),fraction.y);
}

float4 visibility(V i):COLOR0{
    float4 w=float4(i.world,1);
    float3 p=float3(dot(projection0,w),dot(projection1,w),dot(projection2,w));
    float2 uv=float2(p.x,-p.y)*.5+.5+config.x*.5;
    if(any(uv<config.x)||any(uv>1-config.x)||p.z<0||p.z>1)return 0;
    float3 normal=normalize(i.normal);
    float facing=dot(normal,sunlight.xyz);
    // Depth bias grows with the receiver's slope in light space.
    float2 ux=ddx(uv),uy=ddy(uv);
    float zx=ddx(p.z),zy=ddy(p.z);
    float det=ux.x*uy.y-ux.y*uy.x;
    float slope=sqrt(saturate(1-facing*facing))/max(abs(facing),.1);
    float footprint=config.x*config.w/128*min(slope,8);
    if(abs(det)>1e-12){
        float2 gradient=float2(zx*uy.y-zy*ux.y,zy*ux.x-zx*uy.x)/det;
        footprint=config.x*(abs(gradient.x)+abs(gradient.y));
    }
    // Steam's accepted small-entity footprint uses the face normal instead of
    // derivatives spanning adjacent voxel faces. Light-space span here is 256.
    if(detail.x>.5){
        float denom=max(abs(dot(normal,projection2.xyz)),1e-7);
        footprint=config.x*(abs(dot(normal,projection0.xyz))+abs(dot(normal,projection1.xyz)))/denom*config.w*config.w/32768;
    }
    float baseBias=detail.x>.5?max(config.z,.1/256):config.z;
    float b=baseBias+min(footprint,config.x*config.w/128*16)*(sunlight.w>.5?2.1:1.1);
    float lit=0;
    [branch]if(sunlight.w>.5){
        // Soft shadows: 3x3 taps.
        [unroll]for(int y=-1;y<=1;y++)
            [unroll]for(int x=-1;x<=1;x++)
                lit+=filteredDepth(uv+float2(x,y)*config.x,p.z-b);
        lit/=9;
    }
    else lit=filteredDepth(uv,p.z-b);
    float fade=1-smoothstep(config.w*.75,config.w,length(i.world.xy-eye.xy));
    lit*=smoothstep(0,.08,facing);
    float available=fade*(1-saturate(i.local));
    float cloud=cloudOcclusion(w);
    float shade=saturate((1-lit)*config.y+cloud*cloudConfig.y*lit)*available;
    float warm=lit*(1-cloud)*sunTone.w*available*saturate(facing);
    float opacity=saturate(shade+warm);
    return float4(sunTone.rgb*warm/max(opacity,.00001),opacity);
}
)HLSL";

// Compiles the shadow shaders and creates the shadow map. On failure the mod
// stays inactive until the next device reset.
static bool initialize() {
    HMODULE h = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!h) {
        failed = true;
        log("Missing d3dcompiler_47.dll; shadows disabled.");
        return false;
    }
    auto compile = (decltype(&D3DCompile))GetProcAddress(h, "D3DCompile");
    bool ok = compile != nullptr;
    // Entry points: two vertex shaders, then two pixel shaders.
    const char *names[] = {"caster", "receiver", "depth", "visibility"};
    for (int i = 0; i < 4 && ok; i++) {
        bool vertexShader = i < 2;
        ID3DBlob *code = nullptr, *err = nullptr;
        HRESULT hr =
            compile(source, strlen(source), "AlphaShadows", nullptr, nullptr, names[i],
                    vertexShader ? "vs_3_0" : "ps_3_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &err);
        if (err) {
            log((char *)err->GetBufferPointer());
            release(err);
        }
        ok = SUCCEEDED(hr) && code;
        if (ok) {
            DWORD *bytecode = (DWORD *)code->GetBufferPointer();
            ok = SUCCEEDED(
                vertexShader ? dev->CreateVertexShader(bytecode, i ? &receiverVS : &casterVS)
                             : dev->CreatePixelShader(bytecode, i == 2 ? &casterPS : &receiverPS));
        }
        release(code);
    }
    FreeLibrary(h);
    if (ok)
        ok = SUCCEEDED(dev->CreateTexture(resolution, resolution, 1, D3DUSAGE_RENDERTARGET,
                                          D3DFMT_R32F, D3DPOOL_DEFAULT, &mapTexture, nullptr)) &&
             SUCCEEDED(mapTexture->GetSurfaceLevel(0, &mapSurface)) &&
             SUCCEEDED(dev->CreateDepthStencilSurface(resolution, resolution, D3DFMT_D24S8,
                                                      D3DMULTISAMPLE_NONE, 0, TRUE, &mapDepth,
                                                      nullptr));
    if (!ok) {
        cleanup();
        failed = true;
        log("Shadow resource/shader initialization failed; rendering unchanged until device reset.");
        return false;
    }
    ready = true;
    char info[160];
    sprintf_s(info, "R32F sun shadow map initialized: %ux%u, edge filtering=%d, soft=%d",
              resolution, resolution, filterEdges, softShadows);
    log(info);
    return true;
}

// ===========================================================================
// Visible moon
// ===========================================================================

static bool initializeMoon() {
    if (moonShader)
        return true;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler)
        return false;
    auto fn = (decltype(&D3DCompile))GetProcAddress(compiler, "D3DCompile");
    // Pixel-styled disk with three craters. c0 = (x, y, radius, alpha).
    const char *code = R"HLSL(
 float4 disk:register(c0);
 float4 moon(float2 uv:TEXCOORD0):COLOR0{
     float2 q=floor((uv*2-1)*disk.z/3)*3/disk.z;
     float d=length(q);
     clip(1-d);
     float crater=0;
     crater+=length(q-float2(-.3,.2))<.20?.10:0;
     crater+=length(q-float2(.3,-.25))<.15?.07:0;
     crater+=length(q-float2(.15,.4))<.12?.06:0;
     return float4(float3(.66,.76,.92)-crater,disk.w*(.8+.2*(1-smoothstep(.86,1,d))));
 }
 )HLSL";
    ID3DBlob *b = nullptr, *errors = nullptr;
    bool ok = fn && SUCCEEDED(fn(code, strlen(code), "AlphaMoon", nullptr, nullptr, "moon",
                                 "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &errors));
    if (errors)
        log((char *)errors->GetBufferPointer());
    if (ok)
        ok = SUCCEEDED(dev->CreatePixelShader((DWORD *)b->GetBufferPointer(), &moonShader));
    release(b);
    release(errors);
    FreeLibrary(compiler);
    return ok;
}

// Draws the moon as a screen-space quad at the far plane, depth-tested so
// terrain, characters and clouds hide it. Restores all device state afterwards.
static void renderMoon() {
    if (!enabled || !moonEnabled || !visibleMoon || !sceneProjectionKnown || moonPhase.weight <= 0)
        return;
    D3DVIEWPORT9 viewport{};
    dev->GetViewport(&viewport);
    float disk[4]{};
    if (!alphaMoon::disk(disk, sceneProjection, moonPhase, viewport.Width, viewport.Height) ||
        !initializeMoon())
        return;
    IDirect3DStateBlock9 *state = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &state)))
        return;
    internal = true;
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(moonShader);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetTexture(0, nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
    dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 7);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetPixelShaderConstantF(0, disk, 1);
    struct Vertex {
        float x, y, z, w, u, v;
    };
    // Half-pixel offset for D3D9 rasterization; z sits one depth step in front of the far plane.
    float x = disk[0] + viewport.X - .5f, y = disk[1] + viewport.Y - .5f, r = disk[2],
          z = 1.f - 1.f / 16777216.f;
    Vertex v[] = {{x - r, y - r, z, 1, 0, 0},
                  {x + r, y - r, z, 1, 1, 0},
                  {x - r, y + r, z, 1, 0, 1},
                  {x + r, y + r, z, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    state->Apply();
    release(state);
    internal = false;
}

// ===========================================================================
// Replay helpers
// ===========================================================================

// Binds a captured draw's buffers and constants for our replay shaders.
static void bind(const Draw &x) {
    float modelScale =
        sqrtf(x.world[0] * x.world[0] + x.world[1] * x.world[1] + x.world[2] * x.world[2]);
    float detail[4] = {!staticTerrain(x) && modelScale < .5f ? 1.f : 0.f, 0, 0, 0};
    dev->SetPixelShaderConstantF(8, detail, 1);
    dev->SetStreamSource(0, x.vb, x.offset, x.stride);
    dev->SetIndices(x.ib);
    dev->SetVertexDeclaration(x.decl);
    dev->SetVertexShaderConstantF(NATIVE_WORLD_VIEW_PROJ, x.wvp, 4);
    dev->SetVertexShaderConstantF(NATIVE_WORLD, x.world, 3);
    dev->SetVertexShaderConstantF(REPLAY_POINT_LIGHTS, x.points, 20);
}

// Replays a captured draw, through AlphaWorldMotion when available so moving
// leaves cast matching shadows.
static HRESULT replay(const Draw &x) {
    return motionReplay
               ? motionReplay(dev, x.type, x.base, x.min, x.count, x.start, x.primitives, oldDip)
               : oldDip(dev, x.type, x.base, x.min, x.count, x.start, x.primitives);
}

static void connectMotion() {
    if (motionChecked)
        return;
    auto module = GetModuleHandleW(L"AlphaWorldMotion.dll");
    if (!module)
        return;
    motionReplay = (MotionReplay)GetProcAddress(module, "_ReplayAlphaMotion@32");
    motionOrigin = (MotionOrigin)GetProcAddress(module, "_QueryAlphaMotionOrigin@4");
    motionChecked = true;
    log(motionReplay ? "Explicit leaf-motion replay connected."
                     : "Leaf-motion replay unavailable.");
}

// Point sampling with clamped addressing, as the depth comparisons expect.
static void setPointClampSampler(DWORD stage) {
    dev->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

// ===========================================================================
// Cloud shadows
// ===========================================================================

static bool initializeClouds() {
    if (cloudTexture)
        return true;
    bool ok =
        SUCCEEDED(dev->CreateTexture(cloudResolution, cloudResolution, 1, D3DUSAGE_RENDERTARGET,
                                     D3DFMT_R32F, D3DPOOL_DEFAULT, &cloudTexture, nullptr)) &&
        SUCCEEDED(cloudTexture->GetSurfaceLevel(0, &cloudSurface)) &&
        SUCCEEDED(dev->CreateDepthStencilSurface(cloudResolution, cloudResolution, D3DFMT_D24S8,
                                                 D3DMULTISAMPLE_NONE, 0, TRUE, &cloudDepth,
                                                 nullptr));
    if (!ok) {
        release(cloudSurface);
        release(cloudDepth);
        release(cloudTexture);
        cloudShadows = false;
        log("Cloud map unavailable; terrain shadows retained.");
    }
    return ok;
}

// Like lightMatrix(), but with a 4096-unit depth span (clouds are high above the
// terrain) and texel snapping for the smaller cloud map.
static void cloudLightMatrix(float *matrix) {
    lightMatrix(matrix);
    Vec light = normalize({sun[0], sun[1], sun[2]}), forward{-light.x, -light.y, -light.z};
    for (int axis = 0; axis < 3; ++axis)
        matrix[8 + axis] /= 16; // 4096 depth span, terrain keeps 256.
    matrix[11] = .5f - dot(forward, {camera[0], camera[1], camera[2]}) / 4096;
    float snap = 2 * radius / cloudResolution;
    for (int row = 0; row < 2; ++row) {
        Vec direction{matrix[row * 4] * radius, matrix[row * 4 + 1] * radius,
                      matrix[row * 4 + 2] * radius};
        matrix[row * 4 + 3] =
            -floorf(dot(direction, {camera[0], camera[1], camera[2]}) / snap + .5f) * snap / radius;
    }
}

// Squared light-space distance of a cloud's centre from the map centre, or -1
// when its bounding box does not overlap the cloud map at all.
static float cloudDistance(const Draw &draw, const float *matrix) {
    float center[3]{};
    for (int r = 0; r < 3; ++r) {
        center[r] = draw.world[r * 4 + 3];
        for (int c = 0; c < 3; ++c)
            center[r] += draw.world[r * 4 + c] * cloudDimensions[c] * .5f;
    }
    float projected[3]{};
    for (int r = 0; r < 3; ++r) {
        projected[r] = matrix[r * 4 + 3];
        float bound = 0;
        for (int c = 0; c < 3; ++c) {
            projected[r] += matrix[r * 4 + c] * center[c];
            float axis = 0;
            for (int k = 0; k < 3; ++k)
                axis += matrix[r * 4 + k] * draw.world[k * 4 + c];
            bound += fabsf(axis) * cloudDimensions[c] * .5f;
        }
        if (projected[r] + bound < (r == 2 ? 0.f : -1.f) || projected[r] - bound > 1)
            return -1;
    }
    return projected[0] * projected[0] + projected[1] * projected[1];
}

// Renders up to cloudDrawLimit nearest clouds into the cloud map. Refreshes at
// most every cloudInterval ms unless the clouds drifted noticeably. Called while
// the caster shaders are bound; leaves the main shadow map bound on return.
static void renderClouds() {
    if (!cloudShadows || cloudStrength <= 0)
        return;
    double origin[2]{};
    bool hasOrigin = motionOrigin && motionOrigin(origin);
    ULONGLONG now = GetTickCount64();
    // Throttle refreshes unless the clouds have drifted 8+ units since the last map.
    if (now - cloudAttempted < cloudInterval &&
        (!cloudMapKnown || !hasOrigin ||
         fabs(origin[0] - cloudOrigin[0]) + fabs(origin[1] - cloudOrigin[1]) < 8))
        return;
    cloudAttempted = now;
    float matrix[16]{};
    cloudLightMatrix(matrix);
    std::vector<std::pair<float, const Draw *>> candidates;
    for (const auto &draw : draws)
        if (draw.cloud) {
            float distance = cloudDistance(draw, matrix);
            if (distance >= 0)
                candidates.emplace_back(distance, &draw);
        }
    // No affecting clouds means no extra render pass. A short grace period avoids
    // immediately dropping an existing mask when sky geometry is camera-culled.
    if (candidates.empty()) {
        if (now - cloudUpdated > 1000)
            cloudMapKnown = false;
        return;
    }
    if (!initializeClouds())
        return;
    std::sort(candidates.begin(), candidates.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    dev->SetDepthStencilSurface(nullptr);
    dev->SetTexture(1, nullptr);
    dev->SetRenderTarget(0, cloudSurface);
    dev->SetDepthStencilSurface(cloudDepth);
    D3DVIEWPORT9 viewport{0, 0, cloudResolution, cloudResolution, 0, 1};
    dev->SetViewport(&viewport);
    oldClear(dev, 0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffffffff, 1, 0);
    dev->SetVertexShaderConstantF(0, matrix, 4);
    unsigned completed = 0;
    for (size_t i = 0; i < std::min(candidates.size(), size_t(cloudDrawLimit)); ++i) {
        bind(*candidates[i].second);
        completed += SUCCEEDED(replay(*candidates[i].second));
    }
    cloudMapKnown = completed > 0;
    if (cloudMapKnown) {
        memcpy(cloudMatrix, matrix, sizeof(cloudMatrix));
        memcpy(cloudOrigin, origin, sizeof(cloudOrigin));
        cloudUpdated = now;
    }
    dev->SetDepthStencilSurface(nullptr);
    dev->SetRenderTarget(0, mapSurface);
    dev->SetDepthStencilSurface(mapDepth);
    D3DVIEWPORT9 nativeMap{0, 0, resolution, resolution, 0, 1};
    dev->SetViewport(&nativeMap);
}

// ===========================================================================
// Main shadow pass
// ===========================================================================

// Pass 1: replay casters (cached terrain + this frame's depth-writing draws)
// into the light-space depth map.
static unsigned renderCasters(const float *matrix) {
    dev->SetTexture(0, nullptr);
    dev->SetDepthStencilSurface(nullptr);
    for (int i = 1; i < 4; i++)
        dev->SetRenderTarget(i, nullptr);
    dev->SetRenderTarget(0, mapSurface);
    dev->SetDepthStencilSurface(mapDepth);
    D3DVIEWPORT9 mv{0, 0, resolution, resolution, 0, 1};
    dev->SetViewport(&mv);
    oldClear(dev, 0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffffffff, 1, 0);
    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
    dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    updateCache();
    dev->SetVertexShader(casterVS);
    dev->SetPixelShader(casterPS);
    connectMotion();
    renderClouds();
    dev->SetVertexShaderConstantF(0, matrix, 4);
    unsigned failedDraws = 0;
    for (const auto &c : terrainCache) {
        bind(c.draw);
        if (FAILED(replay(c.draw)))
            failedDraws++;
    }
    // Skip terrain already replayed from the cache.
    std::unordered_set<MeshKey, MeshKeyHash> cachedMeshes;
    for (const auto &c : terrainCache)
        cachedMeshes.insert(meshKey(c.draw));
    for (const auto &x : draws) {
        if (!x.casts || x.cloud)
            continue;
        if (staticTerrain(x) && cachedMeshes.count(meshKey(x)))
            continue;
        bind(x);
        if (FAILED(replay(x)))
            failedDraws++;
    }
    return failedDraws;
}

// Pass 2: replay every non-cloud draw over the scene with the visibility shader.
static unsigned renderReceivers(const float *matrix, float daylightWeight,
                                IDirect3DSurface9 *sceneTarget, IDirect3DSurface9 *sceneDepth,
                                const D3DVIEWPORT9 &sceneViewport) {
    dev->SetDepthStencilSurface(nullptr);
    dev->SetRenderTarget(0, sceneTarget);
    dev->SetDepthStencilSurface(sceneDepth);
    dev->SetViewport(&sceneViewport);
    dev->SetVertexShader(receiverVS);
    dev->SetPixelShader(receiverPS);
    // A tiny negative depth bias so replayed surfaces pass LESSEQUAL against themselves.
    float receiverOffset = -2.f / 16777216.f;
    DWORD receiverBits;
    memcpy(&receiverBits, &receiverOffset, 4);
    dev->SetRenderState(D3DRS_DEPTHBIAS, receiverBits);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 7);
    dev->SetTexture(0, mapTexture);
    dev->SetTexture(1, cloudMapKnown ? cloudTexture : nullptr);
    setPointClampSampler(1);

    // Cloud map projection, shifted by how far the clouds drifted since it was drawn.
    float cloudProjection[16];
    memcpy(cloudProjection, cloudMatrix, sizeof(cloudProjection));
    double origin[2]{};
    if (cloudMapKnown && motionOrigin && motionOrigin(origin))
        for (int r = 0; r < 3; ++r)
            cloudProjection[r * 4 + 3] +=
                float(cloudProjection[r * 4] * (origin[0] - cloudOrigin[0]) +
                      cloudProjection[r * 4 + 1] * (origin[1] - cloudOrigin[1]));
    dev->SetPixelShaderConstantF(9, cloudProjection, 3);
    float cloudSettings[4] = {1.f / cloudResolution,
                              cloudMapKnown ? cloudStrength * daylightWeight : 0, 0, 0};
    dev->SetPixelShaderConstantF(12, cloudSettings, 1);
    setPointClampSampler(0);

    float settings[4] = {1.f / resolution, activeStrength * daylightWeight, bias, radius};
    dev->SetPixelShaderConstantF(0, matrix, 3);
    dev->SetPixelShaderConstantF(3, settings, 1);
    dev->SetPixelShaderConstantF(4, camera, 1);
    Vec l = normalize({sun[0], sun[1], sun[2]});
    float light[4] = {l.x, l.y, l.z, softShadows ? 1.f : 0.f};
    dev->SetPixelShaderConstantF(5, light, 1);
    float filter[4] = {filterEdges ? 1.f : 0.f, 0, 0, 0};
    dev->SetPixelShaderConstantF(6, filter, 1);
    float tone[4] = {sunTint[0], sunTint[1], sunTint[2],
                     warmth * daylightWeight * (1 - nightWeight)};
    dev->SetPixelShaderConstantF(7, tone, 1);
    unsigned failedDraws = 0;
    for (const auto &x : draws) {
        if (x.cloud)
            continue;
        bind(x);
        if (FAILED(replay(x)))
            failedDraws++;
    }
    return failedDraws;
}

// Runs once per frame from the HUD depth clear. Saves and restores all device state.
static void render() {
    if (!enabled || draws.empty() || rendered || failed)
        return;
    rendered = true;
    // Shadows fade in over the first 15% of the light's elevation.
    Vec daylight = normalize({sun[0], sun[1], sun[2]});
    float daylightWeight = std::clamp(daylight.z / .15f, 0.f, 1.f);
    if (daylightWeight <= 0 || (activeStrength <= 0 && warmth <= 0))
        return;
    if (!ready && !initialize())
        return;
    IDirect3DStateBlock9 *state = nullptr;
    IDirect3DSurface9 *rt[4]{}, *ds = nullptr;
    D3DVIEWPORT9 vp{};
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &state)))
        return;
    for (int i = 0; i < 4; i++)
        dev->GetRenderTarget(i, &rt[i]);
    dev->GetDepthStencilSurface(&ds);
    dev->GetViewport(&vp);
    if (!ds) {
        release(state);
        for (auto &p : rt)
            release(p);
        return;
    }
    LARGE_INTEGER begin{}, end{}, frequency{};
    QueryPerformanceCounter(&begin);
    QueryPerformanceFrequency(&frequency);
    internal = true;
    float matrix[16];
    lightMatrix(matrix);
    unsigned failedDraws = renderCasters(matrix);
    failedDraws += renderReceivers(matrix, daylightWeight, rt[0], ds, vp);

    // Restore the game's state and targets.
    state->Apply();
    dev->SetDepthStencilSurface(nullptr);
    for (int i = 0; i < 4; i++)
        dev->SetRenderTarget(i, rt[i]);
    dev->SetDepthStencilSurface(ds);
    dev->SetViewport(&vp);
    release(state);
    release(ds);
    for (auto &p : rt)
        release(p);
    internal = false;
    QueryPerformanceCounter(&end);
    renderTotalMs += double(end.QuadPart - begin.QuadPart) * 1000 / frequency.QuadPart;
    if (!failedDraws)
        successful++;
    if (successful == 1 || frames % 600 == 0) {
        char b[256];
        sprintf_s(
            b,
            "Frame %u: %zu meshes, %u failed draws; camera %.2f %.2f %.2f sun %.3f %.3f %.3f; mean CPU submission %.2f ms; retained terrain=%zu",
            frames, draws.size(), failedDraws, camera[0], camera[1], camera[2], sun[0], sun[1],
            sun[2], renderTotalMs / std::max(successful, 1u), terrainCache.size());
        log(b);
    }
}

// ===========================================================================
// Draw capture (DrawIndexedPrimitive hook)
// ===========================================================================

// Alpha's lit world shader is recognised by the constant names in its bytecode.
static bool worldShader(IDirect3DVertexShader9 *s) {
    UINT n = 0;
    if (!s || FAILED(s->GetFunction(nullptr, &n)) || n > 65536)
        return false;
    std::vector<char> b(n);
    if (FAILED(s->GetFunction(b.data(), &n)))
        return false;
    auto has = [&](const char *name) {
        size_t l = strlen(name) + 1;
        return std::search(b.begin(), b.end(), name, name + l) != b.end();
    };
    return has("worldViewProjMatrix") && has("worldMatrix") && has("lightDirection");
}
static std::set<IDirect3DVertexShader9 *> good, bad; // worldShader() results per shader.

// Diagnostics for CaptureShadows(): logs the constant table (CTAB) of up to 20
// distinct depth-tested vertex shaders.
static void logReceiverProbe(IDirect3DVertexShader9 *vs, DWORD z, DWORD write, UINT count) {
    static std::set<IDirect3DVertexShader9 *> probed;
    if (!probed.insert(vs).second || probed.size() > 20)
        return;
    UINT bytes = 0;
    vs->GetFunction(nullptr, &bytes);
    std::vector<char> code(bytes);
    if (FAILED(vs->GetFunction(code.data(), &bytes)))
        return;
    char info[160];
    sprintf_s(info, "Receiver probe: shader=%p depth=%lu write=%lu count=%u", vs, z, write, count);
    log(info);
    for (size_t at = 0; at + 32 < code.size(); at += 4)
        if (!memcmp(code.data() + at, "CTAB", 4)) {
            const char *tab = code.data() + at + 4;
            size_t remain = code.size() - at - 4;
            DWORD n = 0, o = 0; // Constant count and offset of the constant descriptors.
            memcpy(&n, tab + 12, 4);
            memcpy(&o, tab + 16, 4);
            if (n > 200 || o + n * 20 > remain)
                continue;
            for (DWORD j = 0; j < n; j++) {
                DWORD name = 0;
                WORD reg = 0, num = 0;
                memcpy(&name, tab + o + j * 20, 4);
                memcpy(&reg, tab + o + j * 20 + 6, 2);
                memcpy(&num, tab + o + j * 20 + 8, 2);
                if (name < remain && memchr(tab + name, 0, remain - name)) {
                    sprintf_s(info, "Receiver constant: %.100s c%u count%u", tab + name, reg, num);
                    log(info);
                }
            }
        }
}

// AlphaTAA.dll (which hooks Clear before us) exports the current sub-pixel
// jitter so replayed geometry lines up with the jittered scene.
static bool querySceneJitter(float xy[2]) {
    using JitterQuery = BOOL(WINAPI *)(float *);
    static JitterQuery query = nullptr;
    static bool checked = false;
    MEMORY_BASIC_INFORMATION owner{};
    if (!checked && VirtualQuery((void *)oldClear, &owner, sizeof(owner))) {
        query =
            (JitterQuery)GetProcAddress((HMODULE)owner.AllocationBase, "_QueryAlphaSceneJitter@4");
        if (!query)
            query =
                (JitterQuery)GetProcAddress((HMODULE)owner.AllocationBase, "QueryAlphaSceneJitter");
        checked = true;
        log(query ? "Shared temporal scene jitter connected."
                  : "Temporal jitter unavailable; native projection used.");
    }
    return query && query(xy);
}

// Reads the cloud model pointer and its vertex buffer from the game's model array.
static void readCloudModel(DWORD &cloudModel, DWORD &cloudVB) {
    SIZE_T got = 0;
    if (!modelArray)
        return;
    ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)(modelArray + CLOUD_MODEL_INDEX * 4),
                      &cloudModel, 4, &got);
    if (cloudModel)
        ReadProcessMemory(GetCurrentProcess(),
                          (void *)(uintptr_t)(cloudModel + MODEL_VERTEX_BUFFER), &cloudVB, 4, &got);
}

static bool hasProjection(const float *wvp) {
    return fabsf(wvp[12]) + fabsf(wvp[13]) + fabsf(wvp[14]) > 1e-5f;
}

// Records one accepted native world draw into `draws`.
static void captureDraw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT min, UINT count,
                        UINT start, UINT primitives, DWORD write) {
    Draw x{};
    x.casts = write != 0;
    x.type = t;
    x.base = base;
    x.min = min;
    x.count = count;
    x.start = start;
    x.primitives = primitives;
    d->GetVertexShaderConstantF(NATIVE_WORLD_VIEW_PROJ, x.wvp, 4);
    // Query the upstream TAA hook: draw replay uses the same scene projection.
    float xy[2]{};
    if (querySceneJitter(xy))
        for (int c = 0; c < 4; c++) {
            x.wvp[c] += 2 * xy[0] * x.wvp[12 + c];
            x.wvp[4 + c] -= 2 * xy[1] * x.wvp[12 + c];
        }
    d->GetVertexShaderConstantF(NATIVE_WORLD, x.world, 3);
    d->GetVertexShaderConstantF(NATIVE_POINT_LIGHTS, x.points, 20);
    // The first perspective draw into the back buffer gives the camera projection
    // used to place the moon.
    if (!sceneProjectionKnown && hasProjection(x.wvp)) {
        IDirect3DSurface9 *rt = nullptr, *bb = nullptr;
        D3DVIEWPORT9 viewport{};
        d->GetRenderTarget(0, &rt);
        d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        d->GetViewport(&viewport);
        if (rt && rt == bb && viewport.Width >= 320 && viewport.Height >= 180)
            sceneProjectionKnown = alphaMoon::cameraProjection(sceneProjection, x.wvp, x.world);
        release(rt);
        release(bb);
    }
    if (!hasProjection(x.wvp))
        return;
    d->GetVertexShaderConstantF(NATIVE_CAMERA, camera, 1);
    d->GetVertexShaderConstantF(NATIVE_LIGHTING, sun, 1);
    d->GetStreamSource(0, &x.vb, &x.offset, &x.stride);
    d->GetIndices(&x.ib);
    d->GetVertexDeclaration(&x.decl);
    DWORD cloudModel = 0, cloudVB = 0;
    readCloudModel(cloudModel, cloudVB);
    x.cloud = (DWORD)(uintptr_t)x.vb == cloudVB && cloudVB != 0;
    if (x.cloud) {
        x.casts = true;
        DWORD dimensions[3]{};
        if (read(cloudModel + MODEL_DIMENSIONS, dimensions, sizeof(dimensions)))
            for (int axis = 0; axis < 3; ++axis)
                cloudDimensions[axis] = float(std::clamp<DWORD>(dimensions[axis], 1, 255));
    }
    if (x.vb && x.ib && x.decl)
        draws.push_back(x);
    else
        dropDraw(x);
}

static HRESULT WINAPI dip(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT min, UINT count,
                          UINT start, UINT primitives) {
    bool nativeLighting = false;
    if (!internal && enabled && !rendered && draws.size() < MAX_CAPTURED_DRAWS) {
        DWORD z = 0, write = 0;
        d->GetRenderState(D3DRS_ZENABLE, &z);
        d->GetRenderState(D3DRS_ZWRITEENABLE, &write);
        if (z) {
            IDirect3DVertexShader9 *vs = nullptr;
            d->GetVertexShader(&vs);
            if (captureRequest && vs)
                logReceiverProbe(vs, z, write, count);
            bool accepted = good.count(vs) != 0;
            if (!accepted && !bad.count(vs)) {
                accepted = worldShader(vs);
                (accepted ? good : bad).insert(vs);
            }
            release(vs);
            if (accepted) {
                nativeLighting = true;
                captureDraw(d, t, base, min, count, start, primitives, write);
            }
        }
    }
    NightLighting lighting(d, nativeLighting);
    return oldDip(d, t, base, min, count, start, primitives);
}

// ===========================================================================
// Clear / Present / Reset hooks
// ===========================================================================

// Alpha clears only the depth buffer right before drawing its HUD: the 3D scene
// is complete, so this is where the moon and shadows are composited.
static HRESULT WINAPI clear(IDirect3DDevice9 *d, DWORD n, const D3DRECT *rect, DWORD flags,
                            D3DCOLOR c, float z, DWORD stencil) {
    if (!internal && (flags & D3DCLEAR_ZBUFFER) && !(flags & D3DCLEAR_TARGET) && !draws.empty()) {
        renderMoon();
        render();
    }
    return oldClear(d, n, rect, flags, c, z, stencil);
}

// Atomically replaces one device vtable entry, returning the previous function.
static void *exchange(int slot, void *fn) {
    DWORD p = 0;
    if (!VirtualProtect(vt + slot, 4, PAGE_EXECUTE_READWRITE, &p))
        return nullptr;
    void *old = InterlockedExchangePointer(vt + slot, fn);
    DWORD ignore;
    VirtualProtect(vt + slot, 4, p, &ignore);
    return old;
}

// Writes the back buffer to AlphaShadows-capture.bmp (32-bit, top-down).
static void capture() {
    IDirect3DSurface9 *bb = nullptr, *cpu = nullptr;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (!bb)
        return;
    D3DSURFACE_DESC desc{};
    bb->GetDesc(&desc);
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                   D3DPOOL_SYSTEMMEM, &cpu, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(bb, cpu))) {
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(cpu->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            FILE *f = nullptr;
            _wfopen_s(&f, (directory() + L"\\AlphaShadows-capture.bmp").c_str(), L"wb");
            if (f) {
                BITMAPFILEHEADER bh{};
                bh.bfType = 0x4d42; // "BM"
                bh.bfOffBits = sizeof(bh) + sizeof(BITMAPINFOHEADER);
                bh.bfSize = bh.bfOffBits + desc.Width * desc.Height * 4;
                BITMAPINFOHEADER bi{};
                bi.biSize = sizeof(bi);
                bi.biWidth = desc.Width;
                bi.biHeight = -(LONG)desc.Height;
                bi.biPlanes = 1;
                bi.biBitCount = 32;
                fwrite(&bh, sizeof(bh), 1, f);
                fwrite(&bi, sizeof(bi), 1, f);
                for (UINT y = 0; y < desc.Height; y++)
                    fwrite((BYTE *)locked.pBits + y * locked.Pitch, 4, desc.Width, f);
                fclose(f);
            }
            cpu->UnlockRect();
        }
    }
    release(bb);
    release(cpu);
}

// Logs the screenshot and a summary of the largest captured meshes.
static void logCapture() {
    capture();
    char coverage[160];
    sprintf_s(
        coverage, "Shadow coverage: %zu casters, %zu non-depth-writing receivers",
        (size_t)std::count_if(draws.begin(), draws.end(), [](const Draw &x) { return x.casts; }),
        (size_t)std::count_if(draws.begin(), draws.end(), [](const Draw &x) { return !x.casts; }));
    log(coverage);
    auto sorted = draws;
    std::sort(sorted.begin(), sorted.end(),
              [](const Draw &a, const Draw &b) { return a.count > b.count; });
    for (size_t i = 0; i < std::min(sorted.size(), size_t(15)); i++) {
        const auto &x = sorted[i];
        char text[256];
        sprintf_s(
            text,
            "Mesh %zu: VB=%p count=%u stride=%u scale=(%.5f %.5f %.5f) translation=(%.3f %.3f %.3f)",
            i, x.vb, x.count, x.stride, x.world[0], x.world[5], x.world[10], x.world[3], x.world[7],
            x.world[11]);
        log(text);
    }
}

static HRESULT WINAPI present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w,
                              const RGNDATA *r) {
    if (stopRequest) {
        cleanup();
        exchange(SLOT_DRAW_INDEXED_PRIMITIVE, (void *)oldDip);
        exchange(SLOT_CLEAR, (void *)oldClear);
        exchange(SLOT_RESET, (void *)oldReset);
        exchange(SLOT_PRESENT, (void *)oldPresent);
        log("Shadow hooks detached safely.");
        return oldPresent(d, a, b, w, r);
    }
    if (InterlockedExchange(&toggleRequest, 0)) {
        enabled = !enabled;
        log(enabled ? "Sun shadows enabled." : "Sun shadows disabled.");
    }
    // F7 toggles while the game window is focused.
    D3DDEVICE_CREATION_PARAMETERS creation{};
    d->GetCreationParameters(&creation);
    if (GetForegroundWindow() == (w ? w : creation.hFocusWindow) && (GetAsyncKeyState(VK_F7) & 1))
        enabled = !enabled;
    if (captureRequest) {
        InterlockedExchange(&captureRequest, 0);
        logCapture();
    }
    if (frames == 120 && !successful)
        log("No world shadow pass completed; waiting for world geometry/depth clear.");
    // Start the next frame with an empty capture list.
    clearDraws();
    rendered = false;
    sceneProjectionKnown = false;
    frames++;
    return oldPresent(d, a, b, w, r);
}

static HRESULT WINAPI reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *p) {
    cleanup();
    good.clear();
    bad.clear();
    rendered = false;
    sceneProjectionKnown = false;
    return oldReset(d, p);
}

// ===========================================================================
// Exports (callable from other mods / tools via CreateRemoteThread etc.)
// ===========================================================================

extern "C" __declspec(dllexport) DWORD WINAPI DisableShadows(void *) {
    InterlockedExchange(&stopRequest, 1);
    return 0;
}
extern "C" __declspec(dllexport) DWORD WINAPI ToggleShadows(void *) {
    InterlockedExchange(&toggleRequest, 1);
    return 0;
}
extern "C" __declspec(dllexport) DWORD WINAPI CaptureShadows(void *) {
    InterlockedExchange(&captureRequest, 1);
    return 0;
}
// Checks that the light matrix maps the camera to the centre of the shadow map.
// Returns 0 on success, otherwise the failing row + 1.
extern "C" __declspec(dllexport) int ShadowSelfTest() {
    sun[0] = .5f;
    sun[1] = .5f;
    sun[2] = .7071f;
    camera[0] = 10;
    camera[1] = 20;
    camera[2] = 125;
    float m[16];
    lightMatrix(m);
    float p[4] = {10, 20, 125, 1};
    for (int i = 0; i < 3; i++) {
        float v = 0;
        for (int j = 0; j < 4; j++)
            v += m[i * 4 + j] * p[j];
        if (fabsf(v - (i == 2 ? .5f : 0)) > 2.f / resolution)
            return i + 1;
    }
    return 0;
}

// ===========================================================================
// Startup: read settings, locate the game's D3D9 device, install hooks
// ===========================================================================

// Safe read of game memory (fails instead of faulting on bad pointers).
static bool read(DWORD address, void *buffer, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)address, buffer, size,
                             &got) &&
           got == size;
}
static DWORD pointer(DWORD address) {
    DWORD v = 0;
    read(address, &v, 4);
    return v;
}

static void loadSettings() {
    std::wstring ini = directory() + L"\\AlphaDirectional.ini";
    auto integer = [&](const wchar_t *section, const wchar_t *key, int fallback) {
        return GetPrivateProfileIntW(section, key, fallback, ini.c_str());
    };
    auto option = [&](const wchar_t *key, const wchar_t *value, float lo, float hi) {
        wchar_t s[64];
        GetPrivateProfileStringW(L"Shadows", key, value, s, 64, ini.c_str());
        return std::clamp((float)_wtof(s), lo, hi);
    };
    enabled = integer(L"Shadows", L"Enabled", 1) != 0;
    resolution = std::clamp(integer(L"Shadows", L"Resolution", 3072), 512u, 4096u);
    radius = option(L"Radius", L"100", 8, 128);
    strength = option(L"Strength", L"0.60", 0, 1);
    int percent = integer(L"Shadows", L"StrengthPercent", -1); // Overrides Strength when set.
    if (percent >= 0)
        strength = std::clamp(percent * .01f, 0.f, 1.f);
    softShadows = integer(L"Shadows", L"SoftShadows", 0) != 0;
    filterEdges = integer(L"Shadows", L"FilterEdges", 0) != 0;
    bias = option(L"Bias", L"0.00020", .00001f, .005f);
    warmth = std::clamp(integer(L"Shadows", L"SunlitWarmthPercent", 10) * .01f, 0.f, .3f);
    sunTint[0] = option(L"SunlitRed", L"1", 0, 1);
    sunTint[1] = option(L"SunlitGreen", L"0.93", 0, 1);
    sunTint[2] = option(L"SunlitBlue", L"0.76", 0, 1);
    moonEnabled = integer(L"Moon", L"Enabled", 1) != 0;
    visibleMoon = integer(L"Moon", L"VisibleMoon", 1) != 0;
    nightBrightness = std::clamp(integer(L"Moon", L"BrightnessPercent", 4), 0u, 50u) * .01f;
    moonShadowStrength =
        std::clamp(integer(L"Moon", L"ShadowStrengthPercent", 60), 0u, 100u) * .01f;
    cloudShadows = integer(L"Clouds", L"Enabled", 1) != 0;
    cloudStrength = std::clamp(integer(L"Clouds", L"ShadowStrengthPercent", 24), 0u, 50u) * .01f;
}

// The device is found by scanning the game's .data section (and one level of
// pointers from it) for the model array: a known model at PROBE_MODEL_INDEX has
// dimensions 16x16x25 and points to the D3D9 device.
static bool installHooks(const IMAGE_NT_HEADERS32 &nt, DWORD base, DWORD section) {
    std::set<DWORD> candidates;
    auto add = [&](DWORD a, size_t size) {
        if (size > 16 * 1024 * 1024)
            return;
        std::vector<DWORD> b(size / 4);
        if (read(a, b.data(), b.size() * 4))
            for (DWORD p : b)
                if (p > 0x10000 && p < 0x7fff0000 && p % 4 == 0)
                    candidates.insert(p);
    };
    for (int i = 0; i < nt.FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER s{};
        if (read(section + i * sizeof(s), &s, sizeof(s)) && !memcmp(s.Name, ".data", 5))
            add(base + s.VirtualAddress, s.Misc.VirtualSize);
    }
    auto roots = candidates;
    for (DWORD p : roots)
        add(p, 16384);
    for (DWORD p : candidates) {
        DWORD model = pointer(p + PROBE_MODEL_INDEX * 4), dims[3]{};
        if (!read(model + MODEL_DIMENSIONS, dims, 12) || dims[0] != 16 || dims[1] != 16 ||
            dims[2] != 25)
            continue;
        modelArray = p;
        dev = (IDirect3DDevice9 *)(uintptr_t)pointer(model + MODEL_DEVICE);
        vt = (void **)(uintptr_t)pointer((DWORD)(uintptr_t)dev);
        if (!vt)
            continue;
        // With TAA, shadows must execute before its HUD depth switch, so wait
        // until AlphaTAA has hooked Clear first.
        if (GetModuleHandleW(L"AlphaTAA.dll")) {
            MEMORY_BASIC_INFORMATION info{};
            VirtualQuery(vt[SLOT_CLEAR], &info, sizeof(info));
            wchar_t hooked[32768]{};
            GetModuleFileNameW((HMODULE)info.AllocationBase, hooked, 32768);
            if (!wcsstr(hooked, L"AlphaTAA")) {
                dev = nullptr;
                vt = nullptr;
                continue;
            }
        }
        oldClear = (Clear)exchange(SLOT_CLEAR, (void *)clear);
        oldReset = (Reset)exchange(SLOT_RESET, (void *)reset);
        oldDip = (Dip)exchange(SLOT_DRAW_INDEXED_PRIMITIVE, (void *)dip);
        oldPresent = (Present)exchange(SLOT_PRESENT, (void *)present);
        log("AlphaDirectional revision 3 attached (explicit leaf-motion replay, bounded 512px cloud map, native-cycle moon).");
        return true;
    }
    return false;
}

static DWORD WINAPI worker(void *) {
    loadSettings();
    DWORD base = (DWORD)(uintptr_t)GetModuleHandleW(nullptr);
    IMAGE_DOS_HEADER dos{};
    read(base, &dos, sizeof(dos));
    IMAGE_NT_HEADERS32 nt{};
    read(base + dos.e_lfanew, &nt, sizeof(nt));
    DWORD section = base + dos.e_lfanew + 24 + nt.FileHeader.SizeOfOptionalHeader;
    // Retry for up to three minutes while the game creates its device.
    for (int attempt = 0; attempt < 180; attempt++) {
        if (installHooks(nt, base, section))
            return 0;
        Sleep(1000);
    }
    log("Alpha D3D9 device not found.");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        self = m;
        DisableThreadLibraryCalls(m);
        wchar_t p[32768];
        GetModuleFileNameW(nullptr, p, 32768);
        auto name = wcsrchr(p, L'\\');
        if (name && !_wcsicmp(name + 1, L"Cube.exe")) {
            HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
            if (t)
                CloseHandle(t);
        }
    }
    return TRUE;
}
