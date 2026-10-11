#include "AlphaMoonRules.h"
#include <cassert>
#include <cstdio>
int main(){float day[3]={0,0,1},night[3]={0,0,-1},horizon[3]={0,1,0};auto d=alphaMoon::phase(day,.6f),n=alphaMoon::phase(night,.6f),h=alphaMoon::phase(horizon,.6f);assert(d.weight==0&&d.strength==0&&h.weight==0);assert(n.weight==1&&n.strength==.6f&&n.direction[2]==1);
 float native[24]{},changed[24]{};native[0]=0;native[1]=0;native[2]=-1;native[3]=123;native[12]=4;native[16]=.9f;native[23]=1;alphaMoon::lighting(changed,native,n,.04f);assert(changed[6]>changed[5]&&changed[5]>changed[4]);assert(changed[22]>changed[21]&&changed[21]>changed[20]);assert(changed[3]==123&&changed[12]==4&&changed[16]==.9f&&changed[23]==1);
 float world[12]={2,0,0,3,0,4,0,5,0,0,6,7};float wvp[16]={2,0,0,3,0,4,0,5,0,0,6,7,0,0,0,1},vp[16]{};assert(alphaMoon::cameraProjection(vp,wvp,world));for(int i=0;i<16;i++)assert(std::abs(vp[i]-(i%5==0?1.f:0.f))<1e-5f);world[0]=0;assert(!alphaMoon::cameraProjection(vp,wvp,world));
 float projection[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0};float disk[4]{};assert(alphaMoon::disk(disk,projection,n,1920,1080));assert(disk[0]==960&&disk[1]==540&&disk[2]>19&&disk[2]<20);auto behind=n;behind.direction[2]=-1;assert(!alphaMoon::disk(disk,projection,behind,1920,1080));puts("PASS: native phase, twilight, moon shadows, blue lighting, protected constants, camera reconstruction and disk projection.");}
