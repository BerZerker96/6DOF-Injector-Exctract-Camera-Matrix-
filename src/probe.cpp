// 6DOFProbe.dll - injected into a game; finds the camera view/projection matrices and writes a
// MOD BUILD SPEC to 6DOF-<Game>.log next to this dll. Multi-API:
//   D3D9  - hooks SetTransform + SetVertexShaderConstantF
//   D3D10 - hooks Buffer::Map/Unmap + UpdateSubresource
//   D3D11 - hooks Map / Unmap / UpdateSubresource
//   D3D12 - hooks ID3D12Resource::Map (scans the upload heaps)
//   Vulkan- detected; camera is driven CPU-side (the GPU hook is only a D3D convenience, not required)
// A timer thread drives the report + END key + frequency window, so capture works without depending
// on any one API's Present.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d10.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>
#include <tlhelp32.h>
#include <psapi.h>

// ----------------------------------------------------------------- logging
static wchar_t g_logPath[MAX_PATH]={0}; static wchar_t g_profPath[MAX_PATH]={0}; static HMODULE g_self=nullptr; static std::mutex g_logMx;
// THREE logs: (1) g_logPath = the main findings log; (2) g_pfPath = a continuous PER-FRAME camera trace;
// (3) g_aggPath = the AGGRESSIVE log (gated by the loader's "Aggressive deep probe" checkbox) - the stronger
// AOB + FOV hunter and the camera/FOV hijack detail. Hijack verdicts are mirrored into all three.
static wchar_t g_pfPath[MAX_PATH]={0}; static wchar_t g_aggPath[MAX_PATH]={0}; static std::mutex g_pfMx, g_aggMx;
static bool g_aggressive=false;   // checkbox: stronger/aggressive probe + AOB + FOV hunter + writes the 3rd log
static bool g_activeMoveTest=false;   // OPT-IN: write a yaw to the located camera to confirm by moving it. DEFAULT OFF - writing into a live game can crash it (it did on Alan Wake). The AOB capture + chime is the safe default.
// the hijacks and the retry loop are now AUTOMATIC (on by default) - they confirm the real camera/FOV on the
// live game. The GUI can turn any of them off (writes <key>=0); a CLI/no-cfg injection keeps them on.
static bool g_rotHijack=true;     // active pitch/yaw/roll probe: which candidate really ROTATES the camera (default ON)
static bool g_camHijack=true;     // active per-axis camera-placement probe (default ON)
static bool g_fovHijack=true;     // active FOV up/down probe (default ON)
static bool g_hijackRetry=true;   // loop + re-scan the hijack(s) until they land (default ON)
static volatile bool g_rotHijackLanded=false, g_camHijackLanded=false, g_fovHijackLanded=false;   // set once a candidate really rotates/moves the camera / FOV
static void resolveLogPath(){
    wchar_t dll[MAX_PATH]={0}; GetModuleFileNameW(g_self,dll,MAX_PATH);
    wchar_t dir[MAX_PATH]={0}; wcscpy_s(dir,MAX_PATH,dll);
    wchar_t* s=wcsrchr(dir,L'\\'); if(s)*(s+1)=0; else dir[0]=0;                         // dir = probe DLL folder
    if(!dir[0]){ GetTempPathW(MAX_PATH,dir); }
    // all output goes into a "6DOF Output" subfolder (created if missing)
    wchar_t outdir[MAX_PATH]; wcscpy_s(outdir,MAX_PATH,dir); wcscat_s(outdir,MAX_PATH,L"6DOF Output\\");
    CreateDirectoryW(outdir,nullptr);                                                   // ok if it already exists
    DWORD att=GetFileAttributesW(outdir);
    if(att==INVALID_FILE_ATTRIBUTES||!(att&FILE_ATTRIBUTE_DIRECTORY)){                  // e.g. Program Files w/o rights -> temp
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH,tmp);
        wcscpy_s(outdir,MAX_PATH,tmp); wcscat_s(outdir,MAX_PATH,L"6DOF Output\\"); CreateDirectoryW(outdir,nullptr); }
    wchar_t gexe[MAX_PATH]={0}; GetModuleFileNameW(nullptr,gexe,MAX_PATH);
    wchar_t* gb=gexe; for(wchar_t*p=gexe;*p;++p) if(*p==L'\\'||*p==L'/') gb=p+1;        // gb = "Game.exe"
    wcscpy_s(g_profPath,MAX_PATH,outdir);                                               // "<outdir>Game.exe.6dof.json"
    wcscat_s(g_profPath,MAX_PATH,gb); wcscat_s(g_profPath,MAX_PATH,L".6dof.json");      // (runtime profile, named after the game)
    wchar_t* dot=wcsrchr(gb,L'.'); if(dot)*dot=0;                                       // gb = "Game" (for the log name)
    wcscpy_s(g_logPath,MAX_PATH,outdir);                                                // "<outdir>6DOF-Game.log"
    wcscat_s(g_logPath,MAX_PATH,L"6DOF-"); wcscat_s(g_logPath,MAX_PATH,gb); wcscat_s(g_logPath,MAX_PATH,L".log");
    // the two extra logs share the same base name
    wcscpy_s(g_pfPath,MAX_PATH,outdir);  wcscat_s(g_pfPath,MAX_PATH,L"6DOF-");  wcscat_s(g_pfPath,MAX_PATH,gb);  wcscat_s(g_pfPath,MAX_PATH,L".perframe.log");
    wcscpy_s(g_aggPath,MAX_PATH,outdir); wcscat_s(g_aggPath,MAX_PATH,L"6DOF-"); wcscat_s(g_aggPath,MAX_PATH,gb); wcscat_s(g_aggPath,MAX_PATH,L".aggressive.log");
}
// overwrite a UTF-8 text file (used for the standalone JSON profile)
static void writeTextFile(const wchar_t* path,const char* text){
    HANDLE h=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h!=INVALID_HANDLE_VALUE){ DWORD w; WriteFile(h,text,(DWORD)strlen(text),&w,nullptr); CloseHandle(h); }
}
static void Log(const char* fmt,...){
    char buf[2048]; va_list ap; va_start(ap,fmt); int p=vsnprintf(buf,sizeof(buf)-2,fmt,ap); va_end(ap);
    if(p<0)p=0; if(p>(int)sizeof(buf)-2)p=sizeof(buf)-2; buf[p++]='\r'; buf[p++]='\n';
    std::lock_guard<std::mutex> lk(g_logMx);
    HANDLE h=CreateFileW(g_logPath,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h!=INVALID_HANDLE_VALUE){ DWORD w; SetFilePointer(h,0,nullptr,FILE_END); WriteFile(h,buf,(DWORD)p,&w,nullptr); CloseHandle(h); }
}
// shared appender to an arbitrary log path under its own mutex
static void appendLog(const wchar_t* path,std::mutex& mx,const char* buf,int len){
    std::lock_guard<std::mutex> lk(mx);
    HANDLE h=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h!=INVALID_HANDLE_VALUE){ DWORD w; SetFilePointer(h,0,nullptr,FILE_END); WriteFile(h,buf,(DWORD)len,&w,nullptr); CloseHandle(h); }
}
// PER-FRAME log (2nd log) - a continuous, lighter trace of the live camera + active-test progress
static void LogPF(const char* fmt,...){
    char buf[1024]; va_list ap; va_start(ap,fmt); int p=vsnprintf(buf,sizeof(buf)-2,fmt,ap); va_end(ap);
    if(p<0)p=0; if(p>(int)sizeof(buf)-2)p=sizeof(buf)-2; buf[p++]='\r'; buf[p++]='\n'; appendLog(g_pfPath,g_pfMx,buf,p);
}
// AGGRESSIVE log (3rd log) - only written when the loader's aggressive checkbox is on
static void LogAgg(const char* fmt,...){
    if(!g_aggressive) return;
    char buf[1024]; va_list ap; va_start(ap,fmt); int p=vsnprintf(buf,sizeof(buf)-2,fmt,ap); va_end(ap);
    if(p<0)p=0; if(p>(int)sizeof(buf)-2)p=sizeof(buf)-2; buf[p++]='\r'; buf[p++]='\n'; appendLog(g_aggPath,g_aggMx,buf,p);
}
static void Log(const char*,...);
// mirror a line into ALL THREE logs (used for hijack verdicts the user wants visible everywhere)
static void Log3(const char* fmt,...){
    char buf[1024]; va_list ap; va_start(ap,fmt); int p=vsnprintf(buf,sizeof(buf)-2,fmt,ap); va_end(ap);
    if(p<0)p=0; if(p>(int)sizeof(buf)-2)p=sizeof(buf)-2;
    Log("%s",buf);
    int q=p; buf[q++]='\r'; buf[q++]='\n'; appendLog(g_pfPath,g_pfMx,buf,q); if(g_aggressive) appendLog(g_aggPath,g_aggMx,buf,q);
}
static bool Readable(const void* p,size_t n){
    MEMORY_BASIC_INFORMATION mbi; if(!p) return false;
    if(!VirtualQuery(p,&mbi,sizeof(mbi))) return false;
    if(mbi.State!=MEM_COMMIT) return false;
    DWORD ok=PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE;
    if(!(mbi.Protect&ok)||(mbi.Protect&PAGE_GUARD)) return false;
    return ((uintptr_t)p+n)<=((uintptr_t)mbi.BaseAddress+mbi.RegionSize);
}

// ----------------------------------------------------------------- classification (pure math)
static inline bool finite16(const float* m){ for(int i=0;i<16;i++){ float v=m[i]; if(v!=v||v>1e18f||v<-1e18f) return false; } return true; }
static bool ortho3x3(const float* m,float e=0.02f){
    float rx=m[0]*m[0]+m[1]*m[1]+m[2]*m[2],ry=m[4]*m[4]+m[5]*m[5]+m[6]*m[6],rz=m[8]*m[8]+m[9]*m[9]+m[10]*m[10];
    if(fabsf(rx-1)>e||fabsf(ry-1)>e||fabsf(rz-1)>e) return false;
    float d01=m[0]*m[4]+m[1]*m[5]+m[2]*m[6],d02=m[0]*m[8]+m[1]*m[9]+m[2]*m[10],d12=m[4]*m[8]+m[5]*m[9]+m[6]*m[10];
    return fabsf(d01)<e&&fabsf(d02)<e&&fabsf(d12)<e;
}
static bool identityish(const float* m){ float s=0; const float I[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; for(int i=0;i<16;i++)s+=fabsf(m[i]-I[i]); return s<0.01f; }
static bool axisAligned(const float* m){ int n1=0,n0=0; for(int i=0;i<3;i++)for(int j=0;j<3;j++){float v=fabsf(m[i*4+j]); if(v>0.98f&&v<1.02f)n1++; else if(v<0.02f)n0++;} return n1==3&&n0==6; }
static int g_projHand=0;      // +1 = left-handed, -1 = right-handed (0 = unknown)
static bool g_projInfFar=false, g_projRevZ=false;
static int classifyProj(const float* m,bool& rowMaj,float& fovY,float& fovX,float& aspect,float& zn,float& zf){
    rowMaj=true; fovY=fovX=aspect=zn=zf=0;
    bool rowP=fabsf(fabsf(m[11])-1.f)<0.05f&&fabsf(m[15])<0.05f&&m[0]>0.1f&&m[5]>0.1f&&fabsf(m[3])<0.05f&&fabsf(m[7])<0.05f;
    bool colP=fabsf(fabsf(m[14])-1.f)<0.05f&&fabsf(m[15])<0.05f&&m[0]>0.1f&&m[5]>0.1f&&fabsf(m[12])<0.05f&&fabsf(m[13])<0.05f;
    if(!rowP&&!colP) return 0; rowMaj=rowP;
    float a=m[0],b=m[5]; fovX=2.f*atanf(1.f/a)*57.2957795f; fovY=2.f*atanf(1.f/b)*57.2957795f; aspect=b/a;
    if(fovX<20.f||fovX>170.f||fovY<20.f||fovY>170.f||aspect<0.4f||aspect>3.5f) return 0;  // implausible -> not a projection
    float c=m[10],d=rowMaj?m[14]:m[11]; if(fabsf(c)>1e-4f) zn=fabsf(d/c);
    zf=(fabsf(c-1.f)<1e-3f)?0.f:zn/(1.f-fabsf(c)+1e-6f);
    // handedness: the element copying view-space z into clip-space w is m[11] (row-major) / m[14] (col-major).
    // Negative => right-handed view (-Z forward), positive => left-handed. (xdpixel / standard projection algebra.)
    float zw = rowMaj?m[11]:m[14]; g_projHand = (zw<0)? -1 : +1;
    g_projRevZ  = (m[10]<0.f);                 // reversed-Z: depth maps near->1, far->0 (m22 sign flips)
    g_projInfFar= (zf>1e5f)||(zf<=0.f && zn>0.f); // far driven to infinity (no finite far term)
    return 1;
}
static void eulerFromBasis(const float* m,float& pitch,float& yaw,float& roll){
    float r20=m[8],r21=m[9],r22=m[10],r01=m[1],r11=m[5];
    yaw=atan2f(r20,r22)*57.2957795f; pitch=atan2f(-r21,sqrtf(r20*r20+r22*r22))*57.2957795f; roll=atan2f(r01,r11)*57.2957795f;
}
static void cameraPos(const float* m,float out[3]){
    bool colT=(fabsf(m[3])+fabsf(m[7]))>(fabsf(m[12])+fabsf(m[13])); float tx,ty,tz;
    if(colT){tx=m[3];ty=m[7];tz=m[11];}else{tx=m[12];ty=m[13];tz=m[14];}
    out[0]=-(m[0]*tx+m[4]*ty+m[8]*tz); out[1]=-(m[1]*tx+m[5]*ty+m[9]*tz); out[2]=-(m[2]*tx+m[6]*ty+m[10]*tz);
}
static uint64_t quantKey(const float* m){ uint64_t h=1469598103934665603ull; for(int i=0;i<12;i++){int q=(int)(m[i]*64.f); h=(h^(uint32_t)q)*1099511628211ull;} return h; }

// ----------------------------------------------------------------- catalogue
struct Entry{ float m[16]; uint32_t freq=0,off=0,size=0,hits=0,draws=0; int slot=-1; uint64_t lastFrame=0,firstFrame=0; int kind=0; bool rowMaj=true;
              float fovY=0,fovX=0,aspect=0,zn=0,zf=0; float campos[3]={0,0,0}; };
static std::mutex g_catMx; static std::unordered_map<uint64_t,Entry> g_cat;
static std::mutex g_freqMx; static std::unordered_map<uint64_t,uint32_t> g_freq;
static volatile uint64_t g_frame=0; static volatile bool g_spinUsed=false;
static char g_engine[64]="native/unknown", g_game[80]="?", g_api[12]="?", g_fileVer[32]="?";
static volatile uint32_t g_resW=0,g_resH=0;
// Fix (v5.11): CPU-side oracle fallbacks for APIs where the GPU constant-buffer hook captures nothing (D3D12,
// Vulkan, GL). g_cpuProjAddr = a projection matrix found in CPU memory whose aspect matches the screen (the FOV
// ground truth). g_poolBase/End = the heap region holding the densest cluster of view matrices (the transient
// camera POOL) so the page-guard can cover the whole pool, not just the one address the differential locked.
static volatile uintptr_t g_cpuProjAddr=0;
static volatile uintptr_t g_poolBase=0, g_poolEnd=0;

static void classifyInto(Entry& e){
    bool rm; float fy,fx,asp,zn,zf;
    if(classifyProj(e.m,rm,fy,fx,asp,zn,zf)){ e.kind=1;e.rowMaj=rm;e.fovY=fy;e.fovX=fx;e.aspect=asp;e.zn=zn;e.zf=zf; return; }
    if(ortho3x3(e.m)&&!identityish(e.m)&&!axisAligned(e.m)){ e.kind=2; e.rowMaj=(fabsf(e.m[3])+fabsf(e.m[7]))<(fabsf(e.m[12])+fabsf(e.m[13])); cameraPos(e.m,e.campos); return; }
    int z=0; for(int i=0;i<16;i++) if(fabsf(e.m[i])<1e-6f)z++;
    bool wRow=fabsf(e.m[3])>1e-3f||fabsf(e.m[7])>1e-3f||fabsf(e.m[11])>1e-3f||fabsf(e.m[15]-1.f)>1e-3f;
    if(z<8&&wRow&&!identityish(e.m)){ e.kind=3;e.rowMaj=true; return; } e.kind=0;
}
static void scanBuffer(const uint8_t* data,size_t size,uint32_t draws=0,int slot=-1){
    if(!data||size<64||!Readable(data,size)) return;
    // Corpus prior: the view matrix sits in a LOW offset of the constant buffer (slot 0-4, offset 0 or a
    // 64-byte boundary; BF4's CB is 1MB but its view is at +240, Borderlands at +1232). So accept large
    // buffers (raised cap) but only scan their first 64KB - the camera is never deep inside a huge CB, and
    // scanning the whole megabyte every map would stall the game.
    size_t lim=size-64; if(lim>0x10000) lim=0x10000;
    for(size_t off=0;off<=lim;off+=16){ const float* m=(const float*)(data+off);
        if(!finite16(m)) continue; bool z=true; for(int i=0;i<16;i++) if(m[i]!=0){z=false;break;} if(z) continue;
        // reject broadcast/constant blocks (rows or cols of repeated values - not a real matrix)
        bool bcast=true; for(int r=0;r<4&&bcast;r++){ if(fabsf(m[r*4]-m[r*4+1])>1e-4f||fabsf(m[r*4]-m[r*4+2])>1e-4f) bcast=false; } if(bcast) continue;
        Entry probe; memcpy(probe.m,m,64); classifyInto(probe); if(probe.kind==0) continue;
        uint64_t k=quantKey(m);
        { std::lock_guard<std::mutex> lk(g_freqMx); g_freq[k]++; }
        std::lock_guard<std::mutex> lk(g_catMx); Entry& e=g_cat[k];
        if(e.size==0){ e=probe; e.firstFrame=g_frame; } memcpy(e.m,m,64); e.off=(uint32_t)off; e.size=(uint32_t)size; e.lastFrame=g_frame; e.hits++;
        if(draws>e.draws)e.draws=draws; if(slot>=0)e.slot=slot;
        { std::lock_guard<std::mutex> lk2(g_freqMx); e.freq=g_freq[k]; }
    }
}

// ----------------------------------------------------------------- report (MOD BUILD SPEC)
static void mline(const float* m,char* o,size_t n){ snprintf(o,n,
    "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
    m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11],m[12],m[13],m[14],m[15]); }
static void report(){
    std::vector<Entry> proj,view,vp;
    { std::lock_guard<std::mutex> lk(g_catMx);
      for(auto it=g_cat.begin();it!=g_cat.end();){ if(g_frame-it->second.lastFrame>240){it=g_cat.erase(it);continue;}
        const Entry& e=it->second; if(e.kind==1)proj.push_back(e); else if(e.kind==2)view.push_back(e); else if(e.kind==3)vp.push_back(e); ++it; } }
    // primary sort: draw-call weight (the MAIN camera CB feeds the most draws); tiebreak: duplication freq.
    auto bf=[](const Entry&a,const Entry&b){ if(a.draws!=b.draws) return a.draws>b.draws; return a.freq>b.freq; };
    std::sort(proj.begin(),proj.end(),bf); std::sort(view.begin(),view.end(),bf); std::sort(vp.begin(),vp.end(),bf);
    // drop the per-object/skinning flood: keep only well-duplicated candidates - but ONLY when there
    // actually is a flood (high top freq). GL / simple engines emit few matrices/frame; don't over-filter them.
    uint32_t vthr=(view.empty()||view[0].freq<16)?0:std::max(8u,view[0].freq/8), pthr=(vp.empty()||vp[0].freq<16)?0:std::max(8u,vp[0].freq/8);
    auto filt=[](std::vector<Entry>& v,uint32_t t){ v.erase(std::remove_if(v.begin(),v.end(),[&](const Entry&e){return e.freq<t;}),v.end()); };
    filt(view,vthr); filt(vp,pthr);
    // projection: prefer the one whose aspect matches the screen (rejects shadow/cubemap projections).
    float target = (g_resW&&g_resH)? (float)g_resW/(float)g_resH : 0.f;
    const Entry* bP=nullptr; if(!proj.empty()){ if(target>0){ float best=1e9f; for(auto&e:proj){ float d=fabsf(e.aspect-target); if(d<0.15f&&d<best){best=d;bP=&e;} } } if(!bP) bP=&proj[0]; }
    // main view: top draw-weighted view with a real (non-origin) camera position. Corpus tiebreak: when the top
    // candidate sits at an unusual CB location but a near-equal one is at the canonical offset 0/64/128 + slot b0-b4,
    // prefer the canonical one (the main-scene view buffer almost always lives there).
    const Entry* bV=nullptr; for(auto&e:view){ float p=fabsf(e.campos[0])+fabsf(e.campos[1])+fabsf(e.campos[2]); if(p>1.f){bV=&e;break;} } if(!bV&&!view.empty())bV=&view[0];
    if(bV){ bool bvCanon=(bV->off<=128)&&(bV->slot>=0&&bV->slot<=4);
        if(!bvCanon){ for(auto&e:view){ if(&e==bV) continue; if(e.draws*5<bV->draws*4) break;   // only among comparably-drawn views
            float p=fabsf(e.campos[0])+fabsf(e.campos[1])+fabsf(e.campos[2]); if(p<=1.f) continue;
            if((e.off<=128)&&(e.slot>=0&&e.slot<=4)){ bV=&e; break; } } } }
    const Entry* bVP=vp.empty()?nullptr:&vp[0];
    auto det3=[](const float* m){ return m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]); };
    const char* mode=bV?"view":(bVP?"viewproj":"none");
    const char* formula=bV?"Vnew = R_head * V(3x3); translation -= lean":(bVP?"VPnew = Pinv * R_head * P * VP":"n/a");
    char buf[300];
    Log(""); Log("================== CAMERA REPORT (PROJ=%zu VIEW=%zu VP=%zu, draws-weighted) ==================",proj.size(),view.size(),vp.size());
    Log("################### MOD BUILD SPEC  (copy this whole block & send it) ###################");
    Log("GAME=%s",g_game);
    Log("ARCH=%d-bit  API=%s  ENGINE=%s  FILEVER=%s",(int)(sizeof(void*)*8),g_api,g_engine,g_fileVer);
    Log("RESOLUTION=%ux%u  SCREEN_ASPECT=%.4f",g_resW,g_resH,target);
    Log("INJECT_MODE=%s  FORMULA=%s",mode,formula);
    Log("CAMERA_DRAWS=%u  CAMERA_FREQ=%u  (draws = #draw-calls/frame using this camera buffer)",
        bV?bV->draws:(bP?bP->draws:0), bV?bV->freq:(bP?bP->freq:0));
    if(bP){ bool rz=bP->m[10]<0; Log("PROJ off=0x%X size=%u slot=%d layout=%s fovV=%.3f fovH=%.3f aspect=%.4f near=%.5f far=%.3f reversedZ=%d draws=%u freq=%u",
                bP->off,bP->size,bP->slot,bP->rowMaj?"row":"col",bP->fovY,bP->fovX,bP->aspect,bP->zn,bP->zf,rz?1:0,bP->draws,bP->freq);
            mline(bP->m,buf,sizeof(buf)); Log("PROJ_M=%s",buf);
            // handedness + depth convention straight from the chosen projection
            float zw = bP->rowMaj?bP->m[11]:bP->m[14]; int hand=(zw<0)?-1:1; bool revz=bP->m[10]<0;
            bool inffar=(bP->zf>1e5f)||(bP->zf<=0.f&&bP->zn>0.f);
            Log("PROJ_CONVENTION handedness=%s (z->w=%.3f)  reversedZ=%d  infinite_far=%d",
                hand<0?"right (-Z fwd)":"left (+Z fwd)", zw, revz?1:0, inffar?1:0);
            Log("# Invert guidance: view matrices are right-handed even in LH engines, so the sign of \"look\"");
            Log("# can't be inferred statically. If a head axis feels reversed in-game, set invert_yaw / invert_pitch /");
            Log("# invert_roll (or invert_x/y/z for lean) = true in the .6dof.json, or toggle live with F10/F11.");
            // --- FOV: m0=cot(fovH/2), m5=cot(fovV/2). Override by scaling both. ---
            Log("[FOV] current_vertical=%.2f  current_horizontal=%.2f   m0(=cot(H/2))=%.5f  m5(=cot(V/2))=%.5f",bP->fovY,bP->fovX,bP->m[0],bP->m[5]);
            Log("[FOV] OVERRIDE to target vertical T deg: factor=tan(%.2f/2)/tan(T/2); set m0*=factor, m5*=factor in the PROJ buffer each frame.",bP->fovY);
            float f90=tanf(bP->fovY*0.5f*0.0174533f)/tanf(45.f*0.0174533f);
            Log("[FOV] example T=90: factor=%.4f -> m0=%.5f m5=%.5f   (bigger T = wider view = smaller m0/m5)",f90,bP->m[0]*f90,bP->m[5]*f90);
    } else Log("PROJ=none   # no screen-aspect projection isolated; FOV may live inside the VIEWPROJ (use Pinv route)");
    if(bV){ float pi,ya,ro; eulerFromBasis(bV->m,pi,ya,ro); float dt=det3(bV->m);
            bool colT=(fabsf(bV->m[3])+fabsf(bV->m[7]))>(fabsf(bV->m[12])+fabsf(bV->m[13]));
            Log("VIEW off=0x%X size=%u slot=%d layout=%s hand=%s translation=%s recovered=0 draws=%u freq=%u",
                bV->off,bV->size,bV->slot,bV->rowMaj?"row":"col",dt<0?"LH/mirror":"RH",colT?"col3":"row3",bV->draws,bV->freq);
            Log("VIEW_CAMPOS=%.4f,%.4f,%.4f  VIEW_EULER_PYR=%.3f,%.3f,%.3f",bV->campos[0],bV->campos[1],bV->campos[2],pi,ya,ro);
            // ~16% of corpus games upload the INVERSE (view->world / camera) matrix, where the translation IS the camera
            // world position - NOT -R^T*t. cameraPos() assumes world->view, so report the inverse reading too; the
            // correct one matches the player's actual location in the world.
            float tx,ty,tz; if(colT){tx=bV->m[3];ty=bV->m[7];tz=bV->m[11];}else{tx=bV->m[12];ty=bV->m[13];tz=bV->m[14];}
            Log("VIEW_CAMPOS_if_inverse=%.4f,%.4f,%.4f  (use THIS reading if the buffer is a view->world/camera matrix; ~16%% of titles are)",tx,ty,tz);
            // corpus CB-location prior: the main view sits at offset 0/64/128 in slot 0-4. Flag confidence accordingly.
            bool canon=(bV->off<=128)&&(bV->slot>=0&&bV->slot<=4);
            Log("VIEW_LOCATION_CONFIDENCE=%s  (corpus: view matrix lives at CB offset 0/64/128, slot b0-b4; size signature = %u/0x%X)",
                canon?"high (canonical offset+slot)":"unusual (verify by draws + spin-test)",bV->size,bV->off);
            mline(bV->m,buf,sizeof(buf)); Log("VIEW_M=%s",buf);} else Log("VIEW=none");
    if(bVP){ Log("VIEWPROJ off=0x%X size=%u slot=%d layout=%s draws=%u freq=%u",bVP->off,bVP->size,bVP->slot,bVP->rowMaj?"row":"col",bVP->draws,bVP->freq);
             mline(bVP->m,buf,sizeof(buf)); Log("VIEWPROJ_M=%s",buf);} else Log("VIEWPROJ=none");
    // grouped candidates so the recurring main-scene buffer is obvious (ranked by draws then freq).
    auto dumpGroups=[&](const char* tag,std::vector<Entry>& v){
        struct G{ uint32_t off,size; bool row; int slot; uint32_t count,maxFreq,maxDraws,hits; };
        std::vector<G> g;
        for(auto& e:v){ bool m=false; for(auto& q:g) if(q.off==e.off&&q.size==e.size&&q.row==e.rowMaj){ q.count++; if(e.freq>q.maxFreq)q.maxFreq=e.freq; if(e.draws>q.maxDraws)q.maxDraws=e.draws; q.hits+=e.hits; m=true; break; }
            if(!m) g.push_back({e.off,e.size,e.rowMaj,e.slot,1,e.freq,e.draws,e.hits}); }
        std::sort(g.begin(),g.end(),[](const G&a,const G&b){ if(a.maxDraws!=b.maxDraws)return a.maxDraws>b.maxDraws; return (uint64_t)a.count*a.maxFreq>(uint64_t)b.count*b.maxFreq; });
        Log("# TOP %s buffers (draws=draw-calls/frame = main-scene weight):",tag);
        for(size_t i=0;i<g.size()&&i<5;i++) Log("#   %s off=0x%X size=%u slot=%d layout=%s  draws=%u maxfreq=%u distinct=%u hits=%u",
            tag,g[i].off,g[i].size,g[i].slot,g[i].row?"row":"col",g[i].maxDraws,g[i].maxFreq,g[i].count,g[i].hits);
    };
    dumpGroups("VIEW",view); dumpGroups("VIEWPROJ",vp);
    Log("NOTE: the camera buffer is the one with the highest 'draws'. If its offset changes each frame the");
    Log("      buffer is transient (the mod re-finds it by size/layout/structure). Probe is read-only (no spin).");
    Log("################### END MOD BUILD SPEC ###################");
}

// ----------------------------------------------------------------- D3D11
// D3D11 draw-call weighting: the MAIN-scene camera CB is the one used by the most draws (the standard
// ReShade/3DMigoto heuristic). Track VS-bound CBs, count draws per buffer, roll per frame in Present.
static ID3D11Buffer* g_vsCB[8]={0};
static std::mutex g_drawMx; static std::unordered_map<void*,uint32_t> g_drawCur,g_drawPrev; static std::unordered_map<void*,int> g_slotOf;
typedef void(STDMETHODCALLTYPE* VSSetCB_t)(ID3D11DeviceContext*,UINT,UINT,ID3D11Buffer* const*);
typedef void(STDMETHODCALLTYPE* Draw_t)(ID3D11DeviceContext*,UINT,UINT);
typedef void(STDMETHODCALLTYPE* DrawIdx_t)(ID3D11DeviceContext*,UINT,UINT,INT);
typedef void(STDMETHODCALLTYPE* DrawInst_t)(ID3D11DeviceContext*,UINT,UINT,UINT,UINT);
typedef void(STDMETHODCALLTYPE* DrawIdxInst_t)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*,UINT,UINT);
static VSSetCB_t oVSSetCB=nullptr; static Draw_t oDraw=nullptr; static DrawIdx_t oDrawIdx=nullptr; static DrawInst_t oDrawInst=nullptr; static DrawIdxInst_t oDrawIdxInst=nullptr; static Present_t oPresent=nullptr;
static void STDMETHODCALLTYPE hkVSSetCB(ID3D11DeviceContext* c,UINT start,UINT num,ID3D11Buffer* const* pp){
    if(pp) for(UINT i=0;i<num&&start+i<8;i++){ g_vsCB[start+i]=pp[i]; if(pp[i]){ std::lock_guard<std::mutex> lk(g_drawMx); g_slotOf[pp[i]]=(int)(start+i);} }
    oVSSetCB(c,start,num,pp);
}
static inline void countDraw(){ std::lock_guard<std::mutex> lk(g_drawMx); for(int i=0;i<4;i++) if(g_vsCB[i]) g_drawCur[g_vsCB[i]]++; }
static void STDMETHODCALLTYPE hkDraw(ID3D11DeviceContext* c,UINT a,UINT b){ countDraw(); oDraw(c,a,b); }
static void STDMETHODCALLTYPE hkDrawIdx(ID3D11DeviceContext* c,UINT a,UINT b,INT d){ countDraw(); oDrawIdx(c,a,b,d); }
static void STDMETHODCALLTYPE hkDrawInst(ID3D11DeviceContext* c,UINT a,UINT b,UINT d,UINT e){ countDraw(); oDrawInst(c,a,b,d,e); }
static void STDMETHODCALLTYPE hkDrawIdxInst(ID3D11DeviceContext* c,UINT a,UINT b,UINT d,INT e,UINT f){ countDraw(); oDrawIdxInst(c,a,b,d,e,f); }
static uint32_t drawsOf(void* buf){ std::lock_guard<std::mutex> lk(g_drawMx); auto it=g_drawPrev.find(buf); return it==g_drawPrev.end()?0:it->second; }
static int slotOf(void* buf){ std::lock_guard<std::mutex> lk(g_drawMx); auto it=g_slotOf.find(buf); return it==g_slotOf.end()?-1:it->second; }
static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc,UINT si,UINT fl){
    { std::lock_guard<std::mutex> lk(g_drawMx); g_drawPrev.swap(g_drawCur); g_drawCur.clear(); }
    if(!g_resW){ DXGI_SWAP_CHAIN_DESC d; if(SUCCEEDED(sc->GetDesc(&d))){ g_resW=d.BufferDesc.Width; g_resH=d.BufferDesc.Height; } }
    return oPresent(sc,si,fl);
}


