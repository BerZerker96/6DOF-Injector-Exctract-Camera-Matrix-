// ============================================================================
// sixdof_runtime.cpp  -  UNIVERSAL profile-driven 6DOF head-tracking runtime
// ----------------------------------------------------------------------------
// This is the consumer side of the probe->mod bridge. The probe emits a
// <exe>.6dof.json PROFILE describing how to find and drive a game's camera;
// this one fixed DLL loads that profile and becomes the mod - no per-game code
// and no recompile. Drop the DLL + the matching profile next to the game and
// inject (ASI loader / proxy / manual).
//
// Pipeline:  load profile -> AOB-scan the write-site -> CAPTURE the camera struct
// pointer (default: a cave-less HARDWARE BREAKPOINT; fallback: a hardened inline
// cave) -> read OpenTrack UDP -> apply additive head pose to the camera each frame
// (eye-fixed for matrices; add-to-axis for euler), restoring the engine's base
// value so head pose is purely additive.
//
// CAPTURE SAFETY (v5.9): the default path sets Dr0 to the write-site and reads the
// capture register out of the exception CONTEXT - it modifies NO game code, so the
// old "writes a jmp into .text" risk is gone for capture. If forced to the inline
// cave, it now relocates rip-relative displacements, refuses on relative branches,
// patches under a stop-the-world thread suspension with IP fix-up, verifies by
// read-back, and is cleanly uninstalled (bytes restored / breakpoint cleared) on
// unload. The cave mechanism is still validated in-process by selfTestCave() first.
// ============================================================================
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cstring>
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"psapi.lib")

// ----------------------------------------------------------------- profile
struct Profile {
    char     module[80]={0};
    uint8_t  aob[64]; bool aobMask[64]; int aobLen=0;     // write-site signature (?? = wildcard)
    int      captureReg=-1;                               // 0..15 = rax..r15 : holds the struct base at the write
    int      fieldOffset=0;                               // displacement written by the store ([reg+fieldOffset])
    char     kind[24]="unknown";                          // "matrix4x4..." / "quaternion..." / "euler..."
    int      matOff=-1; bool matRow=true;
    int      eulerOff=-1; char eulerRoles[4]={0}; bool eulerRad=true;     // euler angle encoding (radians vs degrees)
    int      quatOff=-1; int fovOff=-1; int eyeOff=-1; int tgtOff=-1; int posOff=-1;   // posOff = standalone position vec3 (euler/quat cams)
    float    clampPitch=35.f, clampYaw=35.f, clampRoll=15.f;              // head-contribution clamps (degrees)
    float    headCmToWorld=0.01f;                                        // OpenTrack cm -> game world units (positional)
    float    posScaleXY=1.0f, posScaleZ=0.3f, lookSens=0.85f, smoothing=0.0f;
    bool     rollEnable=false; int udpPort=4242;
    // per-axis invert (sign of "look right/up" varies by engine handedness + axis layout; can't be inferred
    // statically, so the probe suggests defaults from projection handedness and the user can flip any axis).
    bool     invYaw=false, invPitch=false, invRoll=false, invX=false, invY=false, invZ=false;
    // FOV control. The probe finds the FOV field offset + its encoding; the runtime can leave it alone (off),
    // force a fixed angle (static), or multiply the engine's own FOV (scale). All values here are in DEGREES of
    // horizontal FOV; the field's own encoding (degrees / radians / factor-of-base) is converted at write time.
    int      fovMode=0;                       // 0=off, 1=static (force fovTargetDeg, or fovScale if target<=0), 2=scale-only
    float    fovTargetDeg=0.f;                // absolute H-FOV to force in static mode (<=0 => fall back to fovScale)
    float    fovScale=1.0f;                   // multiply the engine's clean FOV (used in scale mode, or static w/ target<=0)
    float    fovMinDeg=50.f, fovMaxDeg=150.f; // clamp band (degrees)
    char     fovEncoding[12]="degrees";       // how the FIELD stores FOV: "degrees" | "radians" | "factor"
    float    fovBaseDeg=70.f;                 // base FOV for "factor" encoding (factor = deg/base)
    float    fovStepDeg=2.f;                  // F5/F7 nudge step (degrees)
};
static Profile g_p;
static volatile uintptr_t g_capturedBase=0;               // filled by the capture path (HWBP or cave) at runtime
// ---- capture install bookkeeping (for a CLEAN uninstall on unload) ----
static uint8_t  g_origBytes[32]={0};                      // original bytes at the hook site (for cave rollback)
static uint8_t* g_hookSite=nullptr; static int g_hookSteal=0; static uint8_t* g_cave=nullptr;
static int      g_captureMethod=0;                        // resolved method: 0=none, 1=hwbp (cave-less), 2=inline cave
static PVOID    g_hwbpVeh=nullptr; static volatile bool g_hwbpArmed=false; static uintptr_t g_hwbpSite=0;
static volatile long g_hwbpHits=0;
static int g_captureMethodPref=0;                         // profile pref: 0=auto(HWBP first), 1=hwbp-only, 2=force-cave

static void log(const char* f,...){ char b[512]; va_list a; va_start(a,f); vsnprintf(b,sizeof(b),f,a); va_end(a);
    char p[600]; snprintf(p,sizeof(p),"[6DOF-RT] %s\n",b); OutputDebugStringA(p);
    FILE* fp=fopen("sixdof_runtime.log","a"); if(fp){ fputs(p,fp); fclose(fp);} }

// minimal JSON-ish field readers (the profile is flat and machine-emitted, so key search is sufficient)
static bool jStr(const char* j,const char* key,char* out,int cap){ char k[64]; snprintf(k,sizeof(k),"\"%s\"",key);
    const char* p=strstr(j,k); if(!p) return false; p=strchr(p+strlen(k),':'); if(!p) return false; p++;
    while(*p==' '||*p=='\"') p++; int i=0; while(*p && *p!='\"' && *p!=',' && *p!='}' && i<cap-1) out[i++]=*p++; out[i]=0; return i>0; }
static bool jNum(const char* j,const char* key,double& v){ char k[64]; snprintf(k,sizeof(k),"\"%s\"",key);
    const char* p=strstr(j,k); if(!p) return false; p=strchr(p+strlen(k),':'); if(!p) return false; return sscanf(p+1," %lf",&v)==1; }
static bool jBool(const char* j,const char* key){ char k[64]; snprintf(k,sizeof(k),"\"%s\"",key);
    const char* p=strstr(j,k); if(!p) return false; p=strchr(p+strlen(k),':'); if(!p) return false; p++;
    while(*p==' ')p++; return strncmp(p,"true",4)==0; }
