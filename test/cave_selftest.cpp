// ============================================================================
// cave_selftest.cpp - HOST-RUNNABLE correctness test for the runtime's pure
// capture-cave logic: the length-disassembler (insLen) and the stolen-byte
// relocator (copyRelocated). These are the two riskiest functions in the cave
// and have NO OS dependencies, so we can compile them natively (x86-64) and
// prove them against a corpus drawn from our real per-game mods before anyone
// trusts the cave on a live game.
//
// The function bodies in _extracted.inc are pulled VERBATIM from
// runtime/sixdof_runtime.cpp by the build step, so this test exercises the
// shipping code, not a copy that could drift.
//
// Build/run:  g++ -O2 -std=c++17 cave_selftest.cpp -o cave_selftest && ./cave_selftest
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>

// the three pure functions, extracted verbatim from the runtime:
#include "_extracted.inc"

static int g_pass=0, g_fail=0;
static void ck(bool ok,const char* what){ printf("  [%s] %s\n", ok?"PASS":"FAIL", what); if(ok)g_pass++; else g_fail++; }

struct LenCase{ const char* name; uint8_t b[16]; int n; int expect; };

int main(){
    printf("== insLen: instruction-length decoder (corpus from the real mods) ==\n");
    // Each is a real camera-write / prologue instruction we hook or steal across the per-game mods.
    LenCase L[] = {
        {"mov [rdi+0x70],eax   (Witcher quat write)", {0x89,0x47,0x70}, 3, 3},
        {"mov eax,[r14+0x14]   (Witcher src read)",   {0x41,0x8B,0x46,0x14}, 4, 4},
        {"movss [rcx+0x70],xmm0 (SSE store, disp8)",  {0xF3,0x0F,0x11,0x41,0x70}, 5, 5},
        {"movaps [rcx],xmm0    (DAI row copy)",       {0x0F,0x29,0x01}, 3, 3},
        {"movups [rcx+0x10],xmm1",                    {0x0F,0x11,0x49,0x10}, 4, 4},
        {"mov [rdx],rcx        (REX.W store)",        {0x48,0x89,0x0A}, 3, 3},
        {"mov dword [rdi+0x70],imm32 (C7)",           {0xC7,0x47,0x70,0x00,0x00,0x80,0x3F}, 7, 7},
        {"mov rax,[rip+0x3412] (rip-relative)",       {0x48,0x8B,0x05,0x12,0x34,0x00,0x00}, 7, 7},
        {"lea rcx,[rip+0x1234] (rip-relative)",       {0x48,0x8D,0x0D,0x34,0x12,0x00,0x00}, 7, 7},
        {"mov [rax+rcx*4+0x20],edx (SIB+disp8)",      {0x89,0x54,0x88,0x20}, 4, 4},
        {"mov [rax+rcx*4+disp32],edx (SIB+disp32)",   {0x89,0x94,0x88,0x00,0x01,0x00,0x00}, 7, 7},
        {"vmovss [rcx+0x70],xmm0 (2-byte VEX)",       {0xC5,0xFA,0x11,0x41,0x70}, 5, 5},
        {"vmovaps [rcx],ymm0 (3-byte VEX)",           {0xC4,0xC1,0x7C,0x29,0x01}, 5, 5},
        {"mov eax,imm32 (B8)",                        {0xB8,0x00,0x00,0x00,0x00}, 5, 5},
        {"push rbx / pop / ret seq: push rbx",        {0x53}, 1, 1},
        {"jmp rel32 (E9)",                            {0xE9,0x00,0x00,0x00,0x00}, 5, 5},
        {"jcc rel32 (0F 8E)",                         {0x0F,0x8E,0x10,0x00,0x00,0x00}, 6, 6},
        {"jmp rel8 (EB)",                             {0xEB,0x05}, 2, 2},
    };
    for(auto& c: L){ int got=insLen(c.b); char msg[128]; snprintf(msg,sizeof(msg),"%s -> len %d (want %d)",c.name,got,c.expect); ck(got==c.expect,msg); }

    printf("\n== copyRelocated: rip-relative relocation preserves the absolute target ==\n");
    // Place a rip-relative `mov rax,[rip+disp]` at srcVA, relocate it to a nearby cave (dstVA), and confirm the
    // recomputed disp32 still resolves to the SAME absolute address from the cave.
    {
        uint8_t src[7]={0x48,0x8B,0x05,0x12,0x34,0x00,0x00}; int len=7;
        uint64_t srcVA=0x140001000ull;            int32_t d0=*(int32_t*)(src+3);
        uint64_t absTarget=srcVA+len+(int64_t)d0; // what the original instruction reads
        uint8_t dst[7]={0};
        uint64_t dstVA=srcVA+0x100000ull;         // 1 MB away (within rel32 reach)
        bool ok=copyRelocated(dst,src,len,srcVA,dstVA);
        ck(ok,"relocation accepted a near rip-relative store");
        int32_t d1=*(int32_t*)(dst+3);
        uint64_t newAbs=dstVA+len+(int64_t)d1;
        char m[160]; snprintf(m,sizeof(m),"absolute target preserved: orig %#llx == relocated %#llx",
            (unsigned long long)absTarget,(unsigned long long)newAbs);
        ck(newAbs==absTarget,m);
    }
    {
        // Same instruction but a cave > 2 GB away must be REFUSED (disp can't reach in rel32).
        uint8_t src[7]={0x48,0x8B,0x05,0x12,0x34,0x00,0x00}; uint8_t dst[7];
        uint64_t srcVA=0x140001000ull, farVA=srcVA+0x90000000ull; // ~2.25 GB away
        bool ok=copyRelocated(dst,src,7,srcVA,farVA);
        ck(!ok,"relocation REFUSED a rip-relative target out of rel32 reach");
    }

    printf("\n== copyRelocated: refuses what it must not blindly copy ==\n");
    {
        uint8_t br[5]={0xE9,0x10,0x00,0x00,0x00}; uint8_t dst[5];
        ck(!copyRelocated(dst,br,5,0x140001000ull,0x150001000ull), "refuses a relative jmp rel32 in the steal window");
    }
    {
        uint8_t jcc[6]={0x0F,0x8E,0x10,0x00,0x00,0x00}; uint8_t dst[6];
        ck(!copyRelocated(dst,jcc,6,0x140001000ull,0x150001000ull), "refuses a jcc rel32");
    }
    {
        uint8_t sj[2]={0xEB,0x05}; uint8_t dst[2];
        ck(!copyRelocated(dst,sj,2,0x140001000ull,0x150001000ull), "refuses a short jmp rel8");
    }
    {
        uint8_t vex[5]={0xC5,0xFA,0x11,0x41,0x70}; uint8_t dst[5];
        ck(!copyRelocated(dst,vex,5,0x140001000ull,0x140101000ull), "refuses a VEX/AVX store (HWBP path covers it)");
    }

    printf("\n== copyRelocated: plain stores copy byte-for-byte (no relocation needed) ==\n");
    {
        uint8_t s[5]={0xF3,0x0F,0x11,0x41,0x70}; uint8_t dst[5]={0};
        bool ok=copyRelocated(dst,s,5,0x140001000ull,0x140101000ull);
        ck(ok && memcmp(dst,s,5)==0, "movss [rcx+0x70],xmm0 copied verbatim");
    }
    {
        // a realistic 5-byte steal: movss store + start of next instr, all plain stores
        uint8_t s[8]={0x89,0x47,0x70, 0x41,0x8B,0x46,0x14, 0x90}; uint8_t dst[8]={0};
        bool ok=copyRelocated(dst,s,8,0x140001000ull,0x140101000ull);
        ck(ok && memcmp(dst,s,8)==0, "Witcher steal window (3+4+1 bytes) copied verbatim");
    }

    printf("\n== SUMMARY: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail? 1 : 0;
}
