#include "AlphaDirectional.cpp"
#include <cassert>

static unsigned cloudSubmissions;
static Dip nativeSubmit;
static unsigned farSubmissions, nearSubmissions, receiverSubmissions;
static HRESULT WINAPI countCascades(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT min,
                                    UINT count, UINT start, UINT primitives) {
    IDirect3DSurface9 *target = nullptr;
    d->GetRenderTarget(0, &target);
    if (target == mapSurface)
        ++farSubmissions;
    else if (target == nearSurface)
        ++nearSubmissions;
    else
        ++receiverSubmissions;
    release(target);
    return nativeSubmit(d, t, base, min, count, start, primitives);
}
static void checkCascadeReplay() {
    Draw draw{};
    draw.stride = 16;
    draw.type = D3DPT_TRIANGLELIST;
    draw.count = 4;
    draw.primitives = 2;
    float vertices[4][4] = {{-1, -1, 0, 4}, {1, -1, 0, 4}, {1, 1, 0, 4}, {-1, 1, 0, 4}};
    WORD indices[] = {0, 1, 2, 0, 2, 3};
    void *data = nullptr;
    assert(SUCCEEDED(
        dev->CreateVertexBuffer(sizeof(vertices), 0, 0, D3DPOOL_MANAGED, &draw.vb, nullptr)));
    assert(SUCCEEDED(draw.vb->Lock(0, 0, &data, 0)));
    memcpy(data, vertices, sizeof(vertices));
    draw.vb->Unlock();
    assert(SUCCEEDED(dev->CreateIndexBuffer(sizeof(indices), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED,
                                            &draw.ib, nullptr)));
    assert(SUCCEEDED(draw.ib->Lock(0, 0, &data, 0)));
    memcpy(data, indices, sizeof(indices));
    draw.ib->Unlock();
    D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0},
                                    D3DDECL_END()};
    assert(SUCCEEDED(dev->CreateVertexDeclaration(elements, &draw.decl)));
    float identity[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(draw.world, identity, 48);
    memcpy(draw.wvp, identity, 64);
    draws.push_back(draw);
    assert(intersectsMap(draw, identity));
    Draw distant = draw;
    distant.world[3] = 10000;
    assert(!intersectsMap(distant, identity));
    nativeSubmit = (Dip)(*(void ***)dev)[82];
    oldDip = countCascades;
    oldClear = (Clear)(*(void ***)dev)[43];
    sun[0] = sun[1] = camera[0] = camera[1] = camera[2] = 0;
    sun[2] = 1;
    activeStrength = .48f;
    enabled = cascadesEnabled = true;
    rendered = false;
    cloudShadows = false;
    cloudMapKnown = false;
    assert(SUCCEEDED(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffffffff, 1, 0)));
    IDirect3DSurface9 *beforeDepth = nullptr, *afterDepth = nullptr;
    dev->GetDepthStencilSurface(&beforeDepth);
    assert(SUCCEEDED(dev->BeginScene()));
    render();
    assert(SUCCEEDED(dev->EndScene()));
    dev->GetDepthStencilSurface(&afterDepth);
    assert(beforeDepth == afterDepth);
    release(beforeDepth);
    release(afterDepth);
    assert(farSubmissions == 1 && nearSubmissions == 1 && receiverSubmissions == 1);
    clearDraws();
    oldDip = nativeSubmit;
    cloudShadows = true;
    puts(
        "Full cascade replay: one far caster, one near caster, one receiver; native depth restored; bounds culling: PASS");
}
static HRESULT WINAPI countCloud(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT min,
                                 UINT count, UINT start, UINT primitives) {
    ++cloudSubmissions;
    return nativeSubmit(d, t, base, min, count, start, primitives);
}
static BOOL WINAPI fixedOrigin(double *origin) {
    origin[0] = 100;
    origin[1] = 200;
    return TRUE;
}
static void checkCloudCasting() {
    IDirect3DStateBlock9 *state = nullptr;
    assert(SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &state)));
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    dev->GetRenderTarget(0, &rt);
    dev->GetDepthStencilSurface(&ds);
    D3DVIEWPORT9 viewport{};
    dev->GetViewport(&viewport);
    IDirect3DVertexBuffer9 *vb = nullptr;
    IDirect3DIndexBuffer9 *ib = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    float vertices[4][4] = {{0, 0, 0, 4}, {16, 0, 0, 4}, {16, 16, 0, 4}, {0, 16, 0, 4}};
    WORD indices[] = {0, 1, 2, 0, 2, 3};
    void *data = nullptr;
    assert(
        SUCCEEDED(dev->CreateVertexBuffer(sizeof(vertices), 0, 0, D3DPOOL_MANAGED, &vb, nullptr)));
    assert(SUCCEEDED(vb->Lock(0, 0, &data, 0)));
    memcpy(data, vertices, sizeof(vertices));
    vb->Unlock();
    assert(SUCCEEDED(
        dev->CreateIndexBuffer(sizeof(indices), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr)));
    assert(SUCCEEDED(ib->Lock(0, 0, &data, 0)));
    memcpy(data, indices, sizeof(indices));
    ib->Unlock();
    D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0},
                                    D3DDECL_END()};
    assert(SUCCEEDED(dev->CreateVertexDeclaration(elements, &decl)));
    Draw cloud{};
    cloud.cloud = true;
    cloud.vb = vb;
    cloud.ib = ib;
    cloud.decl = decl;
    cloud.stride = 16;
    cloud.type = D3DPT_TRIANGLELIST;
    cloud.count = 4;
    cloud.primitives = 2;
    float transform[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 100};
    memcpy(cloud.world, transform, sizeof(transform));
    camera[0] = camera[1] = camera[2] = 0;
    sun[0] = sun[1] = 0;
    sun[2] = 1;
    for (unsigned i = 0; i < cloudDrawLimit + 8; ++i) {
        vb->AddRef();
        ib->AddRef();
        decl->AddRef();
        draws.push_back(cloud);
    }
    nativeSubmit = (Dip)(*(void ***)dev)[82];
    oldDip = countCloud;
    oldClear = (Clear)(*(void ***)dev)[43];
    motionOrigin = fixedOrigin;
    motionReplay = nullptr;
    cloudShadows = true;
    cloudMapKnown = false;
    dev->SetVertexShader(casterVS);
    dev->SetPixelShader(casterPS);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    assert(SUCCEEDED(dev->BeginScene()));
    renderClouds();
    assert(cloudMapKnown && cloudSubmissions == cloudDrawLimit);
    renderClouds();
    assert(cloudSubmissions == cloudDrawLimit); // immediate repeat reuses mask
    assert(SUCCEEDED(dev->EndScene()));
    IDirect3DSurface9 *cpu = nullptr;
    assert(SUCCEEDED(dev->CreateOffscreenPlainSurface(cloudResolution, cloudResolution, D3DFMT_R32F,
                                                      D3DPOOL_SYSTEMMEM, &cpu, nullptr)));
    assert(SUCCEEDED(dev->GetRenderTargetData(cloudSurface, cpu)));
    float clip[3]{};
    for (int r = 0; r < 3; ++r)
        clip[r] = cloudMatrix[r * 4] * 8 + cloudMatrix[r * 4 + 1] * 8 +
                  cloudMatrix[r * 4 + 2] * 100 + cloudMatrix[r * 4 + 3];
    int x = int((clip[0] * .5f + .5f) * cloudResolution),
        y = int((-clip[1] * .5f + .5f) * cloudResolution);
    D3DLOCKED_RECT locked{};
    assert(SUCCEEDED(cpu->LockRect(&locked, nullptr, D3DLOCK_READONLY)));
    float depth = *((float *)((BYTE *)locked.pBits + y * locked.Pitch) + x);
    cpu->UnlockRect();
    release(cpu);
    assert(fabsf(depth - clip[2]) < .001f);
    Draw distant = cloud;
    distant.world[3] = 10000;
    assert(cloudDistance(distant, cloudMatrix) < 0);
    clearDraws();
    release(vb);
    release(ib);
    release(decl);
    state->Apply();
    dev->SetDepthStencilSurface(nullptr);
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(ds);
    dev->SetViewport(&viewport);
    release(state);
    release(rt);
    release(ds);
    motionOrigin = nullptr;
    oldDip = nativeSubmit;
    puts("Cloud geometry depth, map reuse, 16-draw ceiling and footprint culling: PASS");
}

