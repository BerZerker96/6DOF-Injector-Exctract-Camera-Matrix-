// 6DOFInjectGUI - dark-themed Win32 injector. Pick a running process from the dropdown and click
// INJECT. Portable: run it from any folder; it injects the 6DOFProbe DLL sitting next to it, and the
// probe writes 6DOF-<Game>.log into that same folder.
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
  #define APP_TITLE  L"6DOF Injector (32-bit)"
  #define SELF64     false
#else
  #define PROBE_DLL  L"6DOFProbe.dll"
  #define APP_TITLE  L"6DOF Injector (64-bit)"
  #define SELF64     true
#endif

enum { IDC_COMBO=1001, IDC_REFRESH, IDC_INJECT, IDC_STATUS, IDC_SEARCH };
static HWND g_combo, g_refresh, g_inject, g_status, g_search;
static HFONT g_font, g_fontBtn;
static HBRUSH g_brBg, g_brEdit;
static const COLORREF CL_BG=RGB(28,28,30), CL_TXT=RGB(225,225,225), CL_EDIT=RGB(18,18,20),
                      CL_RED=RGB(200,42,42), CL_REDDN=RGB(150,28,28);

struct Proc { std::wstring name; DWORD pid; };
static std::vector<Proc> g_procs;   // all processes
static std::vector<Proc> g_shown;   // currently shown (after filter)

static void statusf(const wchar_t* fmt, ...) {
    wchar_t buf[1024]; va_list ap; va_start(ap,fmt); _vsnwprintf_s(buf,1024,_TRUNCATE,fmt,ap); va_end(ap);
    int len=GetWindowTextLengthW(g_status);
    SendMessageW(g_status,EM_SETSEL,len,len);
    SendMessageW(g_status,EM_REPLACESEL,FALSE,(LPARAM)buf);
    SendMessageW(g_status,EM_REPLACESEL,FALSE,(LPARAM)L"\r\n");
}

static void applyFilter() {
    wchar_t q[128]={0}; if(g_search) GetWindowTextW(g_search,q,127);
    for(int i=0;q[i];i++) q[i]=towlower(q[i]);
    g_shown.clear();
    SendMessageW(g_combo,CB_RESETCONTENT,0,0);
    for (auto& p : g_procs){
        std::wstring low=p.name; for(auto&c:low) c=towlower(c);
        if (q[0]==0 || low.find(q)!=std::wstring::npos){
            g_shown.push_back(p);
            wchar_t line[300]; _snwprintf_s(line,300,_TRUNCATE,L"%s   [pid %lu]",p.name.c_str(),p.pid);
            SendMessageW(g_combo,CB_ADDSTRING,0,(LPARAM)line);
        }
    }
    if (!g_shown.empty()) SendMessageW(g_combo,CB_SETCURSEL,0,0);
}
static void refreshProcs() {
    g_procs.clear();
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snap!=INVALID_HANDLE_VALUE){
        PROCESSENTRY32W pe{}; pe.dwSize=sizeof(pe);
        if (Process32FirstW(snap,&pe)) do {
            if (pe.th32ProcessID<=4) continue;
            g_procs.push_back({pe.szExeFile, pe.th32ProcessID});
        } while (Process32NextW(snap,&pe));
        CloseHandle(snap);
    }
    std::sort(g_procs.begin(),g_procs.end(),[](const Proc&a,const Proc&b){
        return _wcsicmp(a.name.c_str(),b.name.c_str())<0; });
    applyFilter();
}

static bool injectInto(DWORD pid, const wchar_t* dll, std::wstring& err) {
    HANDLE p=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);
    if (!p){ err=L"OpenProcess failed - try running the injector as Administrator."; return false; }
    // arch check
    BOOL wow=FALSE; IsWow64Process(p,&wow);
    bool target32 = wow;                 // on a 64-bit OS, WOW64 == 32-bit process
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