typedef HRESULT(STDMETHODCALLTYPE* Map11_t)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
typedef void(STDMETHODCALLTYPE* Unmap11_t)(ID3D11DeviceContext*,ID3D11Resource*,UINT);
typedef void(STDMETHODCALLTYPE* Upd11_t)(ID3D11DeviceContext*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT);
static Map11_t oMap11=nullptr; static Unmap11_t oUnmap11=nullptr; static Upd11_t oUpd11=nullptr;
static std::mutex g_m11; static std::unordered_map<ID3D11Resource*,std::pair<void*,size_t>> g_maps11;
static bool isCB11(ID3D11Resource* r,size_t& sz){ D3D11_RESOURCE_DIMENSION d; r->GetType(&d); if(d!=D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    ID3D11Buffer* b=nullptr; if(FAILED(r->QueryInterface(__uuidof(ID3D11Buffer),(void**)&b))||!b) return false; D3D11_BUFFER_DESC bd; b->GetDesc(&bd); b->Release();
    sz=bd.ByteWidth; return (bd.BindFlags&D3D11_BIND_CONSTANT_BUFFER)&&bd.ByteWidth>=64&&bd.ByteWidth<=(8u<<20); }
static HRESULT STDMETHODCALLTYPE hkMap11(ID3D11DeviceContext* c,ID3D11Resource* r,UINT s,D3D11_MAP t,UINT f,D3D11_MAPPED_SUBRESOURCE* ms){
    HRESULT hr=oMap11(c,r,s,t,f,ms); if(SUCCEEDED(hr)&&ms&&ms->pData&&t!=D3D11_MAP_READ){ size_t sz; if(isCB11(r,sz)){ std::lock_guard<std::mutex> lk(g_m11); g_maps11[r]={ms->pData,sz}; } } return hr; }
static void STDMETHODCALLTYPE hkUnmap11(ID3D11DeviceContext* c,ID3D11Resource* r,UINT s){
    std::pair<void*,size_t> rec{nullptr,0}; { std::lock_guard<std::mutex> lk(g_m11); auto it=g_maps11.find(r); if(it!=g_maps11.end()){rec=it->second; g_maps11.erase(it);} }
    if(rec.first) scanBuffer((const uint8_t*)rec.first,rec.second,drawsOf(r),slotOf(r)); oUnmap11(c,r,s); }
static void STDMETHODCALLTYPE hkUpd11(ID3D11DeviceContext* c,ID3D11Resource* r,UINT s,const D3D11_BOX* box,const void* data,UINT rp,UINT dp){
    if(data){ size_t sz; if(isCB11(r,sz)) scanBuffer((const uint8_t*)data,sz,drawsOf(r),slotOf(r)); } oUpd11(c,r,s,box,data,rp,dp); }

// ----------------------------------------------------------------- D3D12 (scan mapped upload heaps)
typedef HRESULT(STDMETHODCALLTYPE* Map12_t)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
typedef void(STDMETHODCALLTYPE* Unmap12_t)(ID3D12Resource*,UINT,const D3D12_RANGE*);
static Map12_t oMap12=nullptr; static Unmap12_t oUnmap12=nullptr;
static std::mutex g_m12; static std::unordered_map<ID3D12Resource*,std::pair<void*,size_t>> g_uploads;
static HRESULT STDMETHODCALLTYPE hkMap12(ID3D12Resource* r,UINT sub,const D3D12_RANGE* rd,void** pp){
    HRESULT hr=oMap12(r,sub,rd,pp);
    if(SUCCEEDED(hr)&&pp&&*pp&&r){ D3D12_RESOURCE_DESC d=r->GetDesc(); if(d.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&d.Width>=64&&d.Width<=(1u<<22)){
        std::lock_guard<std::mutex> lk(g_m12); if(g_uploads.size()<512) g_uploads[r]={*pp,(size_t)d.Width}; } }
    return hr;
}
static void STDMETHODCALLTYPE hkUnmap12(ID3D12Resource* r,UINT sub,const D3D12_RANGE* w){
    { std::lock_guard<std::mutex> lk(g_m12); g_uploads.erase(r); } oUnmap12(r,sub,w);
}
static void scanUploads12(){ std::vector<std::pair<void*,size_t>> snap; { std::lock_guard<std::mutex> lk(g_m12); for(auto&kv:g_uploads) snap.push_back(kv.second); }
    size_t budget=8u<<20; for(auto& u:snap){ if(u.second>budget) break; budget-=u.second; scanBuffer((const uint8_t*)u.first,u.second); } }

// ----------------------------------------------------------------- D3D9
typedef HRESULT(STDMETHODCALLTYPE* SetTrans_t)(IDirect3DDevice9*,D3DTRANSFORMSTATETYPE,const D3DMATRIX*);
typedef HRESULT(STDMETHODCALLTYPE* SetVSC_t)(IDirect3DDevice9*,UINT,const float*,UINT);
static SetTrans_t oSetTrans=nullptr; static SetVSC_t oSetVSC=nullptr;
static HRESULT STDMETHODCALLTYPE hkSetTrans(IDirect3DDevice9* d,D3DTRANSFORMSTATETYPE st,const D3DMATRIX* m){
    if(m&&(st==D3DTS_VIEW||st==D3DTS_PROJECTION)) scanBuffer((const uint8_t*)m,64); return oSetTrans(d,st,m); }
static HRESULT STDMETHODCALLTYPE hkSetVSC(IDirect3DDevice9* d,UINT start,const float* data,UINT cnt){
    if(data&&cnt>=4) scanBuffer((const uint8_t*)data,(size_t)cnt*16); return oSetVSC(d,start,data,cnt); }


// ----------------------------------------------------------------- D3D10
typedef HRESULT(STDMETHODCALLTYPE* Map10_t)(ID3D10Buffer*,D3D10_MAP,UINT,void**);
typedef void(STDMETHODCALLTYPE* Unmap10_t)(ID3D10Buffer*);
typedef void(STDMETHODCALLTYPE* Upd10_t)(ID3D10Device*,ID3D10Resource*,UINT,const D3D10_BOX*,const void*,UINT,UINT);
static Map10_t oMap10=nullptr; static Unmap10_t oUnmap10=nullptr; static Upd10_t oUpd10=nullptr;
static std::mutex g_m10; static std::unordered_map<ID3D10Buffer*,std::pair<void*,size_t>> g_maps10;
static bool isCB10(ID3D10Buffer* b,size_t& sz){ D3D10_BUFFER_DESC d; b->GetDesc(&d); sz=d.ByteWidth; return (d.BindFlags&D3D10_BIND_CONSTANT_BUFFER)&&d.ByteWidth>=64&&d.ByteWidth<=65536; }
static HRESULT STDMETHODCALLTYPE hkMap10(ID3D10Buffer* b,D3D10_MAP t,UINT f,void** pp){
    HRESULT hr=oMap10(b,t,f,pp); if(SUCCEEDED(hr)&&pp&&*pp&&t!=D3D10_MAP_READ){ size_t sz; if(isCB10(b,sz)){ std::lock_guard<std::mutex> lk(g_m10); g_maps10[b]={*pp,sz}; } } return hr; }
static void STDMETHODCALLTYPE hkUnmap10(ID3D10Buffer* b){
    std::pair<void*,size_t> rec{nullptr,0}; { std::lock_guard<std::mutex> lk(g_m10); auto it=g_maps10.find(b); if(it!=g_maps10.end()){rec=it->second; g_maps10.erase(it);} }
    if(rec.first) scanBuffer((const uint8_t*)rec.first,rec.second); oUnmap10(b); }
static void STDMETHODCALLTYPE hkUpd10(ID3D10Device* dv,ID3D10Resource* r,UINT s,const D3D10_BOX* box,const void* data,UINT rp,UINT dp){
    if(data){ ID3D10Buffer* b=nullptr; if(SUCCEEDED(r->QueryInterface(__uuidof(ID3D10Buffer),(void**)&b))&&b){ size_t sz; bool cb=isCB10(b,sz); b->Release(); if(cb) scanBuffer((const uint8_t*)data,sz); } }
    oUpd10(dv,r,s,box,data,rp,dp); }

// ----------------------------------------------------------------- memory scan (CPU-struct cameras)
// For engines that keep the camera in a CPU struct (it only reaches a GPU buffer transiently). Walks
// writable memory for camera matrices and reports STABLE module+offset addresses - what a struct mod needs.
static void moduleOf(void* addr,char* out,size_t n,uintptr_t& off){
    HMODULE m=nullptr; out[0]=0; off=0;
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)addr,&m)&&m){
        char path[MAX_PATH]; GetModuleFileNameA(m,path,MAX_PATH); const char* b=path; for(char* p=path;*p;++p) if(*p=='\\'||*p=='/') b=p+1;
        strncpy(out,b,n-1); off=(uintptr_t)addr-(uintptr_t)m;
    } else { strncpy(out,"heap",n-1); off=(uintptr_t)addr; }
}
static void memScan(){
    Log(""); Log("================== MEMORY SCAN - stable camera addresses ==================");
    Log("# Use these when the GPU buffer is transient/none. A repeating module+offset = the CPU camera struct.");
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint8_t* p=(uint8_t*)si.lpMinimumApplicationAddress; uint8_t* end=(uint8_t*)si.lpMaximumApplicationAddress;
    int viewN=0,projN=0; size_t budget=256u<<20;
    float scrAspect=(g_resW&&g_resH)?(float)g_resW/(float)g_resH:0.f;     // v5.11 oracle fallback bookkeeping
    uintptr_t bestPoolBase=0,bestPoolEnd=0; int bestPoolViews=0;
    uintptr_t bestProjA=0; float bestProjD=1e9f;
    uintptr_t bestGameA=0; float bestGameD=1e9f;   // fallback widescreen projection when no exact screen-aspect match
    while(p<end && budget>0 && (viewN<40 || projN<20 || budget>(192u<<20))){   // keep scanning for the pool even past the display cap (first ~64MB)
        MEMORY_BASIC_INFORMATION mbi; if(!VirtualQuery(p,&mbi,sizeof(mbi))) break;
        uint8_t* base=(uint8_t*)mbi.BaseAddress; size_t rsz=mbi.RegionSize;
        bool rw = mbi.State==MEM_COMMIT && (mbi.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE)) && !(mbi.Protect&PAGE_GUARD);
        int regViews=0;                                                  // count view matrices in THIS region (find the pool)
        if(rw && rsz<=(64u<<20)){
            size_t scan=rsz>budget?budget:rsz; budget-=scan;
            for(size_t o=0;o+64<=scan;o+=16){            // matrices are 16-byte aligned in practice
                const float* m=(const float*)(base+o);
                if(!finite16(m)) continue;
                bool z=true; for(int i=0;i<16;i++) if(m[i]!=0){z=false;break;} if(z) continue;
                Entry e; memcpy(e.m,m,64); classifyInto(e);
                if(e.kind==2){ regViews++;                            // count EVERY view in this region (pool detection)
                    if(viewN<40){ char mod[80]; uintptr_t off; moduleOf((void*)(base+o),mod,sizeof(mod),off);
                        float pi,ya,ro; eulerFromBasis(e.m,pi,ya,ro);
                        Log("# MEM VIEW @ %s+0x%llX  campos=%.1f,%.1f,%.1f euler=%.1f,%.1f,%.1f",mod,(unsigned long long)off,e.campos[0],e.campos[1],e.campos[2],pi,ya,ro); viewN++; }
                    continue; }                                       // keep scanning the region (don't break) so the pool is fully counted
                else if(e.kind==1){
                    if(projN<20){ char mod[80]; uintptr_t off; moduleOf((void*)(base+o),mod,sizeof(mod),off);
                        Log("# MEM PROJ @ %s+0x%llX  fovV=%.1f aspect=%.3f",mod,(unsigned long long)off,e.fovY,e.aspect); }
                    if(e.fovY>10.f&&e.fovY<170.f){ float d=(scrAspect>0)?fabsf(e.aspect-scrAspect):0.f;   // screen-aspect match = the gameplay projection
                        if(d<0.15f && d<bestProjD){ bestProjD=d; bestProjA=(uintptr_t)(base+o); }
                        else if(bestProjA==0 && e.aspect>1.4f && e.aspect<2.5f && d<bestGameD){ bestGameD=d; bestGameA=(uintptr_t)(base+o); } }   // fallback: a plausible widescreen projection (not a square shadow/cube map)
                    projN++; }
            }
        }
        if(regViews>bestPoolViews && regViews>=3){ bestPoolViews=regViews; bestPoolBase=(uintptr_t)base; bestPoolEnd=(uintptr_t)base+rsz; }   // densest view cluster = the pool
        if(rsz==0) break; p=base+rsz;
    }
    if(!bestProjA && bestGameA){ bestProjA=bestGameA; }   // no exact screen-aspect proj -> use the best widescreen one
    if(bestProjA){ g_cpuProjAddr=bestProjA; Log("# ORACLE: CPU projection @ %p matches screen aspect (%.4f) - usable as the FOV ground truth on D3D12/Vulkan.",(void*)bestProjA,scrAspect); }
    if(bestPoolBase){ g_poolBase=bestPoolBase; g_poolEnd=bestPoolEnd; Log("# POOL: densest view-matrix region = %p..%p (%d copies) - the page-guard will cover this whole region.",(void*)bestPoolBase,(void*)bestPoolEnd,bestPoolViews); }
    Log("# done: %d VIEW + %d PROJ candidates. Re-run (HOME) after moving the camera; the one whose",viewN,projN);
    Log("# values changed AND address stayed the same is the live CPU camera. Send those lines to build a struct mod.");
    Log("==========================================================================");
}

// ===================================================================================================
// AUTO PIPELINE: select -> correlate(GPU<->CPU) -> pointer-chain -> write-AOB -> spin-test verify
// Runs automatically. Turns "here are some matrices" into "drive THIS address, here's the chain & writer".
// ===================================================================================================

// ---- camera-shaped value tests (matrix handled by classifyInto; here: quaternion + FOV float) ----
static bool isUnitQuat(const float* q){ float n=q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3];
    if(n!=n||fabsf(n-1.f)>0.02f) return false;
    if(fabsf(q[0])<1e-4f&&fabsf(q[1])<1e-4f&&fabsf(q[2])<1e-4f) return false;   // skip identity (won't move)
    return true; }
static bool isFovFloat(float f){ return (f>0.3f&&f<3.25f)||(f>20.f&&f<155.f); }   // corpus FOV: factor 0-1 / radians ~0.5-3.14 / degrees ~20-150
// a plausible Euler-angle-in-degrees triplet (some engines keep roll/pitch/yaw ~0x80 into the cam struct). Not a basis row.
// Euler angle triple, RADIAN- or DEGREE-encoded (corpus: ~half of CPU-scan cameras are radian eulers).
// Loose form for the field map (examining a known struct): finite, within +/-360deg, not a unit direction/quat part.
static bool isEulerAngles(const float* e){
    for(int i=0;i<3;i++){ if(e[i]!=e[i]||fabsf(e[i])>370.f) return false; }
    float mx=fabsf(e[0]); for(int i=1;i<3;i++) if(fabsf(e[i])>mx)mx=fabsf(e[i]);
    if(mx<0.02f) return false;                                                  // all ~zero: ambiguous
    float ss=e[0]*e[0]+e[1]*e[1]+e[2]*e[2]; if(fabsf(ss-1.f)<0.03f) return false;// reject normalized direction/quat component
    return true;
}
static bool isEulerDeg(const float* e){ return isEulerAngles(e); }              // back-compat alias (field map)
// Tighter form for a DIFFERENTIAL candidate: must carry the camera-euler FINGERPRINT now - a near-zero (roll) axis
// plus a bounded (pitch <= ~90deg / pi-2) axis - so we don't flood the snapshot with every vec3 in memory.
static bool isEulerCamera(const float* e){
    if(!isEulerAngles(e)) return false;
    float mx=fabsf(e[0]); for(int i=1;i<3;i++) if(fabsf(e[i])>mx)mx=fabsf(e[i]);
    bool rad = mx<=6.6f;                                                         // radian-scale if the max component is within 2pi
    float pitchLim = rad?1.62f:93.f, rollLim = rad?0.06f:3.5f;
    int nearZero=0,bounded=0; for(int i=0;i<3;i++){ float a=fabsf(e[i]); if(a<=rollLim)nearZero++; if(a<=pitchLim)bounded++; }
    return nearZero>=1 && bounded>=2;                                            // roll~0 + pitch bounded (yaw may be large/wrapping)
}
static bool looksLikePoint(const float* p){ for(int i=0;i<3;i++){ if(p[i]!=p[i]||fabsf(p[i])>1e6f) return false; } return (fabsf(p[0])+fabsf(p[1])+fabsf(p[2]))>0.5f; }
// A minority of engines store orientation as 16-bit PACKED angles (full circle = +/-32767, wrapping) instead of
// float degrees - BioShock Infinite is the canonical example (AxisX/Y range -32767..32767, MinMaxWrap=true). Such a
// field reads as 2-3 consecutive int16s that sweep a huge integer range while the surrounding floats are tiny/zero.
// Detect a triplet (or pair) of plausible packed angles so euler-style int engines aren't missed by the float detector.
static bool isPackedAngle(const int16_t* p,int n){
    // 16-bit packed-angle detection from a static snapshot is inherently noisy (almost any 4 bytes read as two
    // int16s). The reliable gate: a real packed-angle field is INTEGER data whose float reinterpretation is JUNK
    // (0, subnormal, NaN, or absurdly large) - NOT a normal float. So if these bytes read as a sane float
    // (a coordinate / scale / angle like -3136.5 or 0.68), they are NOT a packed-angle field. This stops the
    // detector firing on ordinary float camera data, which was flooding the struct dump.
    const float* f=(const float*)p;
    for(int i=0;i<(n+1)/2;i++){ float v=f[i];
        bool junk = (v!=v) || fabsf(v)>1.0e7f || (v!=0.f && fabsf(v)<1e-3f);   // NaN/huge/subnormal = plausibly packed ints
        if(!junk) return false;                                               // reads as 0 or a normal float => not packed
    }
    int real=0; for(int i=0;i<n;i++){ int v=p[i]; if(v==-32768) return false; if(abs(v)>=1000) real++; }
    return real>=1;
}
// Identify which euler axis is pitch/yaw/roll from the observed values. Cross-game priors: pitch is clamped to
// +/-90, yaw spans +/-180 (wraps), roll sits near 0. Default to the dominant X=pitch,Y=yaw,Z=roll convention, then
// override from magnitudes: an axis whose |value|>90 must be yaw; the near-zero axis is roll. A single-snapshot
// guess - mark it for the user to verify by moving the camera.
static void eulerRoles(const float* e,char out[4]){
    out[0]='P'; out[1]='Y'; out[2]='R'; out[3]=0;
    float mx=fabsf(e[0]); for(int i=1;i<3;i++) if(fabsf(e[i])>mx)mx=fabsf(e[i]);
    bool rad = mx<=6.6f;                                                  // radian vs degree encoding
    float yawThresh = rad?1.6f:90.f, rollThresh = rad?2.f:2.f;
    int big=-1; float bigv=yawThresh; for(int i=0;i<3;i++){ if(fabsf(e[i])>bigv){bigv=fabsf(e[i]);big=i;} }   // widest range = yaw
    int zero=-1; float zv=rollThresh;  for(int i=0;i<3;i++){ if(fabsf(e[i])<zv){zv=fabsf(e[i]);zero=i;} }      // near-zero = roll
    if(big>=0){ for(int i=0;i<3;i++) out[i]=(i==big)?'Y':'?';
        if(zero>=0&&zero!=big) out[zero]='R';
        for(int i=0;i<3;i++) if(out[i]=='?') out[i]='P'; }
}
static float dist3(const float* a,const float* b){ float dx=a[0]-b[0],dy=a[1]-b[1],dz=a[2]-b[2]; return sqrtf(dx*dx+dy*dy+dz*dz); }
// up vector = 2nd basis row (row-major cam-world) or 2nd column (col-major). Dominant world axis = up axis.
static int upAxisOf(const float* m,bool rowMaj){
    float u[3]; if(rowMaj){u[0]=m[4];u[1]=m[5];u[2]=m[6];}else{u[0]=m[1];u[1]=m[5];u[2]=m[9];}
    int ax=0; float best=fabsf(u[0]); if(fabsf(u[1])>best){best=fabsf(u[1]);ax=1;} if(fabsf(u[2])>best){ax=2;} return ax;
}
// starting WorldUnitsPerMetre by engine family (tune in-game). Most engines: 1 unit = 1 cm => ~100.
static float wupmGuess(){
    if(strstr(g_engine,"Unreal"))   return 100.f;   // 1 uu = 1 cm
    if(strstr(g_engine,"Source"))   return 52.f;    // 1 unit ~= 1.9 cm
    if(strstr(g_engine,"Dunia")||strstr(g_engine,"Dawn")) return 10.f;  // Dunia/Dawn family ran well at 10
    if(strstr(g_engine,"Unity"))    return 1.f;     // Unity is metres
    return 100.f;
}

// ---- decode the store instruction at a write site -> base register + displacement (for a code cave) ----
static const char* regName(int r){ static const char* n[16]={"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"}; return (r>=0&&r<16)?n[r]:"?"; }
static bool decodeStore(const uint8_t* s,int n,int& reg,int& disp,const char*& mnem,int& startIdx){
    bool got=false; int bestEnd=-1;
    for(int i=0;i+3<n;i++){ int p=i; int pfx=0;
        if(s[p]==0xF2||s[p]==0xF3||s[p]==0x66){ pfx=s[p]; p++; }                 // mandatory SSE prefix (comes BEFORE REX/VEX)
        int rexB=0,rexR=0,rexX=0; bool vex=false;
        if(p<n && s[p]==0xC5 && p+1<n){ uint8_t b=s[p+1]; rexR=!((b>>7)&1); vex=true;  // 2-byte VEX (AVX): C5 (implies 0F map)
            int pp=b&3; pfx = pp==1?0x66 : pp==2?0xF3 : pp==3?0xF2 : pfx; p+=2; }
        else if(p<n && s[p]==0xC4 && p+2<n){ uint8_t b1=s[p+1],b2=s[p+2]; vex=true;     // 3-byte VEX (AVX): C4
            rexR=!((b1>>7)&1); rexX=!((b1>>6)&1); rexB=!((b1>>5)&1);
            int pp=b2&3; pfx = pp==1?0x66 : pp==2?0xF3 : pp==3?0xF2 : pfx; p+=3; }
        else if(p<n&&(s[p]&0xF0)==0x40){ rexB=s[p]&1; rexX=(s[p]>>1)&1; rexR=(s[p]>>2)&1; p++; }  // optional REX
        const char* mn=nullptr; int opl=0; int immBytes=0; int oc = (vex? (p<n?s[p]:-1) : (p+1<n&&s[p]==0x0F? s[p+1] : -1));
        if(oc==0x11){ opl=vex?1:2; mn = pfx==0xF3?"movss" : pfx==0xF2?"movsd" : pfx==0x66?"movupd" : "movups"; }   // 0F 11 /r store
        else if(oc==0x29&&pfx==0){ mn=vex?"vmovaps":"movaps"; opl=vex?1:2; }
        else if(oc==0x29&&pfx==0x66){ mn=vex?"vmovapd":"movapd"; opl=vex?1:2; }
        else if(oc==0x7F&&pfx==0x66){ mn=vex?"vmovdqa":"movdqa"; opl=vex?1:2; }
        else if(oc==0x7F&&pfx==0xF3){ mn=vex?"vmovdqu":"movdqu"; opl=vex?1:2; }
        else if(!vex&&p<n&&s[p]==0x89){ mn="mov"; opl=1; }                       // 89 /r = mov r/m32/64, reg (integer store)
        else if(!vex&&p<n&&s[p]==0x88){ mn="mov8"; opl=1; }                      // 88 /r = mov r/m8, reg
        else if(!vex&&p<n&&(s[p]==0xD9||s[p]==0xDD)){                             // x87 store (older D3D9/32-bit engines copy the matrix field-by-field)
            if(p+1>=n) continue; int rf=(s[p+1]>>3)&7; if(rf!=2&&rf!=3) continue; // D9 /2=fst /3=fstp (m32); DD /2=fst /3=fstp (m64). other reg fields are loads/control
            mn=(rf==3)?"fstp":"fst"; opl=1; }
        else if(!vex&&p<n&&s[p]==0xC7){ if(p+1>=n||((s[p+1]>>3)&7)!=0) continue; mn="mov_imm"; opl=1; immBytes=4; }   // C7 /0 = mov r/m32, imm32 (constant store: FOV-set / w=1.0)
        else if(!vex&&p<n&&s[p]==0xC6){ if(p+1>=n||((s[p+1]>>3)&7)!=0) continue; mn="mov8_imm"; opl=1; immBytes=1; } // C6 /0 = mov r/m8, imm8
        else if(oc==0xD6&&pfx==0x66){ mn=vex?"vmovq":"movq"; opl=vex?1:2; }      // 66 0F D6 = 64-bit xmm store (vec2/packed pair)
        else continue;
        int mp=p+opl; if(mp>=n) continue; uint8_t modrm=s[mp]; int mod=modrm>>6,rm=modrm&7;
        if(mod==3) continue;                                                    // reg-reg: not a memory store
        int base,dd=0,end=mp+1;
        if(rm==4){                                                             // SIB byte present (e.g. [base+index*s+disp])
            if(mp+1>=n) continue; uint8_t sib=s[mp+1]; int sbase=sib&7; end=mp+2;
            if(sbase==5&&mod==0){ if(mp+5>=n)continue; dd=*(int32_t*)(s+mp+2); end=mp+6; continue; } // disp32, no base reg -> skip
            base=sbase|(rexB<<3);
            if(mod==1&&end<n){ dd=(int8_t)s[end]; end+=1; }
            else if(mod==2&&end+3<n){ dd=*(int32_t*)(s+end); end+=4; }
        } else if(mod==0&&rm==5){ continue; }                                   // RIP-relative global: handled by static-root scan
        else { base=rm|(rexB<<3);
            if(mod==1&&mp+1<n){ dd=(int8_t)s[mp+1]; end=mp+2; }
            else if(mod==2&&mp+4<n){ dd=*(int32_t*)(s+mp+1); end=mp+5; }
            else if(mod!=0) continue; }
        if(base==4) continue;                                                  // [rsp...] is a stack spill, never a camera field
        (void)rexR; (void)rexX; end+=immBytes;                                 // include trailing imm (C7/C6) in the instruction length
        if(end>bestEnd){ bestEnd=end; reg=base; disp=dd; mnem=mn; startIdx=i; got=true; }  // keep the one nearest rip
    }
    return got;
}

// ---- struct dump with field flagging + representation detection ------------------------------------
// Records what KIND of camera this is and where each field sits, so the mod author gets a layout, not a guess.
static int  g_reprMatOff=-1, g_reprQuatOff=-1, g_reprEulerOff=-1, g_reprFovOff=-1, g_reprEyeOff=-1, g_reprTgtOff=-1, g_reprPackedOff=-1, g_reprPosOff=-1;
static char g_reprEulerRoles[4]={0}; static bool g_reprEulerRad=false; static float g_reprFovVal=0.f;
static bool g_reprMatRow=true; static char g_reprKind[40]="unknown";
// FOV detection upgrades (v5.8): solved encoding, horizontal-vs-vertical axis, and the "no CPU field" verdict.
static char g_reprFovEnc[28]="unknown";   // "degrees" / "radians" / "tan_half" / "cot_half" / "factor_of_base" / "projection_only"
static bool g_reprFovHoriz=false;         // true => the field stores HORIZONTAL fov (apply on X); false => vertical
static bool g_reprFovProjOnly=false;      // true => FOV is baked into the projection matrix; no writable CPU scalar
static float g_reprFovBase=0.f;           // for factor_of_base: the base angle the factor multiplies (deg)
static float g_reprFovProofDeg=0.f;       // the projection-derived vertical FOV used to PROVE the field (ground truth)
// forward declarations (definitions appear later in the file) used by the v5.8 AOB/FOV upgrades
struct Entry; static bool moduleRange(void* addr, uintptr_t& base, size_t& size);
static bool pickBestProj(Entry& out); static float readLiveFovV(); static void crossCheckFov(uintptr_t base);
static void chimeAOBFallback(const char* why); static bool bestViewEntry(Entry& out);   // defined later; used by the CPU/differential path
static DWORD WINAPI hijackRetryThread(LPVOID); static volatile bool g_hijackRetryRunning=false;   // active spin-test (thread defined later)
static DWORD WINAPI cpuMoveTestThread(LPVOID);   // v5.14 CPU write-hold move-test (defined later)
static bool g_writerSaysMatrix=false; static int g_writerMatrixDisp=0; static bool g_writerMatrixVerified=false;  // 4x-SSE-store writer => the data is a matrix
struct WCand { char mod[80]; unsigned long long off; int hits; int decoded; int sys; char mnem[24]; int reg; long disp; int sz; char masked[176]; int matrixRun; int matLow; int conf; char form[80]; };
static WCand g_cand[5]; static int g_candN=0;
static void dumpStructFlags(uintptr_t addr){
    g_reprMatOff=g_reprQuatOff=g_reprEulerOff=g_reprFovOff=g_reprEyeOff=g_reprTgtOff=g_reprPackedOff=g_reprPosOff=-1; strcpy(g_reprKind,"unknown");
    Log("# STRUCT @ %p  (scanning -0x40..+0x180; flagged fields show the camera layout):",(void*)addr);
    int pkLines=0;
    // scan a window starting a little before the matched field (quat/euler/fov often precede the matrix)
    for(int o=-0x40;o<0x180;o+=4){ if(!Readable((void*)(addr+o),4)) continue; const char* flag="";
        if((o&0xF)==0 && Readable((void*)(addr+o),64)){ Entry e; memcpy(e.m,(void*)(addr+o),64); classifyInto(e);
            if(e.kind==2){ flag="VIEW/WORLD matrix (4x4)"; if(g_reprMatOff<0){g_reprMatOff=o;g_reprMatRow=e.rowMaj;} }
            else if(e.kind==1){ flag="PROJECTION matrix"; } }
        bool inMat=(g_reprMatOff>=0 && o>g_reprMatOff && o<g_reprMatOff+64);   // floats inside the 4x4 aren't separate quat/euler fields
        if(!*flag && !inMat && Readable((void*)(addr+o),16) && isUnitQuat((float*)(addr+o))){ flag="unit QUATERNION"; if(g_reprQuatOff<0)g_reprQuatOff=o; }
        if(!*flag && !inMat && Readable((void*)(addr+o),12) && isEulerDeg((float*)(addr+o))){ const float* ev=(const float*)(addr+o); char rl[4]; eulerRoles(ev,rl);
            float emx=fabsf(ev[0]); for(int k=1;k<3;k++) if(fabsf(ev[k])>emx)emx=fabsf(ev[k]);
            static char eb[48]; snprintf(eb,sizeof(eb),"EULER [%c,%c,%c] (%s; pitch bounded,yaw wraps,roll~0)",rl[0],rl[1],rl[2],emx<=6.6f?"radians":"degrees"); flag=eb; if(g_reprEulerOff<0){g_reprEulerOff=o; memcpy(g_reprEulerRoles,rl,4); g_reprEulerRad=(emx<=6.6f);} }
        if(!*flag){ float f=*(float*)(addr+o); if(isFovFloat(f)){ flag=(f>0.5f&&f<1.6f)?"maybe FOV (angle, or a FACTOR of base FOV ~1.0=100%)":"maybe FOV"; if(g_reprFovOff<0){g_reprFovOff=o; g_reprFovVal=f;} } }
        if(!*flag && Readable((void*)(addr+o),6) && isPackedAngle((int16_t*)(addr+o),3)){
            if(g_reprPackedOff<0 || pkLines<3){   // a real packed-angle struct has a FEW angle fields, not a sea of them - cap the noise
                const int16_t* pk=(const int16_t*)(addr+o);
                static char pb[64]; snprintf(pb,sizeof(pb),"PACKED 16-bit angles [%d,%d,%d] (x65536/360=deg; wraps)",pk[0],pk[1],pk[2]);
                flag=pb; if(g_reprPackedOff<0)g_reprPackedOff=o; pkLines++; } }
        if(*flag){ if(o<0) Log("#   -0x%03X = %12.4f   <- %s",-o,*(float*)(addr+o),flag);
                   else    Log("#   +0x%03X = %12.4f   <- %s", o,*(float*)(addr+o),flag); }
    }
    // eye/target look-at rig: two WORLD points a real distance apart (common in JRPG-style rigs). A genuine eye->target pair is
    // well separated (the target sits metres ahead of the eye); two unit DIRECTION vectors are only ~1 apart and are
    // NOT an eye/target pair - so require dist > 20 units to avoid flagging forward/up basis vectors as look-at points.
    for(int a=-0x40;a<0x140 && g_reprEyeOff<0;a+=4){ if(g_reprMatOff>=0 && a>=g_reprMatOff && a<g_reprMatOff+64) continue;   // inside the 4x4 = not a look-at pair
        if(!Readable((void*)(addr+a),12))continue; float* pa=(float*)(addr+a); if(!looksLikePoint(pa))continue;
        for(int b=a+12;b<a+0x40;b+=4){ if(g_reprMatOff>=0 && b>=g_reprMatOff && b<g_reprMatOff+64) continue;
            if(!Readable((void*)(addr+b),12))continue; float* pb=(float*)(addr+b); if(!looksLikePoint(pb))continue;
            float d=dist3(pa,pb); if(d>20.f&&d<5000.f){ g_reprEyeOff=a; g_reprTgtOff=b; Log("#   +0x%03X / +0x%03X  <- possible EYE / TARGET look-at pair (dist=%.1f)",a,b,d); break; } } }
    // decide the primary representation (matrix wins; then quat; then euler; then eye/target)
    // a position vec3 commonly sits right beside the quaternion (typical pos+quat camera struct)
    if(g_reprQuatOff>=0){ for(int d=-16;d<=16;d+=4){ int o=g_reprQuatOff+d; if(d==0||!Readable((void*)(addr+o),12)) continue;
        if(looksLikePoint((float*)(addr+o))){ g_reprPosOff=o; Log("#   +0x%03X  <- camera POSITION vec3 (beside the quaternion)",o); break; } } }
    // EULER (or quat) cameras keep POSITION in a separate vec3 - search a window near the rotation for a world-position
    // triple (mag well above a unit direction). The corpus scans +/-16KB for it; the struct copy is usually within +/-0x80.
    if(g_reprPosOff<0 && (g_reprEulerOff>=0||g_reprQuatOff>=0)){
        int rotOff = g_reprEulerOff>=0?g_reprEulerOff:g_reprQuatOff; int rotSz = g_reprEulerOff>=0?12:16;
        float bestMag=8.f; int bestO=-1;
        for(int o=rotOff-0x80;o<=rotOff+0x80;o+=4){ if(o>=rotOff && o<rotOff+rotSz) continue;   // skip the rotation field itself
            if(!Readable((void*)(addr+o),12)) continue; const float* p=(const float*)(addr+o);
            if(!looksLikePoint(p)) continue; float ss=p[0]*p[0]+p[1]*p[1]+p[2]*p[2]; if(fabsf(ss-1.f)<0.05f) continue;  // not a unit direction
            float mag=fabsf(p[0])+fabsf(p[1])+fabsf(p[2]); if(mag>bestMag){ bestMag=mag; bestO=o; } }
        if(bestO>=0){ g_reprPosOff=bestO; Log("#   +0x%03X  <- camera POSITION vec3 candidate (world coords, mag=%.0f; verify by moving)",bestO,bestMag); } }
    // FOV can sit far from the matrix/quat (some engines keep it hundreds of bytes away); sweep a wider window for a FOV scalar
    if(g_reprFovOff<0){ for(int o=0x180;o<0x400;o+=4){ if(!Readable((void*)(addr+o),4))continue; float f=*(float*)(addr+o);
        if(isFovFloat(f)){ g_reprFovOff=o; g_reprFovVal=f; Log("#   +0x%03X = %12.4f   <- maybe FOV (far field)",o,f); break; } } }
    if(g_reprMatOff>=0)        snprintf(g_reprKind,sizeof(g_reprKind),"matrix4x4 @+0x%X (%s)",g_reprMatOff,g_reprMatRow?"row":"col");
    else if(g_reprQuatOff>=0)  snprintf(g_reprKind,sizeof(g_reprKind),"quaternion @+0x%X",g_reprQuatOff);
    else if(g_reprEulerOff>=0) snprintf(g_reprKind,sizeof(g_reprKind),"euler-deg @+0x%X",g_reprEulerOff);
    else if(g_reprEyeOff>=0)   snprintf(g_reprKind,sizeof(g_reprKind),"eye/target @+0x%X/+0x%X",g_reprEyeOff,g_reprTgtOff);
    else if(g_reprPackedOff>=0) snprintf(g_reprKind,sizeof(g_reprKind),"packed-angle16 @+0x%X",g_reprPackedOff);
    Log("# REPRESENTATION = %s%s",g_reprKind, g_reprFovOff>=0?"  (+FOV float present)":"");
}

static void armWriteWatch(void* addr,int maxSlices,const char* prompt,bool isMatrix=true);  // fwd decl (defined with the write-watch)
static void armWriteWatchN(uintptr_t* cands,int nCands,bool isMatrix,int maxSlices,const char* prompt);  // multi-copy + page-guard fallback

