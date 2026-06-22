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

static void classifyInto(Entry& e){
    bool rm; float fy,fx,asp,zn,zf;
    if(classifyProj(e.m,rm,fy,fx,asp,zn,zf)){ e.kind=1;e.rowMaj=rm;e.fovY=fy;e.fovX=fx;e.aspect=asp;e.zn=zn;e.zf=zf; return; }
    if(ortho3x3(e.m)&&!identityish(e.m)&&!axisAligned(e.m)){ e.kind=2; e.rowMaj=(fabsf(e.m[3])+fabsf(e.m[7]))<(fabsf(e.m[12])+fabsf(e.m[13])); cameraPos(e.m,e.campos); return; }
    int z=0; for(int i=0;i<16;i++) if(fabsf(e.m[i])<1e-6f)z++;
    bool wRow=fabsf(e.m[3])>1e-3f||fabsf(e.m[7])>1e-3f||fabsf(e.m[11])>1e-3f||fabsf(e.m[15]-1.f)>1e-3f;
    if(z<8&&wRow&&!identityish(e.m)){ e.kind=3;e.rowMaj=true; return; } e.kind=0;
}
static void scanBuffer(const uint8_t* data,size_t size,uint32_t draws=0,int slot=-1){
    if(!data||size<64||!Readable(data,size)) return; size_t lim=size-64;
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
    // main view: top draw-weighted view with a real (non-origin) camera position.
    const Entry* bV=nullptr; for(auto&e:view){ float p=fabsf(e.campos[0])+fabsf(e.campos[1])+fabsf(e.campos[2]); if(p>1.f){bV=&e;break;} } if(!bV&&!view.empty())bV=&view[0];
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
    sz=bd.ByteWidth; return (bd.BindFlags&D3D11_BIND_CONSTANT_BUFFER)&&bd.ByteWidth>=64&&bd.ByteWidth<=65536; }
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
    while(p<end && budget>0 && (viewN+projN)<60){
        MEMORY_BASIC_INFORMATION mbi; if(!VirtualQuery(p,&mbi,sizeof(mbi))) break;
        uint8_t* base=(uint8_t*)mbi.BaseAddress; size_t rsz=mbi.RegionSize;
        bool rw = mbi.State==MEM_COMMIT && (mbi.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE)) && !(mbi.Protect&PAGE_GUARD);
        if(rw && rsz<=(64u<<20)){
            size_t scan=rsz>budget?budget:rsz; budget-=scan;
            for(size_t o=0;o+64<=scan;o+=16){            // matrices are 16-byte aligned in practice
                const float* m=(const float*)(base+o);
                if(!finite16(m)) continue;
                bool z=true; for(int i=0;i<16;i++) if(m[i]!=0){z=false;break;} if(z) continue;
                Entry e; memcpy(e.m,m,64); classifyInto(e);
                if(e.kind==2){ char mod[80]; uintptr_t off; moduleOf((void*)(base+o),mod,sizeof(mod),off);
                    float pi,ya,ro; eulerFromBasis(e.m,pi,ya,ro);
                    Log("# MEM VIEW @ %s+0x%llX  campos=%.1f,%.1f,%.1f euler=%.1f,%.1f,%.1f",mod,(unsigned long long)off,e.campos[0],e.campos[1],e.campos[2],pi,ya,ro);
                    if(++viewN>=40) break; }
                else if(e.kind==1){ char mod[80]; uintptr_t off; moduleOf((void*)(base+o),mod,sizeof(mod),off);
                    Log("# MEM PROJ @ %s+0x%llX  fovV=%.1f aspect=%.3f",mod,(unsigned long long)off,e.fovY,e.aspect);
                    if(++projN>=20) break; }
            }
        }
        if(rsz==0) break; p=base+rsz;
    }
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
static bool isFovFloat(float f){ return (f>0.4f&&f<2.8f)||(f>25.f&&f<140.f); }   // radians ~23-160deg or degrees
// a plausible Euler-angle-in-degrees triplet (Witcher3 cam+0x80 = roll/pitch/yaw). Not a basis row.
static bool isEulerDeg(const float* e){
    for(int i=0;i<3;i++){ if(e[i]!=e[i]||fabsf(e[i])>360.5f) return false; }
    if(fabsf(e[0])<1.5f&&fabsf(e[1])<1.5f&&fabsf(e[2])<1.5f) return false;   // a normalized row has comps<=1; need a real angle
    float s=fabsf(e[0])+fabsf(e[1])+fabsf(e[2]); return s>1.5f && s<1000.f;
}
static bool looksLikePoint(const float* p){ for(int i=0;i<3;i++){ if(p[i]!=p[i]||fabsf(p[i])>1e6f) return false; } return (fabsf(p[0])+fabsf(p[1])+fabsf(p[2]))>0.5f; }
// Identify which euler axis is pitch/yaw/roll from the observed values. Cross-game priors: pitch is clamped to
// +/-90, yaw spans +/-180 (wraps), roll sits near 0. Default to the dominant X=pitch,Y=yaw,Z=roll convention, then
// override from magnitudes: an axis whose |value|>90 must be yaw; the near-zero axis is roll. A single-snapshot
// guess - mark it for the user to verify by moving the camera.
static void eulerRoles(const float* e,char out[4]){
    out[0]='P'; out[1]='Y'; out[2]='R'; out[3]=0;
    int big=-1; float bigv=90.f; for(int i=0;i<3;i++){ if(fabsf(e[i])>bigv){bigv=fabsf(e[i]);big=i;} }
    int zero=-1; float zv=2.f;    for(int i=0;i<3;i++){ if(fabsf(e[i])<zv){zv=fabsf(e[i]);zero=i;} }
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
        if(s[p]==0xF2||s[p]==0xF3||s[p]==0x66){ pfx=s[p]; p++; }                 // mandatory SSE prefix (comes BEFORE REX)
        int rexB=0,rexR=0; if(p<n&&(s[p]&0xF0)==0x40){ rexB=s[p]&1; rexR=(s[p]>>2)&1; p++; } // optional REX
        const char* mn=nullptr; int opl=0;
        if(p+1<n&&s[p]==0x0F&&s[p+1]==0x11){ opl=2;                              // 0F 11 /r = mov[u]ps/ss/sd/upd by prefix
            mn = pfx==0xF3?"movss" : pfx==0xF2?"movsd" : pfx==0x66?"movupd" : "movups"; }
        else if(p+1<n&&s[p]==0x0F&&s[p+1]==0x29&&pfx==0){ mn="movaps"; opl=2; }
        else if(p+1<n&&s[p]==0x0F&&s[p+1]==0x7F&&pfx==0x66){ mn="movdqa"; opl=2; }
        else continue;
        int mp=p+opl; if(mp>=n) continue; uint8_t modrm=s[mp]; int mod=modrm>>6,rm=modrm&7;
        if(rm==4||mod==3) continue;                                            // SIB / reg-reg: skip
        int base=rm|(rexB<<3),dd=0,end=mp+1;
        if(mod==1&&mp+1<n){ dd=(int8_t)s[mp+1]; end=mp+2; }
        else if(mod==2&&mp+4<n){ dd=*(int32_t*)(s+mp+1); end=mp+5; }
        else if(mod!=0) continue;                                              // mod==0 RIP-rel (rm==5) handled as no-disp store; skip oddities
        (void)rexR;
        if(end>bestEnd){ bestEnd=end; reg=base; disp=dd; mnem=mn; startIdx=i; got=true; }  // keep the one nearest rip
    }
    return got;
}