static int regIndex(const char* r){ const char* n[16]={"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
    for(int i=0;i<16;i++) if(!strcmp(r,n[i])) return i; return -1; }

static bool loadProfile(const char* path){
    FILE* fp=fopen(path,"rb"); if(!fp) return false; static char buf[8192]; int n=(int)fread(buf,1,sizeof(buf)-1,fp); fclose(fp); buf[n]=0;
    jStr(buf,"module",g_p.module,sizeof(g_p.module));
    char tmp[64]; if(jStr(buf,"capture_register",tmp,sizeof(tmp))) g_p.captureReg=regIndex(tmp);
    double d; if(jNum(buf,"field_offset",d)) g_p.fieldOffset=(int)d;
    jStr(buf,"kind",g_p.kind,sizeof(g_p.kind));
    if(jNum(buf,"offset",d)){} // per-field offsets parsed below by scanning their sub-objects
    // representation offsets: search the specific keys
    auto getOff=[&](const char* sect,const char* key,int& dst){ const char* s=strstr(buf,sect); if(!s)return; const char* o=strstr(s,key);
        if(o){ const char* c=strchr(o,':'); if(c){ double v; if(sscanf(c+1," %lf",&v)==1) dst=(int)v; } } };
    getOff("\"matrix\"","\"offset\"",g_p.matOff);
    getOff("\"euler\"","\"offset\"",g_p.eulerOff);
    if(g_p.eulerOff<0) getOff("\"euler_deg\"","\"offset\"",g_p.eulerOff);   // back-compat with older profiles
    getOff("\"quaternion\"","\"offset\"",g_p.quatOff);
    getOff("\"fov\"","\"offset\"",g_p.fovOff);
    getOff("\"eye_target\"","\"eye_offset\"",g_p.eyeOff);
    getOff("\"eye_target\"","\"target_offset\"",g_p.tgtOff);
    getOff("\"position\"","\"offset\"",g_p.posOff);
    char enc[16]; if(jStr(buf,"encoding",enc,sizeof(enc))) g_p.eulerRad=(strstr(enc,"rad")!=nullptr);
    char roles[8]; if(jStr(buf,"axis_roles",roles,sizeof(roles))) strncpy(g_p.eulerRoles,roles,3);
    if(jNum(buf,"position_scale_xy",d)) g_p.posScaleXY=(float)d;
    if(jNum(buf,"position_scale_z",d))  g_p.posScaleZ=(float)d;
    if(jNum(buf,"head_cm_to_world",d))  g_p.headCmToWorld=(float)d;
    { const char* c=strstr(buf,"\"clamp_deg\""); if(c){ double v; char* sav=(char*)c;
        if(jNum(sav,"pitch",v)) g_p.clampPitch=(float)v; if(jNum(sav,"yaw",v)) g_p.clampYaw=(float)v; if(jNum(sav,"roll",v)) g_p.clampRoll=(float)v; } }
    if(jNum(buf,"look_sensitivity",d))  g_p.lookSens=(float)d;
    if(jNum(buf,"smoothing",d))         g_p.smoothing=(float)d;
    if(jNum(buf,"udp_port",d))          g_p.udpPort=(int)d;
    { char cm[16]; if(jStr(buf,"capture_method",cm,sizeof(cm))){ g_captureMethodPref = strstr(cm,"cave")?2:(strstr(cm,"hwbp")?1:0); } }
    g_p.rollEnable=jBool(buf,"roll_enable");
    g_p.invYaw=jBool(buf,"invert_yaw");   g_p.invPitch=jBool(buf,"invert_pitch"); g_p.invRoll=jBool(buf,"invert_roll");
    g_p.invX  =jBool(buf,"invert_x");     g_p.invY    =jBool(buf,"invert_y");     g_p.invZ   =jBool(buf,"invert_z");
    char mj[8]; if(jStr(buf,"major",mj,sizeof(mj))) g_p.matRow=(mj[0]=='r');
    // ---- FOV apply settings (apply block) + field encoding (representation.fov) ----
    { char m[16]; if(jStr(buf,"fov_mode",m,sizeof(m))){ g_p.fovMode = (strstr(m,"static")?1:(strstr(m,"scale")?2:0)); } }
    if(jNum(buf,"fov_target_deg",d)) g_p.fovTargetDeg=(float)d;
    if(jNum(buf,"fov_scale",d))      g_p.fovScale=(float)d;
    if(jNum(buf,"fov_step_deg",d))   g_p.fovStepDeg=(float)d;
    if(jNum(buf,"fov_base_deg",d))   g_p.fovBaseDeg=(float)d;
    { const char* c=strstr(buf,"\"fov_clamp\""); if(c){ const char* lb=strchr(c,'['); if(lb){ double a,b; if(sscanf(lb+1," %lf , %lf",&a,&b)==2){ g_p.fovMinDeg=(float)a; g_p.fovMaxDeg=(float)b; } } } }
    // How the FOV FIELD stores its value. v5.8: the probe now SOLVES the encoding against the projection's true
    // FOV, so trust the explicit encoding string FIRST (degrees/radians/tan_half/cot_half/factor_of_base); only
    // fall back to the sampled-value heuristic for older profiles that didn't solve it.
    { const char* fs=strstr(buf,"\"fov\""); char fe[40]={0}; bool solved=false;
      if(fs && jStr(fs,"encoding",fe,sizeof(fe))){
          if(strstr(fe,"tan_half")){ strcpy(g_p.fovEncoding,"tan_half"); solved=true; }
          else if(strstr(fe,"cot_half")){ strcpy(g_p.fovEncoding,"cot_half"); solved=true; }
          else if(strstr(fe,"factor")){ strcpy(g_p.fovEncoding,"factor"); solved=true; }
          else if(strstr(fe,"radian")){ strcpy(g_p.fovEncoding,"radians"); solved=true; }
          else if(strstr(fe,"degree")){ strcpy(g_p.fovEncoding,"degrees"); solved=true; }
          else if(strstr(fe,"projection_only")){ strcpy(g_p.fovEncoding,"degrees"); /* no field path; FOV is in the projection */ }
      }
      if(!solved){ double sv=0; bool haveSv=false;                                  // legacy heuristic fallback
          if(fs){ const char* sp=strstr(fs,"\"sample_value\""); if(sp){ const char* c2=strchr(sp,':'); if(c2 && sscanf(c2+1," %lf",&sv)==1) haveSv=true; } }
          if(haveSv && sv>0){ if(sv>=20.0) strcpy(g_p.fovEncoding,"degrees");
                              else if(sv>=1.35 && sv<=3.25) strcpy(g_p.fovEncoding,"radians");
                              else if(sv>=0.3 && sv<1.35) strcpy(g_p.fovEncoding,"factor"); } } }
    // AOB: find "write_aob":"...."
    char aob[300]; if(jStr(buf,"write_aob",aob,sizeof(aob))){
        int len=0; const char* p=aob; while(*p && len<64){ while(*p==' ')p++; if(!*p)break;
            if(p[0]=='?'&&p[1]=='?'){ g_p.aob[len]=0; g_p.aobMask[len]=false; len++; p+=2; }
            else { unsigned b; if(sscanf(p,"%2x",&b)==1){ g_p.aob[len]=(uint8_t)b; g_p.aobMask[len]=true; len++; } p+=2; } }
        g_p.aobLen=len; }
    log("profile: module=%s aobLen=%d capReg=%d fieldOff=0x%X kind=%s matOff=%d eulerOff=%d roles=%s fov=%d",
        g_p.module,g_p.aobLen,g_p.captureReg,g_p.fieldOffset,g_p.kind,g_p.matOff,g_p.eulerOff,g_p.eulerRoles,g_p.fovOff);
    return g_p.aobLen>0 && g_p.module[0];
}

// ----------------------------------------------------------------- AOB scan
static uint8_t* scanModule(const char* mod,const uint8_t* pat,const bool* mask,int len){
    HMODULE h=GetModuleHandleA(mod); if(!h){ log("module %s not loaded",mod); return nullptr; }
    MODULEINFO mi{}; if(!GetModuleInformation(GetCurrentProcess(),h,&mi,sizeof(mi))) return nullptr;
    uint8_t* base=(uint8_t*)mi.lpBaseOfDll; size_t size=mi.SizeOfImage;
    for(size_t i=0;i+len<=size;i++){ bool ok=true;
        for(int j=0;j<len;j++){ if(mask[j] && base[i+j]!=pat[j]){ ok=false; break; } }
        if(ok) return base+i; }
    return nullptr;
}

// ----------------------------------------------------------------- capture cave
// A minimal length-disassembler so the trampoline steals WHOLE instructions (never splits one, which would
// crash on resume). Returns the instruction length at p, or 0 if it hits an encoding it can't decode with
// certainty - in which case we REFUSE to install the cave rather than risk a bad patch.
static int modrmLen(const uint8_t* m){                 // bytes for ModRM(+SIB+disp)
    uint8_t modrm=m[0]; int mod=modrm>>6,rm=modrm&7; int len=1;
    if(mod==3) return 1;
    if(rm==4){ len+=1; uint8_t sib=m[1]; int base=sib&7;             // SIB present
        if(mod==0){ if(base==5) len+=4; } else if(mod==1) len+=1; else len+=4; return len; }
    if(mod==0){ if(rm==5) len+=4; return len; }                      // [disp32] (RIP-rel x64 / abs x86)
    if(mod==1) return len+1;                                         // disp8
    return len+4;                                                    // disp32
}
static int insLen(const uint8_t* p){
    const uint8_t* s=p; int opsz=4;
    for(;;){ uint8_t b=*p;                                           // legacy prefixes
        if(b==0x66){ opsz=2; p++; continue; }
        if(b==0x67||b==0xF0||b==0xF2||b==0xF3||b==0x2E||b==0x36||b==0x3E||b==0x26||b==0x64||b==0x65){ p++; continue; } break; }
    int map=1;
    if(*p==0xC5){ map=2; p+=2; }                                     // 2-byte VEX -> 0F map
    else if(*p==0xC4){ uint8_t mm=p[1]&0x1F; map=(mm==2)?3:(mm==3)?4:2; p+=3; }   // 3-byte VEX
    else { if(sizeof(void*)==8 && (*p&0xF0)==0x40) p++;              // REX (x64)
        if(*p==0x0F){ p++; if(*p==0x38){ map=3; p++; } else if(*p==0x3A){ map=4; p++; } else map=2; } }
    uint8_t op=*p++; int imm=0; bool modrm=false;
    if(map==2){                                                     // 0F map: SSE moves + common 2-byte ops
        if(op>=0x80&&op<=0x8F){ imm=(opsz==2)?2:4; }                 // jcc rel32 (rel16 under 66h)
        else if(op>=0x90&&op<=0x9F){ modrm=true; }                   // setcc r/m8
        else if(op>=0x40&&op<=0x4F){ modrm=true; }                   // cmovcc r,r/m
        else switch(op){ case 0x10:case 0x11:case 0x12:case 0x13:case 0x14:case 0x15:case 0x16:case 0x17:
            case 0x28:case 0x29:case 0x2A:case 0x2B:case 0x2C:case 0x2D:case 0x2E:case 0x2F:
            case 0x51:case 0x54:case 0x55:case 0x56:case 0x57:case 0x58:case 0x59:case 0x5C:case 0x5D:case 0x5E:case 0x5F:
            case 0x6E:case 0x6F:case 0x7E:case 0x7F:case 0xD6:case 0xE7:case 0x1F:
            case 0xAF:case 0xB6:case 0xB7:case 0xBE:case 0xBF: modrm=true; break;   // imul, movzx, movsx
            case 0x70:case 0xC2:case 0xC6: modrm=true; imm=1; break;
            default: return 0; }                                    // unknown 0F op -> refuse
    } else if(map>=3){ modrm=true; }                                // 0F38/0F3A: all have ModRM (3A also imm8, conservative below)
    else {                                                          // primary map
        if((op&0xC4)==0x00 && (op&7)<6){ modrm=true; if((op&7)>=4){ imm=(op&1)?opsz:1; modrm=false; } } // 00..3D arith
        else switch(op){
            case 0x88:case 0x89:case 0x8A:case 0x8B:case 0x8D:case 0x63:case 0x84:case 0x85:case 0x86:case 0x87:
            case 0x00:case 0x01:case 0x02:case 0x03:case 0x30:case 0x31:case 0x32:case 0x33: modrm=true; break;
            case 0xC6: modrm=true; imm=1; break;
            case 0xC7: modrm=true; imm=opsz; break;
            case 0x68: imm=opsz; break; case 0x6A: imm=1; break;
            case 0xE8:case 0xE9: imm=4; break; case 0xEB: imm=1; break;
            case 0xC2: imm=2; break;
            case 0x90:case 0xC3:case 0xC9: break;
            case 0xFF: modrm=true; break;
            default: if(op>=0x50&&op<=0x5F) break;                  // push/pop reg
                     if(op>=0xB8&&op<=0xBF){ imm=opsz; break; }     // mov reg,imm
                     if(op>=0x40&&op<=0x4F) break;                  // (x86) inc/dec; (x64 handled as REX above)
                     return 0; }                                    // unknown primary op -> refuse
    }
    if(modrm){ p+=modrmLen(p); }
    p+=imm;
    int len=(int)(p-s); return (len>0&&len<=15)?len:0;
}

// =============================================================================================================
//  CAVE IMPROVEMENTS (v5.9)
//  Two big de-risks over the old raw-copy cave:
//   1. HARDWARE-BREAKPOINT CAPTURE (default): set Dr0 = the write-site and read the capture register out of the
//      exception CONTEXT. This writes NOTHING into game code, so the historical "writes a jmp into .text -
//      UNVERIFIED" risk is gone for the capture step. The inline cave stays as a fallback for the rare engine that
//      re-pools the struct so fast that single-step overhead matters.
//   2. When the inline cave IS used, it now (a) RELOCATES rip-relative displacements in the stolen bytes,
//      (b) REFUSES on relative branches it can't safely relocate, (c) patches under a STOP-THE-WORLD thread
//      suspension with IP fix-up, (d) verifies the patch by read-back, and (e) is cleanly UNINSTALLED on unload.
// =============================================================================================================

// read the capture register (profile index: 0=rax..7=rdi, 8..15=r8..r15) out of a thread CONTEXT
static uintptr_t regFromCtx(const CONTEXT* c,int r){
#ifdef _WIN64
    switch(r){ case 0:return c->Rax; case 1:return c->Rcx; case 2:return c->Rdx; case 3:return c->Rbx;
        case 4:return c->Rsp; case 5:return c->Rbp; case 6:return c->Rsi; case 7:return c->Rdi;
        case 8:return c->R8; case 9:return c->R9; case 10:return c->R10; case 11:return c->R11;
        case 12:return c->R12; case 13:return c->R13; case 14:return c->R14; case 15:return c->R15; default:return 0; }
#else
    switch(r){ case 0:return c->Eax; case 1:return c->Ecx; case 2:return c->Edx; case 3:return c->Ebx;
        case 4:return c->Esp; case 5:return c->Ebp; case 6:return c->Esi; case 7:return c->Edi; default:return 0; }
#endif
}
// run a callback over every thread of THIS process except the caller (used to arm/disarm debug regs)
template<class F> static void forEachOtherThread(F fn){
    DWORD me=GetCurrentThreadId(), pid=GetCurrentProcessId();
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0); if(snap==INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize=sizeof(te);
    if(Thread32First(snap,&te)){ do{ if(te.th32OwnerProcessID==pid && te.th32ThreadID!=me){
        HANDLE th=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_SUSPEND_RESUME,FALSE,te.th32ThreadID);
        if(th){ fn(th); CloseHandle(th); } } }while(Thread32Next(snap,&te)); }
    CloseHandle(snap);
}
// set or clear Dr0=site (execute breakpoint) on one thread
static void setDr0(HANDLE th,uintptr_t site,bool on){
    CONTEXT c; c.ContextFlags=CONTEXT_DEBUG_REGISTERS; SuspendThread(th);
    if(GetThreadContext(th,&c)){
        if(on){ c.Dr0=site; c.Dr7|=0x1; c.Dr7&=~(0xF<<16); }       // L0=1; RW0=00(exec) LEN0=00(1 byte)
        else  { if(c.Dr0==site){ c.Dr0=0; c.Dr7&=~0x1; } }
        c.ContextFlags=CONTEXT_DEBUG_REGISTERS; SetThreadContext(th,&c);
    }
    ResumeThread(th);
}
// VEH: on the single-step at our site, capture the register and continue.
static LONG CALLBACK hwbpVeh(EXCEPTION_POINTERS* ep){
    if(ep->ExceptionRecord->ExceptionCode==EXCEPTION_SINGLE_STEP &&
       (uintptr_t)ep->ExceptionRecord->ExceptionAddress==g_hwbpSite){
        uintptr_t b=regFromCtx(ep->ContextRecord,g_p.captureReg);   // base register holds the camera struct pointer
        if(b){ g_capturedBase=b; InterlockedIncrement(&g_hwbpHits); }
        ep->ContextRecord->EFlags|=0x10000;   // RF: resume without re-triggering on this instruction
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static bool installCaptureHWBP(uint8_t* site){
    if(g_p.captureReg<0) return false;
    g_hwbpSite=(uintptr_t)site;
    g_hwbpVeh=AddVectoredExceptionHandler(1,hwbpVeh); if(!g_hwbpVeh){ log("hwbp: AddVectoredExceptionHandler failed"); return false; }
    int n=0; forEachOtherThread([&](HANDLE th){ setDr0(th,(uintptr_t)site,true); n++; });
    if(n==0){ log("hwbp: no other threads to arm yet - will re-arm from the watchdog"); }
    g_hwbpArmed=true; g_captureMethod=1;
    log("capture: HARDWARE BREAKPOINT armed at %p (cave-less; reg=%d) on %d thread(s) - no game code modified.",site,g_p.captureReg,n);
    return true;
}
static void disarmHWBP(){
    if(!g_hwbpArmed) return; g_hwbpArmed=false;
    forEachOtherThread([&](HANDLE th){ setDr0(th,g_hwbpSite,false); });
    if(g_hwbpVeh){ RemoveVectoredExceptionHandler(g_hwbpVeh); g_hwbpVeh=nullptr; }
    log("capture: hardware breakpoint disarmed and cleared on all threads.");
}
// re-arm Dr0 on threads spawned after install (debug regs are per-thread, so new threads start clean)
static void rearmHWBPNewThreads(){ if(!g_hwbpArmed) return; forEachOtherThread([&](HANDLE th){ setDr0(th,g_hwbpSite,true); }); }

// copy `steal` bytes from src->dst, RELOCATING any rip-relative disp32 so it still points at the same absolute
// target from the cave. Returns false if a relative BRANCH is present that we won't safely relocate (caller then
// refuses the cave and uses HWBP). srcVA/dstVA are the runtime addresses the bytes will execute from.
static bool copyRelocated(uint8_t* dst,const uint8_t* src,int steal,uintptr_t srcVA,uintptr_t dstVA){
    int o=0; while(o<steal){ int l=insLen(src+o); if(l==0) return false;
        // detect relative branches (rel8/rel32) in the stolen window - refuse rather than half-fix them
        const uint8_t* q=src+o; while(*q==0x66||*q==0x67||*q==0xF0||*q==0xF2||*q==0xF3||(*q>=0x64&&*q<=0x65)||*q==0x2E||*q==0x36||*q==0x3E||*q==0x26) q++;
        if(*q==0xC4||*q==0xC5) return false;   // VEX/AVX store: relocation parsing is non-trivial - refuse, HWBP handles it
        uint8_t op=*q;
        if(op==0xE8||op==0xE9||op==0xEB||(op>=0x70&&op<=0x7F)){ return false; }
        if(op==0x0F){ uint8_t op2=q[1]; if(op2>=0x80&&op2<=0x8F) return false; }   // jcc rel32
        memcpy(dst+o,src+o,l);
#ifdef _WIN64
        // rip-relative memory operand: ModRM mod=00, rm=101 -> trailing disp32 is rip-relative. Recompute it.
        const uint8_t* m=q; if((*m&0xF0)==0x40) m++;                                 // REX
        if(*m==0x0F){ m++; if(*m==0x38||*m==0x3A) m++; }
        m++;                                                                         // opcode
        if((m<src+o+l) && (*m&0xC7)==0x05){                                          // mod=00 rm=101
            int dispPos=(int)((m+1)-(src+o));                                        // offset of disp32 within the instruction
            if(dispPos+4<=l){ int32_t d=*(int32_t*)(src+o+dispPos);
                uintptr_t abs=srcVA+o+l + (int64_t)d;                                // absolute target (rip = next instr)
                int64_t nd=(int64_t)abs - (int64_t)(dstVA+o+l);
                if(nd> 0x7FFFFFFFLL || nd< -0x80000000LL) return false;              // out of rel32 reach from the cave
                *(int32_t*)(dst+o+dispPos)=(int32_t)nd; } }
#endif
        o+=l; }
    return true;
}
// stop-the-world: suspend every other thread; if any IP sits inside [site,site+steal), relocate it into the cave
// (cave layout: [stub bytes][stolen bytes]), so resuming threads never execute a half-written patch.
static void suspendAllAndFix(uint8_t* site,int steal,uint8_t* cave,int stubLen){
    forEachOtherThread([&](HANDLE th){ SuspendThread(th);
        CONTEXT c; c.ContextFlags=CONTEXT_CONTROL; if(GetThreadContext(th,&c)){
#ifdef _WIN64
            uintptr_t ip=c.Rip;
#else
            uintptr_t ip=c.Eip;
#endif
            if(ip>(uintptr_t)site && ip<(uintptr_t)site+steal){
                uintptr_t nip=(uintptr_t)cave+stubLen+(ip-(uintptr_t)site);
#ifdef _WIN64
                c.Rip=nip;
#else
                c.Eip=nip;
#endif
                c.ContextFlags=CONTEXT_CONTROL; SetThreadContext(th,&c);
            } } });
}
static void resumeAll(){ forEachOtherThread([&](HANDLE th){ ResumeThread(th); }); }

// Allocate executable memory within +/-2GB of an anchor so a 5-byte E9 can reach it (both arches).
static uint8_t* allocNear(uint8_t* anchor){
    for(uint64_t d=0x10000; d<0x60000000ull; d+=0x100000){          // probe outward from the anchor
        for(int sign=0;sign<2;sign++){ uintptr_t a=sign?(uintptr_t)anchor-d:(uintptr_t)anchor+d; a&=~0xFFFFull;
            void* m=VirtualAlloc((void*)a,256,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
            if(m){ int64_t rel=(int64_t)((uintptr_t)m-(uintptr_t)anchor); if(rel>-0x7F000000LL&&rel<0x7F000000LL) return (uint8_t*)m; if(m)VirtualFree(m,0,MEM_RELEASE); } } }
    return (uint8_t*)VirtualAlloc(nullptr,256,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);   // fallback (may be out of E9 range on x64)
}
// Install a trampoline: store the capture register into g_capturedBase, run the stolen WHOLE instructions, return.
// 5-byte E9 hook on both arches (cave allocated within reach). *** validated in-process by selfTestCave(). ***
static bool installCapture(uint8_t* site,int /*hintLen*/){
    int steal=0; while(steal<5){ int l=insLen(site+steal); if(l==0){ log("cave: undecodable byte at +%d - REFUSING (use the FN-HOOK trampoline instead)",steal); return false; } steal+=l; }
    if(steal>200){ log("cave: steal too large (%d) - refusing",steal); return false; }
    uint8_t* cave=allocNear(site); if(!cave){ log("cave alloc failed"); return false; }
    int c=0; int r=g_p.captureReg;
#ifdef _WIN64
    if(r==0){ cave[c++]=0x41; cave[c++]=0x53;                                                          // push r11  (preserve scratch)
              cave[c++]=0x49; cave[c++]=0xBB; *(uint64_t*)(cave+c)=(uint64_t)&g_capturedBase; c+=8;   // mov r11,&cap
              cave[c++]=0x49; cave[c++]=0x89; cave[c++]=0x03;                                          // mov [r11],rax
              cave[c++]=0x41; cave[c++]=0x5B; }                                                        // pop r11
    else { cave[c++]=0x50;                                                                             // push rax (preserve scratch)
           cave[c++]=0x48; cave[c++]=0xB8; *(uint64_t*)(cave+c)=(uint64_t)&g_capturedBase; c+=8;       // mov rax,&cap
           cave[c++]=(r>=8)?0x4C:0x48; cave[c++]=0x89; cave[c++]=(uint8_t)(((r&7)<<3)|0x00);           // mov [rax],reg
           cave[c++]=0x58; }                                                                           // pop rax
#else
    cave[c++]=0x89; cave[c++]=(uint8_t)(((r&7)<<3)|0x05); *(uint32_t*)(cave+c)=(uint32_t)(uintptr_t)&g_capturedBase; c+=4;   // mov [&cap],reg (abs, no scratch)
#endif
    memcpy(g_origBytes,site,steal);                                                                   // save for a clean uninstall
    int stubLen=c;                                                                                    // bytes before the stolen copy
    if(!copyRelocated(cave+c,site,steal,(uintptr_t)site,(uintptr_t)(cave+c))){                         // RELOCATE rip-rel; refuse on branches
        log("cave: stolen bytes contain a relative branch or unreachable rip-rel target - REFUSING (HWBP capture is safe here).");
        VirtualFree(cave,0,MEM_RELEASE); return false; }
    c+=steal;
    { int32_t rel=(int32_t)((uintptr_t)(site+steal)-((uintptr_t)(cave+c)+5)); cave[c++]=0xE9; *(int32_t*)(cave+c)=rel; c+=4; }  // jmp back (E9)
    FlushInstructionCache(GetCurrentProcess(),cave,c);
    DWORD op; if(!VirtualProtect(site,steal,PAGE_EXECUTE_READWRITE,&op)){ log("protect failed"); VirtualFree(cave,0,MEM_RELEASE); return false; }
    suspendAllAndFix(site,steal,cave,stubLen);                                                         // STOP-THE-WORLD: freeze threads, relocate any IP in range
    int32_t srel=(int32_t)((uintptr_t)cave-((uintptr_t)site+5)); site[0]=0xE9; *(int32_t*)(site+1)=srel;                       // 5-byte E9 to cave
    for(int s2=5;s2<steal;s2++) site[s2]=0x90;
    FlushInstructionCache(GetCurrentProcess(),site,steal);
    resumeAll();
    VirtualProtect(site,steal,op,&op);
    // READ-BACK VERIFY: confirm the patch landed exactly; roll back if not.
    if(site[0]!=0xE9 || *(int32_t*)(site+1)!=srel){
        log("cave: read-back verify FAILED - rolling back."); DWORD o2; VirtualProtect(site,steal,PAGE_EXECUTE_READWRITE,&o2);
        memcpy(site,g_origBytes,steal); FlushInstructionCache(GetCurrentProcess(),site,steal); VirtualProtect(site,steal,o2,&o2);
        VirtualFree(cave,0,MEM_RELEASE); return false; }
    g_hookSite=site; g_hookSteal=steal; g_cave=cave; g_captureMethod=2;
    log("capture cave installed (%s) at %p -> %p (steal=%d, whole-instruction, rip-rel relocated, verified)", (sizeof(void*)==8)?"x64":"x86", site,cave,steal);
    return true;
}
// Clean removal: restore the original bytes under a stop-the-world, free the cave (or disarm the HWBP).
static void uninstallCapture(){
    if(g_captureMethod==1){ disarmHWBP(); g_captureMethod=0; return; }
    if(g_captureMethod==2 && g_hookSite){
        DWORD op; VirtualProtect(g_hookSite,g_hookSteal,PAGE_EXECUTE_READWRITE,&op);
        suspendAllAndFix(g_hookSite,g_hookSteal,g_cave,0);              // (stubLen 0: nothing should be inside the 5-byte E9 now)
        memcpy(g_hookSite,g_origBytes,g_hookSteal);
        FlushInstructionCache(GetCurrentProcess(),g_hookSite,g_hookSteal);
        resumeAll(); VirtualProtect(g_hookSite,g_hookSteal,op,&op);
        if(g_cave) VirtualFree(g_cave,0,MEM_RELEASE);
        log("capture cave uninstalled and original bytes restored at %p.",g_hookSite);
        g_hookSite=nullptr; g_cave=nullptr; g_captureMethod=0;
    }
}
// In-process validation of the capture cave: synthesize a store function, hook it, call it, and confirm the
// base register reached g_capturedBase AND the original store still ran. Proves the cave on every load.
static bool selfTestCave(){
    uint8_t* fn=(uint8_t*)VirtualAlloc(nullptr,64,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if(!fn){ log("cave self-test: alloc failed"); return false; }
    volatile uint64_t target=0; int c=0; int saveReg=g_p.captureReg;
#ifdef _WIN64
    fn[c++]=0x48; fn[c++]=0x89; fn[c++]=0x0A; fn[c++]=0x90; fn[c++]=0x90; fn[c++]=0xC3;   // mov [rdx],rcx ; nop;nop ; ret
    typedef void(*Fn)(uint64_t,void*); g_p.captureReg=2;                                  // capture base = rdx (arg1)
#else
    fn[c++]=0x89; fn[c++]=0x0A; fn[c++]=0x90; fn[c++]=0x90; fn[c++]=0x90; fn[c++]=0xC3;    // mov [edx],ecx ; nop;nop;nop ; ret
    typedef void(__attribute__((fastcall))*Fn)(uint32_t,void*); g_p.captureReg=2;          // capture base = edx (arg1)
#endif
    FlushInstructionCache(GetCurrentProcess(),fn,c);
    g_capturedBase=0; bool ok=installCapture(fn,0);
    if(ok){ ((Fn)fn)((uintptr_t)0x6D6F6631u,(void*)(uintptr_t)&target);
        bool captured=(g_capturedBase==(uintptr_t)&target), stored=((uint32_t)target==0x6D6F6631u);
        ok=captured&&stored;
        log("cave self-test: %s (register-capture=%s, stolen-store=%s)",ok?"PASS":"FAIL",captured?"ok":"BAD",stored?"ok":"BAD");
    } else log("cave self-test: installCapture refused");
    if(g_cave){ VirtualFree(g_cave,0,MEM_RELEASE); g_cave=nullptr; }                       // free the test cave + reset bookkeeping
    g_hookSite=nullptr; g_hookSteal=0; g_captureMethod=0; memset(g_origBytes,0,sizeof(g_origBytes));
    g_p.captureReg=saveReg; g_capturedBase=0; VirtualFree(fn,0,MEM_RELEASE);              // reset: don't leave a stale base for the apply thread
    return ok;
}

// ---- FULL in-process proof (opt-in via SIXDOF_SELFTEST=1) -------------------------------------------------
// Exercises the OS-dependent capture paths that the host unit-test can't: the hardware-breakpoint capture and the
// inline cave under a CONCURRENT thread hammering the patch site (the stop-the-world correctness case). Writes
// PASS/FAIL to the log so a user can prove the mechanism on THEIR machine before trusting it on a game.
static volatile bool g_stStop=false; static volatile uint64_t g_stTarget=0; static uint8_t* g_stFn=nullptr;
static volatile long g_stCalls=0;
typedef void(*StFn)(uint64_t,void*);
static DWORD WINAPI stHammer(LPVOID){ // mimics the game's render thread writing the camera struct every iteration
    while(!g_stStop){ ((StFn)g_stFn)((uintptr_t)0xCAFEF00D,(void*)(uintptr_t)&g_stTarget); InterlockedIncrement(&g_stCalls); }
    return 0;
}
static void selfTestFull(){
    if(!getenv("SIXDOF_SELFTEST")) return;
    log("==== FULL SELF-TEST (SIXDOF_SELFTEST set) ====");
    int saveReg=g_p.captureReg;
    g_stFn=(uint8_t*)VirtualAlloc(nullptr,64,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if(!g_stFn){ log("selftest: alloc failed"); return; }
    int c=0;
#ifdef _WIN64
    g_stFn[c++]=0x48; g_stFn[c++]=0x89; g_stFn[c++]=0x0A; g_stFn[c++]=0x90; g_stFn[c++]=0x90; g_stFn[c++]=0xC3; // mov [rdx],rcx;nop;nop;ret
    g_p.captureReg=2;                                                                                            // base = rdx
#else
    g_stFn[c++]=0x89; g_stFn[c++]=0x0A; g_stFn[c++]=0x90; g_stFn[c++]=0x90; g_stFn[c++]=0x90; g_stFn[c++]=0xC3;  // mov [edx],ecx;...;ret
    g_p.captureReg=2;
#endif
    FlushInstructionCache(GetCurrentProcess(),g_stFn,c);

    // 1) HARDWARE-BREAKPOINT capture with a concurrent caller thread
    g_stStop=false; g_capturedBase=0; g_hwbpHits=0;
    HANDLE th=CreateThread(nullptr,0,stHammer,nullptr,0,nullptr);
    Sleep(30);                                              // let the hammer thread exist before we arm
    bool armed=installCaptureHWBP(g_stFn);
    Sleep(120);
    bool hwbpCap=(g_capturedBase==(uintptr_t)&g_stTarget) && (g_hwbpHits>0) && ((uint32_t)g_stTarget==0xCAFEF00Du);
    log("selftest HWBP: %s (armed=%s, captured=%s, hits=%ld, store-intact=%s)",
        (armed&&hwbpCap)?"PASS":"FAIL", armed?"y":"n", (g_capturedBase==(uintptr_t)&g_stTarget)?"y":"n",
        (long)g_hwbpHits, ((uint32_t)g_stTarget==0xCAFEF00Du)?"y":"n");
    disarmHWBP();

    // 2) INLINE CAVE under the same concurrent caller (stop-the-world correctness: must not crash, must capture)
    g_capturedBase=0; long before=g_stCalls;
    bool inst=installCapture(g_stFn,0);
    Sleep(120);
    bool progressed=(g_stCalls>before+1000);               // the hammer thread kept running through the patch (no deadlock/crash)
    bool caveCap=(g_capturedBase==(uintptr_t)&g_stTarget);
    log("selftest CAVE+threads: %s (installed=%s, captured=%s, thread-progressed=%s, store-intact=%s)",
        (inst&&caveCap&&progressed)?"PASS":"FAIL", inst?"y":"n", caveCap?"y":"n", progressed?"y":"n",
        ((uint32_t)g_stTarget==0xCAFEF00Du)?"y":"n");

    // 3) CLEAN UNINSTALL while the thread runs, then confirm the original bytes are back and still callable
    uninstallCapture();
    Sleep(40);
    bool restored = (g_stFn[0]==
#ifdef _WIN64
        0x48
#else
        0x89
#endif
    );
    log("selftest UNINSTALL: %s (original first byte restored=%s)", restored?"PASS":"FAIL", restored?"y":"n");

    g_stStop=true; if(th){ WaitForSingleObject(th,1000); CloseHandle(th); }
    VirtualFree(g_stFn,0,MEM_RELEASE); g_stFn=nullptr; g_p.captureReg=saveReg; g_capturedBase=0;
    // reset bookkeeping so the real install starts clean
    g_hookSite=nullptr; g_cave=nullptr; g_captureMethod=0; memset(g_origBytes,0,sizeof(g_origBytes));
    log("==== FULL SELF-TEST complete ====");
}
// ----------------------------------------------------------------- UDP (OpenTrack)
struct Pose{ double x=0,y=0,z=0,yaw=0,pitch=0,roll=0; };
static Pose g_pose, g_center; static volatile bool g_have=false, g_centered=false; static CRITICAL_SECTION g_cs;
static DWORD WINAPI udpThread(LPVOID){
    WSADATA w; WSAStartup(MAKEWORD(2,2),&w);
    SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); if(s==INVALID_SOCKET) return 0;
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons((u_short)g_p.udpPort);
    if(bind(s,(sockaddr*)&a,sizeof(a))!=0){ log("UDP bind failed on %d",g_p.udpPort); return 0; }
    log("UDP listening on 127.0.0.1:%d",g_p.udpPort); double buf[6];
    for(;;){ int n=recvfrom(s,(char*)buf,sizeof(buf),0,nullptr,nullptr);
        if(n==(int)sizeof(buf)){ EnterCriticalSection(&g_cs);
            g_pose.x=buf[0];g_pose.y=buf[1];g_pose.z=buf[2];g_pose.yaw=buf[3];g_pose.pitch=buf[4];g_pose.roll=buf[5];
            if(!g_centered){ g_center=g_pose; g_centered=true; log("first OpenTrack packet: yaw=%.2f pitch=%.2f x=%.2f y=%.2f z=%.2f (centered)",g_pose.yaw,g_pose.pitch,g_pose.x,g_pose.y,g_pose.z); }
            g_have=true; LeaveCriticalSection(&g_cs);} }
}

// ----------------------------------------------------------------- apply
static bool ortho3(const float* m){ float rx=m[0]*m[0]+m[1]*m[1]+m[2]*m[2]; return rx>0.9f&&rx<1.1f; }
// eye-fixed additive head-look on a world-to-view 4x4 (reuses the proven per-game formula)
static void applyMatrix(float* m,const Pose& d){
    if(!ortho3(m)) return;
    float yaw=(float)d.yaw*0.01745329f*g_p.lookSens, pit=(float)d.pitch*0.01745329f*g_p.lookSens;
    float cy=cosf(yaw),sy=sinf(yaw),cp=cosf(pit),sp=sinf(pit);
    float Ry[9]={cy,0,sy, 0,1,0, -sy,0,cy}, Rx[9]={1,0,0, 0,cp,-sp, 0,sp,cp};
    float H[9]; for(int r=0;r<3;r++)for(int k=0;k<3;k++) H[r*3+k]=Rx[r*3]*Ry[k]+Rx[r*3+1]*Ry[3+k]+Rx[r*3+2]*Ry[6+k];
    float R0=m[0],R1=m[1],R2=m[2],R3=m[4],R4=m[5],R5=m[6],R6=m[8],R7=m[9],R8=m[10];
    float t0=m[12],t1=m[13],t2=m[14];
    float ex=-(t0*R0+t1*R3+t2*R6), ey=-(t0*R1+t1*R4+t2*R7), ez=-(t0*R2+t1*R5+t2*R8); // eye = -t*R^T
    // R' = H * R   (apply head rotation in view space)
    float Rm[9]={R0,R1,R2,R3,R4,R5,R6,R7,R8}, Rp[9];
    for(int r=0;r<3;r++)for(int k=0;k<3;k++) Rp[r*3+k]=H[r*3]*Rm[k]+H[r*3+1]*Rm[3+k]+H[r*3+2]*Rm[6+k];
    m[0]=Rp[0];m[1]=Rp[1];m[2]=Rp[2];m[4]=Rp[3];m[5]=Rp[4];m[6]=Rp[5];m[8]=Rp[6];m[9]=Rp[7];m[10]=Rp[8];
    // rebuild translation so the eye stays put, then add positional lean
    float lx=(float)(d.x-g_center.x)*g_p.posScaleXY, ly=(float)(d.y-g_center.y)*g_p.posScaleXY, lz=(float)(d.z-g_center.z)*g_p.posScaleZ;
    float ex2=ex+lx, ey2=ey+ly, ez2=ez+lz;
    m[12]=-(ex2*Rp[0]+ey2*Rp[3]+ez2*Rp[6]); m[13]=-(ex2*Rp[1]+ey2*Rp[4]+ez2*Rp[7]); m[14]=-(ex2*Rp[2]+ey2*Rp[5]+ez2*Rp[8]);
}
// add head yaw/pitch/roll to the identified euler axes - clamped (head contribution), inverted per flags, and
// converted to the field's encoding (radians vs degrees). OpenTrack rotation arrives in degrees.
static inline float clampf(float v,float lim){ return v>lim?lim:(v<-lim?-lim:v); }
static void applyEuler(float* e,const Pose& d){
    float p=clampf((float)(d.pitch-g_center.pitch)*g_p.lookSens,g_p.clampPitch);   // invert already applied upstream (d reflected about center)
    float y=clampf((float)(d.yaw  -g_center.yaw  )*g_p.lookSens,g_p.clampYaw  );
    float r=clampf((float)(d.roll -g_center.roll )*g_p.lookSens,g_p.clampRoll );
    if(g_p.eulerRad){ const float D2R=0.01745329f; p*=D2R; y*=D2R; r*=D2R; }   // field stores radians -> convert head degrees
    for(int i=0;i<3;i++){ char role=g_p.eulerRoles[i];
        if(role=='P') e[i]+=p; else if(role=='Y') e[i]+=y; else if(role=='R'&&g_p.rollEnable) e[i]+=r; }
}
// standalone camera POSITION vec3 (euler/quat cameras keep position separate from rotation): add head lean,
// converting OpenTrack cm -> game world units, per-axis scale (invert already applied upstream).
static void applyPosition(float* pos,const Pose& d){
    pos[0]+=(float)(d.x-g_center.x)*g_p.headCmToWorld*g_p.posScaleXY;
    pos[1]+=(float)(d.y-g_center.y)*g_p.headCmToWorld*g_p.posScaleXY;
    pos[2]+=(float)(d.z-g_center.z)*g_p.headCmToWorld*g_p.posScaleZ;
}
// ---- FOV control: leave alone / force a fixed angle / scale the engine's own FOV ----
// The field stores FOV in one of three encodings (degrees / radians / factor-of-base). We work internally in
// DEGREES and convert at the boundary. To keep "scale" tracking the engine's live FOV (e.g. when the player
// zooms/ADS), we recapture the clean baseline whenever the field diverges from what we last wrote - i.e. the
// engine wrote its own value this frame - then re-assert our target. For "static" the baseline doesn't matter.
static volatile bool  g_fovOn=false;        // runtime toggle (starts = (fovMode!=0))
static float          g_fovNudgeDeg=0.f;     // F5/F7 manual offset (degrees), on top of target/scale
static float          g_fovClean=0.f; static bool g_fovHaveClean=false; static float g_fovLastWrote=-1.f;
static inline float fovToDeg(float v){
    if(strcmp(g_p.fovEncoding,"radians")==0)  return v*57.2957795f;
    if(strcmp(g_p.fovEncoding,"factor")==0)   return v*g_p.fovBaseDeg;
    if(strcmp(g_p.fovEncoding,"tan_half")==0)  return 2.f*atanf(v)*57.2957795f;        // field = tan(fovy/2)
    if(strcmp(g_p.fovEncoding,"cot_half")==0)  return 2.f*atanf(1.f/v)*57.2957795f;     // field = cot(fovy/2) (matrix element)
    return v; }
static inline float fovFromDeg(float d){
    if(strcmp(g_p.fovEncoding,"radians")==0)  return d*0.0174532925f;
    if(strcmp(g_p.fovEncoding,"factor")==0)   return (g_p.fovBaseDeg>1e-3f)? d/g_p.fovBaseDeg : d;
    if(strcmp(g_p.fovEncoding,"tan_half")==0)  return tanf(d*0.0174532925f*0.5f);
    if(strcmp(g_p.fovEncoding,"cot_half")==0){ float t=tanf(d*0.0174532925f*0.5f); return (t>1e-6f)?1.f/t:d; }
    return d; }
static void applyFov(float* fovField){
    float cur=*fovField; if(cur!=cur) return;                       // NaN guard
    float curDeg=fovToDeg(cur);
    // if the field differs from our last write, the engine just wrote its own clean value -> recapture baseline
    bool engineWrote = (g_fovLastWrote<0.f) || (fabsf(cur-g_fovLastWrote)>1e-4f);
    if(engineWrote && curDeg>=g_p.fovMinDeg*0.3f && curDeg<=g_p.fovMaxDeg*2.0f){ g_fovClean=curDeg; g_fovHaveClean=true; }
    float baseDeg = g_fovHaveClean? g_fovClean : curDeg;
    // static mode: force fovTargetDeg (or scale the baseline if no absolute target given); scale mode: baseline*scale
    float wantDeg = (g_p.fovMode==1 && g_p.fovTargetDeg>1.f)? g_p.fovTargetDeg : baseDeg*g_p.fovScale;
    wantDeg += g_fovNudgeDeg;
    if(wantDeg<g_p.fovMinDeg) wantDeg=g_p.fovMinDeg; if(wantDeg>g_p.fovMaxDeg) wantDeg=g_p.fovMaxDeg;
    float wantRaw=fovFromDeg(wantDeg);
    if(fabsf(*fovField-wantRaw)>1e-5f){ *fovField=wantRaw; g_fovLastWrote=wantRaw; }
}
// quaternion helpers (xyzw)
static void qmul(const float*a,const float*b,float*o){
    o[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
    o[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    o[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
    o[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}
static void applyQuat(float* q,const Pose& d){
    float yaw=(float)(d.yaw-g_center.yaw)*0.01745329f*g_p.lookSens, pit=(float)(d.pitch-g_center.pitch)*0.01745329f*g_p.lookSens;
    float qy[4]={0,sinf(yaw*0.5f),0,cosf(yaw*0.5f)}, qp[4]={sinf(pit*0.5f),0,0,cosf(pit*0.5f)};
    float qh[4]; qmul(qy,qp,qh);
    float out[4]; qmul(q,qh,out);                              // camera-local delta (q (x) qhead); node-conjugation variant noted in research doc
    float n=sqrtf(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]+out[3]*out[3]); if(n<1e-6f)return;
    for(int i=0;i<4;i++) q[i]=out[i]/n;
}
// look-at rig: rotate target around eye by head yaw/pitch; lean translates the eye
static void applyEyeTarget(float* eye,float* tgt,const Pose& d){
    float yaw=(float)(d.yaw-g_center.yaw)*0.01745329f*g_p.lookSens, pit=(float)(d.pitch-g_center.pitch)*0.01745329f*g_p.lookSens;
    float f[3]={tgt[0]-eye[0],tgt[1]-eye[1],tgt[2]-eye[2]};
    float cy=cosf(yaw),sy=sinf(yaw); float fx=f[0]*cy+f[2]*sy, fz=-f[0]*sy+f[2]*cy; f[0]=fx; f[2]=fz;   // yaw about Y
    float len=sqrtf(f[0]*f[0]+f[2]*f[2]); float cp=cosf(pit),sp=sinf(pit);                                // pitch about horizontal
    float fy=f[1]*cp - len*sp; float scale=(len>1e-5f)?((len*cp + f[1]*sp)/len):1.f; f[0]*=scale; f[2]*=scale; f[1]=fy;
    float lx=(float)(d.x-g_center.x)*g_p.posScaleXY, ly=(float)(d.y-g_center.y)*g_p.posScaleXY, lz=(float)(d.z-g_center.z)*g_p.posScaleZ;
    eye[0]+=lx; eye[1]+=ly; eye[2]+=lz;
    tgt[0]=eye[0]+f[0]; tgt[1]=eye[1]+f[1]; tgt[2]=eye[2]+f[2];
}

static volatile bool g_enabled=true;
static DWORD WINAPI applyThread(LPVOID){
    static bool announced=false; bool f8=false,f9=false,f10=false,f11=false,f5=false,f6=false,f7=false;
    uint8_t lastOut[64]; int lastLen=0; bool haveLast=false;
    Pose sm{}; bool smInit=false;
    for(;;){ Sleep(2);
        // hotkeys: F8 toggle, F9 recenter, F10/F11 invert yaw/pitch; F6 toggle FOV, F5/F7 narrow/widen FOV
        bool n8=(GetAsyncKeyState(VK_F8)&0x8000)!=0; if(n8&&!f8){ g_enabled=!g_enabled; log("toggle -> %s",g_enabled?"ON":"OFF"); } f8=n8;
        bool n9=(GetAsyncKeyState(VK_F9)&0x8000)!=0; if(n9&&!f9){ EnterCriticalSection(&g_cs); g_center=g_pose; LeaveCriticalSection(&g_cs); log("recenter"); } f9=n9;
        bool n10=(GetAsyncKeyState(VK_F10)&0x8000)!=0; if(n10&&!f10){ g_p.invYaw=!g_p.invYaw; log("invert yaw -> %s",g_p.invYaw?"ON":"OFF"); } f10=n10;
        bool n11=(GetAsyncKeyState(VK_F11)&0x8000)!=0; if(n11&&!f11){ g_p.invPitch=!g_p.invPitch; log("invert pitch -> %s",g_p.invPitch?"ON":"OFF"); } f11=n11;
        bool n6=(GetAsyncKeyState(VK_F6)&0x8000)!=0; if(n6&&!f6){ if(g_p.fovOff<0) log("FOV: no FOV field in profile - nothing to toggle"); else { g_fovOn=!g_fovOn; g_fovHaveClean=false; g_fovLastWrote=-1.f; log("FOV override -> %s",g_fovOn?"ON":"OFF"); } } f6=n6;
        bool n7=(GetAsyncKeyState(VK_F7)&0x8000)!=0; if(n7&&!f7){ if(g_p.fovOff>=0){ if(!g_fovOn){g_fovOn=true;g_fovHaveClean=false;} g_fovNudgeDeg+=g_p.fovStepDeg; log("FOV +%.0f (nudge=%.0f)",g_p.fovStepDeg,g_fovNudgeDeg);} } f7=n7;
        bool n5=(GetAsyncKeyState(VK_F5)&0x8000)!=0; if(n5&&!f5){ if(g_p.fovOff>=0){ if(!g_fovOn){g_fovOn=true;g_fovHaveClean=false;} g_fovNudgeDeg-=g_p.fovStepDeg; log("FOV -%.0f (nudge=%.0f)",g_p.fovStepDeg,g_fovNudgeDeg);} } f5=n5;
        uintptr_t base=g_capturedBase; if(!base) continue;
        // FOV override runs INDEPENDENTLY of head-tracking enable / UDP - it's a static (or scale) write to the FOV
        // field, useful on its own (e.g. widen FOV for comfort) even with tracking off.
        if(g_fovOn && g_p.fovOff>=0){
            uint8_t* ff=(uint8_t*)(base+g_p.fovOff); MEMORY_BASIC_INFORMATION fm;
            if(VirtualQuery(ff,&fm,sizeof(fm))&&fm.State==MEM_COMMIT&&(fm.Protect&(PAGE_READWRITE|PAGE_EXECUTE_READWRITE|PAGE_WRITECOPY)))
                applyFov((float*)ff); }
        if(!g_have||!g_enabled) continue;
        if(!announced){ announced=true; log("camera struct pointer captured: base=%p field=%p kind=%s",(void*)base,(void*)(base+g_p.fieldOffset),g_p.kind); }
        Pose raw; EnterCriticalSection(&g_cs); raw=g_pose; LeaveCriticalSection(&g_cs);
        // optional exponential smoothing (profile.smoothing in 0..0.95; 0 = none)
        float sf=g_p.smoothing; if(sf<0)sf=0; if(sf>0.95f)sf=0.95f; float k=1.0f-sf;
        if(!smInit){ sm=raw; smInit=true; } else {
            sm.x+=k*(raw.x-sm.x); sm.y+=k*(raw.y-sm.y); sm.z+=k*(raw.z-sm.z);
            sm.yaw+=k*(raw.yaw-sm.yaw); sm.pitch+=k*(raw.pitch-sm.pitch); sm.roll+=k*(raw.roll-sm.roll); }
        Pose d=sm;
        // per-axis invert: reflect about the recenter point so every downstream apply (matrix/euler/quat/eye),
        // which all use (d.axis - center.axis), sees a negated delta on the flagged axes.
        if(g_p.invYaw)   d.yaw  =2*g_center.yaw  -d.yaw;
        if(g_p.invPitch) d.pitch=2*g_center.pitch-d.pitch;
        if(g_p.invRoll)  d.roll =2*g_center.roll -d.roll;
        if(g_p.invX)     d.x    =2*g_center.x    -d.x;
        if(g_p.invY)     d.y    =2*g_center.y    -d.y;
        if(g_p.invZ)     d.z    =2*g_center.z    -d.z;
        uint8_t* field=(uint8_t*)(base + g_p.fieldOffset);
        int len = (strncmp(g_p.kind,"matrix",6)==0)?64 : (strncmp(g_p.kind,"euler",5)==0)?12 : (strncmp(g_p.kind,"quat",4)==0)?16 : 24;
        MEMORY_BASIC_INFORMATION mbi;
        if(!VirtualQuery(field,&mbi,sizeof(mbi))||mbi.State!=MEM_COMMIT||!(mbi.Protect&(PAGE_READWRITE|PAGE_EXECUTE_READWRITE|PAGE_WRITECOPY))) { haveLast=false; continue; }
        // ANTI-ACCUMULATION: if the field still equals what we wrote last frame, the engine hasn't refreshed it yet -
        // skip, so we never apply head pose on top of our own already-modified value (which would drift/spin).
        if(haveLast && lastLen==len && memcmp(field,lastOut,len)==0) continue;
        if(strncmp(g_p.kind,"matrix",6)==0 && g_p.matOff>=0)      applyMatrix((float*)field,d);
        else if(strncmp(g_p.kind,"euler",5)==0 && g_p.eulerOff>=0) applyEuler((float*)field,d);
        else if(strncmp(g_p.kind,"quat",4)==0 && g_p.quatOff>=0)   applyQuat((float*)field,d);
        else if(strncmp(g_p.kind,"eye",3)==0 && g_p.eyeOff>=0 && g_p.tgtOff>=0)
            applyEyeTarget((float*)(base+g_p.eyeOff),(float*)(base+g_p.tgtOff),d);
        // euler/quat rotations carry no translation - add positional head lean to the separate position vec3 (matrix
        // cameras embed it, so they skip this). Gated by the same refresh check above (the updater writes both together).
        if(g_p.posOff>=0 && (strncmp(g_p.kind,"euler",5)==0||strncmp(g_p.kind,"quat",4)==0)){
            uint8_t* pf=(uint8_t*)(base+g_p.posOff); MEMORY_BASIC_INFORMATION pm;
            if(VirtualQuery(pf,&pm,sizeof(pm))&&pm.State==MEM_COMMIT&&(pm.Protect&(PAGE_READWRITE|PAGE_EXECUTE_READWRITE|PAGE_WRITECOPY)))
                applyPosition((float*)pf,d); }
        if(len<=64){ memcpy(lastOut,field,len); lastLen=len; haveLast=true; }
    }
}

// ----------------------------------------------------------------- bootstrap
// Watchdog: debug registers are per-thread and DON'T inherit, so a thread spawned after we armed the HWBP would
// miss the breakpoint. Once a second, re-apply Dr0 to every thread (cheap and idempotent). Only runs in HWBP mode.
static DWORD WINAPI watchdogThread(LPVOID){
    while(g_hwbpArmed){ Sleep(1000); rearmHWBPNewThreads(); }
    return 0;
}
static DWORD WINAPI worker(LPVOID){
    InitializeCriticalSection(&g_cs);
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr,exe,sizeof(exe)); char* slash=strrchr(exe,'\\'); char dir[MAX_PATH];
    strncpy(dir,exe,sizeof(dir)); char* ds=strrchr(dir,'\\'); if(ds)*ds=0;
    char* name=slash?slash+1:exe; char prof[MAX_PATH]; snprintf(prof,sizeof(prof),"%s\\%s.6dof.json",dir,name);
    if(!loadProfile(prof)){
        char sub[MAX_PATH]; snprintf(sub,sizeof(sub),"%s\\6DOF Output\\%s.6dof.json",dir,name);   // probe's default output location
        if(!loadProfile(sub)){ char alt[MAX_PATH]; snprintf(alt,sizeof(alt),"%s\\sixdof_profile.json",dir);
            if(!loadProfile(alt)){ log("no profile found (tried %s , %s) - idle.",prof,sub); return 0; } } }
    g_fovOn = (g_p.fovMode!=0) && (g_p.fovOff>=0);     // arm FOV override if the profile asks and a FOV field exists
    log("FOV: mode=%s field=%s encoding=%s target=%.1f scale=%.2f clamp=[%.0f,%.0f] (F6 toggle, F5/F7 -/+%.0f)",
        g_p.fovMode==1?"static":(g_p.fovMode==2?"scale":"off"), g_p.fovOff>=0?"found":"none",
        g_p.fovEncoding, g_p.fovTargetDeg, g_p.fovScale, g_p.fovMinDeg, g_p.fovMaxDeg, g_p.fovStepDeg);
    // wait for the target module, then scan + install
    for(int i=0;i<600 && !GetModuleHandleA(g_p.module);i++) Sleep(100);
    uint8_t* site=scanModule(g_p.module,g_p.aob,g_p.aobMask,g_p.aobLen);
    if(!site){ log("AOB not found in %s - the pattern may need updating for this build.",g_p.module); return 0; }
    log("write-site found at %p",site);
    selfTestFull();   // opt-in (SIXDOF_SELFTEST=1): prove HWBP + threaded cave + uninstall on THIS machine, logged PASS/FAIL
    CreateThread(nullptr,0,udpThread,nullptr,0,nullptr);
    // CAPTURE: prefer the cave-less HARDWARE BREAKPOINT (writes nothing into game code). Fall back to the hardened
    // inline cave only if the profile forces "cave" or HWBP can't arm. The cave is validated in-process first.
    if(g_p.captureReg>=0){
        bool useCave = (g_captureMethodPref==2);       // 2 = force cave
        if(!useCave){
            if(installCaptureHWBP(site)) { /* armed cave-less */ }
            else { log("HWBP arm failed - falling back to the inline cave."); useCave=true; }
        }
        if(useCave){
            bool stOk=selfTestCave();                  // validate the cave mechanism in-process before touching the game
            if(stOk){ if(!installCapture(site,0)) log("cave install refused/failed - capture inactive (drive the camera manually or re-run as HWBP)."); }
            else log("cave self-test FAILED - not installing the game cave.");
        }
    }
    CreateThread(nullptr,0,watchdogThread,nullptr,0,nullptr);   // re-arm HWBP on threads spawned after install
    CreateThread(nullptr,0,applyThread,nullptr,0,nullptr);
    log("runtime active (capture method: %s).", g_captureMethod==1?"hardware breakpoint (cave-less)":g_captureMethod==2?"inline cave":"none");
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); CreateThread(nullptr,0,worker,nullptr,0,nullptr); }
    else if(reason==DLL_PROCESS_DETACH){ uninstallCapture(); }   // restore game bytes / clear the HWBP cleanly
    return TRUE;
}
