#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <vector>
#include <set>
#include <string>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "AlphaMoonRules.h"

// Sun shadow prototype. Replays opaque world meshes into a light-space map,
// then blends visibility over the world before Alpha clears depth for its HUD.
static HMODULE self; static IDirect3DDevice9* dev; static void** vt;
static bool internal=false, enabled=true, rendered=false, ready=false;
static volatile LONG stopRequest=0,captureRequest=0,toggleRequest=0;
static bool softShadows=false,filterEdges=false,failed=false;static DWORD modelArray=0;
static UINT resolution=4096; static float radius=100,strength=.48f,bias=.008f/256,warmth=.03f;static float sunTint[3]={1,.93f,.76f};
static bool moonEnabled=true,visibleMoon=true,sceneProjectionKnown=false;
static float nightBrightness=.04f,moonShadowStrength=.60f,nightWeight=0,activeStrength=0,sceneProjection[16]{};
static alphaMoon::Phase moonPhase;static IDirect3DPixelShader9*moonShader=nullptr;
static unsigned frames=0,successful=0;static double renderTotalMs=0; static float camera[4]{},sun[4]{};
template<class T> static void release(T*& p){if(p){p->Release();p=nullptr;}}
static std::wstring directory(){wchar_t p[32768];GetModuleFileNameW(self,p,32768);std::wstring s=p;s.resize(s.find_last_of(L"\\"));if(s.substr(s.find_last_of(L"\\")+1)==L"Mods")s=s.substr(0,s.find_last_of(L"\\"))+L"\\graphics-rebuild";return s;}
static void log(const char* s){FILE* f=nullptr;_wfopen_s(&f,(directory()+L"\\AlphaDirectional.log").c_str(),L"a");if(f){fprintf(f,"%s\n",s);fclose(f);}}
using Dip=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
using Clear=HRESULT(WINAPI*)(IDirect3DDevice9*,DWORD,const D3DRECT*,DWORD,D3DCOLOR,float,DWORD);
using Present=HRESULT(WINAPI*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
using Reset=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
static Dip oldDip; static Clear oldClear; static Present oldPresent; static Reset oldReset;
struct Draw { bool casts=true; IDirect3DVertexBuffer9* vb=nullptr; IDirect3DIndexBuffer9* ib=nullptr; IDirect3DVertexDeclaration9* decl=nullptr; UINT offset=0,stride=0,min=0,count=0,start=0,primitives=0; INT base=0; D3DPRIMITIVETYPE type; float wvp[16]{},world[12]{},points[80]{}; };
static std::vector<Draw> draws;
struct Cached {Draw draw;ULONGLONG seen;UINT bytes;};static std::vector<Cached> terrainCache;static UINT cachedBytes=0;
static void dropDraw(Draw& x){release(x.vb);release(x.ib);release(x.decl);}
static void clearCache(){for(auto& c:terrainCache)dropDraw(c.draw);terrainCache.clear();cachedBytes=0;}
static bool staticTerrain(const Draw& x){return x.casts&&x.stride==8&&x.type==D3DPT_TRIANGLELIST&&fabsf(x.world[0]-1)<.00001f&&fabsf(x.world[5]-1)<.00001f&&fabsf(x.world[10]-1)<.00001f&&fabsf(x.world[1])+fabsf(x.world[2])+fabsf(x.world[4])+fabsf(x.world[6])+fabsf(x.world[8])+fabsf(x.world[9])<.00001f;}
static bool sameMesh(const Draw&a,const Draw&b){return a.vb==b.vb&&a.ib==b.ib&&a.base==b.base&&a.start==b.start&&a.count==b.count&&a.offset==b.offset&&a.primitives==b.primitives;}
static bool updateCache(){
 ULONGLONG now=GetTickCount64();bool aligned=terrainCache.empty();float shift[3]{};unsigned anchors=0;
 for(const auto& x:draws)if(staticTerrain(x))for(const auto& c:terrainCache)if(sameMesh(x,c.draw)){
  float delta[3]={x.world[3]-c.draw.world[3],x.world[7]-c.draw.world[7],x.world[11]-c.draw.world[11]};
  if(!anchors)memcpy(shift,delta,sizeof(shift));else if(fabsf(delta[0]-shift[0])+fabsf(delta[1]-shift[1])+fabsf(delta[2]-shift[2])>.01f){clearCache();return false;}anchors++;break;}
 if(anchors){aligned=true;for(auto& c:terrainCache){c.draw.world[3]+=shift[0];c.draw.world[7]+=shift[1];c.draw.world[11]+=shift[2];}}
 if(!aligned){clearCache();aligned=true;}
 for(auto it=terrainCache.begin();it!=terrainCache.end();){float dx=it->draw.world[3]-camera[0],dy=it->draw.world[7]-camera[1];if(now-it->seen>30000||fabsf(dx)>radius+64||fabsf(dy)>radius+64){cachedBytes-=it->bytes;dropDraw(it->draw);it=terrainCache.erase(it);}else ++it;}
 for(const auto& x:draws)if(staticTerrain(x)){
  unsigned instances=0;for(const auto& y:draws)if(staticTerrain(y)&&sameMesh(x,y))instances++;if(instances!=1)continue;
  auto found=std::find_if(terrainCache.begin(),terrainCache.end(),[&](const Cached& c){return sameMesh(x,c.draw);});if(found!=terrainCache.end()){if(found->draw.decl!=x.decl){x.decl->AddRef();release(found->draw.decl);}found->draw=x;found->seen=now;continue;}
  // Replace remeshed geometry occupying the same chunk transform.
  for(auto it=terrainCache.begin();it!=terrainCache.end();)if(fabsf(it->draw.world[3]-x.world[3])+fabsf(it->draw.world[7]-x.world[7])+fabsf(it->draw.world[11]-x.world[11])<.001f&&it->draw.vb!=x.vb){cachedBytes-=it->bytes;dropDraw(it->draw);it=terrainCache.erase(it);}else ++it;
  D3DVERTEXBUFFER_DESC vd{};D3DINDEXBUFFER_DESC id{};if(FAILED(x.vb->GetDesc(&vd))||FAILED(x.ib->GetDesc(&id)))continue;UINT bytes=vd.Size+id.Size;if(terrainCache.size()>=512||bytes>96*1024*1024-cachedBytes)continue;
  Draw kept=x;kept.vb->AddRef();kept.ib->AddRef();kept.decl->AddRef();terrainCache.push_back({kept,now,bytes});cachedBytes+=bytes;
 }return aligned;
}
static IDirect3DTexture9* mapTexture=nullptr;static IDirect3DSurface9 *mapSurface=nullptr,*mapDepth=nullptr;
static IDirect3DVertexShader9 *casterVS=nullptr,*receiverVS=nullptr;static IDirect3DPixelShader9 *casterPS=nullptr,*receiverPS=nullptr;
static void clearDraws(){for(auto& x:draws){release(x.vb);release(x.ib);release(x.decl);}draws.clear();}
static void cleanup(){clearCache();clearDraws();release(mapTexture);release(mapSurface);release(mapDepth);release(casterVS);release(receiverVS);release(casterPS);release(receiverPS);release(moonShader);sceneProjectionKnown=false;ready=false;failed=false;}
struct NightLighting{
 IDirect3DDevice9*d;bool changed=false;float saved[24]{};
 NightLighting(IDirect3DDevice9*device,bool native):d(device){if(!native||internal)return;
 float direction[4]{};if(FAILED(d->GetVertexShaderConstantF(33,direction,1)))return;
 moonPhase=moonEnabled?alphaMoon::phase(direction,moonShadowStrength):alphaMoon::Phase{};nightWeight=moonPhase.weight;
 if(nightWeight>0){for(int i=0;i<3;i++)sun[i]=moonPhase.direction[i];activeStrength=moonPhase.strength;
 if(SUCCEEDED(d->GetVertexShaderConstantF(33,saved,6))){float values[24]{};alphaMoon::lighting(values,saved,moonPhase,nightBrightness);changed=SUCCEEDED(d->SetVertexShaderConstantF(33,values,6));}}
 else{memcpy(sun,direction,16);activeStrength=strength;}
 }
 ~NightLighting(){if(changed)d->SetVertexShaderConstantF(33,saved,6);}
};
struct Vec {float x,y,z;};
static float dot(Vec a,Vec b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static Vec cross(Vec a,Vec b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static Vec normalize(Vec a){float n=sqrtf(dot(a,a));return n>1e-6f?Vec{a.x/n,a.y/n,a.z/n}:Vec{0,0,1};}
static void lightMatrix(float* m){Vec l=normalize({sun[0],sun[1],sun[2]}),f{-l.x,-l.y,-l.z};Vec r=normalize(cross(f,fabsf(f.z)>.95f?Vec{0,1,0}:Vec{0,0,1}));Vec u=cross(r,f);Vec c{camera[0],camera[1],camera[2]};float snap=2*radius/resolution;
 float cx=floorf(dot(r,c)/snap+.5f)*snap,cy=floorf(dot(u,c)/snap+.5f)*snap;
 float a[16]={r.x/radius,r.y/radius,r.z/radius,-cx/radius,u.x/radius,u.y/radius,u.z/radius,-cy/radius,f.x/256,f.y/256,f.z/256,.5f-dot(f,c)/256,0,0,0,1};memcpy(m,a,sizeof(a));}
static const char* source=R"HLSL(
float4 light0:register(c0),light1:register(c1),light2:register(c2),light3:register(c3);
float4 worldViewProjMatrix[4]:register(c20);
float4 worldMatrix[3]:register(c27);
float4 pointPositions[10]:register(c40);float4 pointColors[10]:register(c50);
struct V {float4 p:POSITION;float3 world:TEXCOORD0;float depth:TEXCOORD1;float3 normal:TEXCOORD2;float local:TEXCOORD3;};
V caster(float4 p:POSITION){V o;o.normal=0;o.local=0;p.w=1;float4 w=float4(dot(worldMatrix[0],p),dot(worldMatrix[1],p),dot(worldMatrix[2],p),1);o.p=float4(dot(light0,w),dot(light1,w),dot(light2,w),1);o.world=w.xyz;o.depth=o.p.z;return o;}
V receiver(float4 p:POSITION){V o;float3 n=p.w==0?float3(1,0,0):p.w==1?float3(-1,0,0):p.w==2?float3(0,1,0):p.w==3?float3(0,-1,0):p.w==4?float3(0,0,1):float3(0,0,-1);o.normal=normalize(float3(dot(worldMatrix[0].xyz,n),dot(worldMatrix[1].xyz,n),dot(worldMatrix[2].xyz,n)));p.w=1;o.p=float4(dot(worldViewProjMatrix[0],p),dot(worldViewProjMatrix[1],p),dot(worldViewProjMatrix[2],p),dot(worldViewProjMatrix[3],p));o.world=float3(dot(worldMatrix[0],p),dot(worldMatrix[1],p),dot(worldMatrix[2],p));o.depth=0;o.local=0;
 [loop]for(int j=0;j<10;j++)if(pointPositions[j].w>0){float3 delta=o.world-pointPositions[j].xyz;float d2=dot(delta,delta);float falloff=saturate(1-d2/(pointPositions[j].w*pointPositions[j].w));float facing=clamp(-4*dot(o.normal,delta*rsqrt(max(d2,.0001))),.3,1);o.local+=falloff*falloff*facing*max(pointColors[j].r,max(pointColors[j].g,pointColors[j].b));}o.local=saturate(o.local);return o;}
float4 depth(V i):COLOR0{return i.depth;}
sampler2D shadowMap:register(s0);
float4 projection0:register(c0),projection1:register(c1),projection2:register(c2);
float4 config:register(c3);float4 eye:register(c4);float4 sunlight:register(c5);float4 filtering:register(c6);float4 sunTone:register(c7);float4 detail:register(c8);
float compareDepth(float2 uv,float z){return z<=tex2Dlod(shadowMap,float4(uv,0,0)).r?1:0;}
float filteredDepth(float2 uv,float z){if(filtering.x<.5)return compareDepth(uv,z);float2 pixel=uv/config.x-.5;float2 fraction=frac(pixel);float2 base=(floor(pixel)+.5)*config.x;float a=compareDepth(base,z),b=compareDepth(base+float2(config.x,0),z),c=compareDepth(base+float2(0,config.x),z),d=compareDepth(base+config.xx,z);return lerp(lerp(a,b,fraction.x),lerp(c,d,fraction.x),fraction.y);}
float4 visibility(V i):COLOR0{float4 w=float4(i.world,1);float3 p=float3(dot(projection0,w),dot(projection1,w),dot(projection2,w));float2 uv=float2(p.x,-p.y)*.5+.5+config.x*.5;
 if(any(uv<config.x)||any(uv>1-config.x)||p.z<0||p.z>1)return 0;
 float3 normal=normalize(i.normal);float facing=dot(normal,sunlight.xyz);
 float2 ux=ddx(uv),uy=ddy(uv);float zx=ddx(p.z),zy=ddy(p.z);float det=ux.x*uy.y-ux.y*uy.x;
 float slope=sqrt(saturate(1-facing*facing))/max(abs(facing),.1);
 float footprint=config.x*config.w/128*min(slope,8);
 if(abs(det)>1e-12){float2 gradient=float2(zx*uy.y-zy*ux.y,zy*ux.x-zx*uy.x)/det;footprint=config.x*(abs(gradient.x)+abs(gradient.y));}
 // Steam's accepted small-entity footprint uses the face normal instead of
 // derivatives spanning adjacent voxel faces. Light-space span here is 256.
 if(detail.x>.5){float denom=max(abs(dot(normal,projection2.xyz)),1e-7);footprint=config.x*(abs(dot(normal,projection0.xyz))+abs(dot(normal,projection1.xyz)))/denom*config.w*config.w/32768;}
 float baseBias=detail.x>.5?max(config.z,.1/256):config.z;
 float b=baseBias+min(footprint,config.x*config.w/128*16)*(sunlight.w>.5?2.1:1.1);float lit=0;
 [branch]if(sunlight.w>.5){[unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)lit+=filteredDepth(uv+float2(x,y)*config.x,p.z-b);lit/=9;}else lit=filteredDepth(uv,p.z-b);
 float fade=1-smoothstep(config.w*.75,config.w,length(i.world.xy-eye.xy));lit*=smoothstep(0,.08,facing);float available=fade*(1-saturate(i.local));float shade=(1-lit)*config.y*available;float warm=lit*sunTone.w*available*saturate(facing);float opacity=saturate(shade+warm);return float4(sunTone.rgb*warm/max(opacity,.00001),opacity);}
)HLSL";
static bool initialize(){HMODULE h=LoadLibraryW(L"d3dcompiler_47.dll");if(!h){failed=true;log("Missing d3dcompiler_47.dll; shadows disabled.");return false;}auto compile=(decltype(&D3DCompile))GetProcAddress(h,"D3DCompile");bool ok=compile!=nullptr;
 const char* names[]={"caster","receiver","depth","visibility"};for(int i=0;i<4&&ok;i++){ID3DBlob *code=nullptr,*err=nullptr;HRESULT hr=compile(source,strlen(source),"AlphaShadows",nullptr,nullptr,names[i],i<2?"vs_3_0":"ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&err);if(err){log((char*)err->GetBufferPointer());release(err);}ok=SUCCEEDED(hr)&&code;if(ok)ok=SUCCEEDED(i<2?dev->CreateVertexShader((DWORD*)code->GetBufferPointer(),i?&receiverVS:&casterVS):dev->CreatePixelShader((DWORD*)code->GetBufferPointer(),i==2?&casterPS:&receiverPS));release(code);}FreeLibrary(h);
 if(ok)ok=SUCCEEDED(dev->CreateTexture(resolution,resolution,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,D3DPOOL_DEFAULT,&mapTexture,nullptr))&&SUCCEEDED(mapTexture->GetSurfaceLevel(0,&mapSurface))&&SUCCEEDED(dev->CreateDepthStencilSurface(resolution,resolution,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,&mapDepth,nullptr));
 if(!ok){cleanup();failed=true;log("Shadow resource/shader initialization failed; rendering unchanged until device reset.");return false;}ready=true;char info[160];sprintf_s(info,"R32F sun shadow map initialized: %ux%u, edge filtering=%d, soft=%d",resolution,resolution,filterEdges,softShadows);log(info);return true;}
static bool initializeMoon(){if(moonShader)return true;HMODULE compiler=LoadLibraryW(L"d3dcompiler_47.dll");if(!compiler)return false;auto fn=(decltype(&D3DCompile))GetProcAddress(compiler,"D3DCompile");
 const char*code=R"HLSL(
 float4 disk:register(c0);
 float4 moon(float2 uv:TEXCOORD0):COLOR0{float2 q=floor((uv*2-1)*disk.z/3)*3/disk.z;float d=length(q);clip(1-d);float crater=0;crater+=length(q-float2(-.3,.2))<.20?.10:0;crater+=length(q-float2(.3,-.25))<.15?.07:0;crater+=length(q-float2(.15,.4))<.12?.06:0;return float4(float3(.66,.76,.92)-crater,disk.w*(.8+.2*(1-smoothstep(.86,1,d))));}
 )HLSL";
 ID3DBlob*b=nullptr,*errors=nullptr;bool ok=fn&&SUCCEEDED(fn(code,strlen(code),"AlphaMoon",nullptr,nullptr,"moon","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&errors));if(errors)log((char*)errors->GetBufferPointer());if(ok)ok=SUCCEEDED(dev->CreatePixelShader((DWORD*)b->GetBufferPointer(),&moonShader));release(b);release(errors);FreeLibrary(compiler);return ok;}
static void renderMoon(){if(!enabled||!moonEnabled||!visibleMoon||!sceneProjectionKnown||moonPhase.weight<=0)return;D3DVIEWPORT9 viewport{};dev->GetViewport(&viewport);float disk[4]{};if(!alphaMoon::disk(disk,sceneProjection,moonPhase,viewport.Width,viewport.Height)||!initializeMoon())return;
 IDirect3DStateBlock9*state=nullptr;if(FAILED(dev->CreateStateBlock(D3DSBT_ALL,&state)))return;internal=true;dev->SetVertexShader(nullptr);dev->SetPixelShader(moonShader);dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);dev->SetTexture(0,nullptr);dev->SetRenderState(D3DRS_ZENABLE,TRUE);dev->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);dev->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);dev->SetRenderState(D3DRS_DEPTHBIAS,0);dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS,0);dev->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);dev->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);dev->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);dev->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);dev->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);dev->SetRenderState(D3DRS_STENCILENABLE,FALSE);dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);dev->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);dev->SetRenderState(D3DRS_COLORWRITEENABLE,7);dev->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);dev->SetPixelShaderConstantF(0,disk,1);
 struct Vertex{float x,y,z,w,u,v;};float x=disk[0]+viewport.X-.5f,y=disk[1]+viewport.Y-.5f,r=disk[2],z=1.f-1.f/16777216.f;Vertex v[]={{x-r,y-r,z,1,0,0},{x+r,y-r,z,1,1,0},{x-r,y+r,z,1,0,1},{x+r,y+r,z,1,1,1}};dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,v,sizeof(Vertex));state->Apply();release(state);internal=false;
}
static void bind(const Draw& x){float modelScale=sqrtf(x.world[0]*x.world[0]+x.world[1]*x.world[1]+x.world[2]*x.world[2]);float detail[4]={!staticTerrain(x)&&modelScale<.5f?1.f:0.f,0,0,0};dev->SetPixelShaderConstantF(8,detail,1);dev->SetStreamSource(0,x.vb,x.offset,x.stride);dev->SetIndices(x.ib);dev->SetVertexDeclaration(x.decl);dev->SetVertexShaderConstantF(20,x.wvp,4);dev->SetVertexShaderConstantF(27,x.world,3);dev->SetVertexShaderConstantF(40,x.points,20);}
static void render(){if(!enabled||draws.empty()||rendered||failed)return;rendered=true;Vec daylight=normalize({sun[0],sun[1],sun[2]});float daylightWeight=std::clamp(daylight.z/.15f,0.f,1.f);if(daylightWeight<=0||(activeStrength<=0&&warmth<=0))return;if(!ready&&!initialize())return;
 IDirect3DStateBlock9* state=nullptr;IDirect3DSurface9 *rt[4]{},*ds=nullptr;D3DVIEWPORT9 vp{};if(FAILED(dev->CreateStateBlock(D3DSBT_ALL,&state)))return;
 for(int i=0;i<4;i++)dev->GetRenderTarget(i,&rt[i]);dev->GetDepthStencilSurface(&ds);dev->GetViewport(&vp);if(!ds){release(state);for(auto& p:rt)release(p);return;}
 LARGE_INTEGER begin{},end{},frequency{};QueryPerformanceCounter(&begin);QueryPerformanceFrequency(&frequency);internal=true;float matrix[16];lightMatrix(matrix);dev->SetTexture(0,nullptr);dev->SetDepthStencilSurface(nullptr);for(int i=1;i<4;i++)dev->SetRenderTarget(i,nullptr);dev->SetRenderTarget(0,mapSurface);dev->SetDepthStencilSurface(mapDepth);D3DVIEWPORT9 mv{0,0,resolution,resolution,0,1};dev->SetViewport(&mv);
 oldClear(dev,0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xffffffff,1,0);
 dev->SetRenderState(D3DRS_ZENABLE,TRUE);dev->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);dev->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);dev->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);dev->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);dev->SetRenderState(D3DRS_STENCILENABLE,FALSE);dev->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);dev->SetRenderState(D3DRS_COLORWRITEENABLE,15);dev->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);dev->SetRenderState(D3DRS_DEPTHBIAS,0);dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS,0);
 updateCache();dev->SetVertexShader(casterVS);dev->SetPixelShader(casterPS);dev->SetVertexShaderConstantF(0,matrix,4);
 unsigned failed=0;for(const auto& c:terrainCache){bind(c.draw);if(FAILED(oldDip(dev,c.draw.type,c.draw.base,c.draw.min,c.draw.count,c.draw.start,c.draw.primitives)))failed++;}for(const auto& x:draws){if(!x.casts)continue;if(staticTerrain(x)&&std::any_of(terrainCache.begin(),terrainCache.end(),[&](const Cached& c){return sameMesh(c.draw,x);}))continue;bind(x);if(FAILED(oldDip(dev,x.type,x.base,x.min,x.count,x.start,x.primitives)))failed++;}
 dev->SetDepthStencilSurface(nullptr);dev->SetRenderTarget(0,rt[0]);dev->SetDepthStencilSurface(ds);dev->SetViewport(&vp);
 dev->SetVertexShader(receiverVS);dev->SetPixelShader(receiverPS);float receiverOffset=-2.f/16777216.f;DWORD receiverBits;memcpy(&receiverBits,&receiverOffset,4);dev->SetRenderState(D3DRS_DEPTHBIAS,receiverBits);dev->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);dev->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);dev->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);dev->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);dev->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);dev->SetRenderState(D3DRS_COLORWRITEENABLE,7);
 dev->SetTexture(0,mapTexture);dev->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);dev->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);dev->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);dev->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);dev->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);dev->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
 float settings[4]={1.f/resolution,activeStrength*daylightWeight,bias,radius};dev->SetPixelShaderConstantF(0,matrix,3);dev->SetPixelShaderConstantF(3,settings,1);dev->SetPixelShaderConstantF(4,camera,1);Vec l=normalize({sun[0],sun[1],sun[2]});float light[4]={l.x,l.y,l.z,softShadows?1.f:0.f};dev->SetPixelShaderConstantF(5,light,1);float filter[4]={filterEdges?1.f:0.f,0,0,0};dev->SetPixelShaderConstantF(6,filter,1);float tone[4]={sunTint[0],sunTint[1],sunTint[2],warmth*daylightWeight*(1-nightWeight)};dev->SetPixelShaderConstantF(7,tone,1);
 for(const auto& x:draws){bind(x);if(FAILED(oldDip(dev,x.type,x.base,x.min,x.count,x.start,x.primitives)))failed++;}
 state->Apply();dev->SetDepthStencilSurface(nullptr);for(int i=0;i<4;i++)dev->SetRenderTarget(i,rt[i]);dev->SetDepthStencilSurface(ds);dev->SetViewport(&vp);release(state);release(ds);for(auto& p:rt)release(p);internal=false;
 QueryPerformanceCounter(&end);renderTotalMs+=double(end.QuadPart-begin.QuadPart)*1000/frequency.QuadPart;if(!failed)successful++;if(successful==1||frames%600==0){char b[256];sprintf_s(b,"Frame %u: %zu meshes, %u failed draws; camera %.2f %.2f %.2f sun %.3f %.3f %.3f; mean CPU submission %.2f ms; retained terrain=%zu",frames,draws.size(),failed,camera[0],camera[1],camera[2],sun[0],sun[1],sun[2],renderTotalMs/std::max(successful,1u),terrainCache.size());log(b);}}