// ===================================================================================================
// AUTO-MOUSE: when the deep-scan checkbox is on, the probe drives the mouse ITSELF so the app moves the
// camera, logs exactly what motion it injected, and correlates that known motion to which matrix/quat
// responds - giving axis identification (which axis is yaw vs pitch) and the invert signs for free.
// ===================================================================================================
static volatile bool g_autoDriveMouse=false, g_autoWASD=false;
static volatile bool g_extractDone=false, g_notified=false;   // set when a usable write-AOB is captured: stops auto-movement + plays a sound
// decoded write-site capture (declared early so the differential/notify path can read g_wReg); filled by emitWriteAOB
static int g_wReg=-1,g_wDisp=0,g_wSteal=0; static char g_wMnem[16]={0},g_wMod[80]={0},g_wMasked[160]={0},g_wStolenHex[80]={0}; static uintptr_t g_wOff=0;
// v5.14: the differential-confirmed LIVE camera (address + how its orientation/position are stored). This is the CPU
// observation oracle the hijack uses when there's no GPU view (D3D12/Vulkan/pure-CPU) - the renderer's own camera in
// memory, proven live because it moved with the player's input during the differential.
static volatile uintptr_t g_liveCamAddr=0; static int g_liveCamMatOff=-1, g_liveCamEulerOff=-1; static bool g_liveCamEulerRad=true; static char g_liveCamEulerRoles[4]={0};
static uintptr_t g_writerSiteVA=0;   // resolved VA of the captured camera writer (for the writer-level spin test)
static uintptr_t g_diffCopies[8]={0}; static int g_diffCopyN=0;   // coherent pooled copies of the live camera (hijack candidates on CPU)
static char g_wStrong[320]={0}; static int g_wStrongUniq=0;   // corpus-grade unique, wildcarded signature
static int g_invYaw=0,g_invPitch=0;            // invert flags recommended by the correlation probe (fed into the profile)
// Inject only while the GAME window is foreground, so we never spam the desktop if the user alt-tabs.
static bool gameIsForeground(){ HWND fg=GetForegroundWindow(); if(!fg) return false; DWORD pid=0; GetWindowThreadProcessId(fg,&pid); return pid==GetCurrentProcessId(); }
static void injectMouse(int dx,int dy){
    if(!gameIsForeground()||(dx==0&&dy==0)) return;
    INPUT in; memset(&in,0,sizeof(in)); in.type=INPUT_MOUSE; in.mi.dwFlags=MOUSEEVENTF_MOVE; in.mi.dx=dx; in.mi.dy=dy;
    SendInput(1,&in,sizeof(INPUT));
}
// Keyboard via SCANCODE (hardware make/break codes), NOT virtual keys: DirectInput and raw-input games read
// scancodes and ignore SendInput VK events - so scancodes are what makes WASD work across "all game types".
static const WORD SC_W=0x11, SC_A=0x1E, SC_S=0x1F, SC_D=0x20;
static void injectKey(WORD scan,bool down){
    if(!gameIsForeground()) return;
    INPUT in; memset(&in,0,sizeof(in)); in.type=INPUT_KEYBOARD; in.ki.wScan=scan;
    in.ki.dwFlags=KEYEVENTF_SCANCODE|(down?0:KEYEVENTF_KEYUP); SendInput(1,&in,sizeof(INPUT));
}
static void releaseMoveKeys(){ injectKey(SC_W,false); injectKey(SC_A,false); injectKey(SC_S,false); injectKey(SC_D,false); }
// -------- MANUAL player-input monitor --------
// Low-level mouse + keyboard hooks see the player's input even when the game has captured the cursor, and the
// INJECTED flag lets us ignore the probe's OWN auto-input. So we can recognise (and log) when the PLAYER is moving
// the camera/character, use that motion to drive the write-watch/differential, and confirm their input is registering.
static volatile long g_manMouseDx=0, g_manMouseDy=0; static volatile long g_manKeys=0;   // keys bitmask: W=1 A=2 S=4 D=8
static volatile long g_manMouseTotal=0, g_manKeyEvents=0;                                  // lifetime activity counters
static HHOOK g_mouseHook=nullptr, g_kbHook=nullptr;
static LRESULT CALLBACK llMouse(int code,WPARAM wp,LPARAM lp){
    if(code==HC_ACTION && wp==WM_MOUSEMOVE){ MSLLHOOKSTRUCT* m=(MSLLHOOKSTRUCT*)lp;
        if(!(m->flags&LLMHF_INJECTED)){ static POINT prev; static bool init=false;
            if(init){ long dx=m->pt.x-prev.x, dy=m->pt.y-prev.y; InterlockedExchangeAdd(&g_manMouseDx,dx); InterlockedExchangeAdd(&g_manMouseDy,dy);
                InterlockedExchangeAdd(&g_manMouseTotal,(dx<0?-dx:dx)+(dy<0?-dy:dy)); }
            prev=m->pt; init=true; } }
    return CallNextHookEx(nullptr,code,wp,lp);
}
static LRESULT CALLBACK llKey(int code,WPARAM wp,LPARAM lp){
    if(code==HC_ACTION){ KBDLLHOOKSTRUCT* k=(KBDLLHOOKSTRUCT*)lp;
        if(!(k->flags&LLKHF_INJECTED)){ int bit=0;
            if(k->vkCode=='W')bit=1; else if(k->vkCode=='A')bit=2; else if(k->vkCode=='S')bit=4; else if(k->vkCode=='D')bit=8;
            if(bit){ if(wp==WM_KEYDOWN||wp==WM_SYSKEYDOWN){ if(!(g_manKeys&bit)) InterlockedIncrement(&g_manKeyEvents); InterlockedOr(&g_manKeys,bit); }
                     else if(wp==WM_KEYUP||wp==WM_SYSKEYUP) InterlockedAnd(&g_manKeys,~bit); } } }
    return CallNextHookEx(nullptr,code,wp,lp);
}
static DWORD WINAPI manualInputThread(LPVOID){
    g_mouseHook=SetWindowsHookExW(WH_MOUSE_LL,llMouse,g_self,0);
    g_kbHook=SetWindowsHookExW(WH_KEYBOARD_LL,llKey,g_self,0);
    MSG msg; while(GetMessageW(&msg,nullptr,0,0)>0){ TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
// read-and-reset the accumulated manual motion since the last call; returns true if the player did anything
static bool checkManualInput(int& dx,int& dy,int& keys){
    dx=(int)InterlockedExchange(&g_manMouseDx,0); dy=(int)InterlockedExchange(&g_manMouseDy,0); keys=(int)g_manKeys;
    return dx||dy||keys;
}
static void logManualInput(){   // called from the watch/differential loops; rate-limited, only when the player moved
    int dx,dy,mk; if(!checkManualInput(dx,dy,mk)) return;
    int amag=(dx<0?-dx:dx)+(dy<0?-dy:dy); if(amag<3 && !mk) return;
    char keys[8]={0}; int n=0; if(mk&1)keys[n++]='W'; if(mk&2)keys[n++]='A'; if(mk&4)keys[n++]='S'; if(mk&8)keys[n++]='D';
    Log("#   MANUAL INPUT (player): mouse dx=%+d dy=%+d ; WASD=%s  <- your motion is recognised and driving the camera",dx,dy,keys[0]?keys:"-");
}
// Keep relative input working on games that DON'T capture the cursor: if the OS cursor nears a screen edge it
// clamps and relative moves stop registering, so re-center it. Harmless on cursor-locked FPS (the game re-centers anyway).
static void recenterIfEdge(){
    POINT p; if(!GetCursorPos(&p)) return; int sx=GetSystemMetrics(SM_CXSCREEN),sy=GetSystemMetrics(SM_CYSCREEN);
    if(p.x<120||p.x>sx-120||p.y<120||p.y>sy-120) SetCursorPos(sx/2,sy/2);
}
// One-axis turn (mode 0 = yaw/horizontal, 1 = pitch/vertical), ending DISPLACED so the differential sees motion.
// NOTE: not gated by g_extractDone - the axis-correlation probe must still be able to turn the camera after capture.
static void autoTurn(int mode,int ms,int amp){
    if(!g_autoDriveMouse) return;
    DWORD t0=GetTickCount(); long net=0; int n=0;
    while((int)(GetTickCount()-t0)<ms){ if(mode==0) injectMouse(amp,0); else injectMouse(0,amp); net+=amp; n++; if((n&31)==0) recenterIfEdge(); Sleep(5); }
    Log("# AUTO-MOUSE: injected %s turn (%d steps, net %ld counts) - camera should now be displaced.",
        mode==0?"YAW (horizontal)":"PITCH (vertical)",n,net);
}
// Oscillating sweep used DURING a write-watch so the writer keeps firing without the user touching anything.
static void autoOscillate(int ms){
    if(!g_autoDriveMouse || g_extractDone){ Sleep(ms); return; }   // once data is captured, hold still
    DWORD t0=GetTickCount(); int k=0;
    while((int)(GetTickCount()-t0)<ms){ float t=(GetTickCount()-t0)/1000.0f;
        injectMouse((int)lroundf(sinf(t*14.0f)*28.0f),(int)lroundf(cosf(t*9.0f)*16.0f)); if((++k&31)==0) recenterIfEdge(); Sleep(5); }
}
// ---- RANDOMIZED movement: each phase randomly picks a camera mode (spin / yaw-only(x) / pitch-only(y) / still)
// and, if WASD is enabled, a random character move (W/A/S/D). Every phase is logged in detail (mode, duration,
// net mouse delta, keys held) so the log captures exactly what the app did to the camera AND the character.
static void randomPhase(int idx){
    int camMode=rand()%3;                 // 0 spin, 1 yaw-only(x), 2 pitch-only(y)  -- always moving (no idle "still")
    int dir=(rand()&1)?1:-1; int dur=350+rand()%450;   // 350-800ms, snappy
    WORD key=0; const char* keyName="(none)";
    if(g_autoWASD){ int mv=rand()%4;       // always a movement key when WASD is on (no idle gaps)
        if(mv==0){key=SC_W;keyName="W (forward)";} else if(mv==1){key=SC_S;keyName="S (back)";}
        else if(mv==2){key=SC_A;keyName="A (strafe L)";} else {key=SC_D;keyName="D (strafe R)";} }
    const char* camName = camMode==0?(dir>0?"SPIN-RIGHT":"SPIN-LEFT"):camMode==1?"YAW-OSC (x-axis only)":"PITCH-OSC (y-axis only)";
    Log("# RND PHASE %d: camera=%s  character=%s  duration=%dms",idx,camName,keyName,dur);
    if(key) injectKey(key,true);
    DWORD t0=GetTickCount(); long ndx=0,ndy=0; int steps=0;
    while((int)(GetTickCount()-t0)<dur){ float t=(GetTickCount()-t0)/1000.0f; int dx=0,dy=0;
        if(camMode==0) dx=30*dir; else if(camMode==1) dx=(int)lroundf(sinf(t*13.0f)*32.0f); else dy=(int)lroundf(sinf(t*13.0f)*26.0f);
        injectMouse(dx,dy); ndx+=dx; ndy+=dy; steps++; if((steps&31)==0) recenterIfEdge(); Sleep(5); }   // 5ms = ~200Hz, fast & immediate, no idle
    if(key) injectKey(key,false);
    Log("#   -> injected: mouse net dx=%ld dy=%ld over %d steps ; character key held=%s",ndx,ndy,steps,keyName);
}
static void randomExercise(int phases){
    if(!g_autoDriveMouse || g_extractDone) return;
    srand((unsigned)GetTickCount());
    Log("# AUTO-INPUT: %d RANDOMIZED movement phases (camera spin/yaw-x/pitch-y/still%s). Everything injected is logged:",
        phases, g_autoWASD?" + WASD character movement":"");
    for(int i=1;i<=phases && !g_extractDone;i++){ randomPhase(i); logManualInput(); }   // stop early if data captured; also surface the player's own motion
    releaseMoveKeys();
    Log("# AUTO-INPUT: randomized exercise complete (all movement keys released).");
}
// Inject a KNOWN motion (mouse right, mouse down, and - if WASD on - forward) and watch how THIS camera responds:
// which axis moves (axis ID), the sign vs the injected direction (invert flags), and whether the position tracks.
static void correlateAxes(uintptr_t addr,uint8_t type){
    if(!g_autoDriveMouse || type!=1 || !Readable((void*)addr,64)) return;   // need a matrix to read euler/campos from
    Log("# AXIS CORRELATION (auto-input): injecting known motions, measuring this camera's response...");
    auto eul=[&](float&p,float&y,float&r)->bool{ if(!Readable((void*)addr,64))return false; eulerFromBasis((float*)addr,p,y,r); return true; };
    const float TH=0.5f;   // minimum euler change (deg) that counts as a REAL response (below this = no response / stale address)
    bool yawResp=false,pitchResp=false,posResp=false;
    float bp,by,br; if(!eul(bp,by,br)) return;
    autoTurn(0,500,22); Sleep(50);
    float ap,ay,ar; if(eul(ap,ay,ar)){ float dY=ay-by,dP=ap-bp; while(dY>180)dY-=360; while(dY<-180)dY+=360;
        if(fabsf(dY)>=TH||fabsf(dP)>=TH){ yawResp=true; g_invYaw=(dY<0)?1:0;
            Log("#   mouse RIGHT(+x): yaw %.1f->%.1f (d=%+.1f) pitch d=%+.1f  => axis=%s, invert_yaw=%s",
                by,ay,dY,dP, fabsf(dY)>=fabsf(dP)?"YAW":"PITCH?", g_invYaw?"true":"false"); }
        else Log("#   mouse RIGHT(+x): yaw %.1f->%.1f (d=%+.1f) pitch d=%+.1f  => NO rotation response at this fixed address",by,ay,dY,dP); }
    float c2p,c2y,c2r; if(!eul(c2p,c2y,c2r)) return;
    autoTurn(1,500,22); Sleep(50);
    float ep,ey,er; if(eul(ep,ey,er)){ float dP=ep-c2p,dY=ey-c2y; while(dY>180)dY-=360; while(dY<-180)dY+=360;
        if(fabsf(dP)>=TH||fabsf(dY)>=TH){ pitchResp=true; g_invPitch=(dP<0)?1:0;
            Log("#   mouse DOWN(+y): pitch %.1f->%.1f (d=%+.1f) yaw d=%+.1f  => axis=%s, invert_pitch=%s",
                c2p,ep,dP,dY, fabsf(dP)>=fabsf(dY)?"PITCH":"YAW?", g_invPitch?"true":"false"); }
        else Log("#   mouse DOWN(+y): pitch %.1f->%.1f (d=%+.1f) yaw d=%+.1f  => NO rotation response at this fixed address",c2p,ep,dP,dY); }
    if(g_autoWASD && Readable((void*)addr,64)){    // CHARACTER move -> does the camera POSITION track?
        float b0[16]; memcpy(b0,(void*)addr,64); float p0[3]; cameraPos(b0,p0);
        injectKey(SC_W,true); for(int i=0;i<40;i++) Sleep(12); injectKey(SC_W,false); Sleep(60);
        if(Readable((void*)addr,64)){ float a0[16]; memcpy(a0,(void*)addr,64); float p1[3]; cameraPos(a0,p1);
            float md=sqrtf((p1[0]-p0[0])*(p1[0]-p0[0])+(p1[1]-p0[1])*(p1[1]-p0[1])+(p1[2]-p0[2])*(p1[2]-p0[2])); posResp=md>0.5f;
            Log("#   key W (forward ~0.5s): campos %.1f,%.1f,%.1f -> %.1f,%.1f,%.1f (moved %.1f units) => %s",
                p0[0],p0[1],p0[2],p1[0],p1[1],p1[2],md, posResp?"POSITION tracks (good for lean/positional 6DOF)":"position static here"); }
    }
    if(!yawResp && !pitchResp){    // moved during discovery but not now => the fixed address is stale (transient/pooled camera)
        Log("# AXIS CORRELATION: this fixed ADDRESS did not rotate in response to mouse%s, although it moved during discovery.",posResp?" (its POSITION did still track W)":"");
        Log("#   => TRANSIENT/POOLED camera: the matrix instance changes per frame, so a fixed address goes stale. The");
        Log("#      high-frequency writer FUNCTION (FN-HOOK above) is the real locator - trampoline-hook it and add the head");
        Log("#      pose to the matrix in the captured register each call. Invert auto-detect SKIPPED (defaults kept) -");
        Log("#      set invert_yaw / invert_pitch live with F10 / F11 in the runtime.");
    } else {
        Log("# AXIS CORRELATION done: camera responded to injected motion (confirms it IS the live camera); invert_yaw=%s invert_pitch=%s written to the profile.",g_invYaw?"true":"false",g_invPitch?"true":"false");
    }
}
// ---- completion: when a usable camera write-AOB is captured, stop auto-movement and play a sound notification. ----
static DWORD WINAPI doneSoundThread(LPVOID){
    MessageBeep(MB_ICONASTERISK);                       // system notification sound (routed to the default audio device)
    Beep(784,130); Beep(1047,130); Beep(1568,240);      // short ascending success jingle (kernel32 - no extra libs)
    return 0;
}
static volatile bool g_soundPlayed=false;
static void playSuccessSound(){ if(g_soundPlayed) return; g_soundPlayed=true; CreateThread(nullptr,0,doneSoundThread,nullptr,0,nullptr); }
static void notifyExtractionDone(){
    if(g_notified) return; g_notified=true;                       // idempotent: banner logs once
    Entry ov; bool hasViewOracle=bestViewEntry(ov);              // rotation/placement hijack needs a live GPU view to self-confirm
    bool hijackMode=(g_camHijack||g_fovHijack) && hasViewOracle; // no GPU view (D3D12/Vulkan/CPU) => the hijack can never land, so don't wait on it
    Log("");
    Log("################################################################");
    Log("###  EXTRACTION COMPLETE - all needed camera data captured.    ##");
    Log("###  (write-AOB + writer function + representation are saved.)  ##");
    if(hijackMode){
        Log("###  HIJACK MODE: success chime is RESERVED for a CONFIRMED     ##");
        Log("###  camera/FOV hijack landing - NOT for this AOB capture.      ##");
        Log("###  (auto-movement continues so the hijack can keep trying.)   ##");
    } else {
        Log("###  Usable AOB captured (no GPU oracle to hijack-verify on this ##");
        Log("###  API) - this IS the result. Playing the SUCCESS chime.      ##");
    }
    Log("################################################################");
    long mt=g_manMouseTotal, mk=g_manKeyEvents;
    if(mt>0||mk>0) Log("###  (manual player input seen this run: %ld mouse units, %ld WASD presses - it was used too.)",mt,mk);
    if(!hijackMode){ g_extractDone=true; playSuccessSound(); }    // chime now unless a GPU-view hijack is still expected to confirm
}
static volatile bool g_hijackChimed=false, g_fovGaveUp=false;
static void notifyHijackSuccess(){
    if(g_hijackChimed) return; g_hijackChimed=true;
    Log3("");
    Log3("################################################################");
    Log3("###  HIJACK CONFIRMED - the REAL camera was verified on the     ##");
    Log3("###  LIVE game by an actual hijack (see the LANDED lines above).##");
    Log3("###  rotation=%s  placement=%s  FOV=%s  - playing the SUCCESS chime.",
         g_rotHijack?(g_rotHijackLanded?"CONFIRMED":"n/a"):"(off)",
         g_camHijack?(g_camHijackLanded?"CONFIRMED":"n/a"):"(off)",
         g_fovHijack?(g_fovHijackLanded?"CONFIRMED":(g_fovGaveUp?"no separate field":"pending")):"(off)");
    Log3("################################################################");
    g_extractDone=true; playSuccessSound();
}
// The camera is "confirmed" the moment we can move it - a ROTATION or PLACEMENT landing proves the real
// struct/AOB (rotation is the essential head-look axis). Chime as soon as either lands; FOV is secondary and
// never blocks the chime. If only FOV is enabled, its landing stands in.
static void maybeChimeHijackSuccess(){
    bool structConfirmed = (g_rotHijack && g_rotHijackLanded) || (g_camHijack && g_camHijackLanded);
    bool anyStruct = g_rotHijack || g_camHijack;
    if(anyStruct){ if(structConfirmed) notifyHijackSuccess(); }
    else if(g_fovHijack && g_fovHijackLanded) notifyHijackSuccess();
}

// ===================================================================================================
// CONTROLLER (XInput): the user's gamepad already drives the game; XInput allows multiple readers, so the
// probe polls the SAME state (no game hook) and CORRELATES the user's real stick motion to the locked camera -
// right stick <-> rotation (yaw/pitch + invert), left stick <-> position. Works for Xbox / XInput pads (PS pads
// routed through Steam/DS4Windows usually appear as XInput; raw DInput-only pads aren't covered).
// ===================================================================================================
typedef struct { uint16_t wButtons; uint8_t bLT,bRT; int16_t sLX,sLY,sRX,sRY; } XGAMEPAD;
typedef struct { uint32_t dwPacket; XGAMEPAD gp; } XSTATE;
typedef uint32_t (WINAPI* XInputGetState_t)(uint32_t,XSTATE*);
static XInputGetState_t pXIGetState=nullptr; static int g_activePad=-1;
static volatile uintptr_t g_corrAddr=0; static volatile uint8_t g_corrType=0;   // camera locked by the differential -> correlate against it
static bool loadXInput(){
    if(pXIGetState) return true;
    const wchar_t* dlls[]={L"xinput1_4.dll",L"xinput1_3.dll",L"xinput9_1_0.dll",L"xinput1_2.dll",L"xinput1_1.dll"};
    for(auto d:dlls){ HMODULE m=LoadLibraryW(d); if(m){ pXIGetState=(XInputGetState_t)GetProcAddress(m,"XInputGetState"); if(pXIGetState) return true; } }
    return false;
}
static bool pollPad(float& lx,float& ly,float& rx,float& ry,float& lt,float& rt){
    if(!pXIGetState) return false;
    auto nrm=[](int16_t v)->float{ float f=v/32767.0f; return f>1?1:(f<-1?-1:f); };
    for(int k=0;k<4;k++){ int idx=(g_activePad>=0)?g_activePad:k; XSTATE s; memset(&s,0,sizeof(s));
        if(pXIGetState((uint32_t)idx,&s)==0){ lx=nrm(s.gp.sLX); ly=nrm(s.gp.sLY); rx=nrm(s.gp.sRX); ry=nrm(s.gp.sRY); lt=s.gp.bLT/255.f; rt=s.gp.bRT/255.f; g_activePad=idx; return true; }
        if(g_activePad>=0) break; }
    g_activePad=-1; return false;
}
static DWORD WINAPI controllerThread(LPVOID){
    if(!loadXInput()){ Log("# CONTROLLER: XInput not present - gamepad correlation off (DInput-only pad, or no controller)."); return 0; }
    Log("# CONTROLLER: XInput monitor active - move the camera with your gamepad and the log will correlate it.");
    bool announced=false, haveCam=false; float pyaw=0,ppit=0,pcx=0,pcy=0,pcz=0;
    double accRXyaw=0,accRYpit=0,accLpos=0; int corrN=0; DWORD lastLog=0,lastSum=GetTickCount();
    const float DZ=0.20f;
    while(true){
        Sleep(40);
        float lx,ly,rx,ry,lt,rt;
        if(!pollPad(lx,ly,rx,ry,lt,rt)){ haveCam=false; Sleep(400); continue; }
        if(!announced){ Log("# CONTROLLER: gamepad #%d connected (XInput). Right stick = look, left stick = move.",g_activePad); announced=true; }
        bool rS=(fabsf(rx)>DZ||fabsf(ry)>DZ), lS=(fabsf(lx)>DZ||fabsf(ly)>DZ);
        DWORD now=GetTickCount();
        if((rS||lS) && now-lastLog>300){ lastLog=now;
            Log("# CONTROLLER input: Lstick=(%+.2f,%+.2f) Rstick=(%+.2f,%+.2f) LT=%.2f RT=%.2f",lx,ly,rx,ry,lt,rt); }
        uintptr_t ca=g_corrAddr;
        if(ca && g_corrType==1 && Readable((void*)ca,64)){
            float p,y,r; eulerFromBasis((float*)ca,p,y,r); float cp[3]; cameraPos((float*)ca,cp);
            if(haveCam){ float dY=y-pyaw,dP=p-ppit; while(dY>180)dY-=360; while(dY<-180)dY+=360; while(dP>180)dP-=360; while(dP<-180)dP+=360;
                if(rS){ accRXyaw+=(double)rx*dY; accRYpit+=(double)ry*dP; corrN++; }
                if(lS){ accLpos+=sqrtf((cp[0]-pcx)*(cp[0]-pcx)+(cp[1]-pcy)*(cp[1]-pcy)+(cp[2]-pcz)*(cp[2]-pcz)); } }
            pyaw=y; ppit=p; pcx=cp[0]; pcy=cp[1]; pcz=cp[2]; haveCam=true;
        } else haveCam=false;
        if(corrN>25 && now-lastSum>4000){ lastSum=now;
            Log("# CONTROLLER CORRELATION (%d samples on the locked camera):",corrN);
            Log("#   right-stick X <-> yaw   : %s",fabs(accRXyaw)<2?"no clear link yet (move the right stick L/R more)":(accRXyaw>0?"stick-right raises yaw  => invert_yaw=false":"stick-right lowers yaw  => invert_yaw=true"));
            Log("#   right-stick Y <-> pitch : %s",fabs(accRYpit)<2?"no clear link yet (move the right stick U/D more)":(accRYpit>0?"stick-up raises pitch   => invert_pitch=false":"stick-up lowers pitch   => invert_pitch=true"));
            Log("#   left-stick   <-> position: %s",accLpos>5?"position tracks the left stick (movement OK for positional 6DOF)":"little position change seen yet");
        }
    }
    return 0;
}
// ---- DIFFERENTIAL discovery: find the LIVE camera by what changes when you move (no GPU hook needed) ----
struct Snap{ uintptr_t addr; uint8_t type; uint8_t regKind; uint32_t regSize; float v[16]; };   // type 1=matrix 2=quaternion ; regKind 0=private 1=image 2=mapped
static std::vector<Snap> g_snap; static std::mutex g_snapMx;
static void snapshotScan(){
    std::lock_guard<std::mutex> lk(g_snapMx); g_snap.clear();
    Log(""); Log("================== SNAPSHOT (differential discovery) ==================");
    SYSTEM_INFO si; GetSystemInfo(&si); uint8_t* p=(uint8_t*)si.lpMinimumApplicationAddress,*end=(uint8_t*)si.lpMaximumApplicationAddress;
    size_t budget=512u<<20; int mat=0,quat=0,eul=0;
    while(p<end && budget>0 && g_snap.size()<80000){
        MEMORY_BASIC_INFORMATION mbi; if(!VirtualQuery(p,&mbi,sizeof(mbi)))break; uint8_t* base=(uint8_t*)mbi.BaseAddress; size_t rsz=mbi.RegionSize;
        bool rw=mbi.State==MEM_COMMIT&&(mbi.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE))&&!(mbi.Protect&PAGE_GUARD);
        if(rw && rsz<=(64u<<20)){ size_t scan=rsz>budget?budget:rsz; budget-=scan;
            uint8_t rk = (mbi.Type==MEM_IMAGE)?1:(mbi.Type==MEM_MAPPED)?2:0;          // private(0)/image(1)/mapped(2)
            uint32_t rs = (rsz>0xFFFFFFFFull)?0xFFFFFFFFu:(uint32_t)rsz;               // containing-region size (corpus: camera lives in small regions)
            for(size_t o=0;o+64<=scan;o+=4){ const float* f=(const float*)(base+o);
                if((o&0xF)==0){ Entry e; memcpy(e.m,f,64); classifyInto(e); if(e.kind==2){ if(g_snap.size()<80000){ Snap s; s.addr=(uintptr_t)(base+o); s.type=1; s.regKind=rk; s.regSize=rs; memcpy(s.v,f,64); g_snap.push_back(s);} mat++; continue; } }
                if(isUnitQuat(f)){ if(g_snap.size()<80000){ Snap s; s.addr=(uintptr_t)(base+o); s.type=2; s.regKind=rk; s.regSize=rs; memcpy(s.v,f,16); g_snap.push_back(s);} quat++; continue; }
                if(rk==0 && eul<8000 && isEulerCamera(f)){ if(g_snap.size()<80000){ Snap s; s.addr=(uintptr_t)(base+o); s.type=3; s.regKind=rk; s.regSize=rs; memcpy(s.v,f,12); g_snap.push_back(s);} eul++; }   // bare 3-float euler (radian/degree), private heap - ~half of CPU-scan cameras
            }
        }
        if(rsz==0)break; p=base+rsz;
    }
    Log("# captured %d view/world matrices + %d unit-quaternions + %d euler-triples.",mat,quat,eul);
    Log("# NOW: rotate the in-game view ~45 degrees, then press F8. Only the camera will have changed.");
    Log("======================================================================");
}
static void findChains(uintptr_t target);   // fwd
static void emitProfile(const char* cpuMod,unsigned long long cpuOff,bool verified);  // fwd
// Confirm the standalone camera POSITION for a euler/quat camera by MOVING: inject W (forward) and see which nearby
// vec3 actually translates. Movement-verified beats the magnitude heuristic. Needs auto-WASD + game focus.
// Distinguish a real euler camera from transient heap churn: a real camera reads as a valid angle triple across
// several quick samples and never teleports >2pi between 15ms reads; allocator churn zeroes out or jumps randomly.
static bool eulerLooksStable(uintptr_t addr){
    if(!Readable((void*)addr,12)) return false;
    float prev[3]; memcpy(prev,(const void*)addr,12);
    for(int k=0;k<4;k++){ Sleep(15); if(!Readable((void*)addr,12)) return false;
        const float* e=(const float*)addr;
        if(!isEulerCamera(e)) return false;                          // zeroed / non-angle between reads = churn
        float jump=fabsf(e[0]-prev[0])+fabsf(e[1]-prev[1])+fabsf(e[2]-prev[2]);
        if(jump>6.5f) return false;                                  // >~2pi in 15ms is not a physical rotation
        memcpy(prev,e,12); }
    return true;
}
static void confirmCameraPosition(uintptr_t addr,int rotOff,int rotSz){
    if(!g_autoDriveMouse||!g_autoWASD||!gameIsForeground()) return;
    struct C{int o; float p[3];}; std::vector<C> c;
    for(int o=rotOff-0x100;o<=rotOff+0x100;o+=4){ if(o>=rotOff&&o<rotOff+rotSz)continue; if(!Readable((void*)(addr+o),12))continue;
        const float* p=(const float*)(addr+o); if(!looksLikePoint(p))continue; float ss=p[0]*p[0]+p[1]*p[1]+p[2]*p[2]; if(fabsf(ss-1.f)<0.05f)continue;
        C e; e.o=o; e.p[0]=p[0]; e.p[1]=p[1]; e.p[2]=p[2]; c.push_back(e); if(c.size()>=400)break; }
    if(c.empty()) return;
    Log("# POSITION confirm: injecting W (forward) to see which nearby vec3 is the camera position...");
    injectKey(SC_W,true); for(int i=0;i<40 && !g_extractDone;i++) Sleep(12); injectKey(SC_W,false); Sleep(80);
    int best=-1; float bestMove=0.5f;
    for(auto& e:c){ if(!Readable((void*)(addr+e.o),12))continue; const float* p=(const float*)(addr+e.o);
        float d=sqrtf((p[0]-e.p[0])*(p[0]-e.p[0])+(p[1]-e.p[1])*(p[1]-e.p[1])+(p[2]-e.p[2])*(p[2]-e.p[2]));
        if(d>bestMove && d<5000.f){ bestMove=d; best=e.o; } }
    if(best>=0){ g_reprPosOff=best; Log("# POSITION CONFIRMED @ +0x%03X (moved %.1f units on W) - positional 6DOF available.",best,bestMove); }
    else Log("# POSITION: no nearby vec3 responded to W this pass; keeping the magnitude-based candidate.");
}
static bool g_deltaFoundCamera=false;   // set by deltaScan when it locks a camera (gates the pipeline's auto-retry)
static void deltaScan(){
    g_deltaFoundCamera=false;
    std::lock_guard<std::mutex> lk(g_snapMx);
    if(g_snap.empty()){ Log("# delta: no snapshot yet - press F7, move the view, then F8."); return; }
    Log(""); Log("================== DELTA  (changed = the live camera) ==================");
    // ---- collect every mover, then rank by (motion x region-prior x group-consensus) ----
    // Corpus priors that sharpen WHICH address is the real camera (not just the biggest delta):
    //   * the camera struct lives in a SMALL PRIVATE region (the reference framework caps scans at 0x10000);
    //   * the real basis appears as SEVERAL near-identical copies that all move together (majority vote + epsilon),
    //     while a freed/pooled buffer is a lone garbage mover. So consensus + small-private beats raw delta.
    struct Mover{ uintptr_t addr; uint8_t type,regKind; uint32_t regSize; float d; float rot[9]; int group; float score; };
    std::vector<Mover> mv;
    for(auto& s:g_snap){ int n=s.type==1?16:(s.type==3?3:4); if(!Readable((void*)s.addr,(size_t)n*4)) continue;
        const float* f=(const float*)s.addr;
        bool finite=true; for(int i=0;i<n;i++){ float v=f[i]; if(!(v==v)||fabsf(v)>1e6f){ finite=false; break; } }
        if(!finite) continue;
        if(s.type==2 && !isUnitQuat(f)) continue;                       // quaternion must still be unit-norm
        if(s.type==3 && !isEulerCamera(f)) continue;                    // euler triple must still carry the angle fingerprint
        if(s.type==1){ Entry e; memcpy(e.m,f,64); classifyInto(e); if(e.kind!=2) continue; }   // matrix must still be a rotation
        float d=0; for(int i=0;i<n;i++) d+=fabsf(f[i]-s.v[i]);
        if(d>4.0f) d=4.0f;                                              // cap so one big jump can't dominate a coherent mover
        if((s.type==1&&d<=0.02f)||(s.type==2&&d<=0.01f)||(s.type==3&&d<=0.01f)) continue;      // didn't move enough to be the camera
        Mover m; m.addr=s.addr; m.type=s.type; m.regKind=s.regKind; m.regSize=s.regSize; m.d=d; m.group=0; m.score=0;
        if(s.type==1){ m.rot[0]=f[0];m.rot[1]=f[1];m.rot[2]=f[2];m.rot[3]=f[4];m.rot[4]=f[5];m.rot[5]=f[6];m.rot[6]=f[8];m.rot[7]=f[9];m.rot[8]=f[10]; }
        else if(s.type==3){ m.rot[0]=f[0];m.rot[1]=f[1];m.rot[2]=f[2]; m.rot[3]=m.rot[4]=m.rot[5]=m.rot[6]=m.rot[7]=m.rot[8]=0; }   // euler triple
        else { m.rot[0]=f[0];m.rot[1]=f[1];m.rot[2]=f[2];m.rot[3]=f[3]; m.rot[4]=m.rot[5]=m.rot[6]=m.rot[7]=m.rot[8]=0; }
        mv.push_back(m);
    }
    if(mv.empty()){ Log("# nothing changed enough - move the view MORE then F8, or re-snapshot (F7)."); Log("======================================================================"); return; }
    // epsilon majority-vote grouping: matrices/quats whose rotation agrees within EPS are copies of the same camera.
    const float EPS=0.05f;
    for(size_t i=0;i<mv.size();i++){ if(mv[i].group) continue; mv[i].group=(int)i+1;
        for(size_t j=i+1;j<mv.size();j++){ if(mv[j].group||mv[j].type!=mv[i].type) continue;
            int nn=(mv[i].type==1)?9:(mv[i].type==3?3:4); float dd=0; for(int k=0;k<nn;k++) dd+=fabsf(mv[i].rot[k]-mv[j].rot[k]);
            if(dd<EPS) mv[j].group=mv[i].group; } }
    std::unordered_map<int,int> gcount; for(auto&m:mv) gcount[m.group]++;
    for(auto&m:mv){ float rb=1.f;
        // region prior: GENTLY prefer small private regions; only mildly penalize mapped (file/GPU staging). Do NOT
        // heavily penalize multi-MB private regions - game heaps are large and the camera commonly lives in one
        // (over-penalizing a 2MB heap once flipped a real camera matrix below a non-camera quaternion).
        if(m.regKind==0){ if(m.regSize<=0x10000) rb*=1.3f; else if(m.regSize>(64u<<20)) rb*=0.7f; }
        else if(m.regKind==2) rb*=0.5f;
        // STRONGEST camera discriminator: a view MATRIX whose recovered eye sits far from the world origin is a real
        // scene camera (bone/local/identity matrices sit near origin). A bare quaternion has no position and can't
        // earn this - which correctly demotes animation/physics quaternions below the actual camera matrix.
        float camb=1.f;
        if(m.type==1 && Readable((void*)m.addr,64)){ float cp[3]; cameraPos((const float*)m.addr,cp);
            float mag=fabsf(cp[0])+fabsf(cp[1])+fabsf(cp[2]); if(mag>50.f) camb=3.0f; else if(mag>5.f) camb=1.5f; }
        float cons=1.f+0.25f*(gcount[m.group]-1); if(cons>2.5f)cons=2.5f;                            // multiple coherent copies = confidence
        // persistence boost: a matrix that exists as MULTIPLE coherent copies (double/triple-buffered) is a stable,
        // continuously-written camera - the kind whose writer the HW watch reliably traps. A lone (grp==1) matrix is
        // often a transient/temporary instance that goes stale before the watch arms (the recurring Alan Wake miss).
        if(m.type==1 && gcount[m.group]>=2) camb*=1.3f;
        // v5.16: the densest view-matrix pool is where the camera PROVABLY lives (memScan found it). A mover inside
        // that region beats a lone stable-but-transient instance outside it (the Alan Wake 0x141DD0 mis-lock).
        if(g_poolBase && g_poolEnd>g_poolBase && m.addr>=g_poolBase && m.addr<g_poolEnd) camb*=1.8f;
        m.score=m.d*rb*cons*camb; }
    // EULER IS A STRICT FALLBACK. Transient/zeroed heap floods as euler triples (allocator churn shows the max
    // capped delta + many identical copies), which previously out-scored the real VIEW MATRIX. So if ANY real matrix
    // mover exists, drop euler candidates entirely - euler only competes when there is genuinely no matrix to find.
    { bool haveMatrix=false; for(auto& m:mv) if(m.type==1){ haveMatrix=true; break; }
      if(haveMatrix){ size_t before=mv.size();
        mv.erase(std::remove_if(mv.begin(),mv.end(),[](const Mover&m){return m.type==3;}),mv.end());
        if(before!=mv.size()) Log("# (dropped %zu euler-triple movers - a real view matrix is present, so euler is not used)",before-mv.size()); } }
    std::sort(mv.begin(),mv.end(),[](const Mover&a,const Mover&b){return a.score>b.score;});
    Log("# RANKED movers (score = motion x region-prior x consensus x camera-position; corpus: real camera = a far-from-origin VIEW MATRIX):");
    for(size_t i=0;i<mv.size()&&i<8;i++){ char mod[80]; uintptr_t off; moduleOf((void*)mv[i].addr,mod,sizeof(mod),off);
        const char* rk=mv[i].regKind==1?"image":mv[i].regKind==2?"mapped":"private";
        if(mv[i].type==1 && Readable((void*)mv[i].addr,64)){ float pi,ya,ro; eulerFromBasis((float*)mv[i].addr,pi,ya,ro); float cp[3]; cameraPos((float*)mv[i].addr,cp);
            Log("#  [%zu] MATRIX @ %s+0x%llX  d=%.3f score=%.3f region=%s/%uKB grp=%dx  campos=%.0f,%.0f,%.0f euler(p,y,r)=%.1f,%.1f,%.1f",
                i,mod,(unsigned long long)off,mv[i].d,mv[i].score,rk,mv[i].regSize/1024,gcount[mv[i].group],cp[0],cp[1],cp[2],pi,ya,ro); }
        else if(mv[i].type==3 && Readable((void*)mv[i].addr,12)){ const float* e=(const float*)mv[i].addr; char rl[4]; eulerRoles(e,rl);
            float mx=fabsf(e[0]); for(int k=1;k<3;k++) if(fabsf(e[k])>mx)mx=fabsf(e[k]);
            Log("#  [%zu] EULER @ %s+0x%llX  d=%.3f score=%.3f region=%s/%uKB grp=%dx  angles=%.3f,%.3f,%.3f (%s) roles=[%c,%c,%c]",
                i,mod,(unsigned long long)off,mv[i].d,mv[i].score,rk,mv[i].regSize/1024,gcount[mv[i].group],e[0],e[1],e[2],mx<=6.6f?"radians":"degrees",rl[0],rl[1],rl[2]); }
        else Log("#  [%zu] %s @ %s+0x%llX  d=%.3f score=%.3f region=%s/%uKB grp=%dx",
                i,mv[i].type==1?"MATRIX":mv[i].type==3?"EULER":"QUAT",mod,(unsigned long long)off,mv[i].d,mv[i].score,rk,mv[i].regSize/1024,gcount[mv[i].group]); }
    uintptr_t bestAddr=mv[0].addr; uint8_t bestType=mv[0].type;
    // CHURN GUARD: a euler top-pick must be a STABLE camera value, not transient heap that reads as angles one
    // moment and zeroes the next. If the top euler is churn, use the next stable euler; if none is stable, commit
    // nothing rather than emit a false lock (the recurring zeroed-heap flood on pooled-camera D3D12 titles).
    if(bestType==3 && !eulerLooksStable(bestAddr)){
        int repl=-1; for(size_t i=1;i<mv.size()&&i<12;i++){ if(mv[i].type==3 && eulerLooksStable(mv[i].addr)){ repl=(int)i; break; } }
        if(repl>=0){ Log("# (top euler @ +0x%llX was transient heap churn - using the next STABLE euler)",(unsigned long long)mv[0].addr); bestAddr=mv[repl].addr; bestType=mv[repl].type; }
        else { Log("# NO STABLE euler camera: the top euler movers are transient heap churn (read as angles, then zero out).");
               Log("#   -> not committing a euler this pass (emitting churn would be a false lock). Re-run while CONTINUOUSLY");
               Log("#      rotating the view, or rely on the GPU view / page-guard for this pooled-camera title.");
               bestAddr=0; bestType=0; } }
    g_corrAddr=bestAddr; g_corrType=bestType;   // hand the locked camera to the controller thread for gamepad correlation
    if(bestAddr){ g_deltaFoundCamera=true; char mod[80]; uintptr_t off; moduleOf((void*)bestAddr,mod,sizeof(mod),off);
        Log("# BEST mover = %s @ %s+0x%llX (top score; %d coherent cop%s) -> full layout + chains:",
            bestType==1?"matrix":bestType==3?"euler-triple":"quaternion",mod,(unsigned long long)off,gcount[mv[0].group],gcount[mv[0].group]==1?"y":"ies");
        dumpStructFlags(bestAddr); findChains(bestAddr);
        // v5.14: remember this confirmed live camera as the CPU observation oracle for the hijack (no GPU view needed).
        g_liveCamAddr=bestAddr;
        g_liveCamMatOff   = (bestType==1)?0:g_reprMatOff;
        g_liveCamEulerOff = (bestType==3)?0:g_reprEulerOff;
        g_liveCamEulerRad = g_reprEulerRad; memcpy(g_liveCamEulerRoles,g_reprEulerRoles,4);
        if(bestType==3 && Readable((void*)bestAddr,12)){ const float* e=(const float*)bestAddr; char rl[4]; eulerRoles(e,rl);
            float mx=fabsf(e[0]); for(int k=1;k<3;k++) if(fabsf(e[k])>mx)mx=fabsf(e[k]); bool rad=mx<=6.6f; g_reprEulerRad=rad;
            snprintf(g_reprKind,sizeof(g_reprKind),"euler3 @+0x0 (%s %c%c%c)",rad?"rad":"deg",rl[0],rl[1],rl[2]);
            g_reprEulerOff=0; memcpy(g_reprEulerRoles,rl,4);
            Log("# CAMERA IS A BARE EULER TRIPLE (no matrix): pitch/yaw/roll floats in %s. The runtime adds the head pose",rad?"radians":"degrees");
            Log("#   directly to these angles (roles %c,%c,%c). Hook the writer below to make it stick per-frame.",rl[0],rl[1],rl[2]);
            confirmCameraPosition(bestAddr,0,12); }
        // Collect up to 4 DISTINCT copies of this camera (same type, top-ranked) so the write-watch covers a pooled
        // camera no matter which instance is live this frame. If only one exists, the watch falls back to its 4 rows.
        uintptr_t cands[4]; int nc=0;
        // v5.11: also watch the top VIEW-MATRIX mover - a 64B matrix is written by a recognizable SIMD store that is
        // often easier to trap than a lone quaternion, and on pooled cameras (Alan Wake) the matrix writer is the one
        // that stays catchable when the quaternion's fixed address goes stale.
        if(bestType!=1){ for(size_t i=0;i<mv.size();i++){ if(mv[i].type==1){ cands[nc++]=mv[i].addr; Log("# (also watching the top view-matrix mover @ +0x%llX for the writer)",(unsigned long long)(mv[i].addr&~3ull)); break; } } }
        for(size_t i=0;i<mv.size() && nc<4;i++){ if(mv[i].type!=bestType) continue; uintptr_t b=mv[i].addr&~3ull;
            bool dup=false; for(int j=0;j<nc;j++) if((cands[j]&~3ull)==b) dup=true; if(!dup) cands[nc++]=mv[i].addr; }
        if(nc==0) cands[nc++]=bestAddr;
        g_diffCopyN=0; for(int i=0;i<nc && i<8;i++) g_diffCopies[g_diffCopyN++]=cands[i];   // v5.14: hand the pooled copies to the CPU hijack
        crossCheckFov(bestAddr);   // v5.11: prove/correct the FOV against the projection oracle (now works on D3D12 via the CPU projection)
        // This address JUST changed, so it is live RIGHT NOW - the best moment to catch its writer.
        armWriteWatchN(cands,nc,bestType==1,60,"# write-watch armed on the moving camera (~30s, runs the FULL time to gather candidates) - KEEP MOVING the view (mouse or WASD) continuously...");
        correlateAxes(bestAddr,bestType);   // auto-mouse: inject known motion, confirm response + detect invert flags
        Log("# >>> CPU-struct camera. Drive it directly, OR (if the address changes per frame) trampoline-hook the FN-HOOK function above.");
        // ALWAYS write the profile from the differential finding - even if the locator is only partial. This is the
        // CPU/differential path (no GPU view), which previously emitted NOTHING. A profile with the representation is
        // still useful; it's marked verified:false and the runtime ignores it until a write-AOB (write_site) is present.
        emitProfile(mod,(unsigned long long)off,false);
        if(g_wReg>=0) notifyExtractionDone();   // usable AOB -> chimes now if there's no GPU-view oracle to hijack-verify (D3D12/CPU)
        // v5.14: ACTIVELY confirm by really moving the camera. With no GPU oracle we write a yaw to the located camera
        // and its copies and check whether the engine HOLDS the write (a settable camera that the differential already
        // proved reflects the view => writing it MOVES the view) or REVERTS it (a pooled/transient copy => it needs the
        // writer-hook the runtime mod applies, using the captured AOB). Honest, safe, and it really moves a settable cam.
        // v5.14/v5.15: OPT-IN active confirmation. Writing a perturbation into a live game can crash it (it did on
        // Alan Wake), so this is OFF by default - the AOB capture + chime above is the safe, complete result. Enable
        // with active_move_test=1 only if you accept the risk; even then it's gentle (winner only, tiny delta, restored).
        Entry gv; if(g_activeMoveTest && g_liveCamAddr && !bestViewEntry(gv)){ CreateThread(nullptr,0,cpuMoveTestThread,nullptr,0,nullptr); }
        Log("# PROFILE written (verified:false). If it has no write_aob, re-run and keep the view MOVING through the");
        Log("#   ENTIRE write-watch window so the writer instruction gets captured -> that makes it a usable mod.");
    }
    Log("# Re-press F8 after another rotation to confirm the SAME address keeps moving.");
    Log("======================================================================");
}

// pick the current best VIEW candidate (draw-weighted, with a real camera position)
static bool bestViewEntry(Entry& out){
    std::vector<Entry> view;
    { std::lock_guard<std::mutex> lk(g_catMx); for(auto&kv:g_cat){ const Entry&e=kv.second; if(e.kind==2 && g_frame-e.lastFrame<240) view.push_back(e); } }
    if(view.empty()) return false;
    std::sort(view.begin(),view.end(),[](const Entry&a,const Entry&b){
        if(a.draws!=b.draws)return a.draws>b.draws;                 // primary: used by the most draws (shader-consumption proxy)
        if(a.freq!=b.freq)  return a.freq>b.freq;                   // then: most frequently uploaded
        if(a.slot!=b.slot)  return a.slot<b.slot;                   // tie-break: low CB slots are far more common for the view
        bool aa=(a.off==0||(a.off%64)==0), ba=(b.off==0||(b.off%64)==0);
        if(aa!=ba) return aa;                                       // tie-break: view sits at offset 0 or a 64-byte boundary
        return a.off<b.off; });
    for(auto&e:view){ float p=fabsf(e.campos[0])+fabsf(e.campos[1])+fabsf(e.campos[2]); if(p>1.f){ out=e; return true; } }
    out=view[0]; return true;
}

// ---- GPU<->CPU correlation: locate the exact 64-byte matrix in writable memory (the CPU source) ----
struct MemHit{ uintptr_t addr; char mod[80]; uintptr_t off; };
// ---- 4x4 helpers for inverse/transpose correlation (some engines store inverse-view or a transposed copy) ----
static void transpose4(const float* m,float* o){ for(int r=0;r<4;r++)for(int c=0;c<4;c++) o[c*4+r]=m[r*4+c]; }
static bool gj4Inverse(const float* m,float* o){            // general 4x4 inverse via Gauss-Jordan (handles affine + inverse-view)
    double a[4][8];
    for(int r=0;r<4;r++){ for(int c=0;c<4;c++){ a[r][c]=m[r*4+c]; a[r][c+4]=(r==c)?1.0:0.0; } }
    for(int col=0;col<4;col++){
        int piv=col; double best=fabs(a[col][col]);
        for(int r=col+1;r<4;r++){ if(fabs(a[r][col])>best){best=fabs(a[r][col]);piv=r;} }
        if(best<1e-12) return false;
        if(piv!=col) for(int c=0;c<8;c++){ double t=a[col][c]; a[col][c]=a[piv][c]; a[piv][c]=t; }
        double d=a[col][col]; for(int c=0;c<8;c++) a[col][c]/=d;
        for(int r=0;r<4;r++){ if(r==col) continue; double f=a[r][col]; for(int c=0;c<8;c++) a[r][c]-=f*a[col][c]; }
    }
    for(int r=0;r<4;r++)for(int c=0;c<4;c++) o[r*4+c]=(float)a[r][c+4];
    return true;
}
static int findNeedle(const uint8_t* needle,std::vector<MemHit>& hits,int maxHits){
    SYSTEM_INFO si; GetSystemInfo(&si); uint32_t first=*(const uint32_t*)needle;
    uint8_t* p=(uint8_t*)si.lpMinimumApplicationAddress,*end=(uint8_t*)si.lpMaximumApplicationAddress; size_t budget=768u<<20;
    while(p<end && budget>0 && (int)hits.size()<maxHits){
        MEMORY_BASIC_INFORMATION mbi; if(!VirtualQuery(p,&mbi,sizeof(mbi)))break; uint8_t* base=(uint8_t*)mbi.BaseAddress; size_t rsz=mbi.RegionSize;
        bool rw=mbi.State==MEM_COMMIT && (mbi.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE)) && !(mbi.Protect&PAGE_GUARD);
        if(rw && rsz<=(128u<<20)){ size_t scan=rsz>budget?budget:rsz; budget-=scan;
            for(size_t o=0;o+64<=scan;o+=4){ if(*(const uint32_t*)(base+o)==first && memcmp(base+o,needle,64)==0){
                MemHit h; h.addr=(uintptr_t)(base+o); moduleOf(base+o,h.mod,sizeof(h.mod),h.off); hits.push_back(h); if((int)hits.size()>=maxHits)break; } } }
        if(rsz==0)break; p=base+rsz;
    } return (int)hits.size();
}

