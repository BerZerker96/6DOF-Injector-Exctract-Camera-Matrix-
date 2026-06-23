// 6DOFInjectGUI - modern dark-themed Win32 injector. Pick a running process and click INJECT.
// Portable: run from any folder; it injects the 6DOFProbe DLL next to it, and the probe writes
// 6DOF-<Game>.log + <Game>.exe.6dof.json into that same folder.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cwchar>
#include <cwctype>

#ifdef PROBE32
  #define PROBE_DLL  L"6DOFProbe32.dll"
  #define RUNTIME_DLL L"6DOFRuntime32.dll"
  #define APP_TITLE  L"6DOF Injector / Camera AOB Extractor"
  #define ARCH_BADGE L"UNIFIED"
  #define SELF64     false
#else
  #define PROBE_DLL  L"6DOFProbe.dll"
  #define RUNTIME_DLL L"6DOFRuntime.dll"
  #define APP_TITLE  L"6DOF Injector / Camera AOB Extractor"
  #define ARCH_BADGE L"UNIFIED"
  #define SELF64     true
#endif

enum { IDC_COMBO=1001, IDC_REFRESH, IDC_INJECT, IDC_STATUS, IDC_SEARCH, IDC_AUTO, IDC_WASD, IDC_APPLY, IDC_ARCH, IDC_AGGR, IDC_CAMHJ, IDC_FOVHJ, IDC_HJRETRY, IDC_ROTHJ };
static HWND g_combo, g_refresh, g_inject, g_status, g_search, g_auto, g_wasd, g_apply, g_arch, g_aggr, g_camhj, g_fovhj, g_hjretry, g_rothj;
static HFONT g_font, g_fontBtn, g_fontTitle, g_fontSmall;
static HBRUSH g_brBg, g_brEdit, g_brHdr;

// ---- modern dark palette ---------------------------------------------------
static const COLORREF
    CL_BG     = RGB(22,23,27),
    CL_HDR    = RGB(28,30,36),
    CL_PANEL  = RGB(34,36,43),
    CL_PANELH = RGB(44,47,56),
    CL_EDIT   = RGB(16,17,20),
    CL_BORDER = RGB(50,54,64),
    CL_TXT    = RGB(228,230,236),
    CL_MUTE   = RGB(138,144,156),
    CL_ACC    = RGB(48,150,255),
    CL_ACCH   = RGB(82,176,255),
    CL_ACCD   = RGB(36,124,214),
    CL_RED    = RGB(214,48,52),    // INJECT (danger/action)
    CL_REDH   = RGB(238,72,76),
    CL_REDD   = RGB(176,34,38),
    CL_CYAN   = RGB(60,222,238),   // title "6DOF"
    CL_TRED   = RGB(235,58,62),    // title "INJECTOR"
    CL_VIOLET = RGB(176,138,255);  // title "Camera AOB Extractor"

struct Proc { std::wstring name; DWORD pid; };
static std::vector<Proc> g_procs, g_shown;

// ---- custom button (rounded, hover/press, accent or ghost; also a checkbox mode) ----
struct Btn { const wchar_t* text; bool accent; bool danger=false; bool isCheck=false; bool checked=false;
             bool hover=false; bool down=false; WNDPROC orig=nullptr; };
