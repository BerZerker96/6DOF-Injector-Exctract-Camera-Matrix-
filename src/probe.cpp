// 6DOFProbe.dll - injected into a game; finds the camera view/projection matrices and writes a
// MOD BUILD SPEC to 6DOF-<Game>.log next to this dll. Multi-API:
//   D3D9  - hooks SetTransform + SetVertexShaderConstantF
//   D3D10 - hooks Buffer::Map/Unmap + UpdateSubresource
//   D3D11 - hooks Map / Unmap / UpdateSubresource
//   D3D12 - hooks ID3D12Resource::Map (scans the upload heaps)
//   Vulkan- detected and reported (capture path is a separate add-on)
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

// ----------------------------------------------------------------- logging
static wchar_t g_logPath[MAX_PATH]={0}; static HMODULE g_self=nullptr; static std::mutex g_logMx;
static void resolveLogPath(){
    wchar_t dll[MAX_PATH]={0}; GetModuleFileNameW(g_self,dll,MAX_PATH);
    wcscpy_s(g_logPath,MAX_PATH,dll); wchar_t* s=wcsrchr(g_logPath,L'\\'); if(s)*(s+1)=0; else g_logPath[0]=0;
    wchar_t gexe[MAX_PATH]={0}; GetModuleFileNameW(nullptr,gexe,MAX_PATH);
    wchar_t* gb=gexe; for(wchar_t*p=gexe;*p;++p) if(*p==L'\\'||*p==L'/') gb=p+1;
    wchar_t* dot=wcsrchr(gb,L'.'); if(dot)*dot=0;
    if(!g_logPath[0]){ GetTempPathW(MAX_PATH,g_logPath); }
    wcscat_s(g_logPath,MAX_PATH,L"6DOF-"); wcscat_s(g_logPath,MAX_PATH,gb); wcscat_s(g_logPath,MAX_PATH,L".log");
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
static int classifyProj(const float* m,bool& rowMaj,float& fovY,float& fovX,float& aspect,float& zn,float& zf){
    rowMaj=true; fovY=fovX=aspect=zn=zf=0;
    bool rowP=fabsf(fabsf(m[11])-1.f)<0.05f&&fabsf(m[15])<0.05f&&m[0]>0.1f&&m[5]>0.1f&&fabsf(m[3])<0.05f&&fabsf(m[7])<0.05f;
    bool colP=fabsf(fabsf(m[14])-1.f)<0.05f&&fabsf(m[15])<0.05f&&m[0]>0.1f&&m[5]>0.1f&&fabsf(m[12])<0.05f&&fabsf(m[13])<0.05f;
    if(!rowP&&!colP) return 0; rowMaj=rowP;
    float a=m[0],b=m[5]; fovX=2.f*atanf(1.f/a)*57.2957795f; fovY=2.f*atanf(1.f/b)*57.2957795f; aspect=b/a;
    if(fovX<20.f||fovX>170.f||fovY<20.f||fovY>170.f||aspect<0.4f||aspect>3.5f) return 0;  // implausible -> not a projection
    float c=m[10],d=rowMaj?m[14]:m[11]; if(fabsf(c)>1e-4f) zn=fabsf(d/c);
    zf=(fabsf(c-1.f)<1e-3f)?0.f:zn/(1.f-fabsf(c)+1e-6f); return 1;
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
    // drop the per-object/skinning flood: keep only well-duplicated candidates.
    uint32_t vthr=view.empty()?0:std::max(8u,view[0].freq/8), pthr=vp.empty()?0:std::max(8u,vp[0].freq/8);
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

// ----------------------------------------------------------------- fingerprint + API detect
static void detectFingerprint(){
    char exe[MAX_PATH]={0}; GetModuleFileNameA(nullptr,exe,MAX_PATH);
    const char* b=exe; for(char* p=exe;*p;++p) if(*p=='\\'||*p=='/') b=p+1; strncpy(g_game,b,sizeof(g_game)-1);
    struct{const wchar_t* dll;const char* e;} K[]={{L"UnityPlayer.dll","Unity"},{L"GameAssembly.dll","Unity(IL2CPP)"},{L"tier0.dll","Source"},{L"dunia.dll","Dunia"},{L"via.dll","RE Engine"}};
    for(auto&k:K) if(GetModuleHandleW(k.dll)){ strncpy(g_engine,k.e,sizeof(g_engine)-1); break; }
    if(strstr(exe,"-Win64-Shipping")) strncpy(g_engine,"Unreal Engine 4/5",sizeof(g_engine)-1);
}
static volatile bool g_vk=false;
static const char* detectAndInstall(){
    if(GetModuleHandleW(L"d3d12.dll") && install12()){ strcpy(g_api,"D3D12"); install11(); return "D3D12 (+D3D11 fallback)"; }
    if(GetModuleHandleW(L"d3d11.dll") && install11()){ strcpy(g_api,"D3D11"); return "D3D11"; }
    if(GetModuleHandleW(L"d3d10.dll") && install10()){ strcpy(g_api,"D3D10"); return "D3D10"; }
    if(GetModuleHandleW(L"d3d9.dll")  && install9()){  strcpy(g_api,"D3D9");  return "D3D9"; }
    if(GetModuleHandleW(L"vulkan-1.dll")){ strcpy(g_api,"Vulkan"); g_vk=true; return "Vulkan (detected - capture add-on pending)"; }
    if(install11()){ strcpy(g_api,"D3D11?"); return "D3D11 (guess)"; }
    return nullptr;
}

// ----------------------------------------------------------------- timer thread (report + key + frame window)
static volatile int g_reportEverySec=10;
static DWORD WINAPI ticker(LPVOID){
    uint64_t last=GetTickCount64(); bool pe=false;
    while(true){
        Sleep(16);
        g_frame++;
        { std::lock_guard<std::mutex> lk(g_freqMx); g_freq.clear(); }
        scanUploads12();
        bool e=(GetAsyncKeyState(VK_END)&0x8000)!=0; if(e&&!pe){ g_spinUsed=true; report(); } pe=e;
        static bool ph=false; bool hk=(GetAsyncKeyState(VK_HOME)&0x8000)!=0; if(hk&&!ph){ memScan(); } ph=hk;
        uint64_t now=GetTickCount64(); if(now-last>=(uint64_t)g_reportEverySec*1000){ last=now; report(); }
    }
}

static DWORD WINAPI worker(LPVOID){
    resolveLogPath();
    Log("================ 6DOF Probe injected (build %s %s) ================",__DATE__,__TIME__);
    detectFingerprint();
    Log("game=%s  arch=%d-bit  engine=%s",g_game,(int)(sizeof(void*)*8),g_engine);
    const char* picked=nullptr;
    for(int i=0;i<600 && !picked;i++){ picked=detectAndInstall(); if(!picked) Sleep(100); }
    if(picked){ Log("API=%s  hooks installed - capturing. Report auto-writes every %ds; END=report now, HOME=memory scan.",picked,g_reportEverySec);
        if(g_vk) Log("NOTE: Vulkan games aren't captured yet - tell me and I'll add the vkMapMemory hook.");
        CreateThread(nullptr,0,ticker,nullptr,0,nullptr); }
    else Log("ERROR: no supported graphics API found to hook.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){ g_self=h; DisableThreadLibraryCalls(h); CreateThread(nullptr,0,worker,nullptr,0,nullptr); }
    return TRUE;
}