// ---- struct dump with field flagging + representation detection ------------------------------------
// Records what KIND of camera this is and where each field sits, so the mod author gets a layout, not a guess.
static int  g_reprMatOff=-1, g_reprQuatOff=-1, g_reprEulerOff=-1, g_reprFovOff=-1, g_reprEyeOff=-1, g_reprTgtOff=-1;
static char g_reprEulerRoles[4]={0};
static bool g_reprMatRow=true; static char g_reprKind[40]="unknown";
static void dumpStructFlags(uintptr_t addr){
    g_reprMatOff=g_reprQuatOff=g_reprEulerOff=g_reprFovOff=g_reprEyeOff=g_reprTgtOff=-1; strcpy(g_reprKind,"unknown");
    Log("# STRUCT @ %p  (scanning -0x40..+0x180; flagged fields show the camera layout):",(void*)addr);
    // scan a window starting a little before the matched field (quat/euler/fov often precede the matrix)
    for(int o=-0x40;o<0x180;o+=4){ if(!Readable((void*)(addr+o),4)) continue; const char* flag="";
        if((o&0xF)==0 && Readable((void*)(addr+o),64)){ Entry e; memcpy(e.m,(void*)(addr+o),64); classifyInto(e);
            if(e.kind==2){ flag="VIEW/WORLD matrix (4x4)"; if(g_reprMatOff<0){g_reprMatOff=o;g_reprMatRow=e.rowMaj;} }
            else if(e.kind==1){ flag="PROJECTION matrix"; } }
        if(!*flag && Readable((void*)(addr+o),16) && isUnitQuat((float*)(addr+o))){ flag="unit QUATERNION"; if(g_reprQuatOff<0)g_reprQuatOff=o; }
        if(!*flag && Readable((void*)(addr+o),12) && isEulerDeg((float*)(addr+o))){ char rl[4]; eulerRoles((float*)(addr+o),rl);
            static char eb[48]; snprintf(eb,sizeof(eb),"EULER deg [%c,%c,%c] (pitch=+/-90,yaw=+/-180,roll~0; verify)",rl[0],rl[1],rl[2]); flag=eb; if(g_reprEulerOff<0){g_reprEulerOff=o; memcpy(g_reprEulerRoles,rl,4);} }
        if(!*flag){ float f=*(float*)(addr+o); if(isFovFloat(f)){ flag=(f>0.5f&&f<1.6f)?"maybe FOV (angle, or a FACTOR of base FOV ~1.0=100%)":"maybe FOV"; if(g_reprFovOff<0)g_reprFovOff=o; } }
        if(*flag) Log("#   +0x%03X = %12.4f   <- %s",o,*(float*)(addr+o),flag);
    }
    // eye/target look-at rig: two world-points a sane distance apart (NieR/NinoKuni). Search vec3 pairs.
    for(int a=-0x40;a<0x140 && g_reprEyeOff<0;a+=4){ if(!Readable((void*)(addr+a),12))continue; float* pa=(float*)(addr+a); if(!looksLikePoint(pa))continue;
        for(int b=a+12;b<a+0x40;b+=4){ if(!Readable((void*)(addr+b),12))continue; float* pb=(float*)(addr+b); if(!looksLikePoint(pb))continue;
            float d=dist3(pa,pb); if(d>0.3f&&d<5000.f){ g_reprEyeOff=a; g_reprTgtOff=b; Log("#   +0x%03X / +0x%03X  <- possible EYE / TARGET look-at pair (dist=%.1f)",a,b,d); break; } } }
    // decide the primary representation (matrix wins; then quat; then euler; then eye/target)
    // a position vec3 commonly sits right beside the quaternion (typical pos+quat camera struct)
    if(g_reprQuatOff>=0){ for(int d=-16;d<=16;d+=4){ int o=g_reprQuatOff+d; if(d==0||!Readable((void*)(addr+o),12)) continue;
        if(looksLikePoint((float*)(addr+o))){ Log("#   +0x%03X  <- camera POSITION vec3 (beside the quaternion)",o); break; } } }
    // FOV can sit far from the matrix/quat (some engines keep it hundreds of bytes away); sweep a wider window for a FOV scalar
    if(g_reprFovOff<0){ for(int o=0x180;o<0x400;o+=4){ if(!Readable((void*)(addr+o),4))continue; float f=*(float*)(addr+o);
        if(isFovFloat(f)){ g_reprFovOff=o; Log("#   +0x%03X = %12.4f   <- maybe FOV (far field)",o,f); break; } } }
    if(g_reprMatOff>=0)        snprintf(g_reprKind,sizeof(g_reprKind),"matrix4x4 @+0x%X (%s)",g_reprMatOff,g_reprMatRow?"row":"col");
    else if(g_reprQuatOff>=0)  snprintf(g_reprKind,sizeof(g_reprKind),"quaternion @+0x%X",g_reprQuatOff);
    else if(g_reprEulerOff>=0) snprintf(g_reprKind,sizeof(g_reprKind),"euler-deg @+0x%X",g_reprEulerOff);
    else if(g_reprEyeOff>=0)   snprintf(g_reprKind,sizeof(g_reprKind),"eye/target @+0x%X/+0x%X",g_reprEyeOff,g_reprTgtOff);
    Log("# REPRESENTATION = %s%s",g_reprKind, g_reprFovOff>=0?"  (+FOV float present)":"");
}