// ---- pointer chain: static module pointers that resolve (within a small +offset) to a target ----
static void findChains(uintptr_t target){
    Log("# pointer chains -> %p (static roots, depth<=2):",(void*)target);
    SYSTEM_INFO si; GetSystemInfo(&si); int found=0; size_t budget=768u<<20;
    uint8_t* p=(uint8_t*)si.lpMinimumApplicationAddress,*end=(uint8_t*)si.lpMaximumApplicationAddress;
    std::vector<std::pair<uintptr_t,uintptr_t>> lvl1;   // (pointerLocation, offset = target - *loc)
    auto regOK=[](MEMORY_BASIC_INFORMATION&mbi){ return mbi.State==MEM_COMMIT && (mbi.Protect&(PAGE_READWRITE|PAGE_READONLY|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE)) && !(mbi.Protect&PAGE_GUARD); };
    while(p<end && budget>0 && found<12){
        MEMORY_BASIC_INFORMATION mbi; if(!VirtualQuery(p,&mbi,sizeof(mbi)))break; uint8_t* base=(uint8_t*)mbi.BaseAddress; size_t rsz=mbi.RegionSize;
        if(regOK(mbi) && rsz<=(128u<<20)){ size_t scan=rsz>budget?budget:rsz; budget-=scan;
            for(size_t o=0;o+8<=scan;o+=8){ uintptr_t v=*(uintptr_t*)(base+o);
                if(v<=target && target-v<=0x600){ uintptr_t L=(uintptr_t)(base+o); char mod[80]; uintptr_t mo; moduleOf((void*)L,mod,sizeof(mod),mo);
                    if(strcmp(mod,"heap")!=0){ Log("#   %s+0x%llX -> +0x%llX = target",mod,(unsigned long long)mo,(unsigned long long)(target-v)); if(++found>=12)break; }
                    else if(lvl1.size()<48) lvl1.push_back({L,target-v}); } } }
        if(rsz==0)break; p=base+rsz;
    }
    if(found<6 && !lvl1.empty()){ budget=768u<<20; p=(uint8_t*)si.lpMinimumApplicationAddress;
        while(p<end && budget>0 && found<12){
            MEMORY_BASIC_INFORMATION mbi; if(!VirtualQuery(p,&mbi,sizeof(mbi)))break; uint8_t* base=(uint8_t*)mbi.BaseAddress; size_t rsz=mbi.RegionSize;
            if(regOK(mbi) && rsz<=(128u<<20)){ size_t scan=rsz>budget?budget:rsz; budget-=scan;
                for(size_t o=0;o+8<=scan;o+=8){ uintptr_t v=*(uintptr_t*)(base+o);
                    for(auto&pr:lvl1){ if(v<=pr.first && pr.first-v<=0x600){ uintptr_t Q=(uintptr_t)(base+o); char mod[80]; uintptr_t mo; moduleOf((void*)Q,mod,sizeof(mod),mo);
                        if(strcmp(mod,"heap")!=0){ Log("#   %s+0x%llX -> +0x%llX -> +0x%llX = target",mod,(unsigned long long)mo,(unsigned long long)(pr.first-v),(unsigned long long)pr.second); if(++found>=12)break; } } }
                    if(found>=12)break; } }
            if(rsz==0)break; p=base+rsz;
        }
    }
    if(!found) Log("#   (no static chain within depth 2 - target may need a deeper scan or runtime hook)");
}