static void paintBtn(HWND h, Btn* b){
    PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT r; GetClientRect(h,&r);
    HDC mem=CreateCompatibleDC(dc); HBITMAP bm=CreateCompatibleBitmap(dc,r.right,r.bottom); HGDIOBJ ob=SelectObject(mem,bm);
    HBRUSH bg=CreateSolidBrush(CL_BG); FillRect(mem,&r,bg); DeleteObject(bg);
    SetBkMode(mem,TRANSPARENT);
    if(b->isCheck){
        int s=18, by=(r.bottom-s)/2;                                   // box
        COLORREF boxFill=b->checked?CL_ACC:CL_EDIT, boxBord=(b->hover||b->checked)?CL_ACC:CL_BORDER;
        HBRUSH fb=CreateSolidBrush(boxFill); HPEN pen=CreatePen(PS_SOLID,1,boxBord);
        HGDIOBJ obr=SelectObject(mem,fb), ope=SelectObject(mem,pen);
        RoundRect(mem,0,by,s,by+s,5,5);
        if(b->checked){ HPEN ck=CreatePen(PS_SOLID,2,RGB(255,255,255)); HGDIOBJ o2=SelectObject(mem,ck);
            MoveToEx(mem,4,by+9,0); LineTo(mem,8,by+13); LineTo(mem,14,by+5); SelectObject(mem,o2); DeleteObject(ck); }
        SelectObject(mem,obr); SelectObject(mem,ope); DeleteObject(fb); DeleteObject(pen);
        SetTextColor(mem,b->hover?CL_TXT:CL_MUTE); SelectObject(mem,g_font);
        RECT tr={s+10,0,r.right,r.bottom}; DrawTextW(mem,b->text,-1,&tr,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);
        SelectObject(mem,ob); DeleteObject(bm); DeleteDC(mem); EndPaint(h,&ps); return;
    }
    COLORREF fill,txt,bord;
    if(b->danger){ fill=b->down?CL_REDD:(b->hover?CL_REDH:CL_RED); txt=RGB(255,255,255); bord=fill; }
    else if(b->accent){ fill=b->down?CL_ACCD:(b->hover?CL_ACCH:CL_ACC); txt=RGB(255,255,255); bord=fill; }
    else { fill=b->down?CL_EDIT:(b->hover?CL_PANELH:CL_PANEL); txt=b->hover?CL_ACC:CL_TXT; bord=CL_BORDER; }
    HBRUSH fb=CreateSolidBrush(fill); HPEN pen=CreatePen(PS_SOLID,1,bord);
    HGDIOBJ obr=SelectObject(mem,fb), ope=SelectObject(mem,pen);
    int rad=(r.bottom-r.top)>=40?16:10; RoundRect(mem,r.left,r.top,r.right-1,r.bottom-1,rad,rad);
    SetTextColor(mem,txt); SelectObject(mem,b->accent?g_fontBtn:g_font);
    DrawTextW(mem,b->text,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);
    SelectObject(mem,obr); SelectObject(mem,ope); SelectObject(mem,ob);
    DeleteObject(fb); DeleteObject(pen); DeleteObject(bm); DeleteDC(mem); EndPaint(h,&ps);
}
static LRESULT CALLBACK btnProc(HWND h, UINT m, WPARAM w, LPARAM l){
    Btn* b=(Btn*)GetWindowLongPtrW(h,GWLP_USERDATA);
    switch(m){
    case WM_PAINT: paintBtn(h,b); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: if(!b->hover){ b->hover=true; TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0}; TrackMouseEvent(&t); InvalidateRect(h,0,0);} return 0;
    case WM_MOUSELEAVE: b->hover=false; b->down=false; InvalidateRect(h,0,0); return 0;
    case WM_LBUTTONDOWN: b->down=true; SetCapture(h); InvalidateRect(h,0,0); return 0;
    case WM_LBUTTONUP: { bool in=b->down; b->down=false; ReleaseCapture();
        if(in){ POINT p; GetCursorPos(&p); RECT rc; GetWindowRect(h,&rc); if(PtInRect(&rc,p)){
            if(b->isCheck) b->checked=!b->checked;
            SendMessageW(GetParent(h),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(h),BN_CLICKED),(LPARAM)h); } }
        InvalidateRect(h,0,0); return 0; }
    }
    return CallWindowProcW(b->orig,h,m,w,l);
}
static Btn g_bRefresh{L"Refresh",false}, g_bInject{L"INJECT  \u25B6",false,true};
static Btn g_bAuto{L"Auto-run deep scans + auto-move the camera (drives the mouse) after detection",false,false,true,false};
static Btn g_bWasd{L"Auto-move character with WASD (randomized; needs the box above)",false,false,true,false};
static Btn g_bAggr{L"Aggressive deep probe (3rd log): stronger AOB + FOV hunter  \u2192  6DOF-<game>.aggressive.log",false,false,true,false};
static Btn g_bRotHj{L"Rotation HIJACK: pitch/yaw/roll - find which candidate really rotates the camera",false,false,true,true};
static Btn g_bCamHj{L"Camera HIJACK: nudge every candidate's X/Y/Z to find which one really moves the camera",false,false,true,true};
static Btn g_bFovHj{L"FOV HIJACK: drive the FOV up/down on each candidate to find the real FOV field",false,false,true,true};
static Btn g_bHjRetry{L"Auto-retry hijack until it LANDS: loop + re-scan until a candidate really moves the camera / FOV",false,false,true,true};
static Btn g_bApply{L"APPLY MODE: inject the head-tracking runtime instead of the probe (after you have a profile)",false,false,true,false};
static void makeButton(HWND h, Btn* b){ b->orig=(WNDPROC)SetWindowLongPtrW(h,GWLP_WNDPROC,(LONG_PTR)btnProc);
    SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)b); }

