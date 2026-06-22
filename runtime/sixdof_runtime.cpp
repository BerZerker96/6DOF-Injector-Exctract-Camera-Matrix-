// ============================================================================
// sixdof_runtime.cpp  -  UNIVERSAL profile-driven 6DOF head-tracking runtime
// ----------------------------------------------------------------------------
// This is the consumer side of the probe->mod bridge. The probe emits a
// <exe>.6dof.json PROFILE describing how to find and drive a game's camera;
// this one fixed DLL loads that profile and becomes the mod - no per-game code
// and no recompile. Drop the DLL + the matching profile next to the game and
// inject (ASI loader / proxy / manual).
//
// Pipeline:  load profile -> AOB-scan the write-site -> install a code-cave that
// captures the camera struct pointer -> read OpenTrack UDP -> apply additive
// head pose to the camera each frame (eye-fixed for matrices; add-to-axis for
// euler), restoring the engine's base value so head pose is purely additive.
//
// STATUS: reference implementation. The profile parse, AOB scan, UDP read and
// the apply math are complete and reuse the proven per-game-mod formulas. The
// code-cave capture is the one piece that MUST be validated on a live target
// before trusting it - it writes a jump into game code. It is written to the
// standard absolute-jump pattern and is clearly marked. UNVERIFIED until tested.
// ============================================================================
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
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
    int      eulerOff=-1; char eulerRoles[4]={0};
    int      quatOff=-1; int fovOff=-1; int eyeOff=-1; int tgtOff=-1;
    float    posScaleXY=1.0f, posScaleZ=0.3f, lookSens=0.85f, smoothing=0.0f;
    bool     rollEnable=false; int udpPort=4242;
    // per-axis invert (sign of "look right/up" varies by engine handedness + axis layout; can't be inferred
    // statically, so the probe suggests defaults from projection handedness and the user can flip any axis).
    bool     invYaw=false, invPitch=false, invRoll=false, invX=false, invY=false, invZ=false;
};
static Profile g_p;
static volatile uintptr_t g_capturedBase=0;               // filled by the code-cave at runtime

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
    getOff("\"euler_deg\"","\"offset\"",g_p.eulerOff);
    getOff("\"quaternion\"","\"offset\"",g_p.quatOff);
    getOff("\"fov\"","\"offset\"",g_p.fovOff);
    getOff("\"eye_target\"","\"eye_offset\"",g_p.eyeOff);
    getOff("\"eye_target\"","\"target_offset\"",g_p.tgtOff);
    char roles[8]; if(jStr(buf,"axis_roles",roles,sizeof(roles))) strncpy(g_p.eulerRoles,roles,3);
    if(jNum(buf,"position_scale_xy",d)) g_p.posScaleXY=(float)d;
    if(jNum(buf,"position_scale_z",d))  g_p.posScaleZ=(float)d;
    if(jNum(buf,"look_sensitivity",d))  g_p.lookSens=(float)d;
    if(jNum(buf,"smoothing",d))         g_p.smoothing=(float)d;
    if(jNum(buf,"udp_port",d))          g_p.udpPort=(int)d;
    g_p.rollEnable=jBool(buf,"roll_enable");
    g_p.invYaw=jBool(buf,"invert_yaw");   g_p.invPitch=jBool(buf,"invert_pitch"); g_p.invRoll=jBool(buf,"invert_roll");
    g_p.invX  =jBool(buf,"invert_x");     g_p.invY    =jBool(buf,"invert_y");     g_p.invZ   =jBool(buf,"invert_z");
    char mj[8]; if(jStr(buf,"major",mj,sizeof(mj))) g_p.matRow=(mj[0]=='r');
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
// Install a trampoline at the write-site that stores the capture register into g_capturedBase,
// runs the stolen bytes, and returns. x64 absolute-jump pattern. *** UNVERIFIED - test on a live target. ***
static bool installCapture(uint8_t* site,int stealLen){
    if(stealLen<14){ /* need room for a 14-byte abs jmp; pad with the following bytes */ stealLen=14; }
    uint8_t* cave=(uint8_t*)VirtualAlloc(nullptr,256,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if(!cave){ log("cave alloc failed"); return false; }
    int c=0; int r=g_p.captureReg;
    // mov rax, &g_capturedBase ; mov [rax], <reg>   (if reg==rax, capture before clobber by using r11 as scratch)
    bool clobber=(r==0);                          // capture reg is rax
    if(clobber){ // mov [&g_capturedBase], rax via push/pop scratch: mov r11,&g_cap ; mov [r11],rax
        cave[c++]=0x49; cave[c++]=0xBB; *(uint64_t*)(cave+c)=(uint64_t)&g_capturedBase; c+=8;  // mov r11, imm64
        cave[c++]=0x49; cave[c++]=0x89; cave[c++]=0x03;                                        // mov [r11], rax
    } else {
        cave[c++]=0x48; cave[c++]=0xB8; *(uint64_t*)(cave+c)=(uint64_t)&g_capturedBase; c+=8;  // mov rax, imm64
        uint8_t rexB=(r>=8)?0x4C:0x48; uint8_t modrm=0x00 | ((r&7)<<3) | 0x00; // mov [rax], reg
        cave[c++]=rexB; cave[c++]=0x89; cave[c++]=modrm; // [rax] base = 000 -> but rax=000 needs SIB? rax as mem base is fine (modrm 00 reg 000)
    }
    // stolen bytes
    memcpy(cave+c,site,stealLen); c+=stealLen;
    // jmp back to site+stealLen (abs)
    cave[c++]=0xFF; cave[c++]=0x25; *(uint32_t*)(cave+c)=0; c+=4; *(uint64_t*)(cave+c)=(uint64_t)(site+stealLen); c+=8;
    // patch the site: 14-byte absolute jmp to cave
    DWORD op; if(!VirtualProtect(site,stealLen,PAGE_EXECUTE_READWRITE,&op)){ log("protect failed"); return false; }
    int s=0; site[s++]=0xFF; site[s++]=0x25; *(uint32_t*)(site+s)=0; s+=4; *(uint64_t*)(site+s)=(uint64_t)cave; s+=8;
    while(s<stealLen) site[s++]=0x90; // nop pad
    VirtualProtect(site,stealLen,op,&op); FlushInstructionCache(GetCurrentProcess(),site,stealLen);
    log("capture cave installed at %p -> %p (steal=%d)",site,cave,stealLen);
    return true;
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
// add head yaw/pitch/roll to the identified euler axes
static void applyEuler(float* e,const Pose& d){
    float add[3]={(float)(d.pitch-g_center.pitch)*g_p.lookSens,(float)(d.yaw-g_center.yaw)*g_p.lookSens,(float)(d.roll-g_center.roll)*g_p.lookSens};
    for(int i=0;i<3;i++){ char role=g_p.eulerRoles[i];
        if(role=='P') e[i]+=add[0]; else if(role=='Y') e[i]+=add[1]; else if(role=='R'&&g_p.rollEnable) e[i]+=add[2]; }
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
    static bool announced=false; bool f8=false,f9=false,f10=false,f11=false;
    uint8_t lastOut[64]; int lastLen=0; bool haveLast=false;
    Pose sm{}; bool smInit=false;
    for(;;){ Sleep(2);
        // hotkeys: F8 toggle, F9 recenter
        bool n8=(GetAsyncKeyState(VK_F8)&0x8000)!=0; if(n8&&!f8){ g_enabled=!g_enabled; log("toggle -> %s",g_enabled?"ON":"OFF"); } f8=n8;
        bool n9=(GetAsyncKeyState(VK_F9)&0x8000)!=0; if(n9&&!f9){ EnterCriticalSection(&g_cs); g_center=g_pose; LeaveCriticalSection(&g_cs); log("recenter"); } f9=n9;
        bool n10=(GetAsyncKeyState(VK_F10)&0x8000)!=0; if(n10&&!f10){ g_p.invYaw=!g_p.invYaw; log("invert yaw -> %s",g_p.invYaw?"ON":"OFF"); } f10=n10;
        bool n11=(GetAsyncKeyState(VK_F11)&0x8000)!=0; if(n11&&!f11){ g_p.invPitch=!g_p.invPitch; log("invert pitch -> %s",g_p.invPitch?"ON":"OFF"); } f11=n11;
        uintptr_t base=g_capturedBase; if(!base||!g_have||!g_enabled) continue;
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
        if(len<=64){ memcpy(lastOut,field,len); lastLen=len; haveLast=true; }
    }
}

// ----------------------------------------------------------------- bootstrap
static DWORD WINAPI worker(LPVOID){
    InitializeCriticalSection(&g_cs);
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr,exe,sizeof(exe)); char* slash=strrchr(exe,'\\'); char dir[MAX_PATH];
    strncpy(dir,exe,sizeof(dir)); char* ds=strrchr(dir,'\\'); if(ds)*ds=0;
    char* name=slash?slash+1:exe; char prof[MAX_PATH]; snprintf(prof,sizeof(prof),"%s\\%s.6dof.json",dir,name);
    if(!loadProfile(prof)){
        char sub[MAX_PATH]; snprintf(sub,sizeof(sub),"%s\\6DOF Output\\%s.6dof.json",dir,name);   // probe's default output location
        if(!loadProfile(sub)){ char alt[MAX_PATH]; snprintf(alt,sizeof(alt),"%s\\sixdof_profile.json",dir);
            if(!loadProfile(alt)){ log("no profile found (tried %s , %s) - idle.",prof,sub); return 0; } } }
    // wait for the target module, then scan + install
    for(int i=0;i<600 && !GetModuleHandleA(g_p.module);i++) Sleep(100);
    uint8_t* site=scanModule(g_p.module,g_p.aob,g_p.aobMask,g_p.aobLen);
    if(!site){ log("AOB not found in %s - the pattern may need updating for this build.",g_p.module); return 0; }
    log("write-site found at %p",site);
    CreateThread(nullptr,0,udpThread,nullptr,0,nullptr);
    if(g_p.captureReg>=0) installCapture(site,14);     // capture the struct pointer
    CreateThread(nullptr,0,applyThread,nullptr,0,nullptr);
    log("runtime active.");
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); CreateThread(nullptr,0,worker,nullptr,0,nullptr); }
    return TRUE;
}