// ---- write-AOB: hardware-breakpoint the matrix to capture the instruction that writes it ----
// Self-exclusion: the page-guard / HW-watch must NEVER attribute a write to the probe's OWN code (its allocator,
// snapshot vectors, etc. touch the same heap pages). A writer RIP inside 6DOFProbe.dll is an instrumentation
// artifact, not the game's camera updater - ignore it so we don't emit a bogus self-referential locator.
static uintptr_t g_selfBase=0, g_selfEnd=0;
static inline bool isOwnRip(uintptr_t rip){
    if(!g_selfBase && g_self){ g_selfBase=(uintptr_t)g_self; MODULEINFO mi;
        if(GetModuleInformation(GetCurrentProcess(),g_self,&mi,sizeof(mi))) g_selfEnd=g_selfBase+mi.SizeOfImage; }
    return g_selfBase && g_selfEnd && rip>=g_selfBase && rip<g_selfEnd;
}
static uintptr_t g_writers[16]; static int g_writerHits[16]; static volatile int g_writerN=0; static PVOID g_veh=nullptr;
static CONTEXT g_writerCtx[16]; static volatile uintptr_t g_watchAddr=0;   // full register file at the trap + the watched camera address
static int g_writerStackOnly=0;   // v5.16: set when every trapped writer targets the stack (spills, not the camera)
static uintptr_t g_bpAddr[4]={0,0,0,0}; static int g_bpN=0; static uintptr_t g_writerWatch[16]={0};  // up to 4 HW watchpoints; which one fired per writer
// ---- PAGE-GUARD fallback globals: when HW data-BPs on fixed addresses go stale (transient/pooled camera), protect
// the camera's private region read-only and trap ANY writer in the pool via access-violation. Crash-safe design:
// one write-fault records the writer then opens the whole window (no per-page single-step juggling). ----
static volatile uintptr_t g_pgWinBase=0,g_pgWinEnd=0,g_pgBase=0,g_pgEnd=0; static volatile bool g_pgActive=false; static volatile int g_pgGot=0;
static PVOID g_pgVeh=nullptr; static uintptr_t g_pgWriters[16]; static int g_pgHits[16]; static CONTEXT g_pgCtx[16]; static uintptr_t g_pgFault[16]; static volatile int g_pgWriterN=0;
static volatile bool g_pgMode=false;   // true while decoding PAGE-GUARD writers: RIP points AT the store (decode forward), not after it
static LONG CALLBACK veh(EXCEPTION_POINTERS* ep){
    if(ep->ExceptionRecord->ExceptionCode==(DWORD)EXCEPTION_SINGLE_STEP){
#ifdef _WIN64
        uintptr_t rip=(uintptr_t)ep->ContextRecord->Rip;
#else
        uintptr_t rip=(uintptr_t)ep->ContextRecord->Eip;
#endif
        if(isOwnRip(rip)){ ep->ContextRecord->Dr6=0; return EXCEPTION_CONTINUE_EXECUTION; }   // probe's own code touched the page - not the game writer
        if(g_writerN<16){ int idx=-1; for(int i=0;i<g_writerN;i++) if(g_writers[i]==rip){idx=i;break;}
            if(idx>=0) g_writerHits[idx]++; else {
                uintptr_t fired=g_watchAddr; DWORD64 d6=ep->ContextRecord->Dr6;     // which of the 4 watchpoints triggered?
                for(int b=0;b<4;b++) if((d6&(1ull<<b))&&g_bpAddr[b]){ fired=g_bpAddr[b]; break; }
                g_writers[g_writerN]=rip; g_writerHits[g_writerN]=1; g_writerCtx[g_writerN]=*ep->ContextRecord; g_writerWatch[g_writerN]=fired; g_writerN++; } }
        else { for(int i=0;i<16;i++) if(g_writers[i]==rip){ g_writerHits[i]++; break; } }   // keep counting hits once full
        ep->ContextRecord->Dr6=0; return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void setBP(uintptr_t* addrs,int n,bool clear){
    if(!clear){ g_bpN=(n>4?4:n); for(int i=0;i<4;i++) g_bpAddr[i]=(i<g_bpN?addrs[i]:0); }
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0); if(snap==INVALID_HANDLE_VALUE)return;
    THREADENTRY32 te{}; te.dwSize=sizeof(te); DWORD pid=GetCurrentProcessId(),me=GetCurrentThreadId();
    if(Thread32First(snap,&te)){ do{ if(te.th32OwnerProcessID!=pid||te.th32ThreadID==me)continue;
        HANDLE th=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_SUSPEND_RESUME,FALSE,te.th32ThreadID); if(!th)continue;
        SuspendThread(th); CONTEXT c{}; c.ContextFlags=CONTEXT_DEBUG_REGISTERS;
        if(GetThreadContext(th,&c)){
            c.Dr7&=~0xFFull; c.Dr7&=~(0xFFFFull<<16); c.Dr0=c.Dr1=c.Dr2=c.Dr3=0;   // wipe all 4 watchpoints first
            if(!clear){
                for(int i=0;i<g_bpN;i++){
                    switch(i){ case 0:c.Dr0=addrs[0];break; case 1:c.Dr1=addrs[1];break; case 2:c.Dr2=addrs[2];break; case 3:c.Dr3=addrs[3];break; }
                    c.Dr7|=(1ull<<(2*i));            // Li local-enable
                    c.Dr7|=(1ull<<(16+4*i));         // RWi = 01 (break on write)
                    c.Dr7|=(3ull<<(18+4*i)); } }     // LENi = 11 (4 bytes, matches a float field)
            c.Dr6=0; c.ContextFlags=CONTEXT_DEBUG_REGISTERS; SetThreadContext(th,&c);
        }
        ResumeThread(th); CloseHandle(th);
    } while(Thread32Next(snap,&te)); }
    CloseHandle(snap);
}
static void setBP1(void* addr,bool clear){ uintptr_t a=(uintptr_t)addr; setBP(&a,1,clear); }   // single-watchpoint shim
// decoded write-site globals are declared earlier (near the auto-input completion flags) so deltaScan can read g_wReg
// ---- resolve a write-site RIP back to its FUNCTION ENTRY -> a trampoline-hook target ----
// The CPU camera may be a transient buffer (e.g. a per-frame pool), so a fixed ADDRESS goes stale.
// The CODE that writes it doesn't: hook the function (MinHook-style trampoline), let it run, then add
// head pose to whatever camera it just wrote (base register captured below). This is the robust locator.
static bool looksLikePrologue(const uint8_t* p){
    if(p[0]==0x55) return true;                                  // push rbp
    if(p[0]==0x53||p[0]==0x56||p[0]==0x57) return true;          // push rbx/rsi/rdi
    if(p[0]==0x40 && (p[1]==0x53||p[1]==0x55||p[1]==0x56||p[1]==0x57)) return true; // REX push rbx/rbp/rsi/rdi
    if(p[0]==0x41 && (p[1]==0x54||p[1]==0x55||p[1]==0x56||p[1]==0x57)) return true; // push r12/r13/r14/r15 (common function entry)
    if(p[0]==0x48 && p[1]==0x83 && p[2]==0xEC) return true;      // sub rsp,imm8
    if(p[0]==0x48 && p[1]==0x81 && p[2]==0xEC) return true;      // sub rsp,imm32
    // mov [rsp+disp],reg  param/callee-save spill at entry - rax/rcx/rdx/rbx/rbp/rsi/rdi (corpus: rbx & rcx spills are most common)
    if(p[0]==0x48 && p[1]==0x89 && (p[2]==0x44||p[2]==0x4C||p[2]==0x54||p[2]==0x5C||p[2]==0x6C||p[2]==0x74||p[2]==0x7C) && p[3]==0x24) return true;
    if(p[0]==0x44 && p[1]==0x89 && (p[2]==0x44||p[2]==0x4C||p[2]==0x54) && p[3]==0x24) return true; // mov [rsp+x],r8d/r9d/r10d
    if(p[0]==0x48 && p[1]==0x8B && p[2]==0xC4) return true;      // mov rax,rsp
    if(p[0]==0x4C && p[1]==0x8B && p[2]==0xDC) return true;      // mov r11,rsp
    if(p[0]==0x48 && p[1]==0x8B && p[2]==0xEC) return true;      // mov rbp,rsp (rare entry)
    return false;
}
// .PDATA FUNCTION BOUNDS (v5.8, x64): non-leaf x64 functions carry unwind info in the exception directory
// (an array of RUNTIME_FUNCTION{BeginAddress,EndAddress,UnwindData} sorted by RVA). A binary search gives the
// EXACT [begin,end) of the function containing rip - a far more patch-stable anchor than a heuristic prologue
// scan, and the bounds let the cave guarantee its stolen bytes never cross the function end. Returns begin (0 if
// none / leaf / x86).
static uintptr_t findFunctionBoundsPData(uintptr_t rip,uintptr_t& fnBegin,uintptr_t& fnEnd){
    fnBegin=fnEnd=0;
#ifdef _WIN64
    uintptr_t modBase; size_t modSize; if(!moduleRange((void*)rip,modBase,modSize)) return 0;
    auto* dos=(IMAGE_DOS_HEADER*)modBase; if(dos->e_magic!=IMAGE_DOS_SIGNATURE) return 0;
    auto* nt=(IMAGE_NT_HEADERS*)(modBase+dos->e_lfanew); if(nt->Signature!=IMAGE_NT_SIGNATURE) return 0;
    IMAGE_DATA_DIRECTORY ed=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if(!ed.VirtualAddress||ed.Size<sizeof(RUNTIME_FUNCTION)) return 0;
    auto* rf=(RUNTIME_FUNCTION*)(modBase+ed.VirtualAddress); int cnt=ed.Size/sizeof(RUNTIME_FUNCTION);
    uintptr_t rva=rip-modBase; int lo=0,hi=cnt-1;
    while(lo<=hi){ int mid=(lo+hi)/2; if(rva<rf[mid].BeginAddress) hi=mid-1; else if(rva>=rf[mid].EndAddress) lo=mid+1;
        else { fnBegin=modBase+rf[mid].BeginAddress; fnEnd=modBase+rf[mid].EndAddress; return fnBegin; } }
#endif
    (void)rip; return 0;
}
static uintptr_t findFunctionEntry(uintptr_t rip){
    { uintptr_t b,e; uintptr_t pb=findFunctionBoundsPData(rip,b,e); if(pb) return pb; }   // v5.8: exact bounds first
    // walk backward; a function entry is preceded by INT3/NOP alignment padding or the previous func's RET,
    // and itself begins with a recognizable prologue. Return the best candidate within 4KB.
    const int MAXB=4096;
    if(!Readable((void*)(rip-MAXB),MAXB+16)) { // shrink the window if the full range isn't mapped
        for(int w=2048; w>=256; w>>=1){ if(Readable((void*)(rip-w),w+16)){ break; } }
    }
    for(int back=4; back<MAXB; back++){
        uintptr_t cand=rip-back; if(!Readable((void*)(cand-1),20)) continue;
        uint8_t prev=*(uint8_t*)(cand-1);
        bool boundary=(prev==0xCC||prev==0xC3||prev==0x90); // int3 pad / ret / nop pad
        if(boundary && looksLikePrologue((uint8_t*)cand)) return cand;
    }
    // fallback: nearest prologue-looking byte even without a clean boundary
    for(int back=4; back<MAXB; back++){ uintptr_t cand=rip-back; if(Readable((void*)cand,8)&&looksLikePrologue((uint8_t*)cand)) return cand; }
    return 0;
}
static char g_fnMod[80]={0}; static uintptr_t g_fnOff=0; static char g_fnAOB[160]={0}; static char g_camGate[160]={0};
// Disassemble the camera-writer function for RIP-relative GLOBAL loads (mov/lea reg,[rip+disp32]).
// The camera buffer can be transient, but the code reaches it through a STABLE module-global - so the
// global (and the pointer it holds) is a static root you can build a pointer chain from. This is the
// reliable locator for pooled/transient cameras (e.g. render-graph engines).
static void scanFuncGlobals(uintptr_t fn){
#ifndef _WIN64
    Log("#   (RIP-relative global recovery is x64-only; on x86 the camera fn uses absolute [imm32] globals - read the AOB)"); return;
#else
    int reported=0;
    for(int i=0;i<512 && reported<6;i++){
        uintptr_t p=fn+i; if(!Readable((void*)p,8)) break;
        uint8_t* c=(uint8_t*)p;
        if(c[0]==0xC3||c[0]==0xCC) break;                                  // ret / padding -> end of function
        bool rex=(c[0]==0x48||c[0]==0x49||c[0]==0x4C||c[0]==0x4D);
        if(!rex) continue;
        if(!(c[1]==0x8B||c[1]==0x8D)) continue;                            // mov r64,[..] or lea r64,[..]
        if((c[2]&0xC7)!=0x05) continue;                                    // ModRM mod=00 rm=101 => RIP-relative disp32
        int32_t disp=*(int32_t*)(c+3); uintptr_t next=p+7;                 // rip points at the next instruction
        uintptr_t glob=next+(intptr_t)disp;
        char gmod[80]; uintptr_t goff; moduleOf((void*)glob,gmod,sizeof(gmod),goff);
        int reg=((c[2]>>3)&7)|((c[0]&0x4)?8:0);
        const char* kind=(c[1]==0x8D)?"lea":"mov";
        char extra[96]={0};
        if(c[1]==0x8B && Readable((void*)glob,8)){ uintptr_t val=*(uintptr_t*)glob;   // mov loads a pointer/value
            char vmod[80]; uintptr_t voff; if(val){ moduleOf((void*)val,vmod,sizeof(vmod),voff);
                snprintf(extra,sizeof(extra)," -> *global=%s+0x%llX",vmod,(unsigned long long)voff); } }
        Log("#   STATIC-ROOT cand: %s+0x%llX  (%s %s,[rip] in the camera fn)%s",gmod,(unsigned long long)goff,kind,regName(reg),extra);
        reported++;
    }
    if(!reported) Log("#   (no RIP-relative globals found in the first 512 bytes - camera reached via args/registers only)");
#endif
}
// ---- EXPORT: a ready-to-paste Cheat Engine auto-assembler (AOB injection) script, filled from the probe's own capture.
// The CE template is generic/public; only the user's captured AOB/module/register/bytes fill it in. This bridges the
// probe straight into the dominant freecam workflow (aobscanmodule -> code cave -> capture the camera struct pointer).
static void emitCheatEngineTemplate(){
    if(g_wReg<0||!g_wMod[0]||!g_wMasked[0]) return;
    const char* aob=g_wMasked; while(*aob=='|'||*aob==' ') aob++;            // strip the '|' hook marker for CE's scanner
    const char* reg=regName(g_wReg);
    int pad=g_wSteal-5; if(pad<0) pad=0;                                     // jmp is 5 bytes; nop-pad the remainder of the stolen range
    Log("");
    Log("===== EXPORT: Cheat Engine AOB-injection script (paste into a CT 'Auto Assembler' entry) =====");
    Log("[ENABLE]");
    Log("aobscanmodule(camHook,%s,%s)",g_wMod,aob);
    Log("alloc(newmem,$1000,camHook)");
    Log("globalalloc(pCamera,8)            // <- camera struct pointer lands here; read X/Y/Z/rot/FOV at the offsets below");
    Log("label(code) label(return)");
    Log("newmem:");
    Log("  mov [pCamera],%s               // capture the camera struct base the game just wrote to",reg);
    Log("code:");
    Log("  db %s                          // original stolen instruction(s) - executed unchanged",g_wStolenHex[0]?g_wStolenHex:"<stolen bytes>");
    Log("  jmp return");
    Log("camHook:");
    Log("  jmp newmem");
    for(int i=0;i<pad;i++) Log("  nop");
    Log("return:");
    Log("registersymbol(pCamera) registersymbol(camHook)");
    Log("[DISABLE]");
    Log("camHook:");
    Log("  db %s                          // restore original bytes",g_wStolenHex[0]?g_wStolenHex:"<stolen bytes>");
    Log("unregistersymbol(pCamera) unregistersymbol(camHook)");
    Log("dealloc(newmem)");
    Log("// then read the camera from [pCamera]:  field offset = 0x%X (the '??' bytes in the AOB).",g_wDisp<0?-g_wDisp:g_wDisp);
    Log("=================================================================================");
}
// ---- EXPORT: compact field map of the camera struct (offsets the probe detected) ----
static void emitFieldMap(){
    bool any=(g_reprMatOff>=0||g_reprQuatOff>=0||g_reprEulerOff>=0||g_reprFovOff>=0||g_reprEyeOff>=0||g_reprPosOff>=0);
    if(!any) return;
    Log("===== EXPORT: camera struct field map (offsets from the struct base) =====");
    if(g_reprMatOff>=0)   Log("  view_matrix : +0x%X   (4x4 float32, %s-major)",g_reprMatOff,g_reprMatRow?"row":"col");
    if(g_reprQuatOff>=0)  Log("  quaternion  : +0x%X   (x,y,z,w float32)",g_reprQuatOff);
    if(g_reprEulerOff>=0) Log("  euler       : +0x%X   (pitch,yaw,roll %s)",g_reprEulerOff,g_reprEulerRad?"radians":"degrees");
    if(g_reprPosOff>=0)   Log("  position    : +0x%X   (world-coords vec3; for positional 6DOF)",g_reprPosOff);
    if(g_reprEyeOff>=0)   Log("  eye/target  : +0x%X / +0x%X   (look-at vec3 pair)",g_reprEyeOff,g_reprTgtOff);
    if(g_reprFovOff>=0)   Log("  fov         : +0x%X   (scalar; radians if <~3.2, degrees if >~25, OR a FACTOR of a base FOV if ~0.5-1.6 e.g. 1.0=100%%)",g_reprFovOff);
    Log("==========================================================================");
}
// ---- EXPORT: the consolidated machine-readable 6DOF PROFILE - the bridge from probe to a generic runtime.
// Everything a fixed mod engine needs to re-find and drive this camera, in one block. A profile marked
// "verified" has passed the closed-loop view-response test. Save it as <exe>.6dof.json next to the runtime.
static void emitProfile(const char* cpuMod,unsigned long long cpuOff,bool verified){
    // The writer is ground truth about data layout: if it wrote a 64-byte block via consecutive SSE stores, force a
    // MATRIX representation here (a later dumpStructFlags may have re-derived a euler from the same bytes - override it).
    if(g_writerSaysMatrix){ g_reprMatOff=0; g_reprEulerOff=-1; g_reprPosOff=-1; g_reprPackedOff=-1;
        snprintf(g_reprKind,sizeof(g_reprKind),"matrix4x4 @block (%s,%s)",g_reprMatRow?"row":"col",g_writerMatrixVerified?"verified":"writer-inferred"); }
    // Don't let a weaker pass overwrite a stronger profile. Score completeness; only (re)write if this pass is
    // at least as good. (Re-running the differential previously clobbered a good capture with a worse one.)
    static int g_profBestScore=0; static bool g_profEmitted=false;
    int score=(verified?8:0)+(g_wReg>=0?4:0)+(g_wMasked[0]?2:0)+(g_fnAOB[0]?2:0)
              +((g_reprKind[0]&&strcmp(g_reprKind,"unknown"))?1:0);
    if(g_profEmitted && score<=g_profBestScore){
        Log("# profile: kept the earlier, more-complete capture (this pass scored %d <= %d) - not overwriting.",score,g_profBestScore);
        return; }
    g_profBestScore=score; g_profEmitted=true;
    const char* aob=g_wMasked[0]?g_wMasked:""; while(*aob=='|'||*aob==' ') aob++;
    std::string J; char ln[640];
    // append-and-log: every line goes into both the big findings log and the standalone JSON buffer
    #define JL(...) do{ snprintf(ln,sizeof(ln),__VA_ARGS__); Log("%s",ln); J+=ln; J+="\n"; }while(0)
    Log("");
    Log("===== EXPORT: 6DOF PROFILE (also written to %ls) =====",g_profPath);
    JL("{");
    JL("  \"schema\": 1, \"game_exe\": \"%s\", \"api\": \"%s\", \"engine\": \"%s\",",g_game,g_api,g_engine);
    JL("  \"verified\": %s,",verified?"true":"false");
    JL("  \"locator\": {");
    JL("    \"module\": \"%s\", \"write_aob\": \"%s\",",g_wMod[0]?g_wMod:"?",aob);
    JL("    \"capture_register\": \"%s\", \"field_offset\": %d, \"steal_bytes\": %d, \"stolen_hex\": \"%s\",",
        g_wReg>=0?regName(g_wReg):"?",g_wDisp,g_wSteal,g_wStolenHex[0]?g_wStolenHex:"");
    if(g_wStrong[0]) JL("    \"strong_aob\": \"%s\", \"strong_aob_unique\": %s,",g_wStrong,g_wStrongUniq?"true":"false");
    JL("    \"fn_module\": \"%s\", \"fn_offset\": \"0x%llX\", \"fn_entry_aob\": \"%s\",",g_fnMod[0]?g_fnMod:"",(unsigned long long)g_fnOff,g_fnAOB[0]?g_fnAOB:"");
    if(g_camGate[0]) JL("    \"main_camera_gate\": \"%s\",",g_camGate);
    if(g_writerStackOnly)
        JL("    \"stack_only_writers\": true, \"preferred_locator\": \"function_hook\", \"locator_note\": \"write_aob targets the stack (register spill), not the camera - the runtime must trampoline-hook fn_offset and add the pose to the matrix it produces, NOT drive capture_register/field_offset\",");
    JL("    \"cpu_module\": \"%s\", \"cpu_offset\": \"0x%llX\"",cpuMod?cpuMod:"",cpuOff);
    JL("  },");
    // up to 5 ranked writer candidates (the locator above = candidate[0], the differential's most-likely AOB)
    JL("  \"candidates\": [");
    for(int i=0;i<g_candN;i++){ WCand& C=g_cand[i];
        JL("    { \"rank\": %d, \"module\": \"%s\", \"offset\": \"0x%llX\", \"hits\": %d, \"decoded\": %s, \"system_dll\": %s,%s",
            i,C.mod,C.off,C.hits,C.decoded?"true":"false",C.sys?"true":"false", i==0?"  ":"");
        JL("      \"mnemonic\": \"%s\", \"capture_register\": \"%s\", \"field_offset\": %ld, \"writes_bytes\": %d,",
            C.mnem[0]?C.mnem:"?", C.reg>=0?regName(C.reg):"?", C.disp, C.sz);
        JL("      \"matrix_write\": %s, \"matrix_block_low\": %d, \"confidence\": %d, \"write_form\": \"%s\", \"write_aob\": \"%s\" }%s",
            C.matrixRun>=3?"true":"false", C.matLow, C.conf, C.form[0]?C.form:"", C.masked[0]?C.masked:"", i+1<g_candN?",":"");
    }
    JL("  ],");
    JL("  \"differential_most_likely\": { \"rank\": 0, \"module\": \"%s\", \"offset\": \"0x%llX\", \"confidence\": %d, \"is_matrix_writer\": %s, \"write_aob\": \"%s\" },",
        g_candN>0?g_cand[0].mod:(g_wMod[0]?g_wMod:"?"), g_candN>0?g_cand[0].off:(unsigned long long)g_wOff,
        g_candN>0?g_cand[0].conf:0, (g_candN>0&&g_cand[0].matrixRun>=3)?"true":"false", (g_candN>0&&g_cand[0].masked[0])?g_cand[0].masked:(g_wMasked[0]?g_wMasked:""));
    JL("  \"representation\": {");
    JL("    \"kind\": \"%s\",",g_reprKind);
    if(g_reprMatOff>=0)   JL("    \"matrix\": { \"offset\": %d, \"major\": \"%s\" },",g_reprMatOff,g_reprMatRow?"row":"col");
    if(g_reprQuatOff>=0)  JL("    \"quaternion\": { \"offset\": %d, \"order\": \"xyzw\" },",g_reprQuatOff);
    if(g_reprEulerOff>=0) JL("    \"euler\": { \"offset\": %d, \"axis_roles\": \"%c%c%c\", \"encoding\": \"%s\" },",g_reprEulerOff,
        g_reprEulerRoles[0]?g_reprEulerRoles[0]:'?',g_reprEulerRoles[1]?g_reprEulerRoles[1]:'?',g_reprEulerRoles[2]?g_reprEulerRoles[2]:'?',g_reprEulerRad?"radians":"degrees");
    if(g_reprPosOff>=0)   JL("    \"position\": { \"offset\": %d, \"units\": \"world (scale to cm at runtime)\" },",g_reprPosOff);
    if(g_reprEyeOff>=0)   JL("    \"eye_target\": { \"eye_offset\": %d, \"target_offset\": %d },",g_reprEyeOff,g_reprTgtOff);
    if(g_reprPackedOff>=0) JL("    \"packed_angle16\": { \"offset\": %d, \"to_deg\": \"raw*360/65536\", \"wrap\": true },",g_reprPackedOff);
    if(g_reprFovOff>=0){
        // v5.8: encoding SOLVED against the projection's ground-truth vfov (not guessed); axis + base reported.
        const char* enc = (strcmp(g_reprFovEnc,"unknown")!=0) ? g_reprFovEnc
                        : (g_reprFovVal>=20.f?"degrees":(g_reprFovVal>=0.3f&&g_reprFovVal<=3.2f)?"radians":"unknown");
        JL("    \"fov\": { \"offset\": %d, \"sample_value\": %.4f, \"encoding\": \"%s\", \"axis\": \"%s\", \"base_deg\": %.2f, \"proven_against_proj_deg\": %.2f }",
            g_reprFovOff,g_reprFovVal,enc, g_reprFovHoriz?"horizontal":"vertical",
            g_reprFovBase>0?g_reprFovBase:0.f, g_reprFovProofDeg>0?g_reprFovProofDeg:0.f);
    } else if(g_reprFovProjOnly){
        JL("    \"fov\": { \"offset\": -1, \"encoding\": \"projection_only\", \"note\": \"FOV baked into projection; scale m00/m11\", \"proj_vfov_deg\": %.2f }",g_reprFovProofDeg);
    } else                JL("    \"fov\": null");
    JL("  },");
    JL("  \"apply\": { \"mode\": \"additive_eye_fixed\", \"position_scale_xy\": 1.0, \"position_scale_z\": 0.3,");
    // capture_method: "auto" = cave-less hardware breakpoint first, hardened inline cave as fallback ("hwbp"/"cave" force one).
    JL("    \"capture_method\": \"auto\",");
    JL("    \"look_sensitivity\": 0.85, \"smoothing\": 0.0, \"roll_enable\": false, \"udp_port\": 4242,");
    JL("    \"invert_yaw\": %s, \"invert_pitch\": %s, \"invert_roll\": false,",g_invYaw?"true":"false",g_invPitch?"true":"false");
    JL("    \"invert_x\": false, \"invert_y\": false, \"invert_z\": false,");
    // Per-axis apply model (corpus): the head pose is ADDED to the camera field then CLAMPED. Clamps are the HEAD's
    // contribution limits (not the camera's full range) in DEGREES; the runtime converts to the field's encoding
    // (radians for a radian euler, raw for a 4x4) and wraps yaw. FOV clamp from the corpus FOV range.
    JL("    \"rotation_mode\": \"additive\", \"rotation_units\": \"%s\",", (g_reprEulerOff>=0&&!g_reprEulerRad)?"degrees":"radians");
    JL("    \"clamp_deg\": { \"pitch\": 35.0, \"yaw\": 35.0, \"roll\": 15.0 },");
    JL("    \"position_mode\": \"additive\", \"position_units\": \"world\", \"head_cm_to_world\": 0.01,");
    // FOV control for the runtime. Default OFF (the probe found the FOV field; the user opts in). "static" forces
    // fov_target_deg (or scales the engine FOV if target<=0); "scale" multiplies the engine's own FOV. The runtime
    // converts these DEGREE values into the field's own encoding (degrees/radians/factor) using representation.fov.
    // base_fov_deg is only used when the field stores FOV as a factor-of-base. F6 toggles, F5/F7 nudge by fov_step_deg.
    JL("    \"fov_mode\": \"off\", \"fov_target_deg\": 0.0, \"fov_scale\": 1.0, \"fov_step_deg\": 2.0, \"fov_base_deg\": %.1f,",
        g_reprFovBase>0?g_reprFovBase:70.0f);
    JL("    \"fov_clamp\": [50.0, 150.0] },");
    { Entry pjc; bool hp=pickBestProj(pjc);   // v5.8: emit the live ground-truth FOV/aspect/clip planes from the GPU projection
      JL("  \"projection_convention\": { \"handedness\": \"%s\", \"reversed_z\": %s, \"infinite_far\": %s, \"vfov_deg\": %.2f, \"hfov_deg\": %.2f, \"aspect\": %.4f, \"near\": %.4f, \"far\": %.1f }",
        g_projHand<0?"right":(g_projHand>0?"left":"unknown"), g_projRevZ?"true":"false", g_projInfFar?"true":"false",
        hp?pjc.fovY:0.f, hp?pjc.fovX:0.f, hp?pjc.aspect:0.f, hp?pjc.zn:0.f, hp?pjc.zf:0.f); }
    JL("}");
    #undef JL
    writeTextFile(g_profPath,J.c_str());                       // standalone .6dof.json for the runtime
    if(!verified) Log("# NOTE: verified=false - confirm the camera responds (closed-loop or spin-test) before trusting this profile.");
    Log("# FILES: full findings log = %ls ; runtime profile = %ls",g_logPath,g_profPath);
    Log("=================================================================================");
}
// Many camera writers are SHARED across instances (main / shadow / reflection / cutscene). Mods isolate the MAIN
// camera with a cmp/test + conditional-jump gate just before the write (e.g. cmp byte [reg+0xNN],0 / je). Detect and
// report that discriminator so a function-hook can replicate it and move ONLY the gameplay camera.
static bool detectCameraGate(uintptr_t writeRip, char* out, int outsz){
    if(!Readable((void*)(writeRip-64),80)) return false;
    uint8_t buf[80]; memcpy(buf,(void*)(writeRip-64),80);
    for(int i=0;i<60;i++){ int q=i; bool rexB=false; if(buf[q]>=0x40&&buf[q]<=0x4F){ rexB=buf[q]&1; q++; }
        uint8_t op=buf[q]; uint8_t modrm=(q+1<80)?buf[q+1]:0; bool isCmp=false;
        if(op==0x38||op==0x39||op==0x3A||op==0x3B||op==0x84||op==0x85) isCmp=true;        // cmp/test reg forms
        else if((op==0x80||op==0x81||op==0x83) && ((modrm>>3)&7)==7) isCmp=true;          // group /7 = cmp r/m, imm
        if(!isCmp) continue;
        for(int j=q+2;j<q+16 && j<78;j++){ uint8_t jb=buf[j];
            if((jb>=0x70&&jb<=0x7F)||(jb==0x0F && j+1<80 && buf[j+1]>=0x80 && buf[j+1]<=0x8F)){
                int mod=modrm>>6, rm=(modrm&7)|(rexB?8:0); char fld[48]={0};
                static const char* RN[16]={"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
                if(mod!=3 && (modrm&7)!=4){ long disp=0; if(mod==1) disp=(int8_t)buf[q+2]; else if(mod==2) disp=*(int32_t*)(buf+q+2);
                    snprintf(fld,sizeof(fld),"[%s+0x%lX]",RN[rm&15],disp<0?-disp:disp); }
                snprintf(out,outsz,"cmp/test %s + %s ~%d bytes before the store",fld[0]?fld:"a register",(jb==0x0F)?"near jcc":"short jcc",64-i);
                return true; } }
    }
    return false;
}
static void emitFunctionHook(uintptr_t rip){
    uintptr_t fn=findFunctionEntry(rip); if(!fn){ Log("# FN-HOOK: couldn't resolve a function entry above the write-site"); return; }
    char mod[80]; uintptr_t off; moduleOf((void*)fn,mod,sizeof(mod),off);
    if(!Readable((void*)fn,24)){ Log("# FN-HOOK: function entry @ %s+0x%llX (bytes unreadable)",mod,(unsigned long long)off); return; }
    uint8_t* b=(uint8_t*)fn; char aob[160]; int n=0; for(int j=0;j<20 && n<(int)sizeof(aob)-4;j++) n+=snprintf(aob+n,sizeof(aob)-n,"%02X ",b[j]);
    Log("# FN-HOOK: camera-writer FUNCTION @ %s+0x%llX  (trampoline-hook this; call original, then add head pose)",mod,(unsigned long long)off);
    Log("# FN-ENTRY_AOB[entry..+20]: %s",aob);
    Log("# FN-NOTE: at the write-site the camera base is in %s; from the hook, read it from that reg/arg, add pose to its matrix.",g_wReg>=0?regName(g_wReg):"the captured register");
    { char gate[160]; if(detectCameraGate(rip,gate,sizeof(gate))){
        Log("# MAIN-CAMERA GATE: %s -> this writer is SHARED across camera instances (main/shadow/reflection).",gate);
        Log("#   When function-hooking, replicate that check so you move ONLY the gameplay camera. (HW write-watch already isolated the main one by address.)");
        strncpy(g_camGate,gate,sizeof(g_camGate)-1); } }
    Log("# static-root scan (turns a transient camera into a stable global+chain):");
    scanFuncGlobals(fn);
    strncpy(g_fnMod,mod,sizeof(g_fnMod)-1); g_fnOff=off; strncpy(g_fnAOB,aob,sizeof(g_fnAOB)-1);
}
#ifdef _WIN64
static uint64_t ctxReg(const CONTEXT* c,int i){
    switch(i){ case 0:return c->Rax; case 1:return c->Rcx; case 2:return c->Rdx; case 3:return c->Rbx;
        case 4:return c->Rsp; case 5:return c->Rbp; case 6:return c->Rsi; case 7:return c->Rdi;
        case 8:return c->R8; case 9:return c->R9; case 10:return c->R10; case 11:return c->R11;
        case 12:return c->R12; case 13:return c->R13; case 14:return c->R14; case 15:return c->R15; }
    return 0; }
// DETERMINISTIC store identification: among store instructions in [s,s+n), return the one whose
// effective address (computed from the REAL register values captured at the trap) equals the watched
// camera address. This nails SIB+index, AVX/VEX vector stores, any base register - no "nearest rip" guessing.
static bool decodeStoreCtx(const uint8_t* s,int n,const CONTEXT* ctx,uint64_t want,
                           int& reg,int& idxReg,int& scale,long& disp,const char*& mnem,int& startIdx,int& vbytes,int& insLen){
    for(int i=0;i+3<n;i++){ int p=i; int pfx=0; bool vex=false; int rexB=0,rexX=0; int vl=16;
        if(s[p]==0xF2||s[p]==0xF3||s[p]==0x66){ pfx=s[p]; p++; }
        if(p<n && s[p]==0xC5 && p+1<n){ uint8_t b=s[p+1]; vex=true; int L=(b>>2)&1; vl=L?32:16; int pp=b&3; pfx=pp==1?0x66:pp==2?0xF3:pp==3?0xF2:pfx; p+=2; }
        else if(p<n && s[p]==0xC4 && p+2<n){ uint8_t b1=s[p+1],b2=s[p+2]; vex=true; rexX=!((b1>>6)&1); rexB=!((b1>>5)&1); int L=(b2>>2)&1; vl=L?32:16; int pp=b2&3; pfx=pp==1?0x66:pp==2?0xF3:pp==3?0xF2:pfx; p+=3; }
        else if(p<n&&(s[p]&0xF0)==0x40){ rexB=s[p]&1; rexX=(s[p]>>1)&1; p++; }
        const char* mn=nullptr; int opl=0; int sz=4; int oc=(vex?(p<n?s[p]:-1):(p+1<n&&s[p]==0x0F?s[p+1]:-1));
        if(oc==0x11){ opl=vex?1:2; mn=pfx==0xF3?"movss":pfx==0xF2?"movsd":pfx==0x66?"movupd":"movups"; sz=(pfx==0xF3?4:pfx==0xF2?8:vl); }
        else if(oc==0x29&&pfx==0){ mn=vex?"vmovaps":"movaps"; opl=vex?1:2; sz=vl; }
        else if(oc==0x29&&pfx==0x66){ mn=vex?"vmovapd":"movapd"; opl=vex?1:2; sz=vl; }
        else if(oc==0x7F&&pfx==0x66){ mn=vex?"vmovdqa":"movdqa"; opl=vex?1:2; sz=vl; }
        else if(oc==0x7F&&pfx==0xF3){ mn=vex?"vmovdqu":"movdqu"; opl=vex?1:2; sz=vl; }
        else if(oc==0x13&&pfx==0){ mn=vex?"vmovlps":"movlps"; opl=vex?1:2; sz=8; }              // low-2-float store (position vec2/partial)
        else if(oc==0x13&&pfx==0x66){ mn=vex?"vmovlpd":"movlpd"; opl=vex?1:2; sz=8; }
        else if(oc==0xD6&&pfx==0x66){ mn=vex?"vmovq":"movq"; opl=vex?1:2; sz=8; }                // 64-bit xmm store (often a vec2 / packed pair)
        else if(oc==0x17&&pfx==0){ mn=vex?"vmovhps":"movhps"; opl=vex?1:2; sz=8; }                // high-2-float store (paired with movlps for a vec4)
        else if(oc==0x17&&pfx==0x66){ mn=vex?"vmovhpd":"movhpd"; opl=vex?1:2; sz=8; }
        else if(oc==0x2B&&pfx==0){ mn=vex?"vmovntps":"movntps"; opl=vex?1:2; sz=vl; }              // non-temporal aligned store (streaming matrix rows)
        else if(oc==0x2B&&pfx==0x66){ mn=vex?"vmovntpd":"movntpd"; opl=vex?1:2; sz=vl; }
        else if(!vex&&p<n&&s[p]==0x89){ mn="mov"; opl=1; sz=(pfx==0x66)?2:4; }    // 66 89 = 16-bit packed-angle store
        else if(!vex&&p<n&&s[p]==0x88){ mn="mov8"; opl=1; sz=1; }
        else if(!vex&&p<n&&(s[p]==0xD9||s[p]==0xDD)){                             // x87 fst/fstp (older engines copy the matrix via the FPU)
            if(p+1>=n) continue; int rf=(s[p+1]>>3)&7; if(rf!=2&&rf!=3) continue; mn=(rf==3)?"fstp":"fst"; opl=1; sz=(s[p]==0xD9)?4:8; }
        else if(!vex&&p<n&&s[p]==0xC7){ if(p+1>=n||((s[p+1]>>3)&7)!=0) continue; mn="mov_imm"; opl=1; sz=(pfx==0x66)?2:4; }   // C7 /0 = mov r/m, imm (constant store: FOV-set / field init)
        else if(!vex&&p<n&&s[p]==0xC6){ if(p+1>=n||((s[p+1]>>3)&7)!=0) continue; mn="mov8_imm"; opl=1; sz=1; }
        else continue;
        int mp=p+opl; if(mp>=n) continue; uint8_t modrm=s[mp]; int mod=modrm>>6,rm=modrm&7;
        if(mod==3) continue;
        int base=-1,index=-1,sc=1; long dd=0; int end=mp+1;
        if(rm==4){ if(mp+1>=n) continue; uint8_t sib=s[mp+1]; sc=1<<(sib>>6); int ix=(sib>>3)&7; int sb=sib&7; end=mp+2;
            index=ix|(rexX<<3); if(index==4) index=-1;                          // index field 100b = no index
            if(sb==5&&mod==0){ if(mp+5>=n)continue; dd=*(int32_t*)(s+mp+2); end=mp+6; base=-1; }
            else { base=sb|(rexB<<3); if(mod==1&&end<n){ dd=(int8_t)s[end]; end+=1; } else if(mod==2&&end+3<n){ dd=*(int32_t*)(s+end); end+=4; } } }
        else if(mod==0&&rm==5) continue;                                        // RIP-relative -> static-root path
        else { base=rm|(rexB<<3); if(mod==1&&mp+1<n){ dd=(int8_t)s[mp+1]; end=mp+2; } else if(mod==2&&mp+4<n){ dd=*(int32_t*)(s+mp+1); end=mp+5; } else if(mod!=0) continue; }
        uint64_t ea=(uint64_t)(int64_t)dd; if(base>=0) ea+=ctxReg(ctx,base); if(index>=0) ea+=ctxReg(ctx,index)*(uint64_t)sc;
        if(ea==want){ reg=base; idxReg=index; scale=sc; disp=dd; mnem=mn; startIdx=i; vbytes=sz; insLen=end-i; return true; }
    }
    return false;
}
static const char* regName64(int r){ static const char* N[16]={"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"}; return (r>=0&&r<16)?N[r]:"?"; }
#endif
// A "writer" that lives in a GPU driver or D3D/GL runtime module is a constant-buffer UPLOAD
// (the driver memcpy'ing into a mapped buffer with AVX stores), never the game's camera logic.
// Catching one means the differential locked a GPU staging buffer, not the CPU camera.
static bool isDriverModule(const char* mod){
    char b[96]; int i=0; for(; mod[i] && i<95; i++) b[i]=(char)tolower((unsigned char)mod[i]); b[i]=0;
    const char* hits[]={"nvwgf2um","nvd3dum","nvldumd","nvoglv","amdxc","amdvlk","atidx","atiumd",
                        "igd","igc","igvk","igdumd","d3d8","d3d9","d3d10","d3d11","d3d12","dxgi",
                        "vulkan","opengl32","vrclient","openvr","nvcuda"};
    for(const char* h:hits) if(strstr(b,h)) return true; return false;
}
// Windows system / C-runtime DLLs host the generic memcpy/memmove/RtlCopyMemory the engine calls to bulk-copy the
// camera struct. Those are NEVER the camera's own logic - a writer there is the copy path, not the updater. We don't
// hard-drop them (a game may truly copy its camera via memcpy), but they must rank BELOW any game/engine-module writer.
static bool isSystemModule(const char* mod){
    char b[96]; int i=0; for(; mod[i] && i<95; i++) b[i]=(char)tolower((unsigned char)mod[i]); b[i]=0;
    const char* hits[]={"ntdll","kernel32","kernelbase","msvcrt","ucrtbase","vcruntime","msvcp",
                        "user32","gdi32","gdiplus","ole32","combase","rpcrt4","win32u","sechost",
                        "advapi32","shcore","shell32","ws2_32","bcryptprimitives","wow64"};
    for(const char* h:hits) if(strstr(b,h)) return true; return false;
}
// When a writer lands in a system DLL (memcpy) or is otherwise generic, walk the stack from the captured context to
// find the first return address that lands in the GAME (non-system, non-driver, non-probe) module just after a CALL.
// That's the game function that triggered the write - a real, hookable target instead of a shared memcpy.
#ifdef _WIN64
static uintptr_t findGameCaller(const CONTEXT* ctx){
    uintptr_t sp=ctx->Rsp; if(!sp) return 0;
    for(int i=0;i<96;i++){ uintptr_t slot=sp+(uintptr_t)i*8; if(!Readable((void*)slot,8)) break;
        uintptr_t ra=*(uintptr_t*)slot; if(ra<0x10000) continue;
        char m[80]; uintptr_t o; moduleOf((void*)ra,m,sizeof(m),o);
        if(m[0]==0||isSystemModule(m)||isDriverModule(m)||isOwnRip(ra)) continue;     // want game/engine code
        if(!Readable((void*)(ra-7),7)) continue;
        uint8_t* p=(uint8_t*)ra;
        bool afterCall=(p[-5]==0xE8) ||                                              // call rel32
                       (p[-2]==0xFF && (p[-1]&0x38)==0x10) ||                        // call r/m (FF /2), short forms
                       (p[-3]==0xFF && (p[-2]&0x38)==0x10) || (p[-6]==0xFF) || (p[-7]==0xFF);
        if(afterCall) return ra; }
    return 0;
}
#endif
static bool detectMatrixWriteRun(const uint8_t* w,int wn,int wantBase,int& lowDisp,int& count,char* form=nullptr,int formSz=0){
    int sd[32], sb[32], ssz[32], styp[32], ns=0;   // styp: 0=packed SSE, 1=movss, 2=movsd, 3=integer-mov
    for(int p=0;p+1<wn;){
        int q=p,rexB=0,rexW=0,sse=16;
        while(q<wn && (w[q]==0xF3||w[q]==0xF2||w[q]==0x66)){ if(w[q]==0xF3)sse=4; else if(w[q]==0xF2)sse=8; q++; }   // movss=4 / movsd=8 / packed=16
        if(q<wn && w[q]>=0x40 && w[q]<=0x4F){ rexB=w[q]&1; rexW=(w[q]>>3)&1; q++; }                                // REX (.B base ext, .W 64-bit)
        if(q+2<wn && w[q]==0x0F && (w[q+1]==0x29||w[q+1]==0x11)){                        // SSE store: movaps / movups / movss / movsd
            int modrm=w[q+2],mod=modrm>>6,rm=(modrm&7)|(rexB?8:0),ty=(sse==4)?1:(sse==8)?2:0;
            if(mod==1 && q+3<wn){ if(ns<32){ sd[ns]=(int8_t)w[q+3]; sb[ns]=rm; ssz[ns]=sse; styp[ns]=ty; ns++; } p=q+4; continue; }
            if(mod==2 && q+6<wn){ if(ns<32){ sd[ns]=*(int32_t*)(w+q+3); sb[ns]=rm; ssz[ns]=sse; styp[ns]=ty; ns++; } p=q+7; continue; }
        }
        if(q+1<wn && w[q]==0x89){                                                       // integer mov store (matrix written element-by-element, REDengine-style)
            int modrm=w[q+1],mod=modrm>>6,rm=(modrm&7)|(rexB?8:0);
            if((modrm&7)!=4){                                                            // skip SIB forms
                if(mod==1 && q+2<wn){ if(ns<32){ sd[ns]=(int8_t)w[q+2]; sb[ns]=rm; ssz[ns]=rexW?8:4; styp[ns]=3; ns++; } p=q+3; continue; }
                if(mod==2 && q+5<wn){ if(ns<32){ sd[ns]=*(int32_t*)(w+q+2); sb[ns]=rm; ssz[ns]=rexW?8:4; styp[ns]=3; ns++; } p=q+6; continue; } }
        }
        p++;
    }
    for(int t=0;t<ns;t++){ int b=sb[t]; if(wantBase>=0&&b!=wantBase) continue;
        int c=0,lo=0x7fffffff,hi=-0x7fffffff,packed=0,scalar=0,intmov=0;
        for(int u=0;u<ns;u++) if(sb[u]==b){ c++; if(sd[u]<lo)lo=sd[u]; if(sd[u]>hi)hi=sd[u]; if(styp[u]==0)packed++; else if(styp[u]==3)intmov++; else scalar++; }
        int span=hi-lo;
        bool packedRun=packed>=3 && span>=32 && span<=c*16+8;                            // 4x movaps/movups rows
        bool scalarRun=(scalar+intmov)>=8 && span>=32 && span<=c*8+8;                    // element-by-element (movss or integer mov)
        if(packedRun||scalarRun){ lowDisp=lo; count=c;
            if(form&&formSz){ if(packed>=3) snprintf(form,formSz,"%d packed SSE stores (movaps/movups, 16-byte rows, span %d B)",packed,span+16);
                else if(intmov>=scalar) snprintf(form,formSz,"%d integer-mov elements written field-by-field (4B each, span %d B)",intmov,span+4);
                else snprintf(form,formSz,"%d movss/movsd scalar elements (span %d B)",scalar,span+4); }
            return true; } }
    return false;
}
// ---- STRONG AOB: corpus-grade signatures are context-rich, wildcard volatile fields, and are VERIFIED UNIQUE ----
static bool moduleRange(void* addr, uintptr_t& base, size_t& size){
    HMODULE h=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)addr,&h)||!h) return false;
    MODULEINFO mi; if(!GetModuleInformation(GetCurrentProcess(),h,&mi,sizeof(mi))) return false;
    base=(uintptr_t)h; size=mi.SizeOfImage; return true;
}
// count occurrences of a masked pattern (mask: 'x'=must match, '?'=wildcard) within a module image, page-safe, capped.
static int countSig(uintptr_t base,size_t size,const uint8_t* pat,const char* mask,int len,int cap){
    int a=0; while(a<len && mask[a]!='x') a++; if(a>=len) return 99;        // all-wildcard -> treat as non-unique
    uint8_t anchor=pat[a]; int found=0;
    for(uintptr_t region=base; region<base+size; ){
        MEMORY_BASIC_INFORMATION mbi; if(!VirtualQuery((void*)region,&mbi,sizeof(mbi))) break;
        uintptr_t rend=(uintptr_t)mbi.BaseAddress+mbi.RegionSize;
        if(mbi.State==MEM_COMMIT && !(mbi.Protect&(PAGE_NOACCESS|PAGE_GUARD))){
            uintptr_t scanEnd = rend<base+size?rend:base+size;
            for(uintptr_t o=region; (int)(scanEnd-o)>=len; o++){ const uint8_t* p=(const uint8_t*)o;
                if(p[a]!=anchor) continue; bool ok=true; for(int k=0;k<len;k++){ if(mask[k]=='x'&&p[k]!=pat[k]){ok=false;break;} }
                if(ok){ if(++found>=cap) return found; } } }
        region=rend; if(region<=base) break; }
    return found;
}
// wildcard volatile 4-byte fields (call/jmp/jcc rel32 and RIP-relative disp32) in a byte window so the signature
// survives game patches; KEEP everything else incl. the struct field offset (exactly what the corpus does).
static void maskVolatile(const uint8_t* b,int len,char* mask){
    for(int k=0;k<len;k++) mask[k]='x';
    for(int k=0;k+5<=len;k++){
        if(b[k]==0xE8||b[k]==0xE9){ for(int j=1;j<=4&&k+j<len;j++) mask[k+j]='?'; k+=4; continue; }   // call/jmp rel32
        if(b[k]==0x0F && k+6<=len && b[k+1]>=0x80 && b[k+1]<=0x8F){ for(int j=2;j<=5&&k+j<len;j++) mask[k+j]='?'; k+=5; continue; } // jcc rel32
    }
    // RIP-relative: a mov/lea (8B/8D/89) with ModRM mod=00 rm=101 -> 4-byte disp is an absolute pointer; wildcard it.
    for(int k=0;k+6<=len;k++){ int q=k; if((b[q]&0xF0)==0x40) q++;                                  // optional REX
        if(q+5<len && (b[q]==0x8B||b[q]==0x8D||b[q]==0x89||b[q]==0x03||b[q]==0x2B) && (b[q+1]&0xC7)==0x05){
            for(int j=2;j<=5&&q+j<len;j++) mask[q+j]='?'; } }
}
static void sigToStr(const uint8_t* b,const char* mask,int len,char* out,int outsz){
    int n=0; for(int k=0;k<len && n<outsz-4;k++){ if(mask[k]=='?') n+=snprintf(out+n,outsz-n,"?? "); else n+=snprintf(out+n,outsz-n,"%02X ",b[k]); }
}
// Build the strongest signature for a hook site: start at the store, grow context until the masked pattern is UNIQUE
// in the module (or hit the cap). Returns the unique length (0 if it could not be made unique). Fills outSig/outMaskHex.
// RELOCATION-AWARE MASKING (v5.8): every absolute address embedded in code is listed in the PE base-relocation
// table (.reloc). Those bytes change whenever the module rebases (ASLR) - and on x86, where code uses absolute
// addresses heavily (Persona 4, Deus Ex HR), an unmasked reloc byte breaks the signature on every launch. We parse
// the reloc directory of the module and wildcard any signature byte whose RVA is covered by a relocation entry.
static void maskRelocs(uintptr_t site,int len,char* mask){
    uintptr_t modBase; size_t modSize; if(!moduleRange((void*)site,modBase,modSize)) return;
    auto* dos=(IMAGE_DOS_HEADER*)modBase; if(dos->e_magic!=IMAGE_DOS_SIGNATURE) return;
    auto* nt=(IMAGE_NT_HEADERS*)(modBase+dos->e_lfanew); if(nt->Signature!=IMAGE_NT_SIGNATURE) return;
    IMAGE_DATA_DIRECTORY rd=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if(!rd.VirtualAddress||!rd.Size) return;
    uintptr_t siteRVA=site-modBase, lo=siteRVA, hi=siteRVA+len;
    auto* blk=(IMAGE_BASE_RELOCATION*)(modBase+rd.VirtualAddress);
    uintptr_t end=(uintptr_t)blk+rd.Size; int guard=0;
    while((uintptr_t)blk<end && blk->SizeOfBlock>=sizeof(IMAGE_BASE_RELOCATION) && ++guard<100000){
        int n=(blk->SizeOfBlock-sizeof(IMAGE_BASE_RELOCATION))/2; auto* ent=(WORD*)((uintptr_t)blk+sizeof(IMAGE_BASE_RELOCATION));
        for(int i=0;i<n;i++){ int type=ent[i]>>12; if(type==IMAGE_REL_BASED_ABSOLUTE) continue;     // padding entry
            uintptr_t rva=blk->VirtualAddress+(ent[i]&0xFFF);
            int width=(type==IMAGE_REL_BASED_HIGHLOW)?4:(type==IMAGE_REL_BASED_DIR64)?8:(type==IMAGE_REL_BASED_HIGH||type==IMAGE_REL_BASED_LOW)?2:4;
            for(int w=0;w<width;w++){ uintptr_t b=rva+w; if(b>=lo&&b<hi) mask[b-lo]='?'; } }
        blk=(IMAGE_BASE_RELOCATION*)((uintptr_t)blk+blk->SizeOfBlock);
    }
}
static int buildStrongAOB(uintptr_t site,const char* mod,char* outSig,int outSigSz,int& uniqAt){
    uintptr_t base; size_t size; uniqAt=0;
    if(!moduleRange((void*)site,base,size)) { return 0; }
    uint8_t buf[48]; if(!Readable((void*)site,48)){ if(!Readable((void*)site,24)) return 0; memcpy(buf,(void*)site,24);} else memcpy(buf,(void*)site,48);
    int avail = Readable((void*)site,48)?48:24;
    char mask[48];
    for(int len=8; len<=avail; len+=2){
        maskVolatile(buf,len,mask);
        maskRelocs(site,len,mask);                // v5.8: also wildcard ASLR-relocated absolute-address bytes
        int c=countSig(base,size,buf,mask,len,2);
        if(c<=1){ uniqAt=len; sigToStr(buf,mask,len,outSig,outSigSz); return len; }   // unique (1 = itself)
    }
    // couldn't make it unique within avail bytes - emit the longest we have, flagged non-unique
    maskVolatile(buf,avail,mask); maskRelocs(site,avail,mask); sigToStr(buf,mask,avail,outSig,outSigSz); uniqAt=0; return avail;
}
static void emitWriteAOB(){
    g_wReg=-1; g_fnOff=0; g_writerStackOnly=0;
    if(g_writerN==0){ Log("# write-AOB: no writer captured (camera wasn't written during the watch window, or BPs blocked)"); return; }
    // Drop any writer inside a GPU driver / D3D runtime - those are CB uploads (driver memcpy), not the game camera.
    { int w=0; for(int i=0;i<g_writerN;i++){ char m[80]; uintptr_t o; moduleOf((void*)g_writers[i],m,sizeof(m),o);
          if(isOwnRip(g_writers[i])){ Log("# (ignoring writer in 6DOFProbe.dll+0x%llX - the probe's own code, not the game camera)",(unsigned long long)o); continue; }
          if(isDriverModule(m)){ Log("# (ignoring writer in %s+0x%llX - GPU-driver buffer upload, not the game camera)",m,(unsigned long long)o); continue; }
          g_writers[w]=g_writers[i]; g_writerHits[w]=g_writerHits[i]; g_writerCtx[w]=g_writerCtx[i]; g_writerWatch[w]=g_writerWatch[i]; w++; }
      g_writerN=w; }
    if(g_writerN==0){ Log("# write-AOB: the only writers were GPU-driver CB uploads - no game-side camera writer this pass."); 
        Log("#   -> the differential locked a GPU staging buffer; use the GPU<->CPU correlated VIEW above as the camera instead."); return; }
    // RANK the trapped writers by, in order: (1) non-system module - the camera's code never lives in ntdll/CRT;
    // (2) DECODES as an exact-match store - a writer we can't decode yields a useless locator (capture_register "?"),
    // so a clean movaps/mov that resolves to the camera address beats an undecodable SIB/atypical site even with fewer
    // hits; (3) hit count. This is what makes the clean SSE camera store win over a higher-hit store we can't read.
    auto sysW=[&](int i){ char m[80]; uintptr_t o; moduleOf((void*)g_writers[i],m,sizeof(m),o); return isSystemModule(m)?1:0; };
    int dec[16]={0};
    // v5.16: per-writer destination analysis. A clean-decoding, game-module, high-hit store can still be a
    // register SPILL into the stack (movaps [rsp+0x60]) inside the camera-builder - NOT the camera. We now
    // resolve each store's TARGET address from the trap context and classify it: stk[]=writes into the
    // thread stack (a spill, never the camera); cam[]=writes into the locked camera / the densest view pool
    // (this IS the camera writer). Ranking puts camera-targeting non-stack stores first, so a stack spill can
    // no longer win on hit-count alone. (Fixes the Alan Wake Remastered capture that locked [rsp+0x60].)
    int stk[16]={0}, cam[16]={0};
#ifdef _WIN64
    for(int i=0;i<g_writerN;i++){ uintptr_t rip=g_writers[i]; uint8_t* w=g_pgMode?(uint8_t*)rip:(uint8_t*)(rip-24); int wn=24;
        uint64_t want=g_writerWatch[i]?(uint64_t)g_writerWatch[i]:(uint64_t)g_watchAddr;
        int r2=-1,ir=-1,sc=1,vb=0,cs=-1,il=0; long ld=0; const char* cm=nullptr;
        if(want && Readable(w,wn) && decodeStoreCtx(w,wn,&g_writerCtx[i],want,r2,ir,sc,ld,cm,cs,vb,il) && r2>=0){ dec[i]=1;
            uint64_t tgt=(uint64_t)((int64_t)ctxReg(&g_writerCtx[i],r2)+(int64_t)ld);
            if(ir>=0) tgt+=ctxReg(&g_writerCtx[i],ir)*(uint64_t)sc;
            uint64_t rsp=g_writerCtx[i].Rsp;
            // stack store: base is rsp, or target lands in the +/-2MB window around the trapped rsp (a thread stack).
            if(r2==4 || (tgt>rsp-0x40000ULL && tgt<rsp+0x200000ULL)) stk[i]=1;
            // camera store: target is the locked instance (+/-4KB) or anywhere in the densest view pool.
            if(want && tgt>=want-0x1000ULL && tgt<want+0x1000ULL) cam[i]=1;
            if(g_poolBase && g_poolEnd>g_poolBase && tgt>=g_poolBase && tgt<g_poolEnd) cam[i]=1;
            if(cam[i]) stk[i]=0;   // a real camera target overrides any coincidental stack-window flag
        } }
#endif
    for(int a=0;a<g_writerN;a++) for(int b=a+1;b<g_writerN;b++){
        int sa=sysW(a), sb=sysW(b); bool swap;
        if(sa!=sb)               swap=(sb<sa);                       // non-system first
        else if(cam[a]!=cam[b])  swap=(cam[b]>cam[a]);              // then: writes the actual camera/pool first
        else if(stk[a]!=stk[b])  swap=(stk[b]<stk[a]);              // then: NON-stack (real memory) first
        else if(dec[a]!=dec[b])  swap=(dec[b]>dec[a]);              // then: decodable (usable locator) first
        else                     swap=(g_writerHits[b]>g_writerHits[a]); // then: most hits
        if(swap){
        uintptr_t tw=g_writers[a]; g_writers[a]=g_writers[b]; g_writers[b]=tw; int th=g_writerHits[a]; g_writerHits[a]=g_writerHits[b]; g_writerHits[b]=th;
        CONTEXT tc=g_writerCtx[a]; g_writerCtx[a]=g_writerCtx[b]; g_writerCtx[b]=tc;
        uintptr_t tww=g_writerWatch[a]; g_writerWatch[a]=g_writerWatch[b]; g_writerWatch[b]=tww;
        int td=dec[a]; dec[a]=dec[b]; dec[b]=td; int ts=stk[a]; stk[a]=stk[b]; stk[b]=ts; int tcm=cam[a]; cam[a]=cam[b]; cam[b]=tcm; } }
    { char m0[80]; uintptr_t o0; moduleOf((void*)g_writers[0],m0,sizeof(m0),o0);
      const char* why = isSystemModule(m0)?" - only system-DLL copies found, see note"
                      : cam[0]?" - writes the locked camera/pool (true camera store)"
                      : stk[0]?" - WARNING: only stack-spill writers trapped (see note)"
                      : dec[0]?" - clean-decoding game store preferred":" - game-module writer preferred";
      if(g_writerN>1) Log("# write-watch caught %d distinct writers; using %s+0x%llX (%d hits%s) as the camera update.",
          g_writerN,m0,(unsigned long long)o0,g_writerHits[0], why);
      if(isSystemModule(m0)) Log("#   NOTE: every writer was in a Windows system DLL (the camera is bulk-copied via memcpy). Hooking this is too generic; drive the CPU-struct address directly, or find the GAME caller via a return-address walk.");
      bool anyCam=false; for(int i=0;i<g_writerN;i++) if(cam[i]) anyCam=true;
      if(!anyCam && stk[0] && !isSystemModule(m0)){
          Log("#   NOTE: every trapped writer stored into the STACK (register spills inside the camera-builder, e.g.");
          Log("#         movaps [rsp+disp]), NOT the camera struct - these are NOT a usable locator. The camera is");
          Log("#         written elsewhere this frame. Two good moves: (a) re-run keeping the view in CONTINUOUS");
          Log("#         motion through the WHOLE write-watch so a real camera store stays live to trap; or (b)");
          Log("#         trampoline-hook the camera-builder FUNCTION entry below and add the pose to the matrix it");
          Log("#         produces (recover the camera base from the struct it copies into, not from rsp).");
          g_writerStackOnly=1; } }
#ifdef _WIN64
    // Common engine pattern: a 4x4 camera is written ROW-BY-ROW by SEVERAL stores inside ONE function (observed
    // across many engines). If multiple distinct trapped writers resolve to the same function, that is a strong
    // "this IS the camera-matrix updater" signal - and confirms the FN-HOOK is the right trampoline target.
    if(g_writerN>1){ uintptr_t f0=findFunctionEntry(g_writers[0]); int same=0;
        if(f0){ for(int i=0;i<g_writerN;i++){ uintptr_t fi=findFunctionEntry(g_writers[i]); if(fi==f0) same++; }
            if(same>=2){ char fm[80]; uintptr_t fo; moduleOf((void*)f0,fm,sizeof(fm),fo);
                Log("# >>> %d of the writers live in the SAME function %s+0x%llX - the camera matrix is written row-by-row there.",same,fm,(unsigned long long)fo);
                Log("#     This is a high-confidence camera-updater: trampoline-hook that ONE function to drive all rows at once. <<<"); } } }
#endif
    g_candN=0;
    for(int i=0;i<g_writerN && i<5;i++){ uintptr_t rip=g_writers[i]; char mod[80]; uintptr_t off; moduleOf((void*)rip,mod,sizeof(mod),off);
        WCand& C=g_cand[i]; memset(&C,0,sizeof(C)); strncpy(C.mod,mod,sizeof(C.mod)-1); C.off=off; C.hits=g_writerHits[i]; C.reg=-1; C.sys=isSystemModule(mod)?1:0; g_candN=i+1;
        uint8_t* s=(uint8_t*)(rip-16);
        if(!Readable(s,32)){ Log("# CANDIDATE[%d] WRITER @ %s+0x%llX (bytes unreadable)",i,mod,(unsigned long long)off); continue; }
        char hx[128]; int n=0; for(int j=0;j<24 && n<(int)sizeof(hx)-4;j++) n+=snprintf(hx+n,sizeof(hx)-n,"%02X ",s[j]);
        Log("# CANDIDATE[%d] WRITER @ %s+0x%llX  (%d hits; %s%s)",i,mod,(unsigned long long)off,g_writerHits[i],g_pgMode?"page-guard traps AT the store":"data-BP traps after the store",C.sys?"; SYSTEM-DLL (memcpy - low confidence)":"");
        Log("# CANDIDATE[%d] WRITE_AOB bytes[rip-16..+8]: %s",i,hx);
        bool decoded=false;
#ifdef _WIN64
        { int reg2=-1,idxReg=-1,scale=1,vb=0,cstart=-1,ilen=0; long ld=0; const char* cmn=nullptr;
          // HW data-BP: RIP is AFTER the store -> scan the 24 bytes ending at RIP. PAGE-GUARD: RIP is AT the store
          // (faulted before executing) -> scan forward from RIP. Same decoder, different window.
          uint8_t* w=g_pgMode?(uint8_t*)rip:(uint8_t*)(rip-24); int wn=24;
          uint64_t want=g_writerWatch[i]?(uint64_t)g_writerWatch[i]:(uint64_t)g_watchAddr;
          if(want && Readable(w,wn) && decodeStoreCtx(w,wn,&g_writerCtx[i],want,reg2,idxReg,scale,ld,cmn,cstart,vb,ilen) && reg2>=0){
              decoded=true; uintptr_t storeAddr=(uintptr_t)(w+cstart); int slen=ilen; if(slen<1||slen>24) slen=4;
              uintptr_t fnDelta=(uintptr_t)((uint8_t*)rip-(uint8_t*)storeAddr);   // 0 for page-guard, =store length for HW
              char idxs[48]={0}; if(idxReg>=0) snprintf(idxs,sizeof(idxs)," + %s*%d",regName64(idxReg),scale);
              Log("# CANDIDATE[%d] WRITE_SITE decoded (reg-context, EXACT match): %s [%s%s %+ld]  writes %d bytes",i,cmn,regName64(reg2),idxs,ld,vb);
              Log("#   -> camera base register = %s   field offset = 0x%lX%s",regName64(reg2),ld<0?-ld:ld, idxReg>=0?"   (base+index)":"");
              char aob[160]; int an=0; an+=snprintf(aob+an,sizeof(aob)-an,"| ");
              for(int k=0;k<slen && an<(int)sizeof(aob)-4;k++) an+=snprintf(aob+an,sizeof(aob)-an,"%02X ",w[cstart+k]);
              Log("# CANDIDATE[%d] WRITE_AOB_MASKED (| = hook point): %s",i,aob);
              C.decoded=1; strncpy(C.mnem,cmn,sizeof(C.mnem)-1); C.reg=reg2; C.disp=ld; C.sz=vb; strncpy(C.masked,aob,sizeof(C.masked)-1);
              // detect the matrix run for THIS candidate (informational for all; drives reclassify for the primary)
              { int lowDisp=0,nst=0; uint8_t wide[112]; int wlen=0; char form[80]={0};
                if(Readable((void*)(rip-56),112)){ memcpy(wide,(void*)(rip-56),112); wlen=112; }
                if(wlen && detectMatrixWriteRun(wide,wlen,reg2,lowDisp,nst,form,sizeof(form))){ C.matrixRun=nst; C.matLow=lowDisp; strncpy(C.form,form,sizeof(C.form)-1);
                    Log("#   CANDIDATE[%d] writes a 64-byte MATRIX -> %s ; block base %s%+d",i,form,regName64(reg2),lowDisp); } }
              if(i==0){ g_wReg=reg2; g_wDisp=(int)ld; g_wSteal=slen; strncpy(g_wMnem,cmn,sizeof(g_wMnem)-1);
                  strncpy(g_wMod,mod,sizeof(g_wMod)-1); g_wOff=off-fnDelta; strncpy(g_wMasked,aob,sizeof(g_wMasked)-1);
                  int hn=0; for(int k=0;k<slen && hn<(int)sizeof(g_wStolenHex)-4;k++) hn+=snprintf(g_wStolenHex+hn,sizeof(g_wStolenHex)-hn,"%02X ",w[cstart+k]);
                  { char sig[320]; int uq=0; int sl=buildStrongAOB(storeAddr,mod,sig,sizeof(sig),uq);   // corpus-grade: context + wildcards + uniqueness
                    if(sl){ strncpy(g_wStrong,sig,sizeof(g_wStrong)-1); g_wStrongUniq=uq;
                        Log("# STRONG_AOB (%d bytes, %s): %s",sl, uq?"VERIFIED UNIQUE in module":"NOT unique - needs manual extension", sig); } }
                  if(C.matrixRun>=3){ int lowDisp=C.matLow,nst=C.matrixRun;
                      uintptr_t blockBase=(uintptr_t)((int64_t)ctxReg(&g_writerCtx[i],reg2)+(int64_t)lowDisp);
                      bool isMat=false; if(Readable((void*)blockBase,64)){ Entry e; memcpy(e.m,(void*)blockBase,64); classifyInto(e); isMat=(e.kind==2); g_reprMatRow=e.rowMaj; }
                      Log("# >>> PRIMARY writes a 64-byte MATRIX (%s) -> RECLASSIFYING camera as 4x4 matrix at %s%+d.%s",
                          C.form[0]?C.form:"SSE store run",regName64(reg2),lowDisp, isMat?" Verifies as a rotation matrix.":" (treat as transform; check major.)");
                      g_reprMatOff=0; g_reprEulerOff=-1; g_reprPosOff=-1; g_reprPackedOff=-1;
                      snprintf(g_reprKind,sizeof(g_reprKind),"matrix4x4 @block (%s,%s)",g_reprMatRow?"row":"col",isMat?"verified":"writer-inferred");
                      g_wDisp=lowDisp; g_writerSaysMatrix=true; g_writerMatrixDisp=lowDisp; g_writerMatrixVerified=isMat;
                  } } } }
#endif
        int reg,disp,start; const char* mn;
        if(!decoded && decodeStore(s,16,reg,disp,mn,start)){
            int steal=16-start;                                                 // bytes from the store start through rip
            Log("# CANDIDATE[%d] WRITE_SITE decoded: %s [%s%+d]  base=%s field=0x%X steal>=%d",
                i,mn,regName(reg),disp,regName(reg),disp<0?-disp:disp,steal);
            char aob[160]; int an=0; an+=snprintf(aob+an,sizeof(aob)-an,"| ");
            for(int k=start;k<16 && an<(int)sizeof(aob)-4;k++) an+=snprintf(aob+an,sizeof(aob)-an,"%02X ",s[k]);
            Log("# CANDIDATE[%d] WRITE_AOB (| = hook point): %s",i,aob);
            C.decoded=1; strncpy(C.mnem,mn,sizeof(C.mnem)-1); C.reg=reg; C.disp=disp; strncpy(C.masked,aob,sizeof(C.masked)-1);
            if(i==0){ g_wReg=reg; g_wDisp=disp; g_wSteal=steal; strncpy(g_wMnem,mn,sizeof(g_wMnem)-1); strncpy(g_wMod,mod,sizeof(g_wMod)-1); g_wOff=off-start; strncpy(g_wMasked,aob,sizeof(g_wMasked)-1);
                int hn=0; for(int k=start;k<16 && hn<(int)sizeof(g_wStolenHex)-4;k++) hn+=snprintf(g_wStolenHex+hn,sizeof(g_wStolenHex)-hn,"%02X ",s[k]);
                { char sig[320]; int uq=0; int sl=buildStrongAOB((uintptr_t)(s+start),mod,sig,sizeof(sig),uq);
                  if(sl){ strncpy(g_wStrong,sig,sizeof(g_wStrong)-1); g_wStrongUniq=uq;
                      Log("# STRONG_AOB (%d bytes, %s): %s",sl,uq?"VERIFIED UNIQUE in module":"not unique - extend manually",sig); } } }
        } else if(!decoded){ Log("# CANDIDATE[%d] WRITE_SITE: couldn't auto-decode (SIB/atypical); raw AOB above.",i);
            { char raw[176]; int rn=0; for(int k=0;k<24 && rn<(int)sizeof(raw)-4;k++) rn+=snprintf(raw+rn,sizeof(raw)-rn,"%02X ",s[k]); strncpy(C.masked,raw,sizeof(C.masked)-1); }
            if(i==0 && !g_wMasked[0]){     // decode failed, but DON'T lose the signature: ship the raw bytes so the
                strncpy(g_wMod,mod,sizeof(g_wMod)-1); g_wOff=off;   // site is locatable (register/offset stay unknown -> needs CE/manual finish)
                char raw[160]; int rn=0; for(int k=0;k<24 && rn<(int)sizeof(raw)-4;k++) rn+=snprintf(raw+rn,sizeof(raw)-rn,"%02X ",s[k]);
                strncpy(g_wMasked,raw,sizeof(g_wMasked)-1); strncpy(g_wStolenHex,raw,sizeof(g_wStolenHex)-1);
                Log("# CANDIDATE[0] WRITE_AOB (raw, undecoded) saved - capture register unknown.");
            }
        }
        // confidence (0-100): decoded clean store + matrix run + game module + hits share
        C.conf = (C.decoded?50:15) + (C.matrixRun>=3?25:0) + (C.sys?0:15) + (g_writerHits[0]>0? (10*C.hits/(g_writerHits[0]+1)) : 0);
        if(C.conf>100)C.conf=100;
        if(i==0) emitFunctionHook(rip);   // resolve the containing function -> trampoline-hook recipe (robust vs transient buffers)
#ifdef _WIN64
        // If this writer is a generic system-DLL copy (memcpy), walk the stack to the GAME function that called it -
        // that caller is the real, hookable camera code. Surface it for the primary candidate.
        if(i==0 && C.sys){ uintptr_t caller=findGameCaller(&g_writerCtx[i]);
            if(caller){ char cm[80]; uintptr_t co; moduleOf((void*)caller,cm,sizeof(cm),co);
                uintptr_t cfn=findFunctionEntry(caller); char fm[80]; uintptr_t fo=0; if(cfn) moduleOf((void*)cfn,fm,sizeof(fm),fo);
                Log("# STACK-WALK: the memcpy was called by GAME code at %s+0x%llX (function %s+0x%llX) - hook THAT instead.",
                    cm,(unsigned long long)co, cfn?fm:cm,(unsigned long long)(cfn?fo:co));
                if(!g_fnAOB[0] && cfn){ g_fnOff=fo; strncpy(g_fnMod,fm,sizeof(g_fnMod)-1);   // promote the game caller as the FN-HOOK
                    uint8_t* fb=(uint8_t*)cfn; if(Readable(fb,20)){ int an=0; for(int k=0;k<20 && an<(int)sizeof(g_fnAOB)-4;k++) an+=snprintf(g_fnAOB+an,sizeof(g_fnAOB)-an,"%02X ",fb[k]); } }
            } else Log("# STACK-WALK: couldn't resolve a game caller above the memcpy this pass (try again while moving)."); }
#endif
    }
    Log("# ===== DIFFERENTIAL MOST-LIKELY AOB (top-ranked of %d candidates) =====",g_candN);
    if(g_candN>0) Log("#   -> %s+0x%llX  %s  conf=%d%%  AOB: %s",g_cand[0].mod,g_cand[0].off,
        g_cand[0].matrixRun>=3?"[4x4 MATRIX writer]":g_cand[0].decoded?"[clean store]":"[undecoded - needs manual finish]",g_cand[0].conf,g_cand[0].masked[0]?g_cand[0].masked:"?");
    { // accurate capture-QUALITY verdict (no false "complete" - state exactly what we got)
      bool gameMod=(g_candN>0&&!g_cand[0].sys), dec=(g_candN>0&&g_cand[0].decoded), mat=(g_candN>0&&g_cand[0].matrixRun>=3);
      const char* grade = (dec&&gameMod&&g_wStrongUniq)?"STRONG (game-module, decoded, unique signature)"
                        : (dec&&gameMod)?"GOOD (game-module, decoded; signature not provably unique)"
                        : (dec)?"FAIR (decoded but in a shared/system module - check the stack-walk caller)"
                        : "WEAK (writer not auto-decoded - raw AOB only; finish in Cheat Engine)";
      Log("# CAPTURE QUALITY: %s  | strong_aob=%s | hook=%s | representation=%s",
          grade, g_wStrongUniq?"unique":(g_wStrong[0]?"non-unique":"none"), g_fnMod[0]?"function-hook ready":"address-only",
          mat?"4x4 matrix":(g_reprEulerOff>=0?"euler":g_reprQuatOff>=0?"quaternion":g_reprMatOff>=0?"matrix":"see field map")); }
    emitFieldMap();              // EXPORT: struct field offsets the probe detected
    emitCheatEngineTemplate();   // EXPORT: paste-ready Cheat Engine AOB-injection script from this capture
    if(g_wReg>=0 || g_wMasked[0]) notifyExtractionDone();   // usable AOB captured (auto OR manual motion) -> sound + stop
}
// arm the data breakpoint, wait (early-out when a writer is caught), disarm, then emit the write-site + function-hook.
// PAGE-GUARD access-violation handler: on a WRITE into our protected camera window, record the writer (RIP +
// full register context + exact fault address), then open the WHOLE window writable and let the store re-run.
// No single-step / per-page re-protect, so there's no teardown race that could crash the game.
static LONG CALLBACK pgVeh(EXCEPTION_POINTERS* ep){
    if(ep->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && g_pgActive && ep->ExceptionRecord->NumberParameters>=2){
        ULONG_PTR rw=ep->ExceptionRecord->ExceptionInformation[0];
        uintptr_t fault=(uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        if(rw==1 && fault>=g_pgWinBase && fault<g_pgWinEnd){                 // a WRITE to one of our protected pages
            if(fault>=g_pgBase && fault<g_pgEnd){                            // ...landing on the camera -> record the writer
                uintptr_t rip=(uintptr_t)
#ifdef _WIN64
                    ep->ContextRecord->Rip;
#else
                    ep->ContextRecord->Eip;
#endif
                int idx=-1; for(int i=0;i<g_pgWriterN;i++) if(g_pgWriters[i]==rip){idx=i;break;}
                if(isOwnRip(rip)){ /* probe's own allocator/bookkeeping touched the page - ignore, but still open the window below */ }
                else if(idx>=0) g_pgHits[idx]++;
                else if(g_pgWriterN<16){ g_pgWriters[g_pgWriterN]=rip; g_pgHits[g_pgWriterN]=1; g_pgCtx[g_pgWriterN]=*ep->ContextRecord; g_pgFault[g_pgWriterN]=fault; g_pgWriterN++; g_pgGot=1; }
                if(idx>=0&&!isOwnRip(rip)) g_pgGot=1; }
            DWORD old; VirtualProtect((void*)g_pgWinBase,(SIZE_T)(g_pgWinEnd-g_pgWinBase),PAGE_READWRITE,&old);  // open window; store re-runs & succeeds
            return EXCEPTION_CONTINUE_EXECUTION; }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
// Region-wide writer capture for transient/pooled cameras. Protects a small (<=32KB) page-aligned window of the
// camera's PRIVATE heap region read-only and traps whoever writes the camera, regardless of which pooled instance
// is currently live - the key to reaching the AOB when a fixed address won't stay valid.
static void pageGuardCapture(uintptr_t addr,int ms){
    // v5.11: guard up to TWO regions - the locked camera's neighbourhood AND the densest view-matrix POOL - and for
    // a large pool SLIDE a 256KB window across the whole region so the writer is trapped wherever in the ring it
    // currently writes (Alan Wake's pool is ~2.3MB / thousands of copies, far from the address the differential locked).
    struct Rgn{ uintptr_t fullB,fullE,camB,camE; }; Rgn rg[2]; int nrg=0;
    auto addRegion=[&](uintptr_t a, bool wholeRegion){
        if(nrg>=2) return; MEMORY_BASIC_INFORMATION mb; if(!VirtualQuery((void*)a,&mb,sizeof(mb))) return;
        bool wr=(mb.Protect&(PAGE_READWRITE|PAGE_WRITECOPY))!=0;
        bool ex=(mb.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
        if(mb.State!=MEM_COMMIT||mb.Type!=MEM_PRIVATE||!wr||ex||(mb.Protect&PAGE_GUARD)) return;
        uintptr_t rb=(uintptr_t)mb.BaseAddress, re=rb+mb.RegionSize, fb,fe,cB,cE;
        if(wholeRegion){ fb=rb; fe=re; if(fe-fb>0x800000) fe=fb+0x800000;  // sweep the whole pool, capped 8MB
            cB=fb; cE=fe; }
        else { cB=(a>0x800?a-0x800:a); cE=a+0x840; if(cB<rb)cB=rb; if(cE>re)cE=re;
            fb=cB&~0xFFFull; fe=(cE+0xFFF)&~0xFFFull; if(fb<rb)fb=rb; if(fe>re)fe=re; }    // small region: extent == window
        fb&=~0xFFFull; fe=(fe+0xFFF)&~0xFFFull;
        if(fe-fb<0x1000) return;
        for(int i=0;i<nrg;i++) if(rg[i].fullB==fb) return;
        rg[nrg++]={fb,fe,cB,cE};
    };
    addRegion(addr,false);                                                   // region 0: around the locked camera address
    if(g_poolBase && g_poolEnd>g_poolBase && !(addr>=g_poolBase && addr<g_poolEnd)) addRegion(g_poolBase,true);  // region 1: the densest view pool
    if(nrg==0){ Log("# PAGE-GUARD: skipped (camera not in a plain private RW heap region - unsafe to guard)."); return; }
    g_pgWriterN=0; memset(g_pgHits,0,sizeof(g_pgHits)); g_pgMode=true;
    g_pgVeh=AddVectoredExceptionHandler(1,pgVeh);
    Log("# PAGE-GUARD fallback: HW watch found nothing (transient/pooled camera). Guarding %d region(s)%s, sliding a",nrg,nrg>1?" (locked camera + view pool)":"");
    Log("#   256KB window across each. KEEP THE CAMERA MOVING; a brief micro-stutter is normal...");
    const uintptr_t WIN=0x40000;                                             // 256KB sliding window
    int basePer = ms/((nrg>1?nrg:1)*400); if(basePer<3)basePer=3; if(basePer>8)basePer=8;
    for(int ri=0; ri<nrg && g_pgWriterN<6; ri++){
        uintptr_t fb=rg[ri].fullB, fe=rg[ri].fullE; size_t ext=fe-fb;
        int slots=(int)((ext+WIN-1)/WIN); if(slots<1)slots=1;
        int sweeps = (slots>1) ? (slots<10?slots+2:12) : basePer;            // big pool: visit every slice; small region: a few rounds
        g_pgBase=rg[ri].camB; g_pgEnd=rg[ri].camE;
        for(int r=0;r<sweeps && g_pgWriterN<6;r++){
            uintptr_t wB=fb+(uintptr_t)(r%slots)*WIN, wE=wB+WIN; if(wE>fe)wE=fe; size_t span=wE-wB; if(span<0x1000) continue;
            g_pgWinBase=wB; g_pgWinEnd=wE;
            g_pgGot=0; DWORD old; g_pgActive=true;                           // arm the handler BEFORE pages go read-only
            if(!VirtualProtect((void*)wB,span,PAGE_READONLY,&old)){ g_pgActive=false; Log("# PAGE-GUARD: VirtualProtect(RO) failed on a slice - skipping it."); continue; }
            for(int w=0;w<8 && !g_pgGot;w++){ if(g_autoDriveMouse) autoOscillate(50); else Sleep(50); }
            VirtualProtect((void*)wB,span,PAGE_READWRITE,&old);              // restore RW FIRST
            g_pgActive=false; Sleep(15);
        }
    }
    if(g_pgVeh){ RemoveVectoredExceptionHandler(g_pgVeh); g_pgVeh=nullptr; }
    int n=g_pgWriterN; if(n>16)n=16;
    for(int i=0;i<n;i++){ g_writers[i]=g_pgWriters[i]; g_writerHits[i]=g_pgHits[i]; g_writerCtx[i]=g_pgCtx[i]; g_writerWatch[i]=g_pgFault[i]; }
    g_writerN=n; g_watchAddr=addr;
    if(n) Log("# PAGE-GUARD caught %d distinct writer(s) in the pool window(s) -> decoding the AOB.",n);
    else  Log("# PAGE-GUARD caught nothing either - the camera wasn't written in-window (move more / try INSERT again).");
}
// Multi-address write-watch. With >1 candidate, spreads the 4 HW watchpoints across distinct camera COPIES (so a
// pooled camera is caught whichever instance is live); with 1 candidate, watches its 4 matrix rows. If the HW
// breakpoints trap nothing (the address went stale), it falls back to the region-wide PAGE-GUARD capture.
static void armWriteWatchN(uintptr_t* cands,int nCands,bool isMatrix,int maxSlices,const char* prompt){
    uintptr_t addrs[4]; int na=0;
    if(nCands<=1){ uintptr_t base=(cands[0])&~3ull;
        if(isMatrix){ addrs[0]=base; addrs[1]=base+0x10; addrs[2]=base+0x20; addrs[3]=base+0x30; }
        else        { addrs[0]=base; addrs[1]=base+4;    addrs[2]=base+8;    addrs[3]=base+12;   }
        na=4; }
    else { for(int i=0;i<nCands && na<4;i++){ uintptr_t b=cands[i]&~3ull; bool dup=false; for(int j=0;j<na;j++) if(addrs[j]==b)dup=true; if(!dup) addrs[na++]=b; } }
    g_writerN=0; memset((void*)g_writerHits,0,sizeof(g_writerHits)); memset((void*)g_writerWatch,0,sizeof(g_writerWatch));
    g_pgMode=false;   // HW data-BP path: RIP traps AFTER the store
    g_watchAddr=addrs[0]; g_veh=AddVectoredExceptionHandler(1,veh); setBP(addrs,na,false);
    Log("%s",prompt);
    if(nCands>1) Log("#   watching %d distinct camera copies at once (pooled/transient-camera coverage).",na);
    int secs=(maxSlices+1)/2; if(secs<1)secs=1; int elapsed=0;
    for(int t=secs;t>0;t--){ Log("#   write-watch: %s - %d s left (distinct writers so far: %d)",
            g_autoDriveMouse?"AUTO-MOUSE moving the camera (you can also move manually)":"MOVE THE VIEW (mouse or WASD) NOW",t,g_writerN);
        autoOscillate(1000); elapsed++;
        logManualInput();   // recognise + log the player's own mouse/WASD; their motion drives the watch too
        // AGGRESSIVE: if the player is actively moving but we haven't trapped a clean writer yet, keep watching longer.
        if(g_manMouseTotal>200 && g_writerN<2 && t<=1) { t=2; g_manMouseTotal=0; Log("#   (player still moving + few writers - extending the watch)"); }
        if(g_writerN==0 && elapsed>=6 && secs>8){ Log("#   write-watch: nothing trapped after %ds on these fixed addresses (transient camera).",elapsed); break; } }
    setBP(addrs,na,true); if(g_veh){ RemoveVectoredExceptionHandler(g_veh); g_veh=nullptr; }
    if(g_writerN==0) pageGuardCapture(cands[0], 6000);   // fixed addresses went stale -> region-wide fallback (kept separate so it can't clobber a good HW capture)
    emitWriteAOB();
}
static void armWriteWatch(void* addr,int maxSlices,const char* prompt,bool isMatrix){
    uintptr_t a=(uintptr_t)addr; armWriteWatchN(&a,1,isMatrix,maxSlices,prompt);   // single-address wrapper (watches the 4 rows)
}
// capture the FOV writer separately (FOV control is a universal camera-tool feature; it's a distinct movss store).
// Doesn't touch the camera-write turnkey globals - just reports the FOV write-site so a mod can drive FOV too.
// ACTIVE FOV HUNT: inject a zoom/ADS (right mouse) and find which nearby float moves into/within FOV range - that's
// the FOV. Far more reliable than a static struct scan (which can't tell FOV from any other angle-ish float).
struct Entry; static bool pickBestProj(Entry& out); static float readLiveFovV();   // defined later (projection oracle)

// ENCODING SOLVER (v5.8): given a struct float v and the projection's true vertical FOV in degrees, decide which
// encoding maps v -> projDeg. m11 = cot(fovy/2) is the matrix element, so a field can store the angle in degrees,
// radians, tan(fovy/2), cot(fovy/2) (the element itself), or as a multiplier of a fixed base angle.
static bool fovEncMatch(float v,float projDeg,const char** enc,float* base){
    if(fabsf(v-projDeg)<2.5f){ *enc="degrees"; return true; }
    if(v>0.3f&&v<3.2f&&fabsf(v*57.2957795f-projDeg)<3.f){ *enc="radians"; return true; }
    if(v>0.05f&&v<12.f&&fabsf(2.f*atanf(v)*57.2957795f-projDeg)<3.f){ *enc="tan_half"; return true; }
    if(v>0.05f&&v<12.f&&fabsf(2.f*atanf(1.f/v)*57.2957795f-projDeg)<3.f){ *enc="cot_half"; return true; }
    if(v>0.4f&&v<1.8f){ float b=projDeg/v; if(b>50.f&&b<130.f){ *enc="factor_of_base"; *base=b; return true; } }
    return false;
}
// PROJECTION CROSS-CHECK (v5.8): the GPU projection gives the EXACT current FOV as ground truth. A CPU float that
// maps to it under a known encoding IS the FOV field - the strongest possible confirmation - and it simultaneously
// solves the encoding and whether the field is horizontal or vertical. If a projection exists but NO float maps to
// it, FOV is baked into the projection (no CPU scalar) and we say so, so the runtime scales m00/m11 instead.
static void crossCheckFov(uintptr_t base){
    Entry pj; if(!pickBestProj(pj)||!base) return;
    float vy=pj.fovY, vx=pj.fovX; if(vy<10.f||vy>170.f) return; g_reprFovProofDeg=vy;
    int best=-1; float bestVal=0; const char* bestEnc="unknown"; float bestBase=0; bool horiz=false;
    for(int o=0;o<0x800 && best<0;o+=4){ if(!Readable((void*)(base+o),4))continue; float v=*(float*)(base+o); if(v!=v) continue;
        const char* enc="unknown"; float bb=0;
        if(fovEncMatch(v,vy,&enc,&bb)){ best=o; bestVal=v; bestEnc=enc; bestBase=bb; horiz=false; break; }
        if(vx>10.f&&vx<170.f&&fovEncMatch(v,vx,&enc,&bb)){ best=o; bestVal=v; bestEnc=enc; bestBase=bb; horiz=true; break; }   // game stores HFOV
    }
    if(best>=0){
        g_reprFovOff=best; g_reprFovVal=bestVal; strncpy(g_reprFovEnc,bestEnc,sizeof(g_reprFovEnc)-1); g_reprFovBase=bestBase; g_reprFovHoriz=horiz;
        Log("# FOV CROSS-CHECK: struct float @ +0x%X (=%.4f) matches projection %s %.2f deg, encoding '%s'%s -> ENCODING SOLVED, high confidence.",
            best,bestVal,horiz?"hfov":"vfov",horiz?vx:vy,bestEnc, bestBase>0?" (base angle computed)":"");
    } else {
        g_reprFovProjOnly=true; strncpy(g_reprFovEnc,"projection_only",sizeof(g_reprFovEnc)-1);
        Log("# FOV CROSS-CHECK: projection vfov=%.2f deg but NO CPU float maps to it -> FOV is PROJECTION-ONLY (baked into the matrix).",vy);
        Log("#   Runtime should widen FOV by scaling the projection's m00/m11 (GPU CB or its writer), not a struct field.");
    }
}
static void correlateFov(uintptr_t base){
    if(!g_autoDriveMouse||!gameIsForeground()||!base) return;
    float projBefore=readLiveFovV();   // v5.8: capture projection vfov for the two-sample encoding solve
    struct F{int o; float v;}; std::vector<F> snap;
    for(int o=0;o<0x800;o+=4){ if(!Readable((void*)(base+o),4))continue; float v=*(float*)(base+o); if(v==v&&fabsf(v)<2000.f){ F f; f.o=o; f.v=v; snap.push_back(f);} if(snap.size()>2000)break; }
    if(snap.empty()) return;
    Log("# FOV HUNT: injecting zoom (right-mouse) to see which float is the FOV...");
    INPUT in; memset(&in,0,sizeof(in)); in.type=INPUT_MOUSE; in.mi.dwFlags=MOUSEEVENTF_RIGHTDOWN; SendInput(1,&in,sizeof(in));
    for(int i=0;i<36 && !g_extractDone;i++) Sleep(15);
    int best=-1; float bestD=0.03f, bestVal=0;
    for(auto& f:snap){ if(!Readable((void*)(base+f.o),4))continue; float v=*(float*)(base+f.o); float d=fabsf(v-f.v);
        if(d>bestD && isFovFloat(v) && isFovFloat(f.v)){ bestD=d; best=f.o; bestVal=v; } }   // changed AND stays FOV-shaped
    float projAfter=readLiveFovV();   // v5.8: projection vfov at the second sample
    memset(&in,0,sizeof(in)); in.type=INPUT_MOUSE; in.mi.dwFlags=MOUSEEVENTF_RIGHTUP; SendInput(1,&in,sizeof(in));
    if(best>=0){ g_reprFovOff=best; g_reprFovVal=bestVal;
        // TWO-SAMPLE ENCODING SOLVE: the field and the projection moved together; classify the post value against
        // the projection's true post-zoom vfov. Requires the projection to have actually changed (real zoom).
        const char* enc="unknown"; float bb=0;
        if(projAfter>10.f && projBefore>10.f && fabsf(projAfter-projBefore)>0.5f && fovEncMatch(bestVal,projAfter,&enc,&bb)){
            strncpy(g_reprFovEnc,enc,sizeof(g_reprFovEnc)-1); g_reprFovBase=bb; g_reprFovProofDeg=projAfter;
            Log("# FOV CONFIRMED by zoom @ +0x%X (value %.4f, moved %.4f) - projection vfov %.2f->%.2f - ENCODING SOLVED: '%s'.",
                best,bestVal,bestD,projBefore,projAfter,enc);
        } else
            Log("# FOV CONFIRMED by zoom @ +0x%X (value %.4f, moved %.4f on ADS) - active-verified (encoding inferred from value).",best,bestVal,bestD);
    }
    else Log("# FOV HUNT: no float responded to zoom (game may not zoom on right-click); keeping the struct-scan candidate.");
}
static void captureFovWriter(uintptr_t fovAddr){
    if(!fovAddr||!Readable((void*)fovAddr,4)) return;
    g_writerN=0; memset((void*)g_writerHits,0,sizeof(g_writerHits)); memset((void*)g_writerWatch,0,sizeof(g_writerWatch));
    g_pgMode=false; g_watchAddr=fovAddr; g_veh=AddVectoredExceptionHandler(1,veh); setBP1((void*)fovAddr,false);
    Log("# FOV write-watch armed (~12s, aggressive) - ZOOM/AIM repeatedly to change FOV; also keep the view live...");
    for(int t=0;t<24 && g_writerN==0;t++){ if(g_autoDriveMouse && (t&3)==0){ INPUT in; memset(&in,0,sizeof(in)); in.type=INPUT_MOUSE;
            in.mi.dwFlags=MOUSEEVENTF_RIGHTDOWN; SendInput(1,&in,sizeof(in)); Sleep(120); in.mi.dwFlags=MOUSEEVENTF_RIGHTUP; SendInput(1,&in,sizeof(in)); }
        autoOscillate(380); }
    setBP1((void*)fovAddr,true); if(g_veh){ RemoveVectoredExceptionHandler(g_veh); g_veh=nullptr; }
    if(g_writerN==0){ Log("# FOV-WRITE: HW watch caught nothing - trying a page-guard sweep of the FOV page...");
        pageGuardCapture(fovAddr,3000); }
    if(g_writerN==0){ Log("# FOV-WRITE: not captured (FOV constant, or written via an unhooked path)."); return; }
    // rank: prefer non-system + decodable, same as the camera writer
    for(int a=0;a<g_writerN;a++) for(int b=a+1;b<g_writerN;b++){ char ma[80],mb[80]; uintptr_t oa,ob; moduleOf((void*)g_writers[a],ma,sizeof(ma),oa); moduleOf((void*)g_writers[b],mb,sizeof(mb),ob);
        int sa=isSystemModule(ma)?1:0, sb=isSystemModule(mb)?1:0; if((sb<sa)||(sb==sa&&g_writerHits[b]>g_writerHits[a])){
            uintptr_t tw=g_writers[a];g_writers[a]=g_writers[b];g_writers[b]=tw; int th=g_writerHits[a];g_writerHits[a]=g_writerHits[b];g_writerHits[b]=th;
            CONTEXT tc=g_writerCtx[a];g_writerCtx[a]=g_writerCtx[b];g_writerCtx[b]=tc; uintptr_t tww=g_writerWatch[a];g_writerWatch[a]=g_writerWatch[b];g_writerWatch[b]=tww; } }
    uintptr_t rip=g_writers[0]; char mod[80]; uintptr_t off; moduleOf((void*)rip,mod,sizeof(mod),off);
    uint8_t* s=(uint8_t*)(rip-16); if(!Readable(s,32)){ Log("# FOV-WRITE @ %s+0x%llX (bytes unreadable)",mod,(unsigned long long)off); return; }
    bool done=false;
#ifdef _WIN64
    { int r2=-1,ir=-1,sc=1,vb=0,cs=-1,il=0; long ld=0; const char* cm=nullptr; uint8_t* w=g_pgMode?(uint8_t*)rip:(uint8_t*)(rip-24);
      if(Readable(w,24)&&decodeStoreCtx(w,24,&g_writerCtx[0],(uint64_t)fovAddr,r2,ir,sc,ld,cm,cs,vb,il)&&r2>=0){ done=true;
          Log("# FOV-WRITE @ %s+0x%llX  %s [%s %+ld]  (hook this; write your FOV into [%s%+ld])",mod,(unsigned long long)off,cm,regName64(r2),ld,regName64(r2),ld);
          char aob[120]; int an=0; an+=snprintf(aob+an,sizeof(aob)-an,"| "); int slen=il<1||il>16?4:il;
          for(int k=0;k<slen&&an<(int)sizeof(aob)-4;k++) an+=snprintf(aob+an,sizeof(aob)-an,"%02X ",w[cs+k]); Log("# FOV-WRITE_AOB: %s",aob);
          { char sig[320]; int uq=0; int sl=buildStrongAOB((uintptr_t)(w+cs),mod,sig,sizeof(sig),uq);
            if(sl) Log("# FOV-STRONG_AOB (%d bytes, %s): %s",sl,uq?"VERIFIED UNIQUE":"not unique - extend",sig); }
          uintptr_t fn=findFunctionEntry(rip); if(fn){ char fm[80]; uintptr_t fo; moduleOf((void*)fn,fm,sizeof(fm),fo); Log("# FOV-FN-HOOK: %s+0x%llX",fm,(unsigned long long)fo); }
          if(isSystemModule(mod)){ uintptr_t gc=findGameCaller(&g_writerCtx[0]); if(gc){ char gm[80]; uintptr_t go; moduleOf((void*)gc,gm,sizeof(gm),go); Log("# FOV STACK-WALK: real game caller @ %s+0x%llX - hook there.",gm,(unsigned long long)go); } } } }
#endif
    int reg,disp,start; const char* mn;
    if(!done && decodeStore(s,16,reg,disp,mn,start)){
        char aob[160]; int an=0; an+=snprintf(aob+an,sizeof(aob)-an,"| ");
        for(int k=start;k<16 && an<(int)sizeof(aob)-4;k++) an+=snprintf(aob+an,sizeof(aob)-an,"%02X ",s[k]);
        Log("# FOV-WRITE @ %s+0x%llX  store %s [%s%+d]  (hook this to control FOV)",mod,(unsigned long long)(off-start),mn,regName(reg),disp);
        Log("# FOV-WRITE_AOB (exact store bytes): %s",aob);
        uintptr_t fn=findFunctionEntry(rip); if(fn){ char fm[80]; uintptr_t fo; moduleOf((void*)fn,fm,sizeof(fm),fo);
            Log("# FOV-FN-HOOK: %s+0x%llX  (trampoline this; after original, write your FOV into [%s%+d])",fm,(unsigned long long)fo,regName(reg),disp); }
    } else if(!done) Log("# FOV-WRITE @ %s+0x%llX (store form not auto-decoded; AOB bytes near rip apply)",mod,(unsigned long long)off);
}

// ---- spin-test: oscillate a confirmed matrix so the user can SEE if it drives the view ----
static volatile bool g_spinning=false;
static int g_lastVerified=0;   // 1 once a candidate's write is confirmed to move the rendered view
// ---- CLOSED-LOOP VERIFICATION ----------------------------------------------------------------
// The probe captures the GPU view matrix every frame. To PROVE a CPU candidate is the real camera
// (not a downstream copy), perturb it and check whether the live rendered view actually responds.
// This turns "best-ranked guess" into "I moved it and the view moved", and lets the pipeline fall
// through to the next candidate automatically when a write has no effect.
static bool readLiveView(float out[16],int& slot,uint32_t& off){
    Entry bv; if(!bestViewEntry(bv)) return false; memcpy(out,bv.m,64); slot=bv.slot; off=bv.off; return true;
}
static float matDelta(const float* a,const float* b){ float s=0; for(int i=0;i<16;i++) s+=fabsf(a[i]-b[i]); return s; }
// returns: 1 = verified (view responded), 0 = no response (downstream copy), -1 = inconclusive (no live view / not a matrix)
static int verifyByView(void* addr){
    if(!addr||!Readable(addr,64)) return -1;
    float orig[16]; memcpy(orig,addr,64);
    if(!ortho3x3(orig)||identityish(orig)) return -1;            // need a clean rotation matrix to perturb safely
    float V0[16]; int s0; uint32_t o0; if(!readLiveView(V0,s0,o0)){ Log("# verify: no live GPU view to self-observe (Vulkan/pure-CPU path) - use the spin-test + your eyes instead."); return -1; }
    Sleep(70); float Vn[16]; int sn; uint32_t on; if(!readLiveView(Vn,sn,on)) return -1;
    float noise=matDelta(V0,Vn);                                  // baseline frame-to-frame jitter without any write
    // perturb: rotate the candidate's 3x3 by a fixed yaw so the response is unambiguous
    float a=0.5f,c=cosf(a),s=sinf(a); float Ry[9]={c,0,s,0,1,0,-s,0,c};
    float R[9]={orig[0],orig[1],orig[2],orig[4],orig[5],orig[6],orig[8],orig[9],orig[10]},Rn[9];
    for(int r=0;r<3;r++)for(int k=0;k<3;k++) Rn[r*3+k]=R[r*3]*Ry[k]+R[r*3+1]*Ry[3+k]+R[r*3+2]*Ry[6+k];
    float pert[16]; memcpy(pert,orig,64);
    pert[0]=Rn[0];pert[1]=Rn[1];pert[2]=Rn[2];pert[4]=Rn[3];pert[5]=Rn[4];pert[6]=Rn[5];pert[8]=Rn[6];pert[9]=Rn[7];pert[10]=Rn[8];
    for(int rep=0;rep<6;rep++){ if(Readable(addr,64)) memcpy(addr,pert,64); Sleep(16); }   // hold a few frames so the engine uploads
    float V1[16]; int s1; uint32_t o1; bool got=readLiveView(V1,s1,o1);
    if(Readable(addr,64)) memcpy(addr,orig,64);                   // ALWAYS restore
    if(!got) return -1;
    if(s1!=s0||o1!=o0){ Log("# verify: the live view buffer changed identity mid-test - inconclusive."); return -1; }
    float resp=matDelta(V0,V1);
    bool ok = resp > (noise*5.f + 0.03f);
    if(ok) g_lastVerified=1;
    Log("# VERIFY @ %p : view-response delta=%.4f  baseline-noise=%.4f  => %s",addr,resp,noise,
        ok?"CONFIRMED (writing this moved the rendered view - it IS the camera source)":"no response (downstream copy / wrong candidate)");
    return ok?1:0;
}

static void spinTest(void* addr,int seconds){
    if(!addr||!Readable(addr,64)){ Log("# spin-test: target unreadable"); return; }
    float orig[16]; memcpy(orig,addr,64);
    if(!ortho3x3(orig)){ Log("# spin-test: target not orthonormal - skipping (not a clean rotation matrix)"); return; }
    Log("# SPIN-TEST: sweeping the camera at %p for %ds -- WATCH THE SCREEN. If the view pans, this is the real camera.",addr,seconds);
    g_spinning=true; g_spinUsed=true; DWORD t0=GetTickCount();
    while((int)((GetTickCount()-t0)/1000)<seconds){
        if(!Readable(addr,64))break; float base[16]; memcpy(base,orig,64);
        float ang=sinf((GetTickCount()-t0)*0.0016f)*0.6f, c=cosf(ang),s=sinf(ang);
        float Ry[9]={c,0,s,0,1,0,-s,0,c}, R[9]={base[0],base[1],base[2],base[4],base[5],base[6],base[8],base[9],base[10]}, Rn[9];
        for(int r=0;r<3;r++)for(int k=0;k<3;k++) Rn[r*3+k]=R[r*3]*Ry[k]+R[r*3+1]*Ry[3+k]+R[r*3+2]*Ry[6+k];
        float out[16]; memcpy(out,base,64);
        out[0]=Rn[0];out[1]=Rn[1];out[2]=Rn[2];out[4]=Rn[3];out[5]=Rn[4];out[6]=Rn[5];out[8]=Rn[6];out[9]=Rn[7];out[10]=Rn[8];
        memcpy(addr,out,64); Sleep(8);
    }
    if(Readable(addr,64)) memcpy(addr,orig,64);
    g_spinning=false; Log("# SPIN-TEST done & restored. Did the view sweep? yes => drive %p ; no => it's a downstream copy.",addr);
}

// pick the best projection (draw-weighted, screen-aspect matched) for the FOV section
static bool pickBestProj(Entry& out){
    std::vector<Entry> proj;
    { std::lock_guard<std::mutex> lk(g_catMx); for(auto&kv:g_cat){ const Entry&e=kv.second; if(e.kind==1 && g_frame-e.lastFrame<240) proj.push_back(e); } }
    if(!proj.empty()){
        std::sort(proj.begin(),proj.end(),[](const Entry&a,const Entry&b){return a.draws>b.draws;});
        float target=(g_resW&&g_resH)?(float)g_resW/(float)g_resH:0.f; const Entry* bP=nullptr;
        if(target>0){ float best=1e9f; for(auto&e:proj){ float d=fabsf(e.aspect-target); if(d<0.15f&&d<best){best=d;bP=&e;} } }
        if(!bP) bP=&proj[0]; out=*bP; return true;
    }
    // v5.11 FALLBACK: no GPU projection (D3D12/Vulkan/GL hook captured nothing). Use the screen-aspect-matched
    // projection the memory scan found in CPU memory, re-read live so the FOV is current.
    if(g_cpuProjAddr && Readable((void*)g_cpuProjAddr,64)){ Entry e; memcpy(e.m,(void*)g_cpuProjAddr,64); classifyInto(e);
        if(e.kind==1 && e.fovY>10.f && e.fovY<170.f){ out=e; return true; } g_cpuProjAddr=0; }
    return false;
}

// ---- oracles for the active hijack tests: read the live camera position / FOV ----
// Primary = the GPU view (D3D11). v5.14 FALLBACK = the differential-confirmed live camera struct in CPU memory, so
// the hijack can self-observe on D3D12/Vulkan/pure-CPU titles instead of skipping.
static bool readCpuView(float V[16]){
    uintptr_t a=g_liveCamAddr; if(!a) return false;
    if(g_liveCamMatOff>=0 && Readable((void*)(a+g_liveCamMatOff),64)){ memcpy(V,(void*)(a+g_liveCamMatOff),64); return finite16(V); }
    return false;
}
static bool readLiveCamPos(float out[3]){
    float V[16]; int s; uint32_t o; if(readLiveView(V,s,o)){ cameraPos(V,out); return true; }
    if(readCpuView(V)){ cameraPos(V,out); return true; }                         // matrix camera in CPU memory
    if(g_liveCamAddr && g_reprPosOff>=0 && Readable((void*)(g_liveCamAddr+g_reprPosOff),12)){ memcpy(out,(void*)(g_liveCamAddr+g_reprPosOff),12); return true; }
    return false;
}
static float readLiveFovV(){ Entry pj; if(!pickBestProj(pj)) return 0.f; return pj.fovY; }
// orientation oracle: the live camera's pitch/yaw/roll (degrees)
static bool readLiveEuler(float& pitch,float& yaw,float& roll){
    float V[16]; int s; uint32_t o; if(readLiveView(V,s,o)){ eulerFromBasis(V,pitch,yaw,roll); return true; }
    if(readCpuView(V)){ eulerFromBasis(V,pitch,yaw,roll); return true; }          // matrix camera in CPU memory
    if(g_liveCamAddr && g_liveCamEulerOff>=0 && Readable((void*)(g_liveCamAddr+g_liveCamEulerOff),12)){   // bare euler-triple camera
        const float* e=(const float*)(g_liveCamAddr+g_liveCamEulerOff); float k=g_liveCamEulerRad?57.2957795f:1.f;
        const char* rl=g_liveCamEulerRoles; // map role letters P/Y/R to the right output
        for(int i=0;i<3;i++){ float v=e[i]*k; if(rl[i]=='P')pitch=v; else if(rl[i]=='Y')yaw=v; else if(rl[i]=='R')roll=v; }
        return true; }
    return false;
}
// SAFETY NET (since the hijacks are on by default): if the hijack can't confirm - no GPU view/projection oracle
// (Vulkan / pure-CPU), or it never lands within the retry cap - but a usable camera AOB WAS captured, play the
// AOB chime once as a fallback so the user still gets a completion signal. Clearly logged as a fallback.
static void chimeAOBFallback(const char* why){
    if(g_soundPlayed||g_hijackChimed) return;                 // a real success already chimed
    if(!(g_wReg>=0 || g_wMasked[0])) return;                  // nothing worth celebrating yet
    Log3("# NOTE: %s - hijack could not confirm, but a usable camera AOB was captured. Playing the AOB chime as a FALLBACK.",why);
    g_extractDone=true; playSuccessSound();
}

// ===== CAMERA PLACEMENT HIJACK ==============================================================
// Active per-axis test (gated by the loader's "Camera HIJACK" checkbox): for every candidate camera
// address, nudge its translation on X, then Y, then Z, hold a few frames, and read back the LIVE camera
// position from the GPU view. The candidate whose nudge actually moves the rendered camera is the real
// position-driving field. Every candidate's per-axis response is mirrored into all three logs. Auto-restored.
static int cameraHijackSweep(uintptr_t* addrs,int n){
    if(!g_camHijack) return 0;
    Log3(""); Log3("################### CAMERA PLACEMENT HIJACK (active per-axis test) ###################");
    Log3("# nudging each candidate's translation X->Y->Z and watching the live camera position. Auto-restored.");
    float base[3]; if(!readLiveCamPos(base)){
        Log3("# no live GPU view to observe camera position (Vulkan / pure-CPU path) - this hijack needs the GPU view; skipping.");
        Log3("################### CAMERA PLACEMENT HIJACK END ###################"); return 0; }
    LogAgg("# baseline live camPos = %.3f, %.3f, %.3f",base[0],base[1],base[2]);
    const float NUDGE=8.0f;                                   // world units per axis
    int found=0;
    for(int i=0;i<n;i++){ uintptr_t a=addrs[i]; if(!a||!Readable((void*)a,64)) continue;
        float orig[16]; memcpy(orig,(void*)a,64); if(!finite16(orig)) continue;
        bool isMat=ortho3x3(orig);
        // translation slots: match cameraPos's row/col detection so we perturb the real translation
        bool colT=(fabsf(orig[3])+fabsf(orig[7]))>(fabsf(orig[12])+fabsf(orig[13]));
        int tIdx[3]; if(colT){ tIdx[0]=3; tIdx[1]=7; tIdx[2]=11; } else { tIdx[0]=12; tIdx[1]=13; tIdx[2]=14; }
        float resp[3]={0,0,0};
        for(int ax=0; ax<3 && Readable((void*)a,64); ax++){
            float pert[16]; memcpy(pert,orig,64); pert[tIdx[ax]]+=NUDGE;
            for(int rep=0;rep<5;rep++){ if(Readable((void*)a,64)) memcpy((void*)a,pert,64); Sleep(16); }
            float now[3]; if(readLiveCamPos(now)) resp[ax]=fabsf(now[0]-base[0])+fabsf(now[1]-base[1])+fabsf(now[2]-base[2]);
            if(Readable((void*)a,64)) memcpy((void*)a,orig,64); Sleep(12);
            LogPF("# hijack cand[%d] @%p slot=%c(+0x%X) nudge=%.1f -> live-campos delta=%.3f",i,(void*)a,"XYZ"[ax],tIdx[ax]*4,NUDGE,resp[ax]);
        }
        bool drives = (resp[0]+resp[1]+resp[2])>0.05f;
        Log3("# CANDIDATE[%d] @%p %s : X-resp=%.3f  Y-resp=%.3f  Z-resp=%.3f  => %s",
             i,(void*)a,isMat?"(4x4)":"(non-ortho)",resp[0],resp[1],resp[2],
             drives?"DRIVES camera placement - REAL position field":"no placement response (downstream copy / not the camera)");
        if(drives){ found++; Log3("#   -> nudging this candidate moves the camera; use it for positional 6DOF (X/Y/Z = struct translation @ +0x%X/0x%X/0x%X).",tIdx[0]*4,tIdx[1]*4,tIdx[2]*4); }
    }
    // euler/quat cameras keep position in a separate vec3 (g_reprPosOff) - test it too if present
    if(g_reprPosOff>=0 && n>0){ uintptr_t pbase=addrs[0]; uint8_t* pf=(uint8_t*)(pbase+g_reprPosOff);
        if(Readable(pf,12)){ float opos[3]; memcpy(opos,pf,12); float resp[3]={0,0,0};
            for(int ax=0;ax<3;ax++){ float pp[3]; memcpy(pp,opos,12); pp[ax]+=NUDGE;
                for(int rep=0;rep<5;rep++){ if(Readable(pf,12)) memcpy(pf,pp,12); Sleep(16); }
                float now[3]; if(readLiveCamPos(now)) resp[ax]=fabsf(now[0]-base[0])+fabsf(now[1]-base[1])+fabsf(now[2]-base[2]);
                if(Readable(pf,12)) memcpy(pf,opos,12); Sleep(12);
                LogPF("# hijack position-vec3 @+0x%X axis=%c -> live-campos delta=%.3f",g_reprPosOff,"XYZ"[ax],resp[ax]); }
            bool drives=(resp[0]+resp[1]+resp[2])>0.05f;
            Log3("# POSITION-VEC3 @+0x%X : X-resp=%.3f Y-resp=%.3f Z-resp=%.3f => %s",g_reprPosOff,resp[0],resp[1],resp[2],
                 drives?"DRIVES placement - the separate position field for this euler/quat camera":"no response"); if(drives) found++; } }
    Log3("# CAMERA HIJACK: %d candidate(s) actually moved the camera placement.",found);
    Log3("################### CAMERA PLACEMENT HIJACK END ###################");
    return found;
}

// ===== CAMERA ROTATION HIJACK ===============================================================
// Active per-axis test (on by default): for every candidate, rotate its 3x3 by yaw, then pitch, then roll, hold a
// few frames, and read the live camera ORIENTATION back from the rendered view. The candidate whose rotation
// actually turns the rendered camera is the real orientation field - the one head-look must drive. Euler-triple
// cameras' angle field is tested directly. Mirrored into all three logs. Auto-restored.
static int rotationHijackSweep(uintptr_t* addrs,int n){
    if(!g_rotHijack) return 0;
    Log3(""); Log3("################### CAMERA ROTATION HIJACK (active pitch/yaw/roll test) ###################");
    Log3("# rotating each candidate's 3x3 by YAW->PITCH->ROLL and watching the live camera orientation. Auto-restored.");
    float bp,by,br; if(!readLiveEuler(bp,by,br)){
        Log3("# no live GPU view to observe orientation (Vulkan / pure-CPU path) - this hijack needs the GPU view; skipping.");
        Log3("################### CAMERA ROTATION HIJACK END ###################"); return 0; }
    float np,ny,nr; Sleep(32); float noise=0.f; if(readLiveEuler(np,ny,nr)) noise=fabsf(np-bp)+fabsf(ny-by)+fabsf(nr-br);
    float thresh=noise*4.f+1.5f;                              // degrees: clear the frame-to-frame jitter
    const float ANG=0.30f;                                   // ~17 deg perturbation
    int found=0;
    for(int i=0;i<n;i++){ uintptr_t a=addrs[i]; if(!a||!Readable((void*)a,64)) continue;
        float orig[16]; memcpy(orig,(void*)a,64); if(!finite16(orig)||!ortho3x3(orig)) continue;
        float resp[3]={0,0,0};
        for(int ax=0; ax<3 && Readable((void*)a,64); ax++){  // 0=yaw(Y) 1=pitch(X) 2=roll(Z)
            float c=cosf(ANG),s=sinf(ANG),Rax[9];
            if(ax==0){ float t[9]={c,0,s, 0,1,0, -s,0,c}; memcpy(Rax,t,sizeof t); }
            else if(ax==1){ float t[9]={1,0,0, 0,c,-s, 0,s,c}; memcpy(Rax,t,sizeof t); }
            else { float t[9]={c,-s,0, s,c,0, 0,0,1}; memcpy(Rax,t,sizeof t); }
            float M[9]={orig[0],orig[1],orig[2],orig[4],orig[5],orig[6],orig[8],orig[9],orig[10]},P[9];
            for(int r=0;r<3;r++)for(int k=0;k<3;k++) P[r*3+k]=Rax[r*3]*M[k]+Rax[r*3+1]*M[3+k]+Rax[r*3+2]*M[6+k];
            float pert[16]; memcpy(pert,orig,64);
            pert[0]=P[0];pert[1]=P[1];pert[2]=P[2];pert[4]=P[3];pert[5]=P[4];pert[6]=P[5];pert[8]=P[6];pert[9]=P[7];pert[10]=P[8];
            for(int rep=0;rep<5;rep++){ if(Readable((void*)a,64)) memcpy((void*)a,pert,64); Sleep(16); }
            float p2,y2,r2; if(readLiveEuler(p2,y2,r2)) resp[ax]=fabsf(p2-bp)+fabsf(y2-by)+fabsf(r2-br);
            if(Readable((void*)a,64)) memcpy((void*)a,orig,64); Sleep(12);
            LogPF("# rot-hijack cand[%d] @%p axis=%s -> live-euler delta=%.2f deg",i,(void*)a,ax==0?"YAW":ax==1?"PITCH":"ROLL",resp[ax]);
        }
        bool drives=(resp[0]>thresh||resp[1]>thresh||resp[2]>thresh);
        Log3("# CANDIDATE[%d] @%p : yaw-resp=%.2f pitch-resp=%.2f roll-resp=%.2f (thresh=%.2f) => %s",
             i,(void*)a,resp[0],resp[1],resp[2],thresh,
             drives?"DRIVES camera ROTATION - REAL orientation field":"no rotation response (downstream copy / not the camera)");
        if(drives){ found++; Log3("#   -> rotating this candidate's 3x3 turns the camera; this is the orientation field head-look drives."); }
    }
    // euler-triple cameras: perturb the angle field directly (pitch/yaw/roll scalars)
    if(g_reprEulerOff>=0 && n>0){ uint8_t* ef=(uint8_t*)(addrs[0]+g_reprEulerOff);
        if(Readable(ef,12)){ float oe[3]; memcpy(oe,ef,12); float resp[3]={0,0,0}; float step=g_reprEulerRad?0.30f:17.f;
            for(int ax=0;ax<3;ax++){ float pe[3]; memcpy(pe,oe,12); pe[ax]+=step;
                for(int rep=0;rep<5;rep++){ if(Readable(ef,12)) memcpy(ef,pe,12); Sleep(16); }
                float p2,y2,r2; if(readLiveEuler(p2,y2,r2)) resp[ax]=fabsf(p2-bp)+fabsf(y2-by)+fabsf(r2-br);
                if(Readable(ef,12)) memcpy(ef,oe,12); Sleep(12);
                LogPF("# rot-hijack euler @+0x%X idx=%d -> live-euler delta=%.2f deg",g_reprEulerOff,ax,resp[ax]); }
            bool drives=(resp[0]>thresh||resp[1]>thresh||resp[2]>thresh);
            Log3("# EULER-TRIPLE @+0x%X : [0]=%.2f [1]=%.2f [2]=%.2f => %s",g_reprEulerOff,resp[0],resp[1],resp[2],
                 drives?"DRIVES rotation - the euler angle field (head-look adds to these)":"no response"); if(drives) found++; } }
    Log3("# ROTATION HIJACK: %d candidate(s) actually rotated the camera.",found);
    Log3("################### CAMERA ROTATION HIJACK END ###################");
    return found;
}

// ===== FOV HIJACK ==========================================================================
// Active test (gated by the loader's "FOV HIJACK" checkbox): drive each FOV candidate UP then DOWN and read
// the rendered vertical FOV back from the projection. The candidate that actually changes the projection is the
// real FOV field; its working range is logged. Mirrored into all three logs. Auto-restored.
static int fovHijackSweep(uintptr_t base){
    if(!g_fovHijack||!base) return 0;
    Log3(""); Log3("################### FOV HIJACK (active increase/decrease test) ###################");
    float f0=readLiveFovV();
    if(f0<=0.f){ Log3("# no live projection to observe rendered FOV (Vulkan / pure-CPU path) - this hijack needs the GPU projection; skipping.");
        Log3("################### FOV HIJACK END ###################"); return 0; }
    Log3("# baseline rendered FOV(V) = %.2f deg. Writing up/down to each FOV candidate; watching the projection. Auto-restored.",f0);
    std::vector<int> offs; if(g_reprFovOff>=0) offs.push_back(g_reprFovOff);
    int scanLimit = g_aggressive?0x800:0x400;                 // aggressive: scan a wider field of candidate floats
    for(int o=0;o<scanLimit;o+=4){ if(!Readable((void*)(base+o),4))continue; float v=*(float*)(base+o);
        if(v==v && isFovFloat(v) && std::find(offs.begin(),offs.end(),o)==offs.end()){ offs.push_back(o); if((int)offs.size()>(g_aggressive?64:32))break; } }
    LogAgg("# FOV hijack scanning %d candidate float(s) in [base, base+0x%X)",(int)offs.size(),scanLimit);
    int found=0;
    for(int oi=0;oi<(int)offs.size();oi++){ int o=offs[oi]; uint8_t* p=(uint8_t*)(base+o); if(!Readable(p,4))continue;
        float orig=*(float*)p; if(orig!=orig) continue;
        float up=(orig>=20.f)?orig+25.f:(orig>=1.35f)?orig+0.45f:orig*1.35f;     // widen, encoding-aware
        float dn=(orig>=20.f)?orig-15.f:(orig>=1.35f)?orig-0.30f:orig*0.70f;     // narrow
        for(int rep=0;rep<6;rep++){ if(Readable(p,4)) *(float*)p=up; Sleep(16); } float fUp=readLiveFovV();
        for(int rep=0;rep<6;rep++){ if(Readable(p,4)) *(float*)p=dn; Sleep(16); } float fDn=readLiveFovV();
        if(Readable(p,4)) *(float*)p=orig; Sleep(12);
        float resp=fabsf(fUp-fDn);
        LogPF("# fov-hijack @+0x%X orig=%.3f  up=%.3f->FOV %.2f  dn=%.3f->FOV %.2f  resp=%.2f",o,orig,up,fUp,dn,fDn,resp);
        bool real=resp>0.5f;
        if(real){ found++; const char* enc=(orig>=20.f)?"degrees":(orig>=1.35f)?"radians":"factor/percent-of-base";
            Log3("# FOV CANDIDATE @+0x%X : orig=%.3f  rendered-FOV up=%.2f down=%.2f  delta=%.2f => REAL FOV (encoding ~ %s)",o,orig,fUp,fDn,resp,enc);
            Log3("#   -> write a larger value to +0x%X to WIDEN, smaller to NARROW; this is the field the runtime's FOV apply should target.",o);
        } else LogAgg("# FOV @+0x%X : no projection response (delta=%.2f) - not the FOV (or FOV is baked into the viewproj).",o,resp);
    }
    if(found==0) Log3("# FOV HIJACK: no candidate changed the rendered FOV - FOV is likely packed into the view-projection (drive it via the projection m0/m5 instead).");
    else Log3("# FOV HIJACK: %d candidate(s) actually changed the rendered FOV.",found);
    Log3("################### FOV HIJACK END ###################");
    return found;
}

// ---- refresh candidate camera addresses cheaply from the current live GPU view (no write-watch) ----
// ===== CPU MOVE-TEST (v5.14) =================================================================
// No GPU oracle? Confirm the located camera by REALLY MOVING IT: write a yaw to the camera + its pooled copies and
// check whether the engine HOLDS the write. The differential already proved this struct reflects the on-screen view
// (it moved with the player's input), so: write HOLDS => the struct is authoritative => writing it moves the view
// (settable camera - the runtime can drive it at a fixed address). Write REVERTED => the struct is a pooled/transient
// copy the engine recomputes => fixed-address writes can't move it; the runtime must hook the WRITER (the captured
// AOB) and add the pose each frame - which is exactly what it does. Each write is auto-restored.
static void cpuMoveTest(){
    if(!g_liveCamAddr) return;
    bool isEuler=(g_liveCamEulerOff>=0), isMat=(g_liveCamMatOff>=0);
    if(!isEuler && !isMat) return;
    Log3(""); Log3("################### CPU MOVE-TEST (OPT-IN active confirmation - writes into the live game) ###################");
    Log3("# WARNING: this writes a small yaw into the camera to see if it moves. Writing into a running game CAN crash it.");
    Log3("# Testing the located camera (gentle: ~3 deg, restored). The AOB capture above is the safe result either way.");
    int held=0,reverted=0,tested=0; const int MAXCOPIES=2;   // winner + at most one copy (writing many copies is what crashed Alan Wake)
    for(int i=0;i<g_diffCopyN && i<MAXCOPIES;i++){ uintptr_t a=g_diffCopies[i]; if(!a) continue;
        if(isEuler){ uint8_t* ef=(uint8_t*)(a+g_liveCamEulerOff); if(!Readable(ef,12)) continue;
            float oe[3]; memcpy(oe,ef,12); bool ok=true; for(int k=0;k<3;k++) if(!(oe[k]==oe[k])) ok=false; if(!ok) continue;
            float step=g_liveCamEulerRad?0.05f:3.f; int yi=1; for(int k=0;k<3;k++) if(g_liveCamEulerRoles[k]=='Y') yi=k;   // gentle ~3 deg
            float pe[3]; memcpy(pe,oe,12); pe[yi]+=step;
            for(int r=0;r<2;r++){ if(Readable(ef,12)) memcpy(ef,pe,12); Sleep(16); }   // brief (2 frames)
            Sleep(48);
            float cur[3]; bool rd=Readable(ef,12); if(rd) memcpy(cur,ef,12);
            float dhold=rd?fabsf(cur[yi]-pe[yi]):999.f;
            if(Readable(ef,12)) memcpy(ef,oe,12);                                         // restore
            tested++; bool h=(dhold<step*0.35f); if(h)held++; else reverted++;
            Log3("# move-test copy[%d] @%p (euler yaw) => write %s",i,(void*)a,h?"HELD (settable - this write moves the view)":"REVERTED (pooled/transient copy)");
        } else { uint8_t* mp=(uint8_t*)(a+g_liveCamMatOff); if(!Readable(mp,64)) continue;
            float om[16]; memcpy(om,mp,64); if(!finite16(om)||!ortho3x3(om)) continue;
            float c=cosf(0.05f),s=sinf(0.05f); float R[9]={c,0,s,0,1,0,-s,0,c};          // gentle ~3deg yaw about Y
            float M[9]={om[0],om[1],om[2],om[4],om[5],om[6],om[8],om[9],om[10]},P[9];
            for(int r=0;r<3;r++)for(int k=0;k<3;k++)P[r*3+k]=R[r*3]*M[k]+R[r*3+1]*M[3+k]+R[r*3+2]*M[6+k];
            float pm[16]; memcpy(pm,om,64); pm[0]=P[0];pm[1]=P[1];pm[2]=P[2];pm[4]=P[3];pm[5]=P[4];pm[6]=P[5];pm[8]=P[6];pm[9]=P[7];pm[10]=P[8];
            for(int r=0;r<2;r++){ if(Readable(mp,64)) memcpy(mp,pm,64); Sleep(16); }
            Sleep(48);
            float cur[16]; bool rd=Readable(mp,64); if(rd) memcpy(cur,mp,64);
            float dhold=rd?(fabsf(cur[0]-pm[0])+fabsf(cur[1]-pm[1])+fabsf(cur[2]-pm[2])):999.f;
            if(Readable(mp,64)) memcpy(mp,om,64);
            tested++; bool h=(dhold<0.05f); if(h)held++; else reverted++;
            Log3("# move-test copy[%d] @%p (matrix yaw) => write %s",i,(void*)a,h?"HELD (settable - this write moves the view)":"REVERTED (pooled/transient copy)");
        }
    }
    if(held>0){
        Log3("# *** CPU MOVE-TEST: %d/%d write(s) HELD - the located camera is SETTABLE and reflects the view,",held,tested);
        Log3("#     so writing pitch/yaw/roll + x/y/z to it MOVES the camera. The runtime drives it directly. ***");
        notifyHijackSuccess();
    } else if(tested>0){
        Log3("# CPU MOVE-TEST: every fixed-address write was REVERTED => this camera is POOLED/transient.");
        Log3("#   A fixed-address write can't move it; the runtime mod hooks the WRITER (AOB @ %s+0x%llX) and adds the",g_wMod,(unsigned long long)g_wOff);
        Log3("#   head pose each frame - that IS what moves a pooled camera. The captured AOB is the deliverable.");
    } else Log3("# CPU MOVE-TEST: no readable copy to test.");
    Log3("################### CPU MOVE-TEST END ###################");
}
static DWORD WINAPI cpuMoveTestThread(LPVOID){ cpuMoveTest(); return 0; }

static int refreshCandidates(uintptr_t* out,int cap){
    Entry bv; if(!bestViewEntry(bv)){
        // v5.14 CPU fallback: no GPU view (D3D12/Vulkan). Use the differential-confirmed live camera + the coherent
        // pooled copies the differential found, so the hijack has real candidates to perturb and observe.
        int n=0; if(g_liveCamAddr && n<cap) out[n++]=g_liveCamAddr;
        for(int i=0;i<g_diffCopyN && n<cap;i++){ uintptr_t a=g_diffCopies[i]; bool dup=false; for(int j=0;j<n;j++) if(out[j]==a)dup=true; if(!dup) out[n++]=a; }
        return n; }
    std::vector<MemHit> hits; findNeedle((const uint8_t*)bv.m,hits,cap);
    if(hits.empty()){ float inv[16]; if(gj4Inverse(bv.m,inv)) findNeedle((const uint8_t*)inv,hits,cap); }
    if(hits.empty()){ float tr[16]; transpose4(bv.m,tr); findNeedle((const uint8_t*)tr,hits,cap); }
    int n=0; for(auto&h:hits){ if(n>=cap)break; out[n++]=h.addr; }
    if(g_corrAddr){ bool dup=false; for(int k=0;k<n;k++) if(out[k]==g_corrAddr) dup=true; if(!dup&&n<cap) out[n++]=g_corrAddr; }
    return n;
}

// ===== HIJACK AUTO-RETRY LOOP =================================================================
// Gated by the loader's "Auto-retry hijack until it lands" checkbox. Keeps re-scanning the live view for
// fresh candidate copies and re-running the camera / FOV hijack each round until one ACTUALLY moves the
// camera (per axis) / changes the rendered FOV. Drives a little motion between rounds (when auto-mouse is on)
// so a fresh live camera instance exists, and announces the landing (chime) on success.
// (g_hijackRetryRunning defined earlier for the CPU-path spin-test)
static DWORD WINAPI hijackRetryThread(LPVOID){
    if(g_hijackRetryRunning) return 0; g_hijackRetryRunning=true;
    Log3(""); Log3("################### HIJACK AUTO-RETRY LOOP (scan until it LANDS) ###################");
    Log3("# re-scanning candidates + re-running the %s%s%s%s%s hijack every round until it really rotates/moves the camera / changes the FOV.",
        g_rotHijack?"ROTATION":"",(g_rotHijack&&(g_camHijack||g_fovHijack))?" + ":"",
        g_camHijack?"PLACEMENT":"",(g_camHijack&&g_fovHijack)?" + ":"",g_fovHijack?"FOV":"");
    const int MAXR=400;                                    // hard cap (a few minutes worst case)
    const int CAM_MISS_MAX=80;                             // rotation+placement never confirm -> located addresses aren't the camera
    const int FOV_TRIES_MAX=4;                             // FOV field location is fixed; a few tries is plenty (it's expensive)
    int round=0, noOracle=0, camMiss=0, fovTries=0;
    for(; round<MAXR; round++){
        bool needRot=g_rotHijack && !g_rotHijackLanded;
        bool needCam=g_camHijack && !g_camHijackLanded;
        bool needFov=g_fovHijack && !g_fovHijackLanded && !g_fovGaveUp;
        if(!needRot && !needCam && !needFov) break;        // everything that can land has landed (or FOV gave up)
        if(g_autoDriveMouse && gameIsForeground()) autoOscillate(500);   // fresh live instance / fresh copies
        else Sleep(400);
        // no GPU view/projection to verify against (Vulkan / pure-CPU)? after a fair grace period, fall back.
        { float tmp[3]; bool oracle=readLiveCamPos(tmp)||(readLiveFovV()>0.f);
          if(!oracle){ if(++noOracle>=40){ Log3("# retry: no GPU view/projection oracle after %d rounds (Vulkan / pure-CPU title) - the hijack can't self-verify here.",noOracle);
                chimeAOBFallback("no live GPU oracle to verify the camera/FOV against"); break; } continue; }
          noOracle=0; }
        uintptr_t cands[10]; int nc=refreshCandidates(cands,10);
        LogPF("# retry round %d: %d candidate(s) (rotLanded=%d camLanded=%d fovLanded=%d fovTries=%d)",round+1,nc,g_rotHijackLanded?1:0,g_camHijackLanded?1:0,g_fovHijackLanded?1:0,fovTries);
        if(nc==0){ if((round%15)==0) Log3("# retry round %d: no live camera candidate yet - keep gameplay on screen and the camera moving.",round+1); continue; }
        if(needRot){ int got=rotationHijackSweep(cands,nc);
            if(got>0){ g_rotHijackLanded=true; Log3("# *** ROTATION HIJACK LANDED on round %d - %d candidate(s) rotate the camera. The real orientation field/AOB is confirmed. ***",round+1,got); maybeChimeHijackSuccess(); } }
        if(needCam){ int got=cameraHijackSweep(cands,nc);
            if(got>0){ g_camHijackLanded=true; Log3("# *** CAMERA PLACEMENT HIJACK LANDED on round %d - %d candidate(s) move the camera. The real position field/AOB is confirmed. ***",round+1,got); maybeChimeHijackSuccess(); } }
        // both structural channels missed this round? count it; after enough misses the located addresses aren't the camera
        if((needRot && !g_rotHijackLanded) && (needCam && !g_camHijackLanded)){
            if(++camMiss>=CAM_MISS_MAX){ Log3("# retry: neither rotation nor placement moved the view in %d rounds - the located addresses aren't the live camera (transient/pooled or GPU-only).",camMiss);
                chimeAOBFallback("hijack could not confirm any candidate"); break; } }
        else if((!g_rotHijack||g_rotHijackLanded) && (!g_camHijack||g_camHijackLanded)) camMiss=0;
        // FOV: its field offset is fixed, so re-testing every round is wasteful AND expensive - try a few times then conclude.
        if(needFov && fovTries<FOV_TRIES_MAX){ fovTries++;
            int got=fovHijackSweep(cands[0]);
            if(got>0){ g_fovHijackLanded=true; Log3("# *** FOV HIJACK LANDED on round %d - the real FOV field is confirmed and changes the rendered FOV. ***",round+1); maybeChimeHijackSuccess(); }
            else if(fovTries>=FOV_TRIES_MAX){ g_fovGaveUp=true; Log3("# FOV did not respond in %d attempts - treating as NO separate FOV field (baked into the view-projection). It will not block success.",fovTries); maybeChimeHijackSuccess(); } }
    }
    if(round>=MAXR) chimeAOBFallback("hijack did not land within the retry cap");
    Log3("# HIJACK AUTO-RETRY finished after %d round(s):  rotation=%s  placement=%s  fov=%s",round,
        g_rotHijack?(g_rotHijackLanded?"LANDED":"not confirmed"):"n/a",
        g_camHijack?(g_camHijackLanded?"LANDED":"not confirmed"):"n/a",
        g_fovHijack?(g_fovHijackLanded?"LANDED":(g_fovGaveUp?"no separate field":"not confirmed")):"n/a");
    Log3("################### HIJACK AUTO-RETRY LOOP END ###################");
    g_hijackRetryRunning=false; return 0;
}

// ===== consolidated TURNKEY SPEC: one block with everything needed to build the mod for this game =====
static void turnkeySpec(const Entry* bv,uintptr_t cpuAddr,const char* cpuMod,uintptr_t cpuOff,bool spinRan){
    Log(""); Log("################### TURNKEY MOD SPEC  (copy this whole block) ###################");
    Log("GAME=%s   ARCH=%d-bit   API=%s   ENGINE=%s   FILEVER=%s",g_game,(int)(sizeof(void*)*8),g_api,g_engine,g_fileVer);
    Log("RESOLUTION=%ux%u   SCREEN_ASPECT=%.4f",g_resW,g_resH,(g_resW&&g_resH)?(float)g_resW/(float)g_resH:0.f);
    // [1] representation
    if(cpuAddr)  Log("[1] REPRESENTATION = %s   (CPU struct - authoritative; drive this)",g_reprKind);
    else if(bv)  Log("[1] REPRESENTATION = GPU constant-buffer VIEW matrix (%s-major) - no CPU source isolated",bv->rowMaj?"row":"col");
    else         Log("[1] REPRESENTATION = none isolated from GPU; use differential discovery (F7/F8)");
    // [2] locator
    Log("[2] LOCATOR:");
    bool haveLoc=false;
    if(g_wReg>=0){ haveLoc=true;
        Log("    WRITE-SITE  : %s + 0x%llX   store %s [%s%+d]",g_wMod,(unsigned long long)g_wOff,g_wMnem,regName(g_wReg),g_wDisp);
        Log("    => camera base register = %s ; field offset = 0x%X ; code-cave steal >= %d bytes (hook, let game write, then add head pose)",regName(g_wReg),g_wDisp<0?-g_wDisp:g_wDisp,g_wSteal);
        if(g_wMasked[0]) Log("    WRITE_AOB    : %s   (scan this; '|' = hook point, '??' = the field-offset bytes)",g_wMasked); }
    if(g_fnOff){ haveLoc=true;
        Log("    FN-HOOK     : %s + 0x%llX   <- trampoline-hook this FUNCTION (call original, then add head pose to the camera in %s).",g_fnMod,(unsigned long long)g_fnOff,g_wReg>=0?regName(g_wReg):"the write-site reg");
        Log("    FN-ENTRY_AOB: %s",g_fnAOB);
        Log("    *** PREFERRED for transient/pooled cameras (the matrix address changes per frame, the function does not). ***"); }
    if(bv && !cpuAddr){ haveLoc=true;
        Log("    GPU-CB      : VS slot=%d, buffer size=%u, matrix at byte offset 0x%X; inject at Map/Unmap; lock by (size,offset) signature, never by value.",bv->slot,bv->size,bv->off); }
    Log("    (static pointer chains, if any, are listed above as 'pointer chains -> target')");
    if(!haveLoc) Log("    no stable locator captured yet - re-run the write-watch with the camera MOVING, or use F7/F8");
    // [3] layout / math
    const float* M = (cpuAddr&&Readable((void*)cpuAddr,64))?(const float*)cpuAddr:(bv?bv->m:nullptr);
    auto det3=[](const float* m){ return m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]); };
    if(M && (g_reprMatOff>=0 || bv)){ bool rowMaj=cpuAddr?g_reprMatRow:bv->rowMaj; float dt=det3(M);
        bool colT=(fabsf(M[3])+fabsf(M[7]))>(fabsf(M[12])+fabsf(M[13])); int up=upAxisOf(M,rowMaj); float pi,ya,ro; eulerFromBasis(M,pi,ya,ro);
        Log("[3] LAYOUT: %s-major  handedness=%s(det=%.2f)  translation=%s  up-axis=%s",rowMaj?"row":"col",dt<0?"LH/mirror":"RH",dt,colT?"col3(idx3,7,11)":"row3(idx12,13,14)",up==1?"Y-up":(up==2?"Z-up":"X-up"));
        Log("    camPos=%.2f,%.2f,%.2f  euler(P,Y,R)=%.1f,%.1f,%.1f",M[12],M[13],M[14],pi,ya,ro);
        Log("    APPLY: read fresh each frame; compose head R into the 3x3 (world matrix=post-mult, view matrix=pre-mult); add lean along the basis to the translation slot.");
    } else if(g_reprQuatOff>=0){ Log("[3] LAYOUT: quaternion(x,y,z,w) @+0x%X; apply head rotation as q' = q (x) q_head (camera-local) then normalize; lean adds to the position vec3 beside it.",g_reprQuatOff);
        Log("[3b] NODE-HIERARCHY NOTE: if this quaternion is a scene-graph node (common in AAA engines), the head delta must be applied in the node's frame - CONJUGATE: q_local = conj(q) (x) q_head_world (x) q, or try the other multiply order q' = q_head (x) q. Test both; the wrong frame makes the camera orbit/tilt oddly.");
    } else if(g_reprEulerOff>=0){ Log("[3] LAYOUT: euler-degrees @+0x%X, axis roles ~ [%c,%c,%c] (pitch clamps +/-90, yaw wraps +/-180, roll sits ~0 - VERIFY by moving); ADD head yaw/pitch/roll to the matching axis; position is a separate field.",g_reprEulerOff,g_reprEulerRoles[0]?g_reprEulerRoles[0]:'?',g_reprEulerRoles[1]?g_reprEulerRoles[1]:'?',g_reprEulerRoles[2]?g_reprEulerRoles[2]:'?');
    } else if(g_reprEyeOff>=0){ Log("[3] LAYOUT: eye@+0x%X / target@+0x%X look-at; head yaw/pitch rotate target around eye; roll separate float; lean translates eye.",g_reprEyeOff,g_reprTgtOff); }
    // [4] FOV
    Entry pj; bool hp=pickBestProj(pj);
    if(hp){ bool rz=pj.m[10]<0; Log("[4] FOV: proj off=0x%X size=%u slot=%d  fovV=%.2f fovH=%.2f aspect=%.3f near=%.4f far=%.1f reversedZ=%d",pj.off,pj.size,pj.slot,pj.fovY,pj.fovX,pj.aspect,pj.zn,pj.zf,rz?1:0);
            Log("    override: scale m0 & m5 by tan(curV/2)/tan(targetV/2) in the PROJ buffer each frame."); }
    else if(g_reprFovOff>=0) Log("[4] FOV: scalar @+0x%X (write a new vertical FOV: radians if <~3.2, degrees if >~25, OR a FACTOR of a base FOV if the value is ~1.0 - some engines store FOV as a multiplier/percentage of a fixed core angle, not an absolute angle).",g_reprFovOff);
    else Log("[4] FOV: not isolated (may be packed in the VIEWPROJ).");
    // [5] params
    Log("[5] RECOMMENDED INI: WorldUnitsPerMetre=%.0f (engine guess - tune)  Yaw/Pitch/Roll mult=1.0  Smoothing=0.5  inverts=0 (flip one at a time)",wupmGuess());
    // [6] drive decision (the upstream-vs-final-stage lesson)
    if(cpuAddr){ Log("[6] DRIVE: spin-test ran on %s+0x%llX.",cpuMod,(unsigned long long)cpuOff);
        Log("    view SWEPT  -> drive THIS CPU struct (flicker-free via the write-site cave or a HW breakpoint).");
        Log("    view STATIC -> struct is upstream/derived (controller re-derives the final view, e.g. AW2 pitch); hook the engine's FINAL view+FOV setter, or inject the GPU buffer downstream.");
    } else if(bv){ Log("[6] DRIVE: no CPU source -> inject the GPU constant-buffer view (downstream, reaches screen); confirm with an in-game spin.");
    } else { Log("[6] DRIVE: nothing isolated -> F7, rotate view 45deg, F8; the address that keeps moving is the camera."); }
    Log("CONFIDENCE: gpu_view=%s cpu_source=%s write_site=%s spin=%s proj=%s",bv?"yes":"no",cpuAddr?"yes":"no",g_wReg>=0?"decoded":"no",spinRan?"run":"no",hp?"yes":"no");
    Log("################### END TURNKEY MOD SPEC ###################");
}

static volatile bool g_pipelineDone=false, g_pipelineRunning=false;
// Wait until the live camera actually MOVES, so the differential has a signal to isolate it. Samples a spread of
// the snapshot entries and returns once enough have changed (the player looked around, or auto-drive moved it).
static bool awaitCameraMotion(int ms){
    std::vector<std::pair<uintptr_t,int>> pts; std::vector<float> base;
    { std::lock_guard<std::mutex> lk(g_snapMx);
      size_t step=g_snap.size()/256+1;
      for(size_t i=0;i<g_snap.size();i+=step){ const Snap&s=g_snap[i]; int n=s.type==1?16:(s.type==3?3:4);
        if(!Readable((void*)s.addr,(size_t)n*4)) continue; pts.push_back({s.addr,n});
        const float* f=(const float*)s.addr; for(int k=0;k<n;k++) base.push_back(f[k]); } }
    if(pts.empty()) return false;
    Log("# AUTO-DIFFERENTIAL: look around / move the view for a moment so the camera can be isolated...");
    int waited=0;
    while(waited<ms){
        if(g_autoDriveMouse) autoOscillate(200); else Sleep(200);
        waited+=200; logManualInput();
        int changed=0; size_t bi=0;
        for(auto&pt:pts){ if(!Readable((void*)pt.first,(size_t)pt.second*4)){ bi+=pt.second; continue; }
            const float* f=(const float*)pt.first; float d=0; for(int k=0;k<pt.second;k++) d+=fabsf(f[k]-base[bi+k]); bi+=pt.second;
            if(d>0.01f) changed++; }
        if(changed>=4){ Log("# AUTO-DIFFERENTIAL: camera motion detected (%d fields changed) - running the delta now.",changed); return true; }
    }
    Log("# AUTO-DIFFERENTIAL: little motion seen in %dms - running the delta anyway (it will retry automatically if nothing locks).",ms);
    return false;
}
static void runPipeline(){
    if(g_pipelineRunning) return; g_pipelineRunning=true; g_lastVerified=0;
    isOwnRip(0);   // prime g_selfBase/g_selfEnd now (calls GetModuleInformation off the exception-handler path)
    static bool manStarted=false; if(!manStarted){ manStarted=true; CreateThread(nullptr,0,manualInputThread,nullptr,0,nullptr);
        Log("# MANUAL-INPUT monitor active: move the mouse or press W/A/S/D yourself and the probe will use (and log) it."); }
    Log(""); Log("################### AUTO-PIPELINE ###################");
    Entry bv;
    if(!bestViewEntry(bv)){
        Log("# no GPU VIEW candidate (D3D12/Vulkan or pure-CPU camera). Running the AUTOMATIC CPU differential:");
        Log("#   memscan -> snapshot -> wait for motion -> delta -> write-watch (no F7/F8 needed).");
        memScan();                                   // sets the CPU projection oracle + the view-matrix pool region, lists candidates
        snapshotScan();                              // baseline (F7-equivalent)
        awaitCameraMotion(8000);                     // let the player's look (or auto-drive) move the camera
        deltaScan();                                 // isolate the live camera, run the write-watch + pool page-guard, EMIT THE PROFILE (F8-equivalent)
        turnkeySpec(nullptr,0,"",0,false);
        Log("################### AUTO-PIPELINE END ###################");
        g_pipelineRunning=false;
        g_pipelineDone = g_deltaFoundCamera;         // if nothing locked (player was still), let the periodic retry try again
        return; }
    Log("# top GPU VIEW: off=0x%X size=%u slot=%d layout=%s draws=%u freq=%u campos=%.1f,%.1f,%.1f",
        bv.off,bv.size,bv.slot,bv.rowMaj?"row":"col",bv.draws,bv.freq,bv.campos[0],bv.campos[1],bv.campos[2]);
    std::vector<MemHit> hits; findNeedle((const uint8_t*)bv.m,hits,8);
    bool invMatch=false,trMatch=false;
    if(hits.empty()){
        // ~1 in 6 engines store the INVERSE view (camera-to-world), or a transposed copy, on the GPU vs the CPU
        // source - so an exact match misses. Search for the inverse and the transpose before giving up.
        float inv[16],tr[16];
        if(gj4Inverse(bv.m,inv)){ std::vector<MemHit> h2; findNeedle((const uint8_t*)inv,h2,4);
            if(!h2.empty()){ invMatch=true; hits=h2; Log("# correlate: GPU buffer holds the INVERSE of the CPU matrix (engine stores camera-to-world; CPU source is world-to-view)"); } }
        if(hits.empty()){ transpose4(bv.m,tr); std::vector<MemHit> h3; findNeedle((const uint8_t*)tr,h3,4);
            if(!h3.empty()){ trMatch=true; hits=h3; Log("# correlate: GPU buffer is the TRANSPOSE of the CPU matrix (row/col-major differ between GPU and CPU)"); } }
    }
    uintptr_t cpuAddr=0; char cpuMod[80]="heap"; uintptr_t cpuOff=0;
    if(hits.empty()) Log("# correlate: matrix not present in CPU memory right now (pure GPU-side, or it moved between capture & scan)");
    else { Log("# correlate: %d CPU copy(ies) of the camera matrix%s:",(int)hits.size(),invMatch?" (as inverse)":(trMatch?" (as transpose)":""));
        for(auto&h:hits) Log("#   @ %s+0x%llX (%p)",h.mod,(unsigned long long)h.off,(void*)h.addr);
        // CLOSED-LOOP: there are often several identical copies; only the SOURCE moves the rendered view when written.
        // Verify each (most-promising first) and lock onto the one that responds; this auto-rejects downstream copies.
        Log("# verifying which copy actually drives the view (perturb + watch the live GPU view; auto-restored)...");
        int verified=-1;
        for(int i=0;i<(int)hits.size() && i<4;i++){ int r=verifyByView((void*)hits[i].addr);
            if(r==1){ cpuAddr=hits[i].addr; strncpy(cpuMod,hits[i].mod,sizeof(cpuMod)-1); cpuOff=hits[i].off; verified=1; break; }
            if(r==-1 && verified<0) verified=-1; }
        if(!cpuAddr){   // none verified (or no live view to observe) - fall back to the first non-heap copy as before
            for(auto&h:hits){ if(!cpuAddr || (strcmp(h.mod,"heap")!=0 && !strcmp(cpuMod,"heap"))){ cpuAddr=h.addr; strncpy(cpuMod,h.mod,sizeof(cpuMod)-1); cpuOff=h.off; } }
            if(verified==0) Log("# verify: no copy moved the view - the source may be written elsewhere; proceeding with the best copy (treat as UNVERIFIED).");
        }
    }
    if(g_aggressive){ LogAgg("################### AGGRESSIVE DEEP PROBE (3rd log) ###################");
        LogAgg("# stronger AOB + FOV hunt: write-watch runs 2x longer / more candidates; FOV scan is wider. game=%s api=%s engine=%s",g_game,g_api,g_engine); }
    if(cpuAddr){
        g_corrAddr=cpuAddr; g_corrType=1;   // also let the controller thread correlate gamepad motion to this camera
        findChains(cpuAddr);
        dumpStructFlags(cpuAddr);
        armWriteWatch((void*)cpuAddr,g_aggressive?160:80,g_aggressive?
            "# AGGRESSIVE write-watch armed (~80s, doubled to gather more & rarer candidates) - KEEP MOVING the in-game camera the WHOLE time...":
            "# write-watch armed (~40s, runs the FULL time to gather 5 candidates) - KEEP MOVING the in-game camera the WHOLE time (mouse or WASD)...");
        crossCheckFov(cpuAddr);  // v5.8: projection ground-truth first - solves offset + encoding without input
        if(g_reprFovOff<0 && !g_reprFovProjOnly) correlateFov(cpuAddr);   // active zoom fallback only if cross-check didn't lock
        if(g_reprFovOff>=0) captureFovWriter(cpuAddr+g_reprFovOff);   // FOV control is a universal feature - capture its writer too
        spinTest((void*)cpuAddr,3);
        // ---- active hijack tests (on by default): which candidate REALLY rotates / moves the camera / changes FOV ----
        if(g_rotHijack || g_camHijack || g_fovHijack){
            uintptr_t cands[8]; int nc=0; cands[nc++]=cpuAddr;
            for(auto&h:hits){ if(nc>=8) break; bool dup=false; for(int k=0;k<nc;k++) if(cands[k]==h.addr) dup=true; if(!dup) cands[nc++]=h.addr; }
            if(g_rotHijack && !g_rotHijackLanded && rotationHijackSweep(cands,nc)>0){ g_rotHijackLanded=true; maybeChimeHijackSuccess(); }
            if(g_camHijack && !g_camHijackLanded && cameraHijackSweep(cands,nc)>0){ g_camHijackLanded=true; maybeChimeHijackSuccess(); }
            if(g_fovHijack && !g_fovHijackLanded && fovHijackSweep(cands[0])>0){ g_fovHijackLanded=true; maybeChimeHijackSuccess(); }
            // if the retry box is on and a requested hijack hasn't landed, keep scanning on a loop until it does
            bool allLanded=(!g_rotHijack||g_rotHijackLanded)&&(!g_camHijack||g_camHijackLanded)&&(!g_fovHijack||g_fovHijackLanded);
            if(!allLanded){
                if(g_hijackRetry){ if(!g_hijackRetryRunning) CreateThread(nullptr,0,hijackRetryThread,nullptr,0,nullptr); }
                else chimeAOBFallback("one-shot hijack didn't confirm and auto-retry is off");   // still signal via the AOB chime
            }
        }
        Log("# >>> RECOMMENDED TARGET: %s+0x%llX  layout=%s  (drive THIS - the CPU source - not the GPU buffer)",cpuMod,(unsigned long long)cpuOff,bv.rowMaj?"row":"col");
    } else Log("# no CPU copy isolated - the GPU constant-buffer route is your handle (off=0x%X size=%u slot=%d).",bv.off,bv.size,bv.slot);
    // retry loop can also run with NO locked CPU copy: it re-scans candidates from the live GPU view each round.
    if(g_hijackRetry && !g_hijackRetryRunning &&
       ((g_rotHijack && !g_rotHijackLanded) || (g_camHijack && !g_camHijackLanded) || (g_fovHijack && !g_fovHijackLanded)))
        CreateThread(nullptr,0,hijackRetryThread,nullptr,0,nullptr);
    Log("# CONFIDENCE: ortho=yes draws=%u freq=%u cpu_copies=%d view_verified=%s writer_instrs=%d spin_test=%s",
        bv.draws,bv.freq,(int)hits.size(),cpuAddr?(g_lastVerified?"YES":"no/na"):"n/a",g_writerN,cpuAddr?"run":"skipped");
    report();
    turnkeySpec(&bv,cpuAddr,cpuMod,cpuOff,cpuAddr!=0);
    emitProfile(cpuMod,(unsigned long long)cpuOff,g_lastVerified!=0);   // consolidated machine-readable bridge -> runtime
    if(g_wReg>=0) notifyExtractionDone();   // usable AOB -> sound + stop auto-movement
    Log("################### AUTO-PIPELINE END ###################");
    g_pipelineDone=true; g_pipelineRunning=false;
}
static DWORD WINAPI pipelineThread(LPVOID){ runPipeline(); return 0; }
static DWORD WINAPI snapshotThread(LPVOID){ snapshotScan(); return 0; }

// ----------------------------------------------------------------- vtable patch + installers
static void patch(void** vt,int i,void* fn,void** orig){ DWORD op; if(VirtualProtect(&vt[i],sizeof(void*),PAGE_EXECUTE_READWRITE,&op)){ *orig=vt[i]; vt[i]=fn; VirtualProtect(&vt[i],sizeof(void*),op,&op);} }

static bool install11(){
    HWND hw=GetForegroundWindow(); if(!hw)hw=GetDesktopWindow();
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount=1; sd.BufferDesc.Width=8; sd.BufferDesc.Height=8; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=hw; sd.SampleDesc.Count=1; sd.Windowed=TRUE;
    IDXGISwapChain* sc=nullptr; ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr; D3D_FEATURE_LEVEL fl;
    if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&sc,&dev,&fl,&ctx))) return false;
    void** v=*(void***)ctx;
    patch(v,7,(void*)&hkVSSetCB,(void**)&oVSSetCB);
    patch(v,12,(void*)&hkDrawIdx,(void**)&oDrawIdx);
    patch(v,13,(void*)&hkDraw,(void**)&oDraw);
    patch(v,20,(void*)&hkDrawIdxInst,(void**)&oDrawIdxInst);
    patch(v,21,(void*)&hkDrawInst,(void**)&oDrawInst);
    patch(v,14,(void*)&hkMap11,(void**)&oMap11); patch(v,15,(void*)&hkUnmap11,(void**)&oUnmap11); patch(v,48,(void*)&hkUpd11,(void**)&oUpd11);
    void** sv=*(void***)sc; patch(sv,8,(void*)&hkPresent,(void**)&oPresent);
    ctx->Release(); dev->Release(); sc->Release(); return true;
}
static bool install12(){
    HMODULE m=GetModuleHandleW(L"d3d12.dll"); if(!m) return false;
    auto pCreate=(decltype(&D3D12CreateDevice))GetProcAddress(m,"D3D12CreateDevice"); if(!pCreate) return false;
    ID3D12Device* dev=nullptr; if(FAILED(pCreate(nullptr,D3D_FEATURE_LEVEL_11_0,__uuidof(ID3D12Device),(void**)&dev))||!dev) return false;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width=256; rd.Height=1; rd.DepthOrArraySize=1; rd.MipLevels=1;
    rd.Format=DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count=1; rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res=nullptr;
    if(FAILED(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,__uuidof(ID3D12Resource),(void**)&res))||!res){ dev->Release(); return false; }
    void** v=*(void***)res; patch(v,8,(void*)&hkMap12,(void**)&oMap12); patch(v,9,(void*)&hkUnmap12,(void**)&oUnmap12);
    res->Release(); dev->Release(); return true;
}
static bool install10(){
    HMODULE m=GetModuleHandleW(L"d3d10.dll"); if(!m) return false;
    auto pCreate=(decltype(&D3D10CreateDeviceAndSwapChain))GetProcAddress(m,"D3D10CreateDeviceAndSwapChain"); if(!pCreate) return false;
    HWND hw=GetForegroundWindow(); if(!hw)hw=GetDesktopWindow();
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount=1; sd.BufferDesc.Width=8; sd.BufferDesc.Height=8; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=hw; sd.SampleDesc.Count=1; sd.Windowed=TRUE;
    IDXGISwapChain* sc=nullptr; ID3D10Device* dev=nullptr;
    if(FAILED(pCreate(nullptr,D3D10_DRIVER_TYPE_HARDWARE,nullptr,0,D3D10_SDK_VERSION,&sd,&sc,&dev))||!dev) return false;
    void** dv=*(void***)dev; patch(dv,34,(void*)&hkUpd10,(void**)&oUpd10);
    D3D10_BUFFER_DESC bd{}; bd.ByteWidth=256; bd.Usage=D3D10_USAGE_DYNAMIC; bd.BindFlags=D3D10_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags=D3D10_CPU_ACCESS_WRITE;
    ID3D10Buffer* buf=nullptr; if(SUCCEEDED(dev->CreateBuffer(&bd,nullptr,&buf))&&buf){ void** bv=*(void***)buf; patch(bv,10,(void*)&hkMap10,(void**)&oMap10); patch(bv,11,(void*)&hkUnmap10,(void**)&oUnmap10); buf->Release(); }
    dev->Release(); sc->Release(); return true;
}
static bool install9(){
    HMODULE m=GetModuleHandleW(L"d3d9.dll"); if(!m) return false;
    auto pCreate=(IDirect3D9*(WINAPI*)(UINT))GetProcAddress(m,"Direct3DCreate9"); if(!pCreate) return false;
    IDirect3D9* d3d=pCreate(D3D_SDK_VERSION); if(!d3d) return false;
    HWND hw=GetForegroundWindow(); if(!hw)hw=GetDesktopWindow();
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat=D3DFMT_UNKNOWN; pp.hDeviceWindow=hw;
    IDirect3DDevice9* dev=nullptr;
    if(FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,hw,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED,&pp,&dev))||!dev){ d3d->Release(); return false; }
    void** v=*(void***)dev; patch(v,44,(void*)&hkSetTrans,(void**)&oSetTrans); patch(v,94,(void*)&hkSetVSC,(void**)&oSetVSC);
    dev->Release(); d3d->Release(); return true;
}