static bool worldShader(IDirect3DVertexShader9* s){UINT n=0;if(!s||FAILED(s->GetFunction(nullptr,&n))||n>65536)return false;std::vector<char> b(n);if(FAILED(s->GetFunction(b.data(),&n)))return false;auto has=[&](const char* name){size_t l=strlen(name)+1;return std::search(b.begin(),b.end(),name,name+l)!=b.end();};return has("worldViewProjMatrix")&&has("worldMatrix")&&has("lightDirection");}
static std::set<IDirect3DVertexShader9*> good,bad;
static HRESULT WINAPI dip(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,INT base,UINT min,UINT count,UINT start,UINT primitives){bool nativeLighting=false;
 if(!internal&&enabled&&!rendered&&draws.size()<6000){DWORD z=0,write=0;d->GetRenderState(D3DRS_ZENABLE,&z);d->GetRenderState(D3DRS_ZWRITEENABLE,&write);if(z){IDirect3DVertexShader9* vs=nullptr;d->GetVertexShader(&vs);if(captureRequest&&vs){static std::set<IDirect3DVertexShader9*> probed;if(probed.insert(vs).second&&probed.size()<=20){UINT bytes=0;vs->GetFunction(nullptr,&bytes);std::vector<char> code(bytes);if(SUCCEEDED(vs->GetFunction(code.data(),&bytes))){char info[160];sprintf_s(info,"Receiver probe: shader=%p depth=%lu write=%lu count=%u",vs,z,write,count);log(info);for(size_t at=0;at+32<code.size();at+=4)if(!memcmp(code.data()+at,"CTAB",4)){const char* tab=code.data()+at+4;size_t remain=code.size()-at-4;DWORD n=0,o=0;memcpy(&n,tab+12,4);memcpy(&o,tab+16,4);if(n>200||o+n*20>remain)continue;for(DWORD j=0;j<n;j++){DWORD name=0;WORD reg=0,num=0;memcpy(&name,tab+o+j*20,4);memcpy(&reg,tab+o+j*20+6,2);memcpy(&num,tab+o+j*20+8,2);if(name<remain&&memchr(tab+name,0,remain-name)){sprintf_s(info,"Receiver constant: %.100s c%u count%u",tab+name,reg,num);log(info);}}}}}}bool accepted=good.count(vs)!=0;if(!accepted&&!bad.count(vs)){accepted=worldShader(vs);(accepted?good:bad).insert(vs);}release(vs);if(accepted){nativeLighting=true;Draw x{};x.casts=write!=0;x.type=t;x.base=base;x.min=min;x.count=count;x.start=start;x.primitives=primitives;d->GetVertexShaderConstantF(20,x.wvp,4);
 // Query the upstream TAA hook: draw replay uses the same scene projection.
 using JitterQuery=BOOL(WINAPI*)(float*);static JitterQuery query=nullptr;static bool checked=false;MEMORY_BASIC_INFORMATION owner{};
 if(!checked&&VirtualQuery((void*)oldClear,&owner,sizeof(owner))){query=(JitterQuery)GetProcAddress((HMODULE)owner.AllocationBase,"_QueryAlphaSceneJitter@4");if(!query)query=(JitterQuery)GetProcAddress((HMODULE)owner.AllocationBase,"QueryAlphaSceneJitter");checked=true;log(query?"Shared temporal scene jitter connected.":"Temporal jitter unavailable; native projection used.");}float xy[2]{};if(query&&query(xy))for(int c=0;c<4;c++){x.wvp[c]+=2*xy[0]*x.wvp[12+c];x.wvp[4+c]-=2*xy[1]*x.wvp[12+c];}
 d->GetVertexShaderConstantF(27,x.world,3);d->GetVertexShaderConstantF(0,x.points,20);if(!sceneProjectionKnown&&fabsf(x.wvp[12])+fabsf(x.wvp[13])+fabsf(x.wvp[14])>1e-5f){IDirect3DSurface9*rt=nullptr,*bb=nullptr;D3DVIEWPORT9 viewport{};d->GetRenderTarget(0,&rt);d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&bb);d->GetViewport(&viewport);if(rt&&rt==bb&&viewport.Width>=320&&viewport.Height>=180)sceneProjectionKnown=alphaMoon::cameraProjection(sceneProjection,x.wvp,x.world);release(rt);release(bb);}
 if(fabsf(x.wvp[12])+fabsf(x.wvp[13])+fabsf(x.wvp[14])>1e-5f){d->GetVertexShaderConstantF(36,camera,1);d->GetVertexShaderConstantF(33,sun,1);d->GetStreamSource(0,&x.vb,&x.offset,&x.stride);d->GetIndices(&x.ib);d->GetVertexDeclaration(&x.decl);DWORD cloudModel=0,cloudVB=0;SIZE_T got=0;if(modelArray){ReadProcessMemory(GetCurrentProcess(),(void*)(uintptr_t)(modelArray+0xa08*4),&cloudModel,4,&got);if(cloudModel)ReadProcessMemory(GetCurrentProcess(),(void*)(uintptr_t)(cloudModel+0x34),&cloudVB,4,&got);}if(x.vb&&x.ib&&x.decl&&(DWORD)(uintptr_t)x.vb!=cloudVB)draws.push_back(x);else{release(x.vb);release(x.ib);release(x.decl);}}}}}
 NightLighting lighting(d,nativeLighting);return oldDip(d,t,base,min,count,start,primitives);}