// Hidden standalone device: exercises actual D3D9 shaders and pixel output.
int main() {
    self = GetModuleHandleW(nullptr);
    HWND window = CreateWindowW(L"STATIC", L"Alpha shadow test", WS_OVERLAPPED, 0, 0, 128, 128,
                                nullptr, nullptr, self, nullptr);
    assert(window);
    IDirect3D9 *api = Direct3DCreate9(D3D_SDK_VERSION);
    assert(api);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.BackBufferWidth = pp.BackBufferHeight = 128;
    D3DDISPLAYMODE mode{};
    api->GetAdapterDisplayMode(0, &mode);
    pp.BackBufferFormat = mode.Format;
    assert(SUCCEEDED(api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)));
    resolution = 4096;
    filterEdges = true;
    assert(initialize());
    assert(mapSurface && mapDepth);
    assert(ShadowSelfTest() == 0);
    IDirect3DTexture9 *pattern = nullptr;
    assert(SUCCEEDED(
        dev->CreateTexture(16, 16, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &pattern, nullptr)));
    D3DLOCKED_RECT lock{};
    assert(SUCCEEDED(pattern->LockRect(0, &lock, nullptr, 0)));
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            ((float *)((BYTE *)lock.pBits + y * lock.Pitch))[x] = x < 8 ? .4f : 1.f;
    pattern->UnlockRect(0);
    IDirect3DTexture9 *cloudPattern = nullptr;
    assert(SUCCEEDED(
        dev->CreateTexture(1, 1, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &cloudPattern, nullptr)));
    assert(SUCCEEDED(cloudPattern->LockRect(0, &lock, nullptr, 0)));
    *(float *)lock.pBits = .4f;
    cloudPattern->UnlockRect(0);
    D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    IDirect3DVertexDeclaration9 *decl = nullptr;
    assert(SUCCEEDED(dev->CreateVertexDeclaration(elements, &decl)));
    float vertices[6][4] = {{-1, -1, 0, 4}, {1, -1, 0, 4}, {1, 1, 0, 4},
                            {-1, -1, 0, 4}, {1, 1, 0, 4},  {-1, 1, 0, 4}};
    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float matrix[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, .6f};
    float settings[4] = {1.f / 16, .6f, .0002f, 100};
    float eye[4] = {0, 0, 0, 0};
    float light[4] = {0, 0, 1, 0};
    float filter[4] = {0, 0, 0, 0};
    float points[80]{};
    IDirect3DSurface9 *bb = nullptr, *cpu = nullptr;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    assert(SUCCEEDED(
        dev->CreateOffscreenPlainSurface(128, 128, mode.Format, D3DPOOL_SYSTEMMEM, &cpu, nullptr)));
    float testNearRadius = 32, testNearScale = 1;
    // Legacy uniform darkening and ground-distance fade unless a test changes them.
    float shadingRules[4] = {0, 0, .75f, 0}, nativeLighting[32]{};
    bool terrainFace = false;
    auto sample = [&](bool soft, bool filtered, bool local, bool warmed = false, bool gray = false,
                      bool smallEntity = false, bool cloudy = false, bool focused = false) {
        light[3] = soft ? 1.f : 0;
        filter[0] = filtered ? 1.f : 0;
        memset(points, 0, sizeof(points));
        if (local) {
            points[2] = 2;
            points[3] = 50;
            points[40] = points[41] = points[42] = 1;
        }
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, gray ? 0xff808080 : 0xffffffff, 1, 0);
        assert(SUCCEEDED(dev->BeginScene()));
        dev->SetVertexDeclaration(decl);
        dev->SetVertexShader(receiverVS);
        dev->SetPixelShader(receiverPS);
        dev->SetVertexShaderConstantF(20, identity, 4);
        dev->SetVertexShaderConstantF(27, identity, 3);
        dev->SetVertexShaderConstantF(40, points, 20);
        float pointInfo[4] = {10, 0, 0, 0};
        dev->SetVertexShaderConstantF(60, pointInfo, 1);
        dev->SetPixelShaderConstantF(0, matrix, 3);
        Vec a{matrix[0] * .5f, matrix[1] * .5f, matrix[2] * .5f},
            b{-matrix[4] * .5f, -matrix[5] * .5f, -matrix[6] * .5f},
            c{matrix[8], matrix[9], matrix[10]};
        Vec cofactors[3] = {cross(b, c), cross(c, a), cross(a, b)};
        float facePlanes[12]{};
        for (int row = 0; row < 3; row++) {
            facePlanes[row * 4] = cofactors[row].x;
            facePlanes[row * 4 + 1] = cofactors[row].y;
            facePlanes[row * 4 + 2] = cofactors[row].z;
        }
        float detail[4] = {smallEntity ? 1.f : 0.f,
                           (focused || terrainFace) && !smallEntity ? 1.f : 0.f, 0, 0};
        dev->SetPixelShaderConstantF(8, detail, 1);
        dev->SetPixelShaderConstantF(17, shadingRules, 1);
        dev->SetVertexShaderConstantF(61, nativeLighting, 8);
        dev->SetPixelShaderConstantF(3, settings, 1);
        dev->SetPixelShaderConstantF(4, eye, 1);
        dev->SetPixelShaderConstantF(5, light, 1);
        dev->SetPixelShaderConstantF(6, filter, 1);
        float tone[4] = {1, .93f, .76f, warmed ? .10f : 0};
        dev->SetPixelShaderConstantF(7, tone, 1);
        dev->SetTexture(0, pattern);
        dev->SetTexture(1, cloudPattern);
        dev->SetTexture(2, cloudPattern);
        float nearProjection[12];
        memcpy(nearProjection, matrix, sizeof(nearProjection));
        nearProjection[0] *= testNearScale;
        nearProjection[5] *= testNearScale;
        dev->SetPixelShaderConstantF(13, nearProjection, 3);
        float cascadeInfo[4] = {1.f / 2048, testNearRadius, testNearRadius * .8f,
                                focused ? 1.f : 0.f};
        dev->SetPixelShaderConstantF(16, cascadeInfo, 1);
        dev->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetPixelShaderConstantF(9, matrix, 3);
        float cloudSettings[4] = {1.f / 512, cloudy ? .24f : 0, 0, 0};
        dev->SetPixelShaderConstantF(12, cloudSettings, 1);
        dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        assert(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, vertices, 16)));
        dev->EndScene();
        assert(SUCCEEDED(dev->GetRenderTargetData(bb, cpu)));
        assert(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)));
        int transitions = 0;
        for (int x = 12; x < 116; x++) {
            int v = ((BYTE *)lock.pBits)[64 * lock.Pitch + x * 4];
            if (v > 110 && v < 245)
                transitions++;
        }
        int shadow = ((BYTE *)lock.pBits)[64 * lock.Pitch + 32 * 4];
        int unshadowed = ((BYTE *)lock.pBits)[64 * lock.Pitch + 100 * 4];
        cpu->UnlockRect();
        int face = (int)vertices[0][3];
        const float normals[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        float facing =
            normals[face][0] * light[0] + normals[face][1] * light[1] + normals[face][2] * light[2];
        if (!gray && !cloudy && !terrainFace && (!focused || smallEntity) && facing > .1f)
            assert(unshadowed > 245);
        return std::pair<int, int>{transitions, shadow};
    };
    auto crisp = sample(false, false, false), soft = sample(true, false, false),
         filtered = sample(false, true, false), local = sample(false, false, true);
    assert(crisp.second < 120);
    assert(crisp.first == 0);
    assert(soft.first > crisp.first);
    assert(filtered.first > crisp.first);
    assert(local.second > crisp.second + 40);
    printf(
        "D3D9 shader/resource tests PASS; edge transitions crisp=%d soft=%d filtered=%d; shadow pixel=%d local-light pixel=%d\n",
        crisp.first, soft.first, filtered.first, crisp.second, local.second);
    auto clouded = sample(false, false, false, false, false, false, true);
    assert(abs(clouded.second - crisp.second) <= 1);
    assert(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)));
    int cloudLit = ((BYTE *)lock.pBits)[64 * lock.Pitch + 100 * 4];
    cpu->UnlockRect();
    assert(cloudLit > 185 && cloudLit < 205);
    auto cloudLocal = sample(false, false, true, false, false, false, true);
    assert(cloudLocal.second > 245);
    printf("Cloud mask shares receiver pass, preserves terrain shade/local lights: PASS (%d)\n",
           cloudLit);
    auto entity = sample(false, true, false, false, false, true);
    auto focused = sample(false, false, false, false, false, false, false, true);
    assert(abs(focused.second - crisp.second) <= 1);
    assert(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)));
    int nearLit = ((BYTE *)lock.pBits)[64 * lock.Pitch + 100 * 4];
    cpu->UnlockRect();
    assert(nearLit < 120);
    auto spriteFar = sample(false, false, false, false, false, true, false, true);
    assert(abs(spriteFar.second - crisp.second) <= 1);
    puts("Focused map selected on terrain; sprite receivers retain accepted far-map path: PASS");
    testNearRadius = .65f;
    sample(false, false, false, false, false, false, false, true);
    assert(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)));
    int overlap = ((BYTE *)lock.pBits)[64 * lock.Pitch + 100 * 4];
    cpu->UnlockRect();
    assert(overlap > 120 && overlap < 230);
    testNearRadius = 32;
    testNearScale = 8;
    sample(false, false, false, false, false, false, false, true);
    assert(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)));
    int outside = ((BYTE *)lock.pBits)[64 * lock.Pitch + 100 * 4];
    cpu->UnlockRect();
    assert(outside > 245);
    testNearScale = 1;
    puts("Cascade overlap blends; receivers outside focused-map bounds use far map: PASS");
    assert(entity.second < 120);
    puts("Small entity retains external occluder shadows: PASS");
    auto baseGray = sample(false, false, false, false, true),
         warmGray = sample(false, false, false, true, true);
    assert(abs(baseGray.second - warmGray.second) <= 1);
    assert(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)));
    BYTE *pixel = (BYTE *)lock.pBits + 64 * lock.Pitch + 100 * 4;
    assert(pixel[2] > 128 && pixel[2] > pixel[1] && pixel[1] > pixel[0]);
    printf("Warm sunlight on lit gray surface: RGB %u %u %u; shadow remains %d\n", pixel[2],
           pixel[1], pixel[0], warmGray.second);
    cpu->UnlockRect();
    const float nativeNormals[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (int face = 0; face < 6; face++) {
        for (auto &vertex : vertices)
            vertex[3] = (float)face;
        for (int axis = 0; axis < 3; axis++)
            light[axis] = nativeNormals[face][axis];
        auto aligned = sample(false, false, false);
        assert(aligned.second == 102);
        auto focusedSprite = sample(false, false, false, false, false, true, false, true);
        auto farSprite = sample(false, false, false, false, false, true, false, false);
        assert(focusedSprite.second == farSprite.second);
        for (int axis = 0; axis < 3; axis++)
            light[axis] = -nativeNormals[face][axis];
        auto opposite = sample(false, false, false);
        assert(opposite.second == 102);
        printf("Native normal ID %d: aligned=%d opposite=%d PASS\n", face, aligned.second,
               opposite.second);
    }
    light[0] = light[1] = 0;
    light[2] = 1;
    settings[0] = 1.f / 4096;
    for (auto &vertex : vertices)
        vertex[3] = 0;
    auto side = sample(false, false, false);
    assert(side.second == 102);
    light[0] = .7071f;
    light[2] = .7071f;
    auto litSide = sample(false, false, false);
    assert(litSide.second < 120);
    printf("Sun-facing vertical receiver: %d PASS\n", litSide.second);
    light[0] = 0;
    light[2] = 1;
    for (auto &vertex : vertices)
        vertex[3] = 5;
    auto back = sample(false, false, false);
    assert(back.second == 102);
    printf("Unlit side/back faces receive consistent directional shade: %d/%d PASS\n", side.second,
           back.second);
    for (auto &vertex : vertices)
        vertex[3] = 0;
    assert(SUCCEEDED(pattern->LockRect(0, &lock, nullptr, 0)));
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            ((float *)((BYTE *)lock.pBits + y * lock.Pitch))[x] = .5999f;
    pattern->UnlockRect(0);
    auto acne = sample(false, false, false);
    assert(acne.second == 102);
    printf("Grazing-face shade does not depend on near-equal depth: %d PASS\n", acne.second);
    // Steam ports: direct-light model, terrain grazing band and camera fade.
    assert(SUCCEEDED(pattern->LockRect(0, &lock, nullptr, 0)));
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            ((float *)((BYTE *)lock.pBits + y * lock.Pitch))[x] = x < 8 ? .4f : 1.f;
    pattern->UnlockRect(0);
    // Native c32-c39: material alpha, light direction, front, back, ambient.
    auto nativeLight = [&](float ambient, float front) {
        memset(nativeLighting, 0, sizeof(nativeLighting));
        nativeLighting[3] = 1;
        memcpy(nativeLighting + 4, light, 12);
        for (int c = 0; c < 3; c++) {
            nativeLighting[8 + c] = front;
            nativeLighting[24 + c] = ambient;
        }
    };
    auto unshadowedPixel = [&]() {
        assert(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)));
        int v = ((BYTE *)lock.pBits)[64 * lock.Pitch + 100 * 4];
        cpu->UnlockRect();
        return v;
    };
    shadingRules[0] = 1;
    for (auto &vertex : vertices)
        vertex[3] = 4; // Ground facing the sun.
    light[0] = light[1] = 0;
    light[2] = 1;
    nativeLight(0, 1);
    auto allDirect = sample(false, false, false);
    assert(allDirect.second == 102 && unshadowedPixel() > 245);
    nativeLight(.3f, 1); // Shade 1.2 lit, 0.3 unlit: direct share 0.75.
    auto ambientKept = sample(false, false, false);
    assert(ambientKept.second >= 138 && ambientKept.second <= 142);
    auto lantern = sample(false, false, true);
    assert(lantern.second > ambientKept.second + 40);
    printf("Direct-light shadows: all-direct=%d with ambient=%d near lantern=%d PASS\n",
           allDirect.second, ambientKept.second, lantern.second);
    for (auto &vertex : vertices)
        vertex[3] = 5; // Faces away from the sun: already unlit natively.
    nativeLight(.3f, 1);
    auto away = sample(false, false, false);
    assert(away.second > 245);
    printf("Faces turned away keep native shading (no double darkening): %d PASS\n", away.second);
    // A cliff face the sun grazes at ~8 degrees: one consistent shade, no stripes
    // between occluded and unoccluded texels.
    for (auto &vertex : vertices)
        vertex[3] = 0;
    light[0] = .1392f;
    light[2] = .9903f;
    nativeLight(.3f, 1);
    terrainFace = true;
    auto grazing = sample(false, false, false);
    int grazingOpen = unshadowedPixel();
    assert(abs(grazing.second - grazingOpen) <= 2 && grazing.second < 245);
    terrainFace = false;
    auto modelGrazing = sample(false, false, false);
    assert(unshadowedPixel() - modelGrazing.second > 20);
    printf("Grazed terrain face: occluded=%d open=%d (models still resolve occluders) PASS\n",
           grazing.second, grazingOpen);
    // Camera fade: reach measured to the camera (90 blocks up, Radius 100).
    for (auto &vertex : vertices)
        vertex[3] = 4;
    light[0] = 0;
    light[2] = 1;
    nativeLight(0, 1);
    eye[2] = 90;
    auto ground = sample(false, false, false);
    shadingRules[1] = 1;
    auto zoomed = sample(false, false, false);
    assert(ground.second == 102 && zoomed.second > ground.second + 60 && zoomed.second < 245);
    eye[2] = 120;
    auto beyond = sample(false, false, false);
    assert(beyond.second > 245);
    printf("Camera-distance fade: ground=%d camera 90 up=%d beyond radius=%d PASS\n", ground.second,
           zoomed.second, beyond.second);
    eye[2] = 0;
    shadingRules[0] = shadingRules[1] = 0;
    memset(nativeLighting, 0, sizeof(nativeLighting));
    for (auto &vertex : vertices)
        vertex[3] = 4;
    Draw water{};
    water.casts = false;
    water.stride = 8;
    water.type = D3DPT_TRIANGLELIST;
    memcpy(water.world, identity, 48);
    assert(!staticTerrain(water));
    Draw terrain{};
    assert(SUCCEEDED(dev->CreateVertexBuffer(256, 0, 0, D3DPOOL_MANAGED, &terrain.vb, nullptr)));
    assert(SUCCEEDED(
        dev->CreateIndexBuffer(64, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &terrain.ib, nullptr)));
    terrain.decl = decl;
    decl->AddRef();
    terrain.stride = 8;
    terrain.type = D3DPT_TRIANGLELIST;
    terrain.count = 6;
    terrain.primitives = 2;
    memcpy(terrain.world, identity, 48);
    terrain.world[3] = terrain.world[7] = terrain.world[11] = 0;
    Draw other = terrain;
    other.vb->AddRef();
    other.ib->AddRef();
    other.decl->AddRef();
    other.start = 6;
    other.world[3] = 32;
    draws.push_back(terrain);
    draws.push_back(other);
    assert(updateCache() && terrainCache.size() == 2);
    clearDraws();
    Draw moved = terrainCache[0].draw;
    moved.vb->AddRef();
    moved.ib->AddRef();
    moved.decl->AddRef();
    moved.world[3] -= 16;
    moved.world[7] -= 32;
    draws.push_back(moved);
    assert(updateCache() && terrainCache.size() == 2);
    assert(fabsf(terrainCache[1].draw.world[3] - 16) < .0001f &&
           fabsf(terrainCache[1].draw.world[7] + 32) < .0001f);
    clearDraws();
    clearCache();
    puts("Offscreen terrain cache and floating-origin shift: PASS");
    // Moon disk must be sky-depth occluded and retain native state.
    IDirect3DSurface9 *moonDepth = nullptr;
    assert(SUCCEEDED(dev->CreateDepthStencilSurface(128, 128, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0,
                                                    TRUE, &moonDepth, nullptr)));
    assert(SUCCEEDED(dev->SetDepthStencilSurface(moonDepth)));
    float nativeSun[3] = {0, 0, -1};
    moonPhase = alphaMoon::phase(nativeSun, .6f);
    sceneProjectionKnown = true;
    float skyVP[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0};
    memcpy(sceneProjection, skyVP, 64);
    enabled = moonEnabled = visibleMoon = true;
    auto moonPixel = [&](bool occluded) {
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1, 0);
        if (occluded) {
            D3DRECT center{58, 58, 70, 70};
            dev->Clear(1, &center, D3DCLEAR_ZBUFFER, 0, .5f, 0);
        }
        assert(SUCCEEDED(dev->BeginScene()));
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        renderMoon();
        DWORD cull = 0;
        dev->GetRenderState(D3DRS_CULLMODE, &cull);
        assert(cull == D3DCULL_CW);
        assert(SUCCEEDED(dev->EndScene()));
        assert(SUCCEEDED(dev->GetRenderTargetData(bb, cpu)));
        assert(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)));
        BYTE *pixel = (BYTE *)lock.pBits + 64 * lock.Pitch + 64 * 4;
        int blue = pixel[0], red = pixel[2];
        cpu->UnlockRect();
        if (!occluded)
            assert(blue > red);
        return blue;
    };
    assert(moonPixel(false) > 100);
    assert(moonPixel(true) == 0);
    puts("Moon disk cool color, foreground occlusion and native state restoration: PASS");
    // Opaque foliage may draw with ZWRITEENABLE=FALSE. Late moon composition
    // overwrites that foliage; early, once-per-scene composition must not.
    dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1, 0);
    assert(SUCCEEDED(dev->BeginScene()));
    moonSubmitted = false;
    sceneProjectionKnown = true;
    renderMoonBeforeWorld(false);
    assert(!moonSubmitted);
    renderMoonBeforeWorld(true);
    assert(moonSubmitted);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetTexture(0, nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    struct Foreground {
        float x, y, z, w;
        DWORD color;
    };
    Foreground leaf[] = {{58, 58, .5f, 1, 0xff204020},
                         {70, 58, .5f, 1, 0xff204020},
                         {58, 70, .5f, 1, 0xff204020},
                         {70, 70, .5f, 1, 0xff204020}};
    assert(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, leaf, sizeof(Foreground))));
    renderMoonBeforeWorld(true); // Subsequent models must not recompose the moon.
    assert(SUCCEEDED(dev->EndScene()));
    assert(SUCCEEDED(dev->GetRenderTargetData(bb, cpu)));
    assert(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)));
    BYTE *leafPixel = (BYTE *)lock.pBits + 64 * lock.Pitch + 64 * 4;
    assert(leafPixel[0] == 32 && leafPixel[1] == 64 && leafPixel[2] == 32);
    cpu->UnlockRect();
    puts("Moon drawn once before world: non-depth-writing foliage/models cover it: PASS");
    float savedLight[24]{};
    savedLight[2] = -1;
    savedLight[12] = 123;
    dev->SetVertexShaderConstantF(33, savedLight, 6);
    {
        NightLighting modified(dev, true);
        float changed[24]{};
        dev->GetVertexShaderConstantF(33, changed, 6);
        assert(changed[2] > 0 && changed[6] > changed[4] && changed[12] == 123);
    }
    float restoredLight[24]{};
    dev->GetVertexShaderConstantF(33, restoredLight, 6);
    assert(!memcmp(savedLight, restoredLight, sizeof(savedLight)));
    puts("Native moon lighting applied and restored around a draw: PASS");
    checkCloudCasting();
    checkCascadeReplay();
    dev->SetDepthStencilSurface(nullptr);
    release(moonDepth);
    release(cpu);
    release(bb);
    release(decl);
    release(pattern);
    dev->SetTexture(1, nullptr);
    dev->SetTexture(2, nullptr);
    release(cloudPattern);
    cleanup();
    oldReset = (Reset)((*(void ***)dev)[16]);
    for (int repeat = 0; repeat < 3; repeat++) {
        assert(initializeMoon());
        assert(SUCCEEDED(reset(dev, &pp)));
        assert(!moonShader && !ready && !sceneProjectionKnown);
    }
    puts("Moon resources released across three device resets: PASS");
    for (int cycle = 0; cycle < 2; ++cycle) {
        cascadesEnabled = cycle == 0;
        assert(initialize());
        assert(bool(nearTexture) == cascadesEnabled);
        assert(SUCCEEDED(reset(dev, &pp)));
        assert(!nearTexture && !nearDepth && !nearSurface);
    }
    puts("Near/far map allocation and release, including single-map fallback: PASS");
    release(dev);
    release(api);
    DestroyWindow(window);
    return 0;
}