static void doInject(HWND hwnd) {
    int sel=(int)SendMessageW(g_combo,CB_GETCURSEL,0,0);
    if (sel<0 || sel>=(int)g_shown.size()){ statusf(L"Pick a process first."); return; }
    Proc pr=g_shown[sel];
    wchar_t self[MAX_PATH]={0}; GetModuleFileNameW(nullptr,self,MAX_PATH);
    if (wchar_t* s=wcsrchr(self,L'\\')) *(s+1)=0;
    wchar_t dll[MAX_PATH]; wcscpy_s(dll,MAX_PATH,self); wcscat_s(dll,MAX_PATH,PROBE_DLL);
    if (GetFileAttributesW(dll)==INVALID_FILE_ATTRIBUTES){ statusf(L"ERROR: %s not found next to this exe.",PROBE_DLL); return; }
    statusf(L"Injecting into %s [pid %lu] ...",pr.name.c_str(),pr.pid);
    std::wstring err;
    if (injectInto(pr.pid,dll,err)) {
        statusf(L"OK - injected. Play ~20-30s, press END in-game for a report.");
        statusf(L"Log: 6DOF-%s.log  (in this folder).", [&]{ std::wstring n=pr.name; size_t d=n.rfind(L'.'); if(d!=std::wstring::npos)n=n.substr(0,d); return n; }().c_str());
    } else statusf(L"FAILED: %s", err.c_str());
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_brBg=CreateSolidBrush(CL_BG); g_brEdit=CreateSolidBrush(CL_EDIT);
        g_font=CreateFontW(-15,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        g_fontBtn=CreateFontW(-20,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        HINSTANCE hi=((LPCREATESTRUCT)lp)->hInstance;
        CreateWindowExW(0,L"STATIC",L"Filter (type part of the game's exe name):",WS_CHILD|WS_VISIBLE,16,12,460,18,hwnd,0,hi,0);
        g_search=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
                                16,34,358,26,hwnd,(HMENU)IDC_SEARCH,hi,0);
        g_refresh=CreateWindowExW(0,L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
                                382,34,92,26,hwnd,(HMENU)IDC_REFRESH,hi,0);
        g_combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,
                                16,68,458,400,hwnd,(HMENU)IDC_COMBO,hi,0);
        g_inject=CreateWindowExW(0,L"BUTTON",L"INJECT",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
                                16,106,458,44,hwnd,(HMENU)IDC_INJECT,hi,0);
        g_status=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
                                16,162,458,150,hwnd,(HMENU)IDC_STATUS,hi,0);
        for (HWND c : {g_search,g_combo,g_refresh,g_inject,g_status}) SendMessageW(c,WM_SETFONT,(WPARAM)g_font,TRUE);
        SendMessageW(g_inject,WM_SETFONT,(WPARAM)g_fontBtn,TRUE);
        // dark title bar (Win10+) - try both attribute indices
        BOOL dark=TRUE; DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark)); DwmSetWindowAttribute(hwnd,19,&dark,sizeof(dark));
        refreshProcs();
        statusf(L"Ready. Pick the game and click INJECT.");
        statusf(L"Portable: keep %s next to this exe. The log lands here, named after the game.",PROBE_DLL);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc=(HDC)wp; SetTextColor(dc,CL_TXT);
        SetBkColor(dc,(HWND)lp==g_status?CL_EDIT:CL_BG);
        return (LRESULT)((HWND)lp==g_status?g_brEdit:g_brBg);
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
        HDC dc=(HDC)wp; SetTextColor(dc,CL_TXT); SetBkColor(dc,CL_EDIT); return (LRESULT)g_brEdit;
    }
    case WM_CTLCOLORBTN: return (LRESULT)g_brBg;
    case WM_DRAWITEM: {
        if (wp==IDC_INJECT){
            DRAWITEMSTRUCT* d=(DRAWITEMSTRUCT*)lp;
            bool down=(d->itemState&ODS_SELECTED);
            HBRUSH b=CreateSolidBrush(down?CL_REDDN:CL_RED);
            FillRect(d->hDC,&d->rcItem,b); DeleteObject(b);
            SetBkMode(d->hDC,TRANSPARENT); SetTextColor(d->hDC,RGB(255,255,255));
            HFONT old=(HFONT)SelectObject(d->hDC,g_fontBtn);
            DrawTextW(d->hDC,L"INJECT",-1,&d->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SelectObject(d->hDC,old);
            if (d->itemState&ODS_FOCUS){ RECT r=d->rcItem; InflateRect(&r,-3,-3); DrawFocusRect(d->hDC,&r); }
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp)==IDC_REFRESH){ refreshProcs(); statusf(L"Process list refreshed (%zu).",g_procs.size()); }
        else if (LOWORD(wp)==IDC_INJECT){ doInject(hwnd); }
        else if (LOWORD(wp)==IDC_SEARCH && HIWORD(wp)==EN_CHANGE){ applyFilter(); }
        return 0;
    case WM_ERASEBKGND: { RECT r; GetClientRect(hwnd,&r); FillRect((HDC)wp,&r,g_brBg); return 1; }
    case WM_DESTROY:
        DeleteObject(g_brBg); DeleteObject(g_brEdit); DeleteObject(g_font); DeleteObject(g_fontBtn);
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
    int W=500,H=372;
    int sx=(GetSystemMetrics(SM_CXSCREEN)-W)/2, sy=(GetSystemMetrics(SM_CYSCREEN)-H)/2;
    HWND hwnd=CreateWindowExW(0,wc.lpszClassName,APP_TITLE,
        (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX),
        sx,sy,W,H,nullptr,nullptr,hInst,nullptr);
    ShowWindow(hwnd,show); UpdateWindow(hwnd);
    MSG m; while (GetMessageW(&m,nullptr,0,0)){ if(!IsDialogMessageW(hwnd,&m)){ TranslateMessage(&m); DispatchMessageW(&m);} }
    return 0;
}