static void statusf(const wchar_t* fmt, ...) {
    wchar_t buf[1024]; va_list ap; va_start(ap,fmt); _vsnwprintf_s(buf,1024,_TRUNCATE,fmt,ap); va_end(ap);
    int len=GetWindowTextLengthW(g_status); SendMessageW(g_status,EM_SETSEL,len,len);
    SendMessageW(g_status,EM_REPLACESEL,FALSE,(LPARAM)buf); SendMessageW(g_status,EM_REPLACESEL,FALSE,(LPARAM)L"\r\n");
}
// ---- feedback loop: tail the probe's log file into the status box ----------
static volatile LONG g_tailGen=0;
struct TailCtx { std::wstring path; LONG gen; };
static DWORD WINAPI tailThread(LPVOID arg){
    TailCtx* c=(TailCtx*)arg; std::wstring path=c->path; LONG gen=c->gen; delete c;
    LONGLONG pos=0; std::string carry;
    for(int i=0;i<1200 && gen==g_tailGen;i++){            // ~10 min, or until a newer inject supersedes
        HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        if(h!=INVALID_HANDLE_VALUE){
            LARGE_INTEGER sz{}; GetFileSizeEx(h,&sz);
            if(sz.QuadPart>pos){
                LARGE_INTEGER mv; mv.QuadPart=pos; SetFilePointerEx(h,mv,nullptr,FILE_BEGIN);
                DWORD want=(DWORD)((sz.QuadPart-pos>16384)?16384:(sz.QuadPart-pos));
                std::string buf(want,0); DWORD got=0; if(ReadFile(h,&buf[0],want,&got,nullptr)&&got){ pos+=got; carry.append(buf,0,got);
                    size_t nl; while((nl=carry.find('\n'))!=std::string::npos){ std::string line=carry.substr(0,nl); carry.erase(0,nl+1);
                        while(!line.empty()&&(line.back()=='\r'||line.back()=='\n')) line.pop_back();
                        int wl=MultiByteToWideChar(CP_UTF8,0,line.c_str(),-1,nullptr,0);
                        std::wstring wl2(wl>0?wl-1:0,0); if(wl>0) MultiByteToWideChar(CP_UTF8,0,line.c_str(),-1,&wl2[0],wl);
                        if(gen==g_tailGen) statusf(L"%s",wl2.c_str()); }
                }
            }
            CloseHandle(h);
        }
        Sleep(500);
    }
    return 0;
}
static void startTail(const std::wstring& gameExe){
    wchar_t self[MAX_PATH]={0}; GetModuleFileNameW(nullptr,self,MAX_PATH);
    if(wchar_t* s=wcsrchr(self,L'\\')) *(s+1)=0;
    std::wstring base=gameExe; size_t d=base.rfind(L'.'); if(d!=std::wstring::npos) base=base.substr(0,d);
    std::wstring path=std::wstring(self)+L"6DOF Output\\6DOF-"+base+L".log";
    LONG gen=InterlockedIncrement(&g_tailGen);
    CreateThread(nullptr,0,tailThread,new TailCtx{path,gen},0,nullptr);
    statusf(L"--- live probe log (%s) ---",path.c_str());
}