// ----------------------------------------------------------------- OpenGL (IAT hooks - pointer swaps)
// GL has no vtables. Fixed-function matrices arrive via glLoadMatrixf/glMultMatrixf/glLoadMatrixd;
// modern shader matrices via glUniformMatrix4fv (resolved through wglGetProcAddress). We IAT-hook the
// opengl32 imports in every loaded module (safe - just swapping pointers) and feed matrices to scanBuffer.
typedef void (WINAPI* glLoadf_t)(const float*);
typedef void (WINAPI* glMultf_t)(const float*);
typedef void (WINAPI* glLoadd_t)(const double*);
typedef PROC (WINAPI* wglGPA_t)(LPCSTR);
typedef BOOL (WINAPI* wglSwap_t)(HDC);
typedef void (WINAPI* glUnifM4_t)(int,int,unsigned char,const float*);
static glLoadf_t oGlLoadf=nullptr; static glMultf_t oGlMultf=nullptr; static glLoadd_t oGlLoadd=nullptr;
static wglGPA_t oWglGPA=nullptr; static wglSwap_t oWglSwap=nullptr; static glUnifM4_t oGlUnifM4=nullptr;
static void WINAPI hkGlLoadf(const float* m){ if(m&&Readable(m,64)) scanBuffer((const uint8_t*)m,64,0,-1); if(oGlLoadf)oGlLoadf(m); }
static void WINAPI hkGlMultf(const float* m){ if(m&&Readable(m,64)) scanBuffer((const uint8_t*)m,64,0,-1); if(oGlMultf)oGlMultf(m); }
static void WINAPI hkGlLoadd(const double* d){ if(d&&Readable(d,128)){ float f[16]; for(int i=0;i<16;i++)f[i]=(float)d[i]; scanBuffer((const uint8_t*)f,64,0,-1);} if(oGlLoadd)oGlLoadd(d); }
static void WINAPI hkGlUnifM4(int loc,int cnt,unsigned char tr,const float* v){
    if(v&&cnt>=1&&Readable(v,(size_t)cnt*64)) for(int i=0;i<cnt&&i<8;i++) scanBuffer((const uint8_t*)(v+i*16),64,0,-1);
    if(oGlUnifM4)oGlUnifM4(loc,cnt,tr,v); }
