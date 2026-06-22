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
  #define APP_TITLE  L"6DOF Injector"
  #define ARCH_BADGE L"32-BIT"
  #define SELF64     false
#else
  #define PROBE_DLL  L"6DOFProbe.dll"
  #define APP_TITLE  L"6DOF Injector"
  #define ARCH_BADGE L"64-BIT"
  #define SELF64     true
#endif

enum { IDC_COMBO=1001, IDC_REFRESH, IDC_INJECT, IDC_STATUS, IDC_SEARCH, IDC_AUTO };
static HWND g_combo, g_refresh, g_inject, g_status, g_search, g_auto;
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
    CL_TRED   = RGB(235,58,62);    // title "INJECTOR"

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
static Btn g_bAuto{L"Auto-run deep scans (memory + differential) after detection",false,false,true,false};
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
    if (SELF64 && target32){ err=L"That game is 32-bit - use 6DOFInjectGUI32.exe instead."; CloseHandle(p); return false; }
    if (!SELF64 && !target32){ err=L"That game is 64-bit - use 6DOFInjectGUI.exe instead."; CloseHandle(p); return false; }
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
static void doInject(HWND) {
    int sel=(int)SendMessageW(g_combo,CB_GETCURSEL,0,0);
    if (sel<0 || sel>=(int)g_shown.size()){ statusf(L"Pick a process first."); return; }
    Proc pr=g_shown[sel];
    wchar_t self[MAX_PATH]={0}; GetModuleFileNameW(nullptr,self,MAX_PATH);
    if (wchar_t* s=wcsrchr(self,L'\\')) *(s+1)=0;
    wchar_t dll[MAX_PATH]; wcscpy_s(dll,MAX_PATH,self); wcscat_s(dll,MAX_PATH,PROBE_DLL);
    if (GetFileAttributesW(dll)==INVALID_FILE_ATTRIBUTES){ statusf(L"ERROR: %s not found next to this exe.",PROBE_DLL); return; }
    // write the GUI->probe config flag next to the DLL so the probe knows whether to auto-run the deep scans
    wchar_t cfg[MAX_PATH]; wcscpy_s(cfg,MAX_PATH,self); wcscat_s(cfg,MAX_PATH,L"6DOF.cfg");
    if(HANDLE cf=CreateFileW(cfg,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr); cf!=INVALID_HANDLE_VALUE){
        const char* line=g_bAuto.checked?"auto_extra_tests=1\n":"auto_extra_tests=0\n";
        DWORD wr; WriteFile(cf,line,(DWORD)strlen(line),&wr,nullptr); CloseHandle(cf); }
    statusf(L"Injecting into %s [pid %lu]  (auto deep scans: %s) ...",pr.name.c_str(),pr.pid,g_bAuto.checked?L"ON":L"off");
    std::wstring err;
    if (injectInto(pr.pid,dll,err)) {
        statusf(L"OK - injected. The probe AUTO-RUNS its discovery pipeline ~5s in.");
        statusf(L"Be in normal gameplay (not a menu). WATCH THE SCREEN: a brief camera sweep = it found the real camera.");
        if(g_bAuto.checked) statusf(L"Auto deep scans ON: memory scan + differential + report run automatically after detection (move the camera when asked).");
        else statusf(L"Keys in-game:  INSERT re-run  \u00B7  END report  \u00B7  HOME memory scan  \u00B7  F7/F8 differential.");
        statusf(L"Output folder:  .\\6DOF Output\\   ->  6DOF-<game>.log  +  <game>.exe.6dof.json (runtime profile).");
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
        HINSTANCE hi=((LPCREATESTRUCT)lp)->hInstance; const int PAD=20, W=540;
        CreateWindowExW(0,L"STATIC",L"FILTER",WS_CHILD|WS_VISIBLE,PAD,92,200,16,hwnd,0,hi,0);
        g_search=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
                                PAD,112,330,32,hwnd,(HMENU)IDC_SEARCH,hi,0);
        g_refresh=CreateWindowExW(0,L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                W-PAD-150,112,150,32,hwnd,(HMENU)IDC_REFRESH,hi,0);
        CreateWindowExW(0,L"STATIC",L"TARGET PROCESS",WS_CHILD|WS_VISIBLE,PAD,156,260,16,hwnd,0,hi,0);
        g_combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,
                                PAD,176,W-2*PAD,420,hwnd,(HMENU)IDC_COMBO,hi,0);
        g_auto=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                PAD,216,W-2*PAD,24,hwnd,(HMENU)IDC_AUTO,hi,0);
        g_inject=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                PAD,248,W-2*PAD,50,hwnd,(HMENU)IDC_INJECT,hi,0);
        g_status=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
                                PAD+10,324,W-2*PAD-20,148,hwnd,(HMENU)IDC_STATUS,hi,0);
        SendMessageW(g_search,WM_SETFONT,(WPARAM)g_font,TRUE);
        SendMessageW(g_combo,WM_SETFONT,(WPARAM)g_font,TRUE);
        SendMessageW(g_status,WM_SETFONT,(WPARAM)g_font,TRUE);
        makeButton(g_refresh,&g_bRefresh); makeButton(g_inject,&g_bInject); makeButton(g_auto,&g_bAuto);
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
        SelectObject(dc,g_fontTitle); SetTextColor(dc,CL_CYAN); TextOutW(dc,20,16,L"6DOF",4);
        SIZE sz; GetTextExtentPoint32W(dc,L"6DOF",4,&sz);
        SetTextColor(dc,CL_TRED); TextOutW(dc,20+sz.cx+8,16,L"INJECTOR",8);
        SelectObject(dc,g_fontSmall); SetTextColor(dc,CL_MUTE);
        TextOutW(dc,21,50,L"Camera discovery  +  head-tracking probe",40);
        RECT bd={cr.right-92,18,cr.right-20,42}; roundOutline(dc,bd,CL_ACC,12);
        SetTextColor(dc,CL_ACC); DrawTextW(dc,ARCH_BADGE,-1,&bd,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        HPEN ap=CreatePen(PS_SOLID,2,CL_ACC); HGDIOBJ opn=SelectObject(dc,ap); MoveToEx(dc,0,76,0); LineTo(dc,cr.right,76); SelectObject(dc,opn); DeleteObject(ap);
        RECT sp={20,318,cr.right-20,478}; roundOutline(dc,sp,CL_BORDER,10);
        SelectObject(dc,g_fontSmall); SetTextColor(dc,CL_MUTE);
        TextOutW(dc,20,cr.bottom-24,L"Loopback UDP 4242  \u00B7  OpenTrack  \u00B7  log + .6dof.json profile",58);
        EndPaint(hwnd,&ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_COMMAND:
        if (LOWORD(wp)==IDC_REFRESH){ refreshProcs(); statusf(L"Process list refreshed (%zu).",g_procs.size()); }
        else if (LOWORD(wp)==IDC_INJECT){ doInject(hwnd); }
        else if (LOWORD(wp)==IDC_AUTO){ statusf(L"Auto deep scans %s. Applies on the next INJECT.",g_bAuto.checked?L"ENABLED":L"disabled"); }
        else if (LOWORD(wp)==IDC_SEARCH && HIWORD(wp)==EN_CHANGE){ applyFilter(); }
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
    int W=540,H=548;
    int sx=(GetSystemMetrics(SM_CXSCREEN)-W)/2, sy=(GetSystemMetrics(SM_CYSCREEN)-H)/2;
    HWND hwnd=CreateWindowExW(0,wc.lpszClassName,APP_TITLE,
        (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX),
        sx,sy,W,H,nullptr,nullptr,hInst,nullptr);
    ShowWindow(hwnd,show); UpdateWindow(hwnd);
    MSG m; while (GetMessageW(&m,nullptr,0,0)){ if(!IsDialogMessageW(hwnd,&m)){ TranslateMessage(&m); DispatchMessageW(&m);} }
    return 0;
}