static void applyFilter() {
    wchar_t q[128]={0}; if(g_search) GetWindowTextW(g_search,q,127);
    for(int i=0;q[i];i++) q[i]=towlower(q[i]);
    g_shown.clear(); SendMessageW(g_combo,CB_RESETCONTENT,0,0);
    for (auto& p : g_procs){ std::wstring low=p.name; for(auto&c:low) c=towlower(c);
        if (q[0]==0 || low.find(q)!=std::wstring::npos){ g_shown.push_back(p);
            wchar_t line[300]; _snwprintf_s(line,300,_TRUNCATE,L"  %s   \u00B7  pid %lu",p.name.c_str(),p.pid);
            SendMessageW(g_combo,CB_ADDSTRING,0,(LPARAM)line); } }
    if (!g_shown.empty()) SendMessageW(g_combo,CB_SETCURSEL,0,0);
}
static void refreshProcs() {
    g_procs.clear(); HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snap!=INVALID_HANDLE_VALUE){ PROCESSENTRY32W pe{}; pe.dwSize=sizeof(pe);
        if (Process32FirstW(snap,&pe)) do { if (pe.th32ProcessID>4) g_procs.push_back({pe.szExeFile, pe.th32ProcessID});
        } while (Process32NextW(snap,&pe)); CloseHandle(snap); }
    std::sort(g_procs.begin(),g_procs.end(),[](const Proc&a,const Proc&b){ return _wcsicmp(a.name.c_str(),b.name.c_str())<0; });
    applyFilter();
}
static bool injectInto(DWORD pid, const wchar_t* dll, std::wstring& err) {
    HANDLE p=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);
    if (!p){ err=L"OpenProcess failed - try running the injector as Administrator."; return false; }
    BOOL wow=FALSE; IsWow64Process(p,&wow); bool target32=wow;
    if (SELF64 && target32){ err=L"That game is actually 32-bit - set the dropdown to 'Auto-detect' (it will delegate automatically)."; CloseHandle(p); return false; }
    if (!SELF64 && !target32){ err=L"That game is actually 64-bit - set the dropdown to 'Auto-detect' (it will delegate automatically)."; CloseHandle(p); return false; }
    size_t bytes=(wcslen(dll)+1)*sizeof(wchar_t);
    void* remote=VirtualAllocEx(p,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if (!remote){ err=L"VirtualAllocEx failed."; CloseHandle(p); return false; }
    if (!WriteProcessMemory(p,remote,dll,bytes,nullptr)){ err=L"WriteProcessMemory failed."; VirtualFreeEx(p,remote,0,MEM_RELEASE); CloseHandle(p); return false; }
    auto loadLib=(LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");
    HANDLE th=CreateRemoteThread(p,nullptr,0,loadLib,remote,0,nullptr);
    if (!th){ err=L"CreateRemoteThread failed."; VirtualFreeEx(p,remote,0,MEM_RELEASE); CloseHandle(p); return false; }
    WaitForSingleObject(th,10000); DWORD ec=0; GetExitCodeThread(th,&ec);
    CloseHandle(th); VirtualFreeEx(p,remote,0,MEM_RELEASE); CloseHandle(p);
    if (ec==0){ err=L"LoadLibrary returned 0 - the DLL didn't load (wrong arch, or missing dependency)."; return false; }
    return true;
}
// Cross-arch injection: a 64-bit process can't CreateRemoteThread into a 32-bit (WOW64) target and vice-versa.
// So when the chosen target arch isn't this GUI's arch, delegate to the bundled matching-arch CLI injector.
static bool delegateCLI(DWORD pid, bool wantRuntime, bool target32, std::wstring& err){
    wchar_t self[MAX_PATH]={0}; GetModuleFileNameW(nullptr,self,MAX_PATH); if(wchar_t* s=wcsrchr(self,L'\\')) *(s+1)=0;
    std::wstring cli=std::wstring(self)+(target32?L"6DOFInject32.exe":L"6DOFInject.exe");
    if(GetFileAttributesW(cli.c_str())==INVALID_FILE_ATTRIBUTES){
        err=std::wstring(target32?L"6DOFInject32.exe":L"6DOFInject.exe")+L" not found next to this exe (needed for the other architecture)."; return false; }
    wchar_t cmd[MAX_PATH+64]; _snwprintf_s(cmd,MAX_PATH+64,_TRUNCATE,L"\"%s\" %lu%s",cli.c_str(),pid,wantRuntime?L" --runtime":L"");
    STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    if(!CreateProcessW(nullptr,cmd,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,self,&si,&pi)){ err=L"failed to launch the matching-arch CLI injector."; return false; }
    WaitForSingleObject(pi.hProcess,20000); DWORD ec=1; GetExitCodeProcess(pi.hProcess,&ec);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if(ec!=0){ err=L"the matching-arch CLI injector reported a failure (try running as Administrator)."; return false; }
    return true;
}
// arch selection: 0=auto-detect, 1=force 64-bit, 2=force 32-bit
static int chosenArch(){ int s=(int)SendMessageW(g_arch,CB_GETCURSEL,0,0); return s<0?0:s; }
static void doInject(HWND) {
    int sel=(int)SendMessageW(g_combo,CB_GETCURSEL,0,0);
    if (sel<0 || sel>=(int)g_shown.size()){ statusf(L"Pick a process first."); return; }
    Proc pr=g_shown[sel];
    bool applyMode=g_bApply.checked;
    // decide the TARGET arch: auto-detect from the process, or honor the dropdown override
    bool target32;
    { int ca=chosenArch();
      if(ca==1) target32=false; else if(ca==2) target32=true;
      else { HANDLE q=OpenProcess(PROCESS_QUERY_INFORMATION,FALSE,pr.pid); BOOL wow=FALSE; if(q){ IsWow64Process(q,&wow); CloseHandle(q); } target32=wow; } }
    wchar_t self[MAX_PATH]={0}; GetModuleFileNameW(nullptr,self,MAX_PATH);
    if (wchar_t* s=wcsrchr(self,L'\\')) *(s+1)=0;
    // write the GUI->probe config flag next to the DLLs (read by whichever arch's probe loads)
    { wchar_t cfg[MAX_PATH]; wcscpy_s(cfg,MAX_PATH,self); wcscat_s(cfg,MAX_PATH,L"6DOF.cfg");
      if(HANDLE cf=CreateFileW(cfg,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr); cf!=INVALID_HANDLE_VALUE){
        char cfgbuf[224]; int cl=snprintf(cfgbuf,sizeof(cfgbuf),"auto_extra_tests=%d\nauto_wasd=%d\naggressive=%d\nrot_hijack=%d\ncam_hijack=%d\nfov_hijack=%d\nhijack_retry=%d\n",
            g_bAuto.checked?1:0, (g_bAuto.checked&&g_bWasd.checked)?1:0,
            g_bAggr.checked?1:0, g_bRotHj.checked?1:0, g_bCamHj.checked?1:0, g_bFovHj.checked?1:0, g_bHjRetry.checked?1:0);
        DWORD wr; WriteFile(cf,cfgbuf,(DWORD)cl,&wr,nullptr); CloseHandle(cf); } }
    const wchar_t* archStr = target32?L"32-bit":L"64-bit";
    bool native = (SELF64 && !target32) || (!SELF64 && target32);     // can this GUI inject the target directly?
    std::wstring err;
    bool ok;
    if(native){
        const wchar_t* DLL_NAME = applyMode?RUNTIME_DLL:PROBE_DLL;
        wchar_t dll[MAX_PATH]; wcscpy_s(dll,MAX_PATH,self); wcscat_s(dll,MAX_PATH,DLL_NAME);
        if (GetFileAttributesW(dll)==INVALID_FILE_ATTRIBUTES){ statusf(L"ERROR: %s not found next to this exe.",DLL_NAME); return; }
        statusf(L"Injecting %s (%s) into %s [pid %lu] ...",DLL_NAME,archStr,pr.name.c_str(),pr.pid);
        ok=injectInto(pr.pid,dll,err);
    } else {
        statusf(L"Target is %s; delegating to %s for %s injection into %s [pid %lu] ...",
            archStr, target32?L"6DOFInject32.exe":L"6DOFInject.exe", applyMode?L"runtime":L"probe", pr.name.c_str(), pr.pid);
        ok=delegateCLI(pr.pid,applyMode,target32,err);
    }
    if (ok) {
        if(applyMode){
            statusf(L"OK - runtime injected. It loads <game>.6dof.json, runs a cave SELF-TEST, then applies OpenTrack head pose.");
            statusf(L"In-game:  F8 toggle on/off  \u00B7  F9 recenter  \u00B7  F10 invert yaw  \u00B7  F11 invert pitch.");
            statusf(L"Make sure OpenTrack is sending UDP to 127.0.0.1:4242, and that <game>.exe.6dof.json sits next to the game exe.");
        } else {
            statusf(L"OK - injected. The probe AUTO-RUNS its discovery pipeline ~5s in.");
            statusf(L"Be in normal gameplay (not a menu). WATCH THE SCREEN: a brief camera sweep = it found the real camera.");
            if(g_bAuto.checked) statusf(L"Auto deep scans ON: memory scan + differential + report run automatically after detection (move the camera when asked).");
            else statusf(L"Keys in-game:  INSERT re-run  \u00B7  END report  \u00B7  HOME memory scan  \u00B7  F7/F8 differential.");
            statusf(L"Output folder:  .\\6DOF Output\\   ->  6DOF-<game>.log  +  <game>.exe.6dof.json (runtime profile).");
        }
        startTail(pr.name);   // stream the probe's findings live into this window
    } else statusf(L"FAILED: %s", err.c_str());
}

static void roundOutline(HDC dc, RECT r, COLORREF border, int rad){
    HPEN pen=CreatePen(PS_SOLID,1,border); HGDIOBJ op=SelectObject(dc,pen); HGDIOBJ ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
    RoundRect(dc,r.left,r.top,r.right,r.bottom,rad,rad); SelectObject(dc,op); SelectObject(dc,ob); DeleteObject(pen);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_brBg=CreateSolidBrush(CL_BG); g_brEdit=CreateSolidBrush(CL_EDIT); g_brHdr=CreateSolidBrush(CL_HDR);
        g_font     =CreateFontW(-15,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        g_fontBtn  =CreateFontW(-18,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        g_fontTitle=CreateFontW(-26,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        g_fontSmall=CreateFontW(-12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        HINSTANCE hi=((LPCREATESTRUCT)lp)->hInstance; const int PAD=20, W=700;
        CreateWindowExW(0,L"STATIC",L"TARGET ARCHITECTURE",WS_CHILD|WS_VISIBLE,PAD,92,300,16,hwnd,0,hi,0);
        g_arch=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,
                                PAD,112,320,200,hwnd,(HMENU)IDC_ARCH,hi,0);
        SendMessageW(g_arch,CB_ADDSTRING,0,(LPARAM)L"  Auto-detect (recommended)");
        SendMessageW(g_arch,CB_ADDSTRING,0,(LPARAM)L"  64-bit game");
        SendMessageW(g_arch,CB_ADDSTRING,0,(LPARAM)L"  32-bit game");
        SendMessageW(g_arch,CB_SETCURSEL,0,0);
        CreateWindowExW(0,L"STATIC",L"FILTER",WS_CHILD|WS_VISIBLE,PAD,156,200,16,hwnd,0,hi,0);
        g_search=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
                                PAD,176,W-2*PAD-160,32,hwnd,(HMENU)IDC_SEARCH,hi,0);
        g_refresh=CreateWindowExW(0,L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                W-PAD-150,176,150,32,hwnd,(HMENU)IDC_REFRESH,hi,0);
        CreateWindowExW(0,L"STATIC",L"TARGET PROCESS",WS_CHILD|WS_VISIBLE,PAD,220,260,16,hwnd,0,hi,0);
        g_combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,
                                PAD,240,W-2*PAD,420,hwnd,(HMENU)IDC_COMBO,hi,0);
        g_auto=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                PAD,288,W-2*PAD,24,hwnd,(HMENU)IDC_AUTO,hi,0);
        g_wasd=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                PAD,316,W-2*PAD,24,hwnd,(HMENU)IDC_WASD,hi,0);
        g_aggr=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                PAD,344,W-2*PAD,24,hwnd,(HMENU)IDC_AGGR,hi,0);
        g_rothj=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                PAD,372,W-2*PAD,24,hwnd,(HMENU)IDC_ROTHJ,hi,0);
        g_camhj=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                PAD,400,W-2*PAD,24,hwnd,(HMENU)IDC_CAMHJ,hi,0);
        g_fovhj=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                PAD,428,W-2*PAD,24,hwnd,(HMENU)IDC_FOVHJ,hi,0);
        g_hjretry=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                PAD,456,W-2*PAD,24,hwnd,(HMENU)IDC_HJRETRY,hi,0);
        g_apply=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                PAD,484,W-2*PAD,24,hwnd,(HMENU)IDC_APPLY,hi,0);
        g_inject=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                PAD,516,W-2*PAD,52,hwnd,(HMENU)IDC_INJECT,hi,0);
        g_status=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
                                PAD+10,592,W-2*PAD-20,224,hwnd,(HMENU)IDC_STATUS,hi,0);
        SendMessageW(g_search,WM_SETFONT,(WPARAM)g_font,TRUE);
        SendMessageW(g_combo,WM_SETFONT,(WPARAM)g_font,TRUE);
        SendMessageW(g_arch,WM_SETFONT,(WPARAM)g_font,TRUE);
        SendMessageW(g_status,WM_SETFONT,(WPARAM)g_font,TRUE);
        makeButton(g_refresh,&g_bRefresh); makeButton(g_inject,&g_bInject); makeButton(g_auto,&g_bAuto); makeButton(g_wasd,&g_bWasd); makeButton(g_apply,&g_bApply);
        makeButton(g_aggr,&g_bAggr); makeButton(g_rothj,&g_bRotHj); makeButton(g_camhj,&g_bCamHj); makeButton(g_fovhj,&g_bFovHj); makeButton(g_hjretry,&g_bHjRetry);
        BOOL dark=TRUE; DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark)); DwmSetWindowAttribute(hwnd,19,&dark,sizeof(dark));
        refreshProcs();
        statusf(L"Ready. Pick the game and click INJECT.");
        statusf(L"Portable - keep %s next to this exe; output goes to the \"6DOF Output\" subfolder.",PROBE_DLL);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc=(HDC)wp; SetTextColor(dc,(HWND)lp==g_status?CL_TXT:CL_MUTE);
        SetBkColor(dc,(HWND)lp==g_status?CL_EDIT:CL_BG); return (LRESULT)((HWND)lp==g_status?g_brEdit:g_brBg);
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: { HDC dc=(HDC)wp; SetTextColor(dc,CL_TXT); SetBkColor(dc,CL_EDIT); return (LRESULT)g_brEdit; }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps); RECT cr; GetClientRect(hwnd,&cr);
        HBRUSH bg=CreateSolidBrush(CL_BG); FillRect(dc,&cr,bg); DeleteObject(bg);
        RECT hr={0,0,cr.right,76}; FillRect(dc,&hr,g_brHdr);
        SetBkMode(dc,TRANSPARENT);
        SelectObject(dc,g_fontTitle); SetTextColor(dc,CL_CYAN); TextOutW(dc,20,12,L"6DOF",4);
        SIZE sz; GetTextExtentPoint32W(dc,L"6DOF",4,&sz);
        SetTextColor(dc,CL_TRED); TextOutW(dc,20+sz.cx+8,12,L"INJECTOR",8);
        SelectObject(dc,g_fontBtn); SetTextColor(dc,CL_VIOLET); TextOutW(dc,21,46,L"Camera AOB Extractor",20);
        SIZE cs; GetTextExtentPoint32W(dc,L"Camera AOB Extractor",20,&cs);
        SelectObject(dc,g_fontSmall); SetTextColor(dc,CL_MUTE);
        TextOutW(dc,21+cs.cx+10,51,L"\u00B7  camera discovery + head-tracking probe",42);
        RECT bd={cr.right-92,18,cr.right-20,42}; roundOutline(dc,bd,CL_ACC,12);
        SetTextColor(dc,CL_ACC); DrawTextW(dc,ARCH_BADGE,-1,&bd,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        HPEN ap=CreatePen(PS_SOLID,2,CL_ACC); HGDIOBJ opn=SelectObject(dc,ap); MoveToEx(dc,0,76,0); LineTo(dc,cr.right,76); SelectObject(dc,opn); DeleteObject(ap);
        RECT sp={20,584,cr.right-20,824}; roundOutline(dc,sp,CL_BORDER,10);
        SelectObject(dc,g_fontSmall); SetTextColor(dc,CL_MUTE);
        TextOutW(dc,20,cr.bottom-24,L"Loopback UDP 4242  \u00B7  OpenTrack  \u00B7  log + .6dof.json profile",58);
        EndPaint(hwnd,&ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_COMMAND:
        if (LOWORD(wp)==IDC_REFRESH){ refreshProcs(); statusf(L"Process list refreshed (%zu).",g_procs.size()); }
        else if (LOWORD(wp)==IDC_INJECT){ doInject(hwnd); }
        else if (LOWORD(wp)==IDC_AUTO){ statusf(L"Auto deep scans %s. Applies on the next INJECT.",g_bAuto.checked?L"ENABLED":L"disabled"); }
        else if (LOWORD(wp)==IDC_WASD){ statusf(L"Auto WASD character movement %s. (Needs 'Auto-run deep scans' on; applies on next INJECT.)",g_bWasd.checked?L"ENABLED":L"disabled"); }
        else if (LOWORD(wp)==IDC_AGGR){ statusf(L"Aggressive deep probe %s. Adds a 3rd, harder-hitting log (6DOF-<game>.aggressive.log): longer write-watch, more candidates, wider FOV hunt. Applies on next INJECT.",g_bAggr.checked?L"ENABLED":L"disabled"); }
        else if (LOWORD(wp)==IDC_ROTHJ){ statusf(L"Rotation HIJACK %s. After detection the probe rotates each candidate (pitch/yaw/roll) and logs which one actually turns the camera, in all three logs. Applies on next INJECT.",g_bRotHj.checked?L"ENABLED":L"disabled"); }
        else if (LOWORD(wp)==IDC_CAMHJ){ statusf(L"Camera HIJACK %s. After detection the probe nudges each candidate's X/Y/Z and logs which one actually moves the camera (per axis), in all three logs. Applies on next INJECT.",g_bCamHj.checked?L"ENABLED":L"disabled"); }
        else if (LOWORD(wp)==IDC_FOVHJ){ statusf(L"FOV HIJACK %s. After detection the probe drives each FOV candidate up/down and logs which one really changes the rendered FOV, in all three logs. Applies on next INJECT.",g_bFovHj.checked?L"ENABLED":L"disabled"); }
        else if (LOWORD(wp)==IDC_HJRETRY){ statusf(L"Auto-retry hijack %s. Keeps re-scanning and re-running the camera/FOV hijack on a loop until a candidate ACTUALLY moves the camera / changes the FOV, then logs the landing (chime). Needs Camera and/or FOV HIJACK ticked. Applies on next INJECT.",g_bHjRetry.checked?L"ENABLED":L"disabled"); }
        else if (LOWORD(wp)==IDC_SEARCH && HIWORD(wp)==EN_CHANGE){ applyFilter(); }
        else if (LOWORD(wp)==IDC_ARCH && HIWORD(wp)==CBN_SELCHANGE){ int a=chosenArch();
            statusf(L"Target architecture: %s.%s", a==0?L"Auto-detect":a==1?L"forced 64-bit":L"forced 32-bit",
                a==0?L"":L" (Auto-detect is safest; a wrong choice will fail to load.)"); }
        return 0;
    case WM_DESTROY:
        DeleteObject(g_brBg); DeleteObject(g_brEdit); DeleteObject(g_brHdr);
        DeleteObject(g_font); DeleteObject(g_fontBtn); DeleteObject(g_fontTitle); DeleteObject(g_fontSmall);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int show) {
    WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.lpszClassName=L"SixDofInjector"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hbrBackground=CreateSolidBrush(CL_BG);
    wc.hIcon=LoadIconW(hInst,MAKEINTRESOURCEW(1));
    wc.hIconSm=(HICON)LoadImageW(hInst,MAKEINTRESOURCEW(1),IMAGE_ICON,16,16,0);
    RegisterClassExW(&wc);
    int W=700,H=912;
    int sx=(GetSystemMetrics(SM_CXSCREEN)-W)/2, sy=(GetSystemMetrics(SM_CYSCREEN)-H)/2;
    HWND hwnd=CreateWindowExW(0,wc.lpszClassName,APP_TITLE,
        (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX),
        sx,sy,W,H,nullptr,nullptr,hInst,nullptr);
    ShowWindow(hwnd,show); UpdateWindow(hwnd);
    MSG m; while (GetMessageW(&m,nullptr,0,0)){ if(!IsDialogMessageW(hwnd,&m)){ TranslateMessage(&m); DispatchMessageW(&m);} }
    return 0;
}