static BOOL WINAPI hkWglSwap(HDC h){ g_frame++; return oWglSwap?oWglSwap(h):FALSE; }
static PROC WINAPI hkWglGPA(LPCSTR name){ PROC p=oWglGPA?oWglGPA(name):nullptr;
    if(name && p){ if(!strcmp(name,"glUniformMatrix4fv")){ if(!oGlUnifM4)oGlUnifM4=(glUnifM4_t)p; return (PROC)&hkGlUnifM4; } }
    return p; }
static bool iatHook(HMODULE mod,const char* dll,const char* fn,void* newFn,void** orig){
    uint8_t* base=(uint8_t*)mod; auto dos=(IMAGE_DOS_HEADER*)base; if(!base||dos->e_magic!=IMAGE_DOS_SIGNATURE) return false;
    auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew); if(nt->Signature!=IMAGE_NT_SIGNATURE) return false;
    auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if(!dir.VirtualAddress) return false;
    auto imp=(IMAGE_IMPORT_DESCRIPTOR*)(base+dir.VirtualAddress); bool done=false;
    for(;imp->Name;imp++){ const char* dn=(const char*)(base+imp->Name); if(_stricmp(dn,dll)!=0) continue;
        auto oft=(IMAGE_THUNK_DATA*)(base+(imp->OriginalFirstThunk?imp->OriginalFirstThunk:imp->FirstThunk));
        auto ft=(IMAGE_THUNK_DATA*)(base+imp->FirstThunk);
        for(;oft->u1.AddressOfData;oft++,ft++){ if(oft->u1.Ordinal&IMAGE_ORDINAL_FLAG) continue;
            auto ibn=(IMAGE_IMPORT_BY_NAME*)(base+oft->u1.AddressOfData);
            if(strcmp((const char*)ibn->Name,fn)==0){ DWORD op;
                if(VirtualProtect(&ft->u1.Function,sizeof(void*),PAGE_READWRITE,&op)){
                    if(orig&&!*orig)*orig=(void*)(uintptr_t)ft->u1.Function; ft->u1.Function=(uintptr_t)newFn;
                    VirtualProtect(&ft->u1.Function,sizeof(void*),op,&op); done=true; } } } }
    return done;
}
static bool installGL(){
    HMODULE gl=GetModuleHandleW(L"opengl32.dll"); if(!gl) return false;
    oGlLoadf=(glLoadf_t)GetProcAddress(gl,"glLoadMatrixf"); oGlMultf=(glMultf_t)GetProcAddress(gl,"glMultMatrixf");
    oGlLoadd=(glLoadd_t)GetProcAddress(gl,"glLoadMatrixd"); oWglGPA=(wglGPA_t)GetProcAddress(gl,"wglGetProcAddress");
    oWglSwap=(wglSwap_t)GetProcAddress(gl,"wglSwapBuffers");
    HMODULE mods[512]; DWORD need=0; int hooked=0;
    if(EnumProcessModules(GetCurrentProcess(),mods,sizeof(mods),&need)){ int n=need/sizeof(HMODULE);
        for(int i=0;i<n;i++){
            if(iatHook(mods[i],"opengl32.dll","glLoadMatrixf",(void*)&hkGlLoadf,(void**)&oGlLoadf)) hooked++;
            iatHook(mods[i],"opengl32.dll","glMultMatrixf",(void*)&hkGlMultf,(void**)&oGlMultf);
            iatHook(mods[i],"opengl32.dll","glLoadMatrixd",(void*)&hkGlLoadd,(void**)&oGlLoadd);
            if(iatHook(mods[i],"opengl32.dll","wglGetProcAddress",(void*)&hkWglGPA,(void**)&oWglGPA)) hooked++;
            iatHook(mods[i],"opengl32.dll","wglSwapBuffers",(void*)&hkWglSwap,(void**)&oWglSwap);
        } }
    Log("# OpenGL: IAT hooks placed (matrix-feeding entrypoints found in %d module import(s)).",hooked);
    return true;   // even with 0 static imports, the CPU memory pipeline (correlate/chain/spin) still works
}