static HRESULT WINAPI clear(IDirect3DDevice9* d,DWORD n,const D3DRECT* rect,DWORD flags,D3DCOLOR c,float z,DWORD stencil){if(!internal&&(flags&D3DCLEAR_ZBUFFER)&&!(flags&D3DCLEAR_TARGET)&&!draws.empty()){renderMoon();render();}return oldClear(d,n,rect,flags,c,z,stencil);}
static void* exchange(int slot,void* fn){DWORD p=0;if(!VirtualProtect(vt+slot,4,PAGE_EXECUTE_READWRITE,&p))return nullptr;void* old=InterlockedExchangePointer(vt+slot,fn);DWORD ignore;VirtualProtect(vt+slot,4,p,&ignore);return old;}
static void capture(){IDirect3DSurface9 *bb=nullptr,*cpu=nullptr;dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&bb);if(!bb)return;D3DSURFACE_DESC desc{};bb->GetDesc(&desc);if(SUCCEEDED(dev->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&cpu,nullptr))&&SUCCEEDED(dev->GetRenderTargetData(bb,cpu))){D3DLOCKED_RECT locked{};if(SUCCEEDED(cpu->LockRect(&locked,nullptr,D3DLOCK_READONLY))){FILE* f=nullptr;_wfopen_s(&f,(directory()+L"\\AlphaShadows-capture.bmp").c_str(),L"wb");if(f){BITMAPFILEHEADER bh{};bh.bfType=0x4d42;bh.bfOffBits=sizeof(bh)+sizeof(BITMAPINFOHEADER);bh.bfSize=bh.bfOffBits+desc.Width*desc.Height*4;BITMAPINFOHEADER bi{};bi.biSize=sizeof(bi);bi.biWidth=desc.Width;bi.biHeight=-(LONG)desc.Height;bi.biPlanes=1;bi.biBitCount=32;fwrite(&bh,sizeof(bh),1,f);fwrite(&bi,sizeof(bi),1,f);for(UINT y=0;y<desc.Height;y++)fwrite((BYTE*)locked.pBits+y*locked.Pitch,4,desc.Width,f);fclose(f);}cpu->UnlockRect();}}release(bb);release(cpu);}
static HRESULT WINAPI present(IDirect3DDevice9* d,const RECT* a,const RECT* b,HWND w,const RGNDATA* r){if(stopRequest){cleanup();exchange(82,(void*)oldDip);exchange(43,(void*)oldClear);exchange(16,(void*)oldReset);exchange(17,(void*)oldPresent);log("Shadow hooks detached safely.");return oldPresent(d,a,b,w,r);}if(InterlockedExchange(&toggleRequest,0)){enabled=!enabled;log(enabled?"Sun shadows enabled.":"Sun shadows disabled.");}D3DDEVICE_CREATION_PARAMETERS creation{};d->GetCreationParameters(&creation);if(GetForegroundWindow()==(w?w:creation.hFocusWindow)&&(GetAsyncKeyState(VK_F7)&1))enabled=!enabled;
 if(captureRequest){InterlockedExchange(&captureRequest,0);capture();char coverage[160];sprintf_s(coverage,"Shadow coverage: %zu casters, %zu non-depth-writing receivers",(size_t)std::count_if(draws.begin(),draws.end(),[](const Draw& x){return x.casts;}),(size_t)std::count_if(draws.begin(),draws.end(),[](const Draw& x){return !x.casts;}));log(coverage);auto sorted=draws;std::sort(sorted.begin(),sorted.end(),[](const Draw&a,const Draw&b){return a.count>b.count;});for(size_t i=0;i<std::min(sorted.size(),size_t(15));i++){const auto& x=sorted[i];char text[256];sprintf_s(text,"Mesh %zu: VB=%p count=%u stride=%u scale=(%.5f %.5f %.5f) translation=(%.3f %.3f %.3f)",i,x.vb,x.count,x.stride,x.world[0],x.world[5],x.world[10],x.world[3],x.world[7],x.world[11]);log(text);}}if(frames==120&&!successful)log("No world shadow pass completed; waiting for world geometry/depth clear.");clearDraws();rendered=false;sceneProjectionKnown=false;frames++;return oldPresent(d,a,b,w,r);}