static void armWriteWatch(void* addr,int maxSlices,const char* prompt);  // fwd decl (defined with the write-watch)
// ---- DIFFERENTIAL discovery: find the LIVE camera by what changes when you move (no GPU hook needed) ----
struct Snap{ uintptr_t addr; uint8_t type; float v[16]; };   // type 1=matrix 2=quaternion
static std::vector<Snap> g_snap; static std::mutex g_snapMx;
static void snapshotScan(){
    std::lock_guard<std::mutex> lk(g_snapMx); g_snap.clear();
    Log(""); Log("================== SNAPSHOT (differential discovery) ==================");
    SYSTEM_INFO si; GetSystemInfo(&si); uint8_t* p=(uint8_t*)si.lpMinimumApplicationAddress,*end=(uint8_t*)si.lpMaximumApplicationAddress;
    size_t budget=512u<<20; int mat=0,quat=0;
    while(p<end && budget>0 && g_snap.size()<80000){
        MEMORY_BASIC_INFORMATION mbi; if(!VirtualQuery(p,&mbi,sizeof(mbi)))break; uint8_t* base=(uint8_t*)mbi.BaseAddress; size_t rsz=mbi.RegionSize;
        bool rw=mbi.State==MEM_COMMIT&&(mbi.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE))&&!(mbi.Protect&PAGE_GUARD);
        if(rw && rsz<=(64u<<20)){ size_t scan=rsz>budget?budget:rsz; budget-=scan;
            for(size_t o=0;o+64<=scan;o+=4){ const float* f=(const float*)(base+o);
                if((o&0xF)==0){ Entry e; memcpy(e.m,f,64); classifyInto(e); if(e.kind==2){ if(g_snap.size()<80000){ g_snap.push_back({(uintptr_t)(base+o),1,{}}); memcpy(g_snap.back().v,f,64);} mat++; continue; } }
                if(isUnitQuat(f)){ if(g_snap.size()<80000){ g_snap.push_back({(uintptr_t)(base+o),2,{}}); memcpy(g_snap.back().v,f,16);} quat++; }
            }
        }
        if(rsz==0)break; p=base+rsz;
    }
    Log("# captured %d view/world matrices + %d unit-quaternions.",mat,quat);
    Log("# NOW: rotate the in-game view ~45 degrees, then press F8. Only the camera will have changed.");
    Log("======================================================================");
}
static void findChains(uintptr_t target);   // fwd
static void deltaScan(){
    std::lock_guard<std::mutex> lk(g_snapMx);
    if(g_snap.empty()){ Log("# delta: no snapshot yet - press F7, move the view, then F8."); return; }
    Log(""); Log("================== DELTA  (changed = the live camera) ==================");
    int shown=0; uintptr_t bestAddr=0; uint8_t bestType=0; float bestD=0;
    for(auto& s:g_snap){ int n=s.type==1?16:4; if(!Readable((void*)s.addr,(size_t)n*4)) continue;
        const float* f=(const float*)s.addr; float d=0; for(int i=0;i<n;i++) d+=fabsf(f[i]-s.v[i]);
        char mod[80]; uintptr_t off;
        if(s.type==1 && d>0.02f){ if(shown<24){ moduleOf((void*)s.addr,mod,sizeof(mod),off); float pi,ya,ro; eulerFromBasis(f,pi,ya,ro);
                Log("# CHANGED MATRIX @ %s+0x%llX  euler(p,y,r)=%.1f,%.1f,%.1f",mod,(unsigned long long)off,pi,ya,ro); shown++; }
            if(d>bestD){ bestD=d; bestAddr=s.addr; bestType=1; } }
        else if(s.type==2 && d>0.01f){ if(shown<24){ moduleOf((void*)s.addr,mod,sizeof(mod),off);
                Log("# CHANGED QUAT   @ %s+0x%llX  q=%.3f %.3f %.3f %.3f",mod,(unsigned long long)off,f[0],f[1],f[2],f[3]); shown++; }
            if(d>bestD){ bestD=d; bestAddr=s.addr; bestType=2; } }
    }
    if(!shown){ Log("# nothing changed enough - move the view MORE then F8, or re-snapshot (F7)."); Log("======================================================================"); return; }
    if(bestAddr){ char mod[80]; uintptr_t off; moduleOf((void*)bestAddr,mod,sizeof(mod),off);
        Log("# STRONGEST mover = %s @ %s+0x%llX -> full layout + chains:",bestType==1?"matrix":"quaternion",mod,(unsigned long long)off);
        dumpStructFlags(bestAddr); findChains(bestAddr);
        // This address JUST changed, so it is live RIGHT NOW - the best moment to catch its writer.
        armWriteWatch((void*)bestAddr,6,"# write-watch armed on the moving camera (~3s) - KEEP MOVING the view continuously...");
        Log("# >>> CPU-struct camera. Drive it directly, OR (if the address changes per frame) trampoline-hook the FN-HOOK function above.");
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
static uintptr_t g_writers[16]; static int g_writerHits[16]; static volatile int g_writerN=0; static PVOID g_veh=nullptr;
static LONG CALLBACK veh(EXCEPTION_POINTERS* ep){
    if(ep->ExceptionRecord->ExceptionCode==(DWORD)EXCEPTION_SINGLE_STEP){
#ifdef _WIN64
        uintptr_t rip=(uintptr_t)ep->ContextRecord->Rip;
#else
        uintptr_t rip=(uintptr_t)ep->ContextRecord->Eip;
#endif
        if(g_writerN<16){ int idx=-1; for(int i=0;i<g_writerN;i++) if(g_writers[i]==rip){idx=i;break;}
            if(idx>=0) g_writerHits[idx]++; else { g_writers[g_writerN]=rip; g_writerHits[g_writerN]=1; g_writerN++; } }
        else { for(int i=0;i<16;i++) if(g_writers[i]==rip){ g_writerHits[i]++; break; } }   // keep counting hits once full
        ep->ContextRecord->Dr6=0; return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void setBPAllThreads(void* addr,bool clear){
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0); if(snap==INVALID_HANDLE_VALUE)return;
    THREADENTRY32 te{}; te.dwSize=sizeof(te); DWORD pid=GetCurrentProcessId(),me=GetCurrentThreadId();
    if(Thread32First(snap,&te)){ do{ if(te.th32OwnerProcessID!=pid||te.th32ThreadID==me)continue;
        HANDLE th=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_SUSPEND_RESUME,FALSE,te.th32ThreadID); if(!th)continue;
        SuspendThread(th); CONTEXT c{}; c.ContextFlags=CONTEXT_DEBUG_REGISTERS;
        if(GetThreadContext(th,&c)){
            if(clear){ c.Dr0=0; c.Dr7&=~1ull; }
            else { c.Dr0=(uintptr_t)addr; c.Dr7&=~((0xFull)<<16); c.Dr7|=(1ull<<16); c.Dr7|=(2ull<<18); c.Dr7|=1ull; c.Dr6=0; } // RW0=01(write) LEN0=10(8B) L0
            c.ContextFlags=CONTEXT_DEBUG_REGISTERS; SetThreadContext(th,&c);
        }
        ResumeThread(th); CloseHandle(th);
    } while(Thread32Next(snap,&te)); }
    CloseHandle(snap);
}
// decoded write-site, captured for the turnkey spec
static int g_wReg=-1,g_wDisp=0,g_wSteal=0; static char g_wMnem[16]={0},g_wMod[80]={0},g_wMasked[160]={0},g_wStolenHex[80]={0}; static uintptr_t g_wOff=0;
// ---- resolve a write-site RIP back to its FUNCTION ENTRY -> a trampoline-hook target ----
// The CPU camera may be a transient buffer (e.g. a per-frame pool), so a fixed ADDRESS goes stale.
// The CODE that writes it doesn't: hook the function (MinHook-style trampoline), let it run, then add
// head pose to whatever camera it just wrote (base register captured below). This is the robust locator.
static bool looksLikePrologue(const uint8_t* p){
    if(p[0]==0x55) return true;                                  // push rbp
    if(p[0]==0x53||p[0]==0x56||p[0]==0x57) return true;          // push rbx/rsi/rdi
    if(p[0]==0x40 && (p[1]==0x53||p[1]==0x55||p[1]==0x56||p[1]==0x57)) return true; // REX push
    if(p[0]==0x48 && p[1]==0x83 && p[2]==0xEC) return true;      // sub rsp,imm8
    if(p[0]==0x48 && p[1]==0x81 && p[2]==0xEC) return true;      // sub rsp,imm32
    if(p[0]==0x48 && p[1]==0x89 && (p[2]==0x5C||p[2]==0x6C||p[2]==0x74||p[2]==0x7C) && p[3]==0x24) return true; // mov [rsp+x],reg
    if(p[0]==0x48 && p[1]==0x8B && p[2]==0xC4) return true;      // mov rax,rsp
    if(p[0]==0x4C && p[1]==0x8B && p[2]==0xDC) return true;      // mov r11,rsp
    if(p[0]==0x48 && p[1]==0x8B && p[2]==0xEC) return true;      // mov rbp,rsp (rare entry)
    return false;
}
static uintptr_t findFunctionEntry(uintptr_t rip){
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
static char g_fnMod[80]={0}; static uintptr_t g_fnOff=0; static char g_fnAOB[160]={0};
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
    bool any=(g_reprMatOff>=0||g_reprQuatOff>=0||g_reprEulerOff>=0||g_reprFovOff>=0||g_reprEyeOff>=0);
    if(!any) return;
    Log("===== EXPORT: camera struct field map (offsets from the struct base) =====");
    if(g_reprMatOff>=0)   Log("  view_matrix : +0x%X   (4x4 float32, %s-major)",g_reprMatOff,g_reprMatRow?"row":"col");
    if(g_reprQuatOff>=0)  Log("  quaternion  : +0x%X   (x,y,z,w float32)",g_reprQuatOff);
    if(g_reprEulerOff>=0) Log("  euler_deg   : +0x%X   (pitch,yaw,roll degrees)",g_reprEulerOff);
    if(g_reprEyeOff>=0)   Log("  eye/target  : +0x%X / +0x%X   (look-at vec3 pair)",g_reprEyeOff,g_reprTgtOff);
    if(g_reprFovOff>=0)   Log("  fov         : +0x%X   (scalar; radians if <~3.2, degrees if >~25, OR a FACTOR of a base FOV if ~0.5-1.6 e.g. 1.0=100%%)",g_reprFovOff);
    Log("==========================================================================");
}
// ---- EXPORT: the consolidated machine-readable 6DOF PROFILE - the bridge from probe to a generic runtime.
// Everything a fixed mod engine needs to re-find and drive this camera, in one block. A profile marked
// "verified" has passed the closed-loop view-response test. Save it as <exe>.6dof.json next to the runtime.
static void emitProfile(const char* cpuMod,unsigned long long cpuOff,bool verified){
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
    JL("    \"fn_module\": \"%s\", \"fn_offset\": \"0x%llX\", \"fn_entry_aob\": \"%s\",",g_fnMod[0]?g_fnMod:"",(unsigned long long)g_fnOff,g_fnAOB[0]?g_fnAOB:"");
    JL("    \"cpu_module\": \"%s\", \"cpu_offset\": \"0x%llX\"",cpuMod?cpuMod:"",cpuOff);
    JL("  },");
    JL("  \"representation\": {");
    JL("    \"kind\": \"%s\",",g_reprKind);
    if(g_reprMatOff>=0)   JL("    \"matrix\": { \"offset\": %d, \"major\": \"%s\" },",g_reprMatOff,g_reprMatRow?"row":"col");
    if(g_reprQuatOff>=0)  JL("    \"quaternion\": { \"offset\": %d, \"order\": \"xyzw\" },",g_reprQuatOff);
    if(g_reprEulerOff>=0) JL("    \"euler_deg\": { \"offset\": %d, \"axis_roles\": \"%c%c%c\" },",g_reprEulerOff,
        g_reprEulerRoles[0]?g_reprEulerRoles[0]:'?',g_reprEulerRoles[1]?g_reprEulerRoles[1]:'?',g_reprEulerRoles[2]?g_reprEulerRoles[2]:'?');
    if(g_reprEyeOff>=0)   JL("    \"eye_target\": { \"eye_offset\": %d, \"target_offset\": %d },",g_reprEyeOff,g_reprTgtOff);
    if(g_reprFovOff>=0)   JL("    \"fov\": { \"offset\": %d, \"encoding\": \"radians|degrees|factor (auto-detect at runtime)\" }",g_reprFovOff);
    else                  JL("    \"fov\": null");
    JL("  },");
    JL("  \"apply\": { \"mode\": \"additive_eye_fixed\", \"position_scale_xy\": 1.0, \"position_scale_z\": 0.3,");
    JL("    \"look_sensitivity\": 0.85, \"smoothing\": 0.0, \"roll_enable\": false, \"udp_port\": 4242,");
    JL("    \"invert_yaw\": false, \"invert_pitch\": false, \"invert_roll\": false,");
    JL("    \"invert_x\": false, \"invert_y\": false, \"invert_z\": false },");
    JL("  \"projection_convention\": { \"handedness\": \"%s\", \"reversed_z\": %s, \"infinite_far\": %s }",
        g_projHand<0?"right":(g_projHand>0?"left":"unknown"), g_projRevZ?"true":"false", g_projInfFar?"true":"false");
    JL("}");
    #undef JL
    writeTextFile(g_profPath,J.c_str());                       // standalone .6dof.json for the runtime
    if(!verified) Log("# NOTE: verified=false - confirm the camera responds (closed-loop or spin-test) before trusting this profile.");
    Log("# FILES: full findings log = %ls ; runtime profile = %ls",g_logPath,g_profPath);
    Log("=================================================================================");
}
static void emitFunctionHook(uintptr_t rip){
    uintptr_t fn=findFunctionEntry(rip); if(!fn){ Log("# FN-HOOK: couldn't resolve a function entry above the write-site"); return; }
    char mod[80]; uintptr_t off; moduleOf((void*)fn,mod,sizeof(mod),off);
    if(!Readable((void*)fn,24)){ Log("# FN-HOOK: function entry @ %s+0x%llX (bytes unreadable)",mod,(unsigned long long)off); return; }
    uint8_t* b=(uint8_t*)fn; char aob[160]; int n=0; for(int j=0;j<20 && n<(int)sizeof(aob)-4;j++) n+=snprintf(aob+n,sizeof(aob)-n,"%02X ",b[j]);
    Log("# FN-HOOK: camera-writer FUNCTION @ %s+0x%llX  (trampoline-hook this; call original, then add head pose)",mod,(unsigned long long)off);
    Log("# FN-ENTRY_AOB[entry..+20]: %s",aob);
    Log("# FN-NOTE: at the write-site the camera base is in %s; from the hook, read it from that reg/arg, add pose to its matrix.",g_wReg>=0?regName(g_wReg):"the captured register");
    Log("# static-root scan (turns a transient camera into a stable global+chain):");
    scanFuncGlobals(fn);
    strncpy(g_fnMod,mod,sizeof(g_fnMod)-1); g_fnOff=off; strncpy(g_fnAOB,aob,sizeof(g_fnAOB)-1);
}
static void emitWriteAOB(){
    g_wReg=-1; g_fnOff=0;
    if(g_writerN==0){ Log("# write-AOB: no writer captured (camera wasn't written during the watch window, or BPs blocked)"); return; }
    // MAJORITY VOTE: the per-frame camera update traps far more often than incidental writes; sort by hit count so
    // the most-frequent writer is primary (writers[0]). This is more reliable than taking the first-trapped site.
    for(int a=0;a<g_writerN;a++) for(int b=a+1;b<g_writerN;b++) if(g_writerHits[b]>g_writerHits[a]){
        uintptr_t tw=g_writers[a]; g_writers[a]=g_writers[b]; g_writers[b]=tw; int th=g_writerHits[a]; g_writerHits[a]=g_writerHits[b]; g_writerHits[b]=th; }
    if(g_writerN>1) Log("# write-watch caught %d distinct writers; using the most frequent (%d hits) as the camera update.",g_writerN,g_writerHits[0]);
    for(int i=0;i<g_writerN && i<3;i++){ uintptr_t rip=g_writers[i]; char mod[80]; uintptr_t off; moduleOf((void*)rip,mod,sizeof(mod),off);
        uint8_t* s=(uint8_t*)(rip-16);
        if(!Readable(s,32)){ Log("# WRITER @ %s+0x%llX (bytes unreadable)",mod,(unsigned long long)off); continue; }
        char hx[128]; int n=0; for(int j=0;j<24 && n<(int)sizeof(hx)-4;j++) n+=snprintf(hx+n,sizeof(hx)-n,"%02X ",s[j]);
        Log("# WRITER @ %s+0x%llX  (%d hits; data-BP traps after the store)",mod,(unsigned long long)off,g_writerHits[i]);
        Log("# WRITE_AOB bytes[rip-16..+8]: %s",hx);
        int reg,disp,start; const char* mn;
        if(decodeStore(s,16,reg,disp,mn,start)){
            int steal=16-start;                                                 // bytes from the store start through rip
            Log("# WRITE_SITE decoded: %s [%s%+d]  -> camera base register = %s, field offset = 0x%X, steal>=%d bytes for a cave",
                mn,regName(reg),disp,regName(reg),disp<0?-disp:disp,steal);
            // build a paste-ready masked AOB (displacement wildcarded, scanner-paste style) from the store start through rip
            int q=start; if(s[q]==0xF2||s[q]==0xF3||s[q]==0x66) q++; if((s[q]&0xF0)==0x40) q++; q+=2; // past prefix/REX/0F+opcode
            int rmod=s[q]>>6; q++; int dlen=(rmod==1?1:rmod==2?4:0); int dstart=q;
            char aob[160]; int an=0; an+=snprintf(aob+an,sizeof(aob)-an,"| ");   // '|' marks the capture/hook point (store start)
            for(int k=start;k<16 && an<(int)sizeof(aob)-4;k++){
                if(k>=dstart && k<dstart+dlen) an+=snprintf(aob+an,sizeof(aob)-an,"?? ");
                else an+=snprintf(aob+an,sizeof(aob)-an,"%02X ",s[k]); }
            Log("# WRITE_AOB_MASKED (paste into an AOB scanner; ?? = field offset, | = hook point): %s",aob);
            if(i==0){ g_wReg=reg; g_wDisp=disp; g_wSteal=steal; strncpy(g_wMnem,mn,sizeof(g_wMnem)-1); strncpy(g_wMod,mod,sizeof(g_wMod)-1); g_wOff=off-start; strncpy(g_wMasked,aob,sizeof(g_wMasked)-1);
                int hn=0; for(int k=start;k<16 && hn<(int)sizeof(g_wStolenHex)-4;k++) hn+=snprintf(g_wStolenHex+hn,sizeof(g_wStolenHex)-hn,"%02X ",s[k]); }
        } else Log("# WRITE_SITE: couldn't auto-decode the store form (SIB/atypical); use the AOB bytes above.");
        if(i==0) emitFunctionHook(rip);   // resolve the containing function -> trampoline-hook recipe (robust vs transient buffers)
    }
    emitFieldMap();              // EXPORT: struct field offsets the probe detected
    emitCheatEngineTemplate();   // EXPORT: paste-ready Cheat Engine AOB-injection script from this capture
}
// arm the data breakpoint, wait (early-out when a writer is caught), disarm, then emit the write-site + function-hook.
static void armWriteWatch(void* addr,int maxSlices,const char* prompt){
    g_writerN=0; memset((void*)g_writerHits,0,sizeof(g_writerHits)); g_veh=AddVectoredExceptionHandler(1,veh); setBPAllThreads(addr,false);
    Log("%s",prompt);
    for(int t=0;t<maxSlices && g_writerN==0;t++) Sleep(500);
    setBPAllThreads(addr,true); if(g_veh){ RemoveVectoredExceptionHandler(g_veh); g_veh=nullptr; }
    emitWriteAOB();
}
// capture the FOV writer separately (FOV control is a universal camera-tool feature; it's a distinct movss store).
// Doesn't touch the camera-write turnkey globals - just reports the FOV write-site so a mod can drive FOV too.
static void captureFovWriter(uintptr_t fovAddr){
    if(!fovAddr||!Readable((void*)fovAddr,4)) return;
    g_writerN=0; memset((void*)g_writerHits,0,sizeof(g_writerHits)); g_veh=AddVectoredExceptionHandler(1,veh); setBPAllThreads((void*)fovAddr,false);
    Log("# FOV write-watch armed (~3s) - zoom/aim to change FOV if you can, else just keep the view live...");
    for(int t=0;t<6 && g_writerN==0;t++) Sleep(500);
    setBPAllThreads((void*)fovAddr,true); if(g_veh){ RemoveVectoredExceptionHandler(g_veh); g_veh=nullptr; }
    if(g_writerN==0){ Log("# FOV-WRITE: not captured (FOV constant during the watch)"); return; }
    uintptr_t rip=g_writers[0]; char mod[80]; uintptr_t off; moduleOf((void*)rip,mod,sizeof(mod),off);
    uint8_t* s=(uint8_t*)(rip-16); if(!Readable(s,32)){ Log("# FOV-WRITE @ %s+0x%llX (bytes unreadable)",mod,(unsigned long long)off); return; }
    int reg,disp,start; const char* mn;
    if(decodeStore(s,16,reg,disp,mn,start)){
        int q=start; if(s[q]==0xF2||s[q]==0xF3||s[q]==0x66) q++; if((s[q]&0xF0)==0x40) q++; q+=2;
        int rmod=s[q]>>6; q++; int dlen=(rmod==1?1:rmod==2?4:0); int dstart=q;
        char aob[160]; int an=0; an+=snprintf(aob+an,sizeof(aob)-an,"| ");
        for(int k=start;k<16 && an<(int)sizeof(aob)-4;k++){ if(k>=dstart&&k<dstart+dlen) an+=snprintf(aob+an,sizeof(aob)-an,"?? "); else an+=snprintf(aob+an,sizeof(aob)-an,"%02X ",s[k]); }
        Log("# FOV-WRITE @ %s+0x%llX  store %s [%s%+d]  (hook this to control FOV; the scalar is the field at the offset)",mod,(unsigned long long)(off-start),mn,regName(reg),disp);
        Log("# FOV-WRITE_AOB_MASKED: %s",aob);
        uintptr_t fn=findFunctionEntry(rip); if(fn){ char fm[80]; uintptr_t fo; moduleOf((void*)fn,fm,sizeof(fm),fo);
            Log("# FOV-FN-HOOK: %s+0x%llX  (trampoline this; after original, write your FOV into [%s%+d])",fm,(unsigned long long)fo,regName(reg),disp); }
    } else Log("# FOV-WRITE @ %s+0x%llX (store form not auto-decoded; AOB bytes near rip apply)",mod,(unsigned long long)off);
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
    if(proj.empty()) return false;
    std::sort(proj.begin(),proj.end(),[](const Entry&a,const Entry&b){return a.draws>b.draws;});
    float target=(g_resW&&g_resH)?(float)g_resW/(float)g_resH:0.f; const Entry* bP=nullptr;
    if(target>0){ float best=1e9f; for(auto&e:proj){ float d=fabsf(e.aspect-target); if(d<0.15f&&d<best){best=d;bP=&e;} } }
    if(!bP) bP=&proj[0]; out=*bP; return true;
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
static void runPipeline(){
    if(g_pipelineRunning) return; g_pipelineRunning=true; g_lastVerified=0;
    Log(""); Log("################### AUTO-PIPELINE ###################");
    Entry bv;
    if(!bestViewEntry(bv)){ Log("# no GPU VIEW candidate yet; scanning memory for a CPU camera instead..."); memScan();
        turnkeySpec(nullptr,0,"",0,false);
        Log("################### AUTO-PIPELINE END (see MEM candidates above) ###################"); g_pipelineRunning=false; g_pipelineDone=true; return; }
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
    if(cpuAddr){
        findChains(cpuAddr);
        dumpStructFlags(cpuAddr);
        armWriteWatch((void*)cpuAddr,8,"# write-watch armed (~4s) - KEEP MOVING the in-game camera the WHOLE time (look around continuously)...");
        if(g_reprFovOff>=0) captureFovWriter(cpuAddr+g_reprFovOff);   // FOV control is a universal feature - capture its writer too
        spinTest((void*)cpuAddr,3);
        Log("# >>> RECOMMENDED TARGET: %s+0x%llX  layout=%s  (drive THIS - the CPU source - not the GPU buffer)",cpuMod,(unsigned long long)cpuOff,bv.rowMaj?"row":"col");
    } else Log("# no CPU copy isolated - the GPU constant-buffer route is your handle (off=0x%X size=%u slot=%d).",bv.off,bv.size,bv.slot);
    Log("# CONFIDENCE: ortho=yes draws=%u freq=%u cpu_copies=%d view_verified=%s writer_instrs=%d spin_test=%s",
        bv.draws,bv.freq,(int)hits.size(),cpuAddr?(g_lastVerified?"YES":"no/na"):"n/a",g_writerN,cpuAddr?"run":"skipped");
    report();
    turnkeySpec(&bv,cpuAddr,cpuMod,cpuOff,cpuAddr!=0);
    emitProfile(cpuMod,(unsigned long long)cpuOff,g_lastVerified!=0);   // consolidated machine-readable bridge -> runtime
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
}
static DWORD WINAPI autoExtraThread(LPVOID){
    Log(""); Log("################### AUTO DEEP SCANS (enabled in the loader) ###################");
    Log("# Main camera detection is done - now running the deep passes automatically.");
    Sleep(600);
    Log("# [1/4] memory scan (HOME) ...");                 memScan();
    Log("# [2/4] differential baseline snapshot (F7) ..."); snapshotScan();
    Log("# >>> MOVE / ROTATE the camera continuously for ~6s so the differential can isolate it <<<");
    for(int i=6;i>0;i--){ Log("#       capturing motion delta in %d ...",i); Sleep(1000); }
    Log("# [3/4] differential delta (F8) ...");            deltaScan();
    Log("# [4/4] full report (END) ...");                  report();
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
        // AUTOMATIC: once enough catalogue has built up (~5s), run the full discovery pipeline once.
        if(!g_pipelineDone && !g_pipelineRunning && g_frame==300){ CreateThread(nullptr,0,pipelineThread,nullptr,0,nullptr); }
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
    Log("================ 6DOF Probe injected (build %s %s) ================",__DATE__,__TIME__);
    detectFingerprint();
    Log("game=%s  arch=%d-bit  engine=%s  auto_deep_scans=%s",g_game,(int)(sizeof(void*)*8),g_engine,g_autoExtra?"ON":"off");
    const char* picked=nullptr;
    for(int i=0;i<600 && !picked;i++){ picked=detectAndInstall(); if(!picked) Sleep(100); }
    if(picked){ Log("API=%s  hooks installed - capturing.",picked);
        Log("AUTO: full discovery pipeline (select->correlate GPU/CPU->pointer-chain->write-AOB->spin-test) runs ~5s after injection.");
        Log("KEYS: INSERT=re-run pipeline   END=report now   HOME=memory scan   F7=snapshot / F8=delta (find camera by motion).  Report also auto-writes every %ds.",g_reportEverySec);
        recommendApproach();
        CreateThread(nullptr,0,ticker,nullptr,0,nullptr); }
    else { Log("ERROR: no graphics API hooked - the API-independent CPU pipeline still works: press F7, move the view, F8.");
        recommendApproach(); CreateThread(nullptr,0,ticker,nullptr,0,nullptr); }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){ g_self=h; DisableThreadLibraryCalls(h); CreateThread(nullptr,0,worker,nullptr,0,nullptr); }
    return TRUE;
}