// ----------------------------------------------------------------- fingerprint + API detect
static bool g_unity=false, g_il2cpp=false, g_mono=false, g_wrappedSwap=false, g_overlayTool=false;
static void detectFingerprint(){
    char exe[MAX_PATH]={0}; GetModuleFileNameA(nullptr,exe,MAX_PATH);
    const char* b=exe; for(char* p=exe;*p;++p) if(*p=='\\'||*p=='/') b=p+1; strncpy(g_game,b,sizeof(g_game)-1);
    // engine by loaded module (most reliable)
    struct{const wchar_t* dll;const char* e;} K[]={
        {L"UnityPlayer.dll","Unity"},{L"GameAssembly.dll","Unity (IL2CPP)"},
        {L"via.dll","RE Engine"},{L"engine2.dll","Source 2"},{L"tier0.dll","Source"},
        {L"dunia.dll","Dunia (Far Cry)"},{L"engine_x64_rwdi.dll","Chrome (Dying Light)"},
        {L"CrySystem.dll","CryEngine"},{L"GameFramework.dll","CryEngine"},{L"d3d12core.dll",nullptr},
        {L"oo2core_9_win64.dll",nullptr}};
    for(auto&k:K) if(k.e && GetModuleHandleW(k.dll)){ strncpy(g_engine,k.e,sizeof(g_engine)-1); break; }
    // managed runtime (Unity): IL2CPP vs Mono
    if(GetModuleHandleW(L"UnityPlayer.dll")||GetModuleHandleW(L"GameAssembly.dll")){ g_unity=true;
        if(GetModuleHandleW(L"GameAssembly.dll")) g_il2cpp=true;
        if(GetModuleHandleW(L"mono-2.0-bdwgc.dll")||GetModuleHandleW(L"monobleedingedge.dll")) g_mono=true; }
    // engine by exe name (fallbacks when no telltale module)
    auto has=[&](const char* s){ return strstr(exe,s)!=nullptr; };
    // upscaler / frame-generation wrappers insert a PROXY swapchain - a Present-vtable hook may grab the wrapper's
    // swapchain (or crash on hotsample/resize). Detect them so we can warn and prefer the CPU pipeline if needed.
    const wchar_t* W[]={L"sl.interposer.dll",L"sl.dlss_g.dll",L"nvngx_dlssg.dll",L"libxess.dll",L"libxess_dx11.dll",
        L"amd_fidelityfx_dx12.dll",L"amd_fidelityfx_vk.dll",L"ffx_framegeneration.dll",L"dlssg_to_fsr3_amd_is_better.dll"};
    for(auto w:W) if(GetModuleHandleW(w)){ g_wrappedSwap=true; break; }
    // overlays/profilers (RivaTuner/Afterburner, etc.) can hold the hardware debug registers or hook Present,
    // which can make the write-watch miss its writer or destabilize a present hook. Detect and warn.
    const wchar_t* O[]={L"RTSSHooks64.dll",L"RTSSHooks.dll",L"RTSS.dll",L"fraps64.dll",L"overlay.dll"};
    for(auto o:O) if(GetModuleHandleW(o)){ g_overlayTool=true; break; }
    if(g_engine[0]=='n'){ // still "native/unknown"
        if(has("-Win64-Shipping")||has("-WinGDK-Shipping")) strncpy(g_engine,"Unreal Engine 4/5",sizeof(g_engine)-1);
        else if(has("Cyberpunk2077")) strncpy(g_engine,"REDengine 4",sizeof(g_engine)-1);
        else if(has("witcher3")||has("witcher")) strncpy(g_engine,"REDengine 3",sizeof(g_engine)-1);
        else if(has("GTA5")||has("RDR2")) strncpy(g_engine,"RAGE",sizeof(g_engine)-1);
        else if(has("SkyrimSE")||has("Fallout4")||has("Starfield")) strncpy(g_engine,"Creation",sizeof(g_engine)-1);
        else if(has("eldenring")||has("DarkSouls")||has("sekiro")) strncpy(g_engine,"FromSoft (Dantelion)",sizeof(g_engine)-1);
    }
}
static volatile bool g_vk=false;
// engine/API-tailored guidance: route each game to the method most likely to actually drive its camera.
static void recommendApproach(){
    Log("---- RECOMMENDED APPROACH (engine=%s api=%s) ----",g_engine,g_api);
    if(g_unity){
        Log("# UNITY: the reliable camera is MANAGED - drive Camera.worldToCameraMatrix (the view matrix) each");
        Log("#   frame from a %s shim (BepInEx/Harmony), never the Transform - leaving the transform alone keeps",g_il2cpp?"IL2CPP (GameAssembly)":(g_mono?"Mono":"managed"));
        Log("#   aim decoupled for free. Hook a render callback (RenderPipelineManager.beginCameraRendering for");
        Log("#   SRP/Unity6, or Camera LateUpdate for built-in RP). GPU-side the view is unity_MatrixV in the");
        Log("#   UnityPerCamera cbuffer, but most shaders use the baked unity_MatrixVP, so editing the GPU view");
        Log("#   often won't move the scene - prefer the managed worldToCameraMatrix route.");
    } else if(strstr(g_engine,"RE Engine")){
        Log("# RE ENGINE: use the type database (find_type(\"via.Camera\"), get GameObject/Transform) to reach the");
        Log("#   camera Transform and edit its world matrix from the camera-controller update hook; no AOB needed.");
    } else if(strstr(g_engine,"Unreal")){
        Log("# UNREAL: CPU side, the view comes from FMinimalViewInfo/APlayerCameraManager (pattern-scan the camera");
        Log("#   manager update); GPU side a standalone view CB usually exists. Either the write-site cave or a");
        Log("#   function hook on the camera-manager update works. FOV is a field on the camera manager.");
    } else if(strstr(g_engine,"REDengine")||strstr(g_engine,"RAGE")){
        Log("# QUATERNION/SINGLETON ENGINE: camera is quaternion+position reached via a global singleton. Recover");
        Log("#   the singleton from the writer function's RIP-relative globals (STATIC-ROOT, below), then chain to it.");
    } else if(strstr(g_engine,"Creation")){
        Log("# CREATION: camera is a scene-graph node (NiNode world/local 3x3 + a worldToCam matrix). Hook the");
        Log("#   camera update, write BOTH the node and the render camera; decouple aim by save/restore around the tick.");
    }
    if(g_vk){ Log("# VULKAN: this is NOT a limitation. Camera control is CPU-side and API-independent - shipped Vulkan camera");
        Log("#   tools drive the camera by hijacking its CPU struct (AOB + code cave), and only use a graphics hook for an");
        Log("#   on-screen overlay (often a SEPARATE D3D swapchain), which you don't need to build a head-tracking mod.");
        Log("#   So the CPU pipeline here (F7->move->F8, then write-watch -> FN-HOOK + STATIC-ROOT) is the COMPLETE solution."); }
    if(g_wrappedSwap){ Log("# UPSCALER/FRAME-GEN WRAPPER DETECTED (Streamline/DLSS-G/XeSS/FSR3): a proxy swapchain is in front of the");
        Log("#   real one - a Present-vtable GPU hook may grab the wrapper (or crash on hotsample). Prefer the CPU pipeline");
        Log("#   (F7/F8 + write-watch), or resolve the REAL swapchain behind the proxy before hooking Present."); }
    if(g_overlayTool) Log("# OVERLAY/PROFILER DETECTED (RivaTuner/Afterburner/FRAPS): it can hold the hardware debug registers or hook"
        " Present - if the write-watch catches nothing or things destabilize, close it and retry.");
    Log("# GPU-CB TIMING: if you inject into the GPU view buffer, write at Present (before post/ReShade) and consistently");
    Log("#   each frame - writing after post-processing or at a varying point causes flicker/misaligned frames.");
    Log("# ANY API/engine fallback: F7->move->F8 (differential) isolates the live camera in CPU memory, then the");
    Log("#   write-watch resolves the writer FUNCTION + a STATIC-ROOT global - works the same on D3D/GL/Vulkan.");
    Log("# NOTE: the GPU constant-buffer hook is just a FAST FINDER for D3D games - camera control itself is always");
    Log("#   CPU-side, so no game-renderer graphics hook is required to build the mod on any API.");
}
static const char* detectAndInstall(){
    if(GetModuleHandleW(L"d3d12.dll") && install12()){ strcpy(g_api,"D3D12"); install11(); return "D3D12 (+D3D11 fallback)"; }
    if(GetModuleHandleW(L"d3d11.dll") && install11()){ strcpy(g_api,"D3D11"); return "D3D11"; }
    if(GetModuleHandleW(L"d3d10.dll") && install10()){ strcpy(g_api,"D3D10"); return "D3D10"; }
    if(GetModuleHandleW(L"d3d9.dll")  && install9()){  strcpy(g_api,"D3D9");  return "D3D9"; }
    if(GetModuleHandleW(L"opengl32.dll") && installGL()){ strcpy(g_api,"OpenGL"); return "OpenGL"; }
    if(GetModuleHandleW(L"vulkan-1.dll")){ strcpy(g_api,"Vulkan"); g_vk=true; return "Vulkan (camera is CPU-side: F7/F8 + write-watch is the full route)"; }
    if(install11()){ strcpy(g_api,"D3D11?"); return "D3D11 (guess)"; }
    return nullptr;
}

// ----------------------------------------------------------------- timer thread (report + key + frame window)
static volatile int g_reportEverySec=10;
// GUI checkbox bridge: the loader drops "6DOF.cfg" next to the probe DLL; if it asks for auto deep scans,
// we chain the normally-hotkey-driven passes (memory scan + F7/F8 differential + report) right after the
// main pipeline detects & logs the camera - no keypresses needed.
static volatile bool g_autoExtra=false;
static void readConfig(){
    wchar_t dll[MAX_PATH]={0}; GetModuleFileNameW(g_self,dll,MAX_PATH);
    wchar_t* s=wcsrchr(dll,L'\\'); if(!s) return; *(s+1)=0;
    wchar_t cfg[MAX_PATH]; wcscpy_s(cfg,MAX_PATH,dll); wcscat_s(cfg,MAX_PATH,L"6DOF.cfg");
    FILE* f=_wfopen(cfg,L"rb"); if(!f) return;
    char buf[256]={0}; size_t n=fread(buf,1,sizeof(buf)-1,f); fclose(f); buf[n<sizeof(buf)?n:sizeof(buf)-1]=0;
    if(strstr(buf,"auto_extra_tests=1")) g_autoExtra=true;
    g_autoDriveMouse = g_autoExtra && !strstr(buf,"auto_mouse=0");   // the deep-scan checkbox also drives the mouse (opt-out: auto_mouse=0)
    if(strstr(buf,"auto_wasd=1")) g_autoWASD=true;                   // second checkbox: auto-move the character with WASD
    if(strstr(buf,"aggressive=1")) g_aggressive=true;               // 3rd log + stronger AOB/FOV hunting
    // the hijacks + retry default ON; the GUI disables them with an explicit =0 (a CLI/no-cfg run keeps them on)
    if(strstr(buf,"rot_hijack=0")) g_rotHijack=false;
    if(strstr(buf,"cam_hijack=0")) g_camHijack=false;
    if(strstr(buf,"fov_hijack=0")) g_fovHijack=false;
    if(strstr(buf,"hijack_retry=0")) g_hijackRetry=false;
    if(strstr(buf,"active_move_test=1")) g_activeMoveTest=true;   // opt-in: actively perturb the camera to confirm by moving (can crash some games)
}
static DWORD WINAPI autoExtraThread(LPVOID){
    Log(""); Log("################### AUTO DEEP SCANS (enabled in the loader) ###################");
    Log("# AUTO-MOUSE=%s  AUTO-WASD=%s. Will RETRY until the camera writer (AOB) is captured.",g_autoDriveMouse?"ON":"off",g_autoWASD?"ON":"off");
    Sleep(600);
    Log("# memory scan (HOME) ...");  memScan();
    const int MAXTRY=6;
    for(int attempt=1; attempt<=MAXTRY && !g_extractDone; attempt++){
        Log(""); Log("########## DEEP-SCAN ATTEMPT %d / %d ##########",attempt,MAXTRY);
        Log("# differential baseline snapshot (F7) ..."); snapshotScan();
        if(g_autoDriveMouse){
            if(attempt==1){ Log("# >>> AUTO-INPUT: the app moves the camera ITSELF. Focus the GAME WINDOW, be in gameplay. Starting in 2s... <<<");
                for(int i=2;i>0;i--){ Log("#   starting in %d ...",i); Sleep(1000); } }
            randomExercise(10);     // randomized, logged camera (+WASD) motion - fresh each attempt picks a fresh live instance
            autoTurn(0,1500,26);    // fast displacement so the differential ends clearly moved
        } else {
            Log("# >>> MOVE / ROTATE the camera continuously for ~6s so the differential can isolate it <<<");
            for(int i=6;i>0;i--){ Log("#   capturing motion delta in %d ...",i); Sleep(1000); }
        }
        Log("# differential delta (F8) + write-watch ..."); deltaScan();
        if(g_extractDone){ Log("# >>> SUCCESS on attempt %d - writer/AOB captured (sound played). <<<",attempt); break; }
        Log("# attempt %d caught no writer (that camera instance was transient/stale). Retrying with FRESH motion.",attempt);
        Log("#   >>> You can ALSO move the camera / character MANUALLY right now - the next write-watch catches YOUR motion too,");
        Log("#       and the sound will play whether the capture comes from the app's movement or yours. <<<");
        Sleep(400);
    }
    if(!g_extractDone){
        Log("# Auto-capture didn't land after %d attempts (camera is heavily transient/pooled).",MAXTRY);
        Log("#   MANUAL FINISH: move the view continuously and press F8 (or INSERT to re-run the whole pipeline).");
        Log("#   When the writer is caught you'll hear the SAME completion sound. The camera matrix is at the address in the profile.");
    }
    Log("# full report (END) ...");  report();
    Log("################### AUTO DEEP SCANS COMPLETE ###################");
    return 0;
}

static DWORD WINAPI ticker(LPVOID){
    uint64_t last=GetTickCount64(); bool pe=false,ph=false,pi2=false,pf7=false,pf8=false; bool autoExtraStarted=false;
    while(true){
        Sleep(16);
        g_frame++;
        { std::lock_guard<std::mutex> lk(g_freqMx); g_freq.clear(); }
        scanUploads12();
        // PER-FRAME log (2nd log): a few times a second, once a live VIEW exists, trace the camera position.
        // (bestViewEntry returns false until a view is catalogued, so this naturally stays quiet pre-detection.)
        if((g_frame%15)==0){ Entry pv; if(bestViewEntry(pv)){ float fovv=readLiveFovV();
            LogPF("frame=%llu  campos=%.2f,%.2f,%.2f  fovV=%.2f  draws=%u slot=%d off=0x%X",
                (unsigned long long)g_frame,pv.campos[0],pv.campos[1],pv.campos[2],fovv,pv.draws,pv.slot,pv.off); } }
        // AUTOMATIC: run the discovery pipeline ~5s after injection, then RETRY every ~15s until a camera is
        // actually isolated (on D3D12/pooled titles the player needs to be moving for the differential to lock).
        if(!g_pipelineDone && !g_pipelineRunning && (g_frame==300 || (g_frame%900==0))){ CreateThread(nullptr,0,pipelineThread,nullptr,0,nullptr); }
        // AUTOMATIC (opt-in via loader checkbox): after the main pipeline finishes, chain the deep passes once.
        if(g_autoExtra && g_pipelineDone && !g_pipelineRunning && !autoExtraStarted){ autoExtraStarted=true; CreateThread(nullptr,0,autoExtraThread,nullptr,0,nullptr); }
        bool e=(GetAsyncKeyState(VK_END)&0x8000)!=0; if(e&&!pe){ g_spinUsed=true; report(); } pe=e;
        bool hk=(GetAsyncKeyState(VK_HOME)&0x8000)!=0; if(hk&&!ph){ memScan(); } ph=hk;
        bool ik=(GetAsyncKeyState(VK_INSERT)&0x8000)!=0; if(ik&&!pi2&&!g_pipelineRunning){ g_pipelineDone=false; CreateThread(nullptr,0,pipelineThread,nullptr,0,nullptr); } pi2=ik;
        bool f7=(GetAsyncKeyState(VK_F7)&0x8000)!=0; if(f7&&!pf7){ CreateThread(nullptr,0,snapshotThread,nullptr,0,nullptr); } pf7=f7;
        bool f8=(GetAsyncKeyState(VK_F8)&0x8000)!=0; if(f8&&!pf8){ deltaScan(); } pf8=f8;
        uint64_t now=GetTickCount64(); if(now-last>=(uint64_t)g_reportEverySec*1000){ last=now; report(); }
    }
}

static DWORD WINAPI worker(LPVOID){
    resolveLogPath();
    readConfig();
    Log("================ 6DOF Probe injected  v5.16 (writer-selection now REJECTS stack register-spills (movaps [rsp+disp]) and PREFERS the store that targets the locked camera / view-matrix pool, so a hot spill can no longer masquerade as the camera locator; differential also boosts movers inside the proven pool; active CPU move-test is now OPT-IN and gentle - default is the safe read-only capture + chime, since writing into a live game can crash it; CPU observation oracle; fallback chime now fires on CPU/D3D12 titles when a usable AOB is captured - no GPU oracle needed; AUTOMATIC differential on no-GPU-view titles + sliding-window pool page-guard; D3D12/Vulkan CPU projection oracle + pooled-camera writer capture: auto-guards the whole view-matrix pool region and watches the matrix mover so transient cameras like Alan Wake get an AOB; was cave-less capture by default: the runtime now captures the camera pointer via a HARDWARE BREAKPOINT - no game code modified - with a hardened inline cave fallback that relocates rip-relative bytes, patches stop-the-world, and uninstalls cleanly; plus the v5.8 reloc/.pdata signatures and projection-solved FOV)  (build %s %s) ================",__DATE__,__TIME__);
    if(g_aggressive){ LogAgg("================ 6DOF AGGRESSIVE LOG  (build %s %s) ================",__DATE__,__TIME__);
        LogAgg("# This log only exists because the loader's \"Aggressive deep probe\" box was checked. It carries the harder-hitting AOB/FOV hunt and the camera/FOV hijack detail."); }
    LogPF("================ 6DOF PER-FRAME LOG  (build %s %s) ================",__DATE__,__TIME__);
    LogPF("# Continuous per-frame camera trace (campos / FOV / draw counts) + active-test progress. Lighter than the main log.");
    detectFingerprint();
    Log("game=%s  arch=%d-bit  engine=%s  auto_deep_scans=%s  auto_mouse=%s  auto_wasd=%s",g_game,(int)(sizeof(void*)*8),g_engine,g_autoExtra?"ON":"off",g_autoDriveMouse?"ON":"off",g_autoWASD?"ON (app moves the character)":"off");
    Log("LOGS: main=6DOF-<game>.log  per-frame=6DOF-<game>.perframe.log  aggressive=%s%s",g_aggressive?"6DOF-<game>.aggressive.log":"(off - tick 'Aggressive deep probe')",
        g_camHijack||g_fovHijack?"   HIJACK:":"");
    if(g_rotHijack) Log("  ROTATION HIJACK ON: will rotate each candidate (yaw/pitch/roll) and log which one turns the camera (all three logs).");
    if(g_camHijack) Log("  CAMERA PLACEMENT HIJACK ON: will nudge each candidate's X/Y/Z and log which one moves the camera (all three logs).");
    if(g_fovHijack) Log("  FOV HIJACK ON: will drive each FOV candidate up/down and log which one changes the rendered FOV (all three logs).");
    if(g_hijackRetry) Log("  HIJACK AUTO-RETRY ON: will loop + re-scan until a candidate ACTUALLY rotates/moves the camera / changes the FOV, then log the LANDING (chime).");
    if(g_rotHijack||g_camHijack||g_fovHijack) Log("  NOTE: the success CHIME is reserved for a CONFIRMED hijack landing (the camera proven by a live rotation/placement test) - the ordinary AOB capture stays SILENT (a fallback chime covers no-oracle titles).");
    const char* picked=nullptr;
    for(int i=0;i<600 && !picked;i++){ picked=detectAndInstall(); if(!picked) Sleep(100); }
    if(picked){ Log("API=%s  hooks installed - capturing.",picked);
        Log("AUTO: full discovery pipeline (select->correlate GPU/CPU->pointer-chain->write-AOB->spin-test) runs ~5s after injection.");
        Log("KEYS: INSERT=re-run pipeline   END=report now   HOME=memory scan   F7=snapshot / F8=delta (find camera by motion).  Report also auto-writes every %ds.",g_reportEverySec);
        recommendApproach();
        CreateThread(nullptr,0,controllerThread,nullptr,0,nullptr);   // poll the gamepad + correlate to the camera
        CreateThread(nullptr,0,ticker,nullptr,0,nullptr); }
    else { Log("ERROR: no graphics API hooked - the API-independent CPU pipeline still works: press F7, move the view, F8.");
        recommendApproach(); CreateThread(nullptr,0,controllerThread,nullptr,0,nullptr); CreateThread(nullptr,0,ticker,nullptr,0,nullptr); }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){ g_self=h; DisableThreadLibraryCalls(h); CreateThread(nullptr,0,worker,nullptr,0,nullptr); }
    return TRUE;
}