static HRESULT WINAPI reset(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* p){cleanup();good.clear();bad.clear();rendered=false;sceneProjectionKnown=false;return oldReset(d,p);}
extern "C" __declspec(dllexport) DWORD WINAPI DisableShadows(void*){InterlockedExchange(&stopRequest,1);return 0;}
extern "C" __declspec(dllexport) DWORD WINAPI ToggleShadows(void*){InterlockedExchange(&toggleRequest,1);return 0;}
extern "C" __declspec(dllexport) DWORD WINAPI CaptureShadows(void*){InterlockedExchange(&captureRequest,1);return 0;}
extern "C" __declspec(dllexport) int ShadowSelfTest(){sun[0]=.5f;sun[1]=.5f;sun[2]=.7071f;camera[0]=10;camera[1]=20;camera[2]=125;float m[16];lightMatrix(m);float p[4]={10,20,125,1};for(int i=0;i<3;i++){float v=0;for(int j=0;j<4;j++)v+=m[i*4+j]*p[j];if(fabsf(v-(i==2?.5f:0))>2.f/resolution)return i+1;}return 0;}
static bool read(DWORD a,void* b,size_t n){SIZE_T got=0;return ReadProcessMemory(GetCurrentProcess(),(LPCVOID)(uintptr_t)a,b,n,&got)&&got==n;}
static DWORD pointer(DWORD a){DWORD v=0;read(a,&v,4);return v;}
static DWORD WINAPI worker(void*){std::wstring ini=directory()+L"\\AlphaDirectional.ini";enabled=GetPrivateProfileIntW(L"Shadows",L"Enabled",1,ini.c_str())!=0;resolution=std::clamp(GetPrivateProfileIntW(L"Shadows",L"Resolution",3072,ini.c_str()),512u,4096u);auto option=[&](const wchar_t* key,const wchar_t* value,float lo,float hi){wchar_t s[64];GetPrivateProfileStringW(L"Shadows",key,value,s,64,ini.c_str());return std::clamp((float)_wtof(s),lo,hi);};radius=option(L"Radius",L"100",8,128);strength=option(L"Strength",L"0.60",0,1);int percent=GetPrivateProfileIntW(L"Shadows",L"StrengthPercent",-1,ini.c_str());if(percent>=0)strength=std::clamp(percent*.01f,0.f,1.f);softShadows=GetPrivateProfileIntW(L"Shadows",L"SoftShadows",0,ini.c_str())!=0;filterEdges=GetPrivateProfileIntW(L"Shadows",L"FilterEdges",0,ini.c_str())!=0;bias=option(L"Bias",L"0.00020",.00001f,.005f);warmth=std::clamp(GetPrivateProfileIntW(L"Shadows",L"SunlitWarmthPercent",10,ini.c_str())*.01f,0.f,.3f);sunTint[0]=option(L"SunlitRed",L"1",0,1);sunTint[1]=option(L"SunlitGreen",L"0.93",0,1);sunTint[2]=option(L"SunlitBlue",L"0.76",0,1);
 moonEnabled=GetPrivateProfileIntW(L"Moon",L"Enabled",1,ini.c_str())!=0;visibleMoon=GetPrivateProfileIntW(L"Moon",L"VisibleMoon",1,ini.c_str())!=0;nightBrightness=std::clamp(GetPrivateProfileIntW(L"Moon",L"BrightnessPercent",4,ini.c_str()),0u,50u)*.01f;moonShadowStrength=std::clamp(GetPrivateProfileIntW(L"Moon",L"ShadowStrengthPercent",60,ini.c_str()),0u,100u)*.01f;
 DWORD base=(DWORD)(uintptr_t)GetModuleHandleW(nullptr);IMAGE_DOS_HEADER dos{};read(base,&dos,sizeof(dos));IMAGE_NT_HEADERS32 nt{};read(base+dos.e_lfanew,&nt,sizeof(nt));DWORD section=base+dos.e_lfanew+24+nt.FileHeader.SizeOfOptionalHeader;
 for(int attempt=0;attempt<180;attempt++){std::set<DWORD> candidates;auto add=[&](DWORD a,size_t size){if(size>16*1024*1024)return;std::vector<DWORD> b(size/4);if(read(a,b.data(),b.size()*4))for(DWORD p:b)if(p>0x10000&&p<0x7fff0000&&p%4==0)candidates.insert(p);};for(int i=0;i<nt.FileHeader.NumberOfSections;i++){IMAGE_SECTION_HEADER s{};if(read(section+i*sizeof(s),&s,sizeof(s))&&!memcmp(s.Name,".data",5))add(base+s.VirtualAddress,s.Misc.VirtualSize);}auto roots=candidates;for(DWORD p:roots)add(p,16384);
 for(DWORD p:candidates){DWORD model=pointer(p+0x988*4),dims[3]{};if(!read(model+0x44,dims,12)||dims[0]!=16||dims[1]!=16||dims[2]!=25)continue;modelArray=p;dev=(IDirect3DDevice9*)(uintptr_t)pointer(model+0x58);vt=(void**)(uintptr_t)pointer((DWORD)(uintptr_t)dev);if(!vt)continue;/* With TAA, shadows must execute before its HUD depth switch. */if(GetModuleHandleW(L"AlphaTAA.dll")){MEMORY_BASIC_INFORMATION info{};VirtualQuery(vt[43],&info,sizeof(info));wchar_t hooked[32768]{};GetModuleFileNameW((HMODULE)info.AllocationBase,hooked,32768);if(!wcsstr(hooked,L"AlphaTAA")){dev=nullptr;vt=nullptr;continue;}}oldClear=(Clear)exchange(43,(void*)clear);oldReset=(Reset)exchange(16,(void*)reset);oldDip=(Dip)exchange(82,(void*)dip);oldPresent=(Present)exchange(17,(void*)present);log("AlphaDirectional revision 2 attached (native-cycle moon, blue night light, shared sun/moon shadow map).");return 0;}Sleep(1000);}log("Alpha D3D9 device not found.");return 0;}
BOOL WINAPI DllMain(HINSTANCE m,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){self=m;DisableThreadLibraryCalls(m);wchar_t p[32768];GetModuleFileNameW(nullptr,p,32768);auto name=wcsrchr(p,L'\\');if(name&&!_wcsicmp(name+1,L"Cube.exe")){HANDLE t=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(t)CloseHandle(t);}}return TRUE;}




