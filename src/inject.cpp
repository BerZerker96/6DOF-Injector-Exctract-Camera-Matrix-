// 6DOFInject.exe - injects 6DOFProbe.dll into a running game so it can hook D3D11 and write the
// MOD BUILD SPEC to 6DOF-Probe.log next to the game exe.
//
// Usage:
//   6DOFInject.exe <process.exe>      e.g.  6DOFInject.exe Game.exe
//   6DOFInject.exe <PID>
// If the process isn't running yet, it waits up to 90s for it to appear (launch the game meanwhile).
// The injector arch must match the game: use 6DOFInject.exe for 64-bit games, 6DOFInject32.exe for 32-bit.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>
#include <cstdlib>

#ifdef PROBE32
  #define PROBE_DLL   L"6DOFProbe32.dll"
  #define RUNTIME_DLL L"6DOFRuntime32.dll"
  #define ARCHNAME  "32-bit"
  #define SELF64 false
#else
  #define PROBE_DLL   L"6DOFProbe.dll"
  #define RUNTIME_DLL L"6DOFRuntime.dll"
  #define ARCHNAME  "64-bit"
  #define SELF64 true
#endif

static DWORD findPid(const wchar_t* name){
    DWORD pid=0; HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snap==INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize=sizeof(pe);
    if (Process32FirstW(snap,&pe)) do { if (_wcsicmp(pe.szExeFile,name)==0){ pid=pe.th32ProcessID; break; } } while (Process32NextW(snap,&pe));
    CloseHandle(snap); return pid;
}

static bool inject(DWORD pid, const wchar_t* dllPath){
    HANDLE p=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);
    if (!p){ wprintf(L"  OpenProcess failed (%lu). Run the injector as Administrator.\n",GetLastError()); return false; }
    size_t bytes=(wcslen(dllPath)+1)*sizeof(wchar_t);
    void* remote=VirtualAllocEx(p,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if (!remote){ wprintf(L"  VirtualAllocEx failed (%lu)\n",GetLastError()); CloseHandle(p); return false; }
    if (!WriteProcessMemory(p,remote,dllPath,bytes,nullptr)){ wprintf(L"  WriteProcessMemory failed (%lu)\n",GetLastError()); VirtualFreeEx(p,remote,0,MEM_RELEASE); CloseHandle(p); return false; }
    HMODULE k32=GetModuleHandleW(L"kernel32.dll");
    auto loadLib=(LPTHREAD_START_ROUTINE)GetProcAddress(k32,"LoadLibraryW");
    HANDLE th=CreateRemoteThread(p,nullptr,0,loadLib,remote,0,nullptr);
    if (!th){ wprintf(L"  CreateRemoteThread failed (%lu)\n",GetLastError()); VirtualFreeEx(p,remote,0,MEM_RELEASE); CloseHandle(p); return false; }
    WaitForSingleObject(th,10000);
    DWORD ec=0; GetExitCodeThread(th,&ec);   // nonzero = HMODULE of loaded dll (low 32 bits)
    CloseHandle(th); VirtualFreeEx(p,remote,0,MEM_RELEASE); CloseHandle(p);
    if (ec==0){ wprintf(L"  LoadLibraryW returned 0 - the DLL failed to load (arch mismatch? wrong dll?).\n"); return false; }
    return true;
}

int wmain(int argc, wchar_t** argv){
    wprintf(L"6DOF Injector (%hs)\n", ARCHNAME);
    if (argc<2){ wprintf(L"Usage: %ls <process.exe | PID> [--runtime]\n  (default injects the probe to FIND the camera; --runtime injects the apply-side head-tracking DLL)\n", argv[0]); return 1; }

    // pick which DLL: probe (find camera) or runtime (apply head pose). The arch is auto-matched to this build,
    // which must match the target - verified below.
    bool wantRuntime=false; const wchar_t* target=nullptr;
    for(int i=1;i<argc;i++){ if(_wcsicmp(argv[i],L"--runtime")==0||_wcsicmp(argv[i],L"-r")==0) wantRuntime=true; else if(!target) target=argv[i]; }
    if(!target){ wprintf(L"ERROR: no target process given.\n"); return 1; }
    const wchar_t* DLL_NAME = wantRuntime?RUNTIME_DLL:PROBE_DLL;

    // resolve full path to the chosen dll next to this exe
    wchar_t selfDir[MAX_PATH]={0}; GetModuleFileNameW(nullptr,selfDir,MAX_PATH);
    if (wchar_t* s=wcsrchr(selfDir,L'\\')) *(s+1)=0;
    wchar_t dll[MAX_PATH]; wcscpy_s(dll,MAX_PATH,selfDir); wcscat_s(dll,MAX_PATH,DLL_NAME);
    if (GetFileAttributesW(dll)==INVALID_FILE_ATTRIBUTES){ wprintf(L"ERROR: %ls not found next to the injector.\n",DLL_NAME); return 1; }

    // target: numeric PID or process name
    DWORD pid=0; wchar_t* end=nullptr; unsigned long asNum=wcstoul(target,&end,10);
    bool numeric=(end&&*end==0&&asNum>0);
    if (numeric){ pid=(DWORD)asNum; }
    else {
        wprintf(L"Waiting for '%ls' (up to 90s; launch the game now if needed)...\n",target);
        for (int i=0;i<180 && pid==0;i++){ pid=findPid(target); if(!pid) Sleep(500); }
        if (!pid){ wprintf(L"ERROR: process '%ls' not found.\n",target); return 1; }
    }
    // verify the target's architecture matches this injector (cross-arch injection can't work)
    { HANDLE q=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
      if(q){ BOOL wow=FALSE; IsWow64Process(q,&wow); CloseHandle(q); bool target32=wow;     // wow64 => 32-bit game on 64-bit OS
        if(SELF64 && target32){ wprintf(L"ERROR: that game is 32-bit - use the 32-bit injector (6DOFInject32.exe).\n"); return 1; }
        if(!SELF64 && !target32){ wprintf(L"ERROR: that game is 64-bit - use the 64-bit injector (6DOFInject.exe).\n"); return 1; } } }
    wprintf(L"Injecting %ls into PID %lu ...\n",DLL_NAME,pid);
    Sleep(1500);   // small grace so the game's D3D device is up
    if (inject(pid,dll)){
        if(wantRuntime) wprintf(L"OK - runtime injected. It loads <game>.6dof.json, runs a cave self-test, and applies OpenTrack head pose.\n  In-game: F8 toggle, F9 recenter, F10/F11 invert yaw/pitch.\n");
        else            wprintf(L"OK - probe injected. Look for 6DOF-Probe.log next to the game exe.\n  In-game: press END for a report now (it also auto-writes every ~10s).\n");
        return 0; }
    wprintf(L"Injection failed.\n"); return 1;
}
