// Salvatore-specific xNVSE plugin. Copyright (c) 2026 2Shay. MIT.
// Engine layouts: pinned xNVSE SDK; interception location: ShowOff reverse engineering.
// Pre-equip cancellation, then a revalidated game-loop transaction into the existing quest.
// v1.2: draws Salvatore's hidden lit VATS proxy only into VATS limb occlusion queries, keeps VATS from
// finishing his single limb before its query result arrives, and lets his theme pause in menus and carry on
// from where it paused (see below).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "nvse_abi.h"

constexpr uintptr_t kClickCall = 0x7805CC;
constexpr uintptr_t kSelection = 0x11D9EA8;
constexpr uintptr_t kPlayer = 0x11DEA3C;
constexpr unsigned kRuntime = 0x040020D0;
constexpr uint8_t kSOUN = 13, kWEAP = 40, kCREA = 43;

struct Form { void* vtable; uint8_t type, pad[3]; uint32_t flags, id; };
struct Extra { void* vtable; uint8_t type, pad[3]; Extra* next; };
struct ExtraList { void* vtable; Extra* head; uint8_t rest[24]; };
template<class T> struct Node { T* data; Node* next; };
struct Entry { Node<ExtraList>* lists; int32_t count; Form* form; };
struct Changes { Node<Entry>* entries; void* owner; };
struct FormBucket { FormBucket* next; uint32_t key; Form* form; };
struct FormMap { void* vtable; uint32_t count; FormBucket** buckets; uint32_t items; };
static_assert(sizeof(Entry)==12 && sizeof(ExtraList)==32 && offsetof(Form,id)==12,"engine ABI");

static ScriptAPI* scripts;
static ConsoleAPI* consoleAPI;
static FILE* logFile;
static void* targetExpression;
static Form* target;
static Form* quest;
static Form* salvatore;  // his CREA base form (VATS target check)
static void *menuExpression, *countExpression, *stateExpression, *versionExpression, *latchExpression;
static void *transaction;
static uint32_t expressionPrefix=0xFFFFFFFF;
static bool deployQueued, deploymentAudit, selectionFeedbackPending;
static uintptr_t clickedIdentity;
static DWORD queuedAt, committedAt;
static unsigned clicks, ordinary;
static bool hookInstalled, loaded, auditPending;
static uint8_t installedBytes[5];

static void Log(const char* format, ...) {
    if (!logFile) return;
    fprintf(logFile,"tick=%lu ",(unsigned long)GetTickCount());
    va_list args; va_start(args,format); vfprintf(logFile,format,args); va_end(args);
    fputc('\n',logFile); fflush(logFile);
}
template<class T> static T Read(const void* p, size_t offset) {
    T value; memcpy(&value, static_cast<const uint8_t*>(p)+offset,sizeof value); return value;
}
static Extra* Find(ExtraList* list, uint8_t type) {
    unsigned guard=0;
    for (Extra* x=list?list->head:nullptr; x && guard++<128; x=x->next)
        if(x->type==type) return x;
    return nullptr;
}
static uint8_t Mods(ExtraList* list) {
    auto* x=Find(list,0x8D); return x?Read<uint8_t>(x,12):0;
}
struct Health { bool valid, explicitHealth; double current, maximum, percent; uint8_t mods; };
static Health GetHealth(Form* form, ExtraList* list) {
    Health h{};
    if(!form || form->type!=kWEAP) return h;
    h.maximum=Read<uint32_t>(form,0x98); h.mods=Mods(list);
    // Weapon-mod effect 10 adds to maximum condition. No player skill multiplier.
    for(unsigned i=0;i<3;++i)
        if((h.mods&(1u<<i)) && Read<uint32_t>(form,0x180+4*i)==10) {
            h.maximum+=Read<float>(form,0x18C+4*i);
            break; // engine's first matching increase-health mod
        }
    auto* x=Find(list,0x25); h.explicitHealth=x!=nullptr;
    h.current=x?Read<float>(x,12):h.maximum;
    h.valid=isfinite(h.current)&&isfinite(h.maximum)&&h.maximum>0&&h.current>=0;
    if(h.valid) h.percent=fmin(100.0,100.0*h.current/h.maximum);
    return h;
}
struct Gun { uint32_t id; uintptr_t stack; double current; uint8_t mods; };
static Gun Snapshot() {
    Gun gun{};
    auto* player=*reinterpret_cast<uint8_t**>(kPlayer);
    if(!player) return gun;
    auto* x=Find(reinterpret_cast<ExtraList*>(player+0x44),0x15);
    auto* changes=x?Read<Changes*>(x,12):nullptr;
    unsigned guard=0;
    for(auto* n=changes?changes->entries:nullptr;n && guard++<8192;n=n->next) {
        Entry* e=n->data;
        if(!e||!e->form||e->form->type!=kWEAP) continue;
        unsigned inner=0;
        for(auto* s=e->lists;s && inner++<8192;s=s->next)
            if(s->data&&Find(s->data,0x16)) {
                const auto h=GetHealth(e->form,s->data);
                return {e->form->id,reinterpret_cast<uintptr_t>(s->data),h.current,h.mods};
            }
    }
    return gun; // No ExtraWorn WEAP: unarmed. Never equip Fists to simulate it.
}
static Gun before;
static Gun committedGun;
static bool Same(const Gun& a,const Gun& b) {
    return a.id==b.id&&a.stack==b.stack&&a.current==b.current&&a.mods==b.mods;
}

extern "C" __attribute__((used,noinline)) int sg_should_cancel() {
    auto* e=*reinterpret_cast<Entry**>(kSelection);
    if(!loaded||!target||!e||e->form!=target) {
        ++ordinary;
        Log("PASS_THROUGH item=%08X total=%u",e&&e->form?e->form->id:0,ordinary);
        return 0;
    }
    ++clicks;
    ExtraList* list=e->lists?e->lists->data:nullptr;
    Health h=GetHealth(e->form,list);
    before=Snapshot(); auditPending=true;
    Log("CANCEL click=%u item=%08X clickedStack=%p count=%d explicitHealth=%d health=%.9g max=%.9g percent=%.9g valid=%d gun=%08X gunStack=%08X gunHealth=%.9g gunMods=%u",
        clicks,e->form->id,list,e->count,h.explicitHealth,h.current,h.maximum,h.percent,h.valid,
        before.id,(unsigned)before.stack,before.current,before.mods);
    // Capture only an identity token, never a dereferenceable inventory handle.
    // Deferred code enumerates current inventory anew and reads fresh health.
    if(!deployQueued && !deploymentAudit) {
        deployQueued=true; selectionFeedbackPending=true; clickedIdentity=reinterpret_cast<uintptr_t>(list);
        queuedAt=GetTickCount();
    } else Log("CLICK_ALREADY_PENDING click=%u",clicks);
    return 1;
}

// Preserve all caller registers/flags. Returning from the helper is NOT sufficient:
// cancellation skips the rest of this click branch using its established epilogue.
extern "C" __attribute__((naked,used)) void sg_click_hook() {
    __asm__ volatile(
        "pushfl\n\tpushal\n\tcall _sg_should_cancel\n\ttestl %eax,%eax\n\tjz 1f\n\t"
        "popal\n\tpopfl\n\taddl $4,%esp\n\tpushl $0x780B8E\n\tret\n\t"
        "1: popal\n\tpopfl\n\tpushl $0x780D60\n\tret\n\t");
}
static bool InstallHook() {
    // Exact original CALL 0x780D60. Existing hooks are refused, never overwritten.
    const uint8_t expected[]={0xE8,0x8F,0x07,0x00,0x00};
    auto* site=reinterpret_cast<uint8_t*>(kClickCall);
    if(memcmp(site,expected,5)) {
        Log("HOOK_REFUSED site=%08X bytes=%02X%02X%02X%02X%02X",(unsigned)kClickCall,site[0],site[1],site[2],site[3],site[4]);
        return false;
    }
    installedBytes[0]=0xE8;
    int32_t relative=reinterpret_cast<uintptr_t>(&sg_click_hook)-(kClickCall+5);
    memcpy(installedBytes+1,&relative,4);
    DWORD oldProtect;
    if(!VirtualProtect(site,5,PAGE_EXECUTE_READWRITE,&oldProtect)) return false;
    memcpy(site,installedBytes,5); FlushInstructionCache(GetCurrentProcess(),site,5);
    DWORD ignored; VirtualProtect(site,5,oldProtect,&ignored);
    Log("HOOK_INSTALLED original=E88F070000 mode=deployment_candidate1");
    return true;
}
// v1.2 VATS visibility. FNV's VATS limb occlusion query (VATSEffect 0x800F30, render mode 8) AppCulls every
// root child without skinned geometry (all of Salvatore's clip gates) and only draws BSSM_CONSTALPHA passes,
// which NoLighting sprites never get, so his limb visibility was always 0 and every VATS chance 0%. The v1.2
// skeleton adds an AppCulled billboard with one lit slab under 'Bip01 NonAccum'. During his two query passes
// the limb render's whole-root cull (0x80161F) is followed by one more cull of that billboard alone, so the
// engine counts the slab like any lit rigid mesh; normal rendering, shadows and the VATS highlight never draw
// it. The billboard is refreshed (world transform and bound) and un-culled first: the scene cull skips any
// node whose world bound is missing or empty (NiNode::OnVisible 0xA5DBE0), and the candidate3 route through
// the Part Node drew nothing. Evidence: projects/salvatore/handoff/V1.2-VATS-RUNTIME-TRACE-20261027.md.
constexpr uintptr_t kQueryPass = 0x800F30, kFindObject = 0x4AAE30, kCullScene = 0xB6BEE0;
constexpr uintptr_t kQueryCalls[2] = {0x874A82, 0x8741AF};  // pass 1 "entire limb", pass 2 "partial limb"
constexpr uintptr_t kRootCull = 0x80161F;  // limb render 0x801450: cull the target's whole 3D root
constexpr const char* kProxy = "SalvatoreVatsProxyBillboard";
typedef uint32_t (__fastcall* QueryPassFn)(void*, void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (__cdecl* CullSceneFn)(void* camera, void* scene, void* culler);
typedef void (__fastcall* UpdateFn)(void* object, void* edx, void* updateData, uint32_t flags);
static bool queryHooked[2], cullHooked;
static uint8_t queryBytes[2][5], cullBytes[5];
static uint8_t *activeProxy, *activeRoot;  // set only while one of Salvatore's two query passes runs
static uint8_t* countedRoot;  // 3D root whose latest query pass drew the proxy
static uint32_t activePass;
static bool proxyShown;
static unsigned proxyDraws, proxyMisses;

struct Bound { float x, y, z, r; };
static Bound WorldBound(const uint8_t* object) {
    auto* b=object?Read<const float*>(object,0x20):nullptr;  // NiAVObject world NiBound*, may be null
    return b?Bound{b[0],b[1],b[2],b[3]}:Bound{0,0,0,-1};
}
static uint8_t* FirstChild(const uint8_t* node) {
    auto* children=Read<uint8_t**>(node,0xA0);  // NiNode children array; +0xA6 size
    return children&&Read<uint16_t>(node,0xA6)?children[0]:nullptr;
}
static int VatsPasses(const uint8_t* geometry) {
    auto* property=geometry?Read<const uint8_t*>(geometry,0xA8):nullptr;  // BSShaderProperty
    auto* list=property?Read<const uint8_t*>(property,0x44):nullptr;    // mode-8 pass list (0xB67CEE)
    return list?static_cast<int>(Read<uint32_t>(list,0x10)):-1;
}

// The limb render's cdecl call that culls the target's 3D root into the query accumulator.
extern "C" __attribute__((used)) void __cdecl sg_vats_root_cull(void* camera, void* scene, void* culler) {
    reinterpret_cast<CullSceneFn>(kCullScene)(camera,scene,culler);
    uint8_t* proxy=activeProxy;
    if(!proxy||scene!=activeRoot||!camera||!culler) return;
    activeProxy=nullptr;  // once per query pass
    auto* flags=reinterpret_cast<uint32_t*>(proxy+0x30);  // NiAVObject flags; bit 0 AppCulled
    const uint32_t flagsBefore=*flags;
    const Bound before=WorldBound(proxy), parent=WorldBound(Read<uint8_t*>(proxy,0x18));
    if(*flags&1) {*flags&=~1u; proxyShown=true;}
    uint8_t updateData[16]{};  // NiUpdateData: time 0, no controller update
    Read<UpdateFn>(Read<void*>(proxy,0),0xA4)(proxy,nullptr,updateData,0);  // UpdateDownwardPass
    reinterpret_cast<CullSceneFn>(kCullScene)(camera,proxy,culler);
    if(proxyDraws++<6) {
        auto* slab=FirstChild(proxy);
        const Bound after=WorldBound(proxy), geo=WorldBound(slab);
        auto* bb=reinterpret_cast<const float*>(proxy+0x8C);  // world translation
        auto* at=slab?reinterpret_cast<const float*>(slab+0x8C):bb;
        auto* eye=reinterpret_cast<const float*>(static_cast<uint8_t*>(camera)+0x8C);
        Log("VATS_PROXY_DRAWN pass=%u n=%u flags=%08X bound=%.1f->%.1f parentBound=%.1f at=(%.0f,%.0f,%.0f) "
            "slab=%p flags=%08X bound=%.1f at=(%.0f,%.0f,%.0f) shader=%p property=%p vatsPasses=%d camera=(%.0f,%.0f,%.0f)",
            activePass,proxyDraws,flagsBefore,before.r,after.r,parent.r,bb[0],bb[1],bb[2],
            slab,slab?Read<uint32_t>(slab,0x30):0,geo.r,at[0],at[1],at[2],
            slab?Read<void*>(slab,0xC0):nullptr,slab?Read<void*>(slab,0xA8):nullptr,VatsPasses(slab),eye[0],eye[1],eye[2]);
    }
}
// Thiscall VATSEffect::RenderLimbQuery(camera, pass, ...6 args) via fastcall (ecx=effect; callee pops 0x18).
extern "C" __attribute__((used)) uint32_t __fastcall sg_vats_query_pass(void* effect, void* edx, void* camera,
        uint32_t pass, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    auto* root=Read<uint8_t*>(effect,8);  // the target 3D this pass renders (VATSEffect::SetTarget 0x800AC0)
    auto* proxy=root?reinterpret_cast<uint8_t*(__cdecl*)(void*,const char*)>(kFindObject)(root,kProxy):nullptr;
    activeProxy=proxy; activeRoot=root; activePass=pass; proxyShown=false;
    uint32_t result=reinterpret_cast<QueryPassFn>(kQueryPass)(effect,edx,camera,pass,a,b,c,d);
    if(proxyShown) *reinterpret_cast<uint32_t*>(proxy+0x30)|=1u;
    if(activeProxy&&proxyMisses++<3) Log("VATS_PROXY_NOT_DRAWN pass=%u",pass);
    countedRoot=proxy&&!activeProxy?root:nullptr;
    activeProxy=nullptr; activeRoot=nullptr; proxyShown=false;
    return result;
}

// v1.2 VATS menu. Salvatore's body part data has one VATS limb (torso). VATSMenu's per-frame target update
// (0x7F3E00, called only at 0x7ED349) finalizes a target once no unfinished limb is flagged on-screen (+0x2C)
// and none is left to request; it never waits for a requested query (+0x2E). The remaining requests keep a
// normal target busy until its results arrive, but Salvatore's single limb was finalized in the frame after its
// request, before the result was read. Melee then never got a chance (only guns requery later), and guns read
// that old result on requery: 0 pixels when the query ran before the VATS camera had turned to him. While his
// limb is unfinished this wrapper flags it on-screen, so the menu waits for (or requests) its result, and
// discards a result with no pixels at all from a query that drew the proxy (he was off-screen), so the menu
// asks again. The engine still reads and scores every real result, and its 2-second VATS timeout still applies.
constexpr uintptr_t kMenuUpdateCall = 0x7ED349, kMenuUpdate = 0x7F3E00, kGetVatsEffect = 0x705910;
constexpr uintptr_t kVatsTarget = 0x11F21CC, kVatsTargets = 0x11DB150;
typedef uint32_t (__fastcall* MenuUpdateFn)(void*, void*);
static bool menuHooked;
static uint8_t menuBytes[5];
static uint8_t* heldLimb;
static unsigned menuLogs, heldFrames, blindResults;

// Limb entry: +0x20 limb, +0x24 visibility, +0x28 chance, +0x2C on-screen, +0x2D done, +0x2E requested.
// VATSEffect (result reader 0x800A00): +0x1AC ready[16], +0x1BC entire-limb samples[16]. effect is null
// unless the latest query was Salvatore's and drew the proxy. Returns 0 finished, 1 held, 2 blind discarded.
static int HoldLimb(uint8_t* limb, uint8_t* effect) {
    if(limb[0x2D]) return 0;
    int action=1;
    const uint32_t i=Read<uint32_t>(limb,0x20);
    if(limb[0x2E]&&effect&&i<16&&effect[0x1AC+i]&&!Read<uint32_t>(effect,0x1BC+4*i)) {
        effect[0x1AC+i]=0; limb[0x2E]=0; action=2;
    }
    limb[0x2C]=1;
    return action;
}
// VATSMenu target list ({data,next} at 0x11DB150; data: +0 ref, +8 limb list {entry,next}, +0x10 finalized).
static uint8_t* SalvatoreTarget() {
    auto* ref=*reinterpret_cast<uint8_t**>(kVatsTarget);
    if(!ref||!salvatore||Read<Form*>(ref,0x20)!=salvatore) return nullptr;
    unsigned guard=0;
    for(auto* n=reinterpret_cast<Node<uint8_t>*>(kVatsTargets);n&&guard++<64;n=n->next)
        if(n->data&&Read<uint8_t*>(n->data,0)==ref) return n->data;
    return nullptr;
}
static uint8_t* OnlyLimb(uint8_t* target) {
    auto* limbs=target?reinterpret_cast<Node<uint8_t>*>(target+8):nullptr;
    return limbs&&limbs->data&&!limbs->next?limbs->data:nullptr;
}
// Thiscall VATSMenu::UpdateTarget() via fastcall (ecx=menu, no stack arguments, returns al).
extern "C" __attribute__((used)) uint32_t __fastcall sg_vats_menu_update(void* menu, void* edx) {
    uint8_t* target=SalvatoreTarget();
    uint8_t* limb=target&&!target[0x10]?OnlyLimb(target):nullptr;
    if(limb) {
        auto* effect=reinterpret_cast<uint8_t*(__cdecl*)()>(kGetVatsEffect)();
        if(effect&&(Read<uint8_t*>(effect,4)!=Read<uint8_t*>(target,0)||Read<uint8_t*>(effect,8)!=countedRoot)) effect=nullptr;
        if(limb!=heldLimb) {heldLimb=limb;heldFrames=0;blindResults=0;}
        ++heldFrames;
        if(HoldLimb(limb,effect)==2&&blindResults++<3&&menuLogs++<24) Log("VATS_MENU_BLIND_RESULT_DISCARDED frame=%u",heldFrames);
    }
    const uint32_t result=reinterpret_cast<MenuUpdateFn>(kMenuUpdate)(menu,edx);
    target=limb?SalvatoreTarget():nullptr;
    if(target&&OnlyLimb(target)==limb) {
        float chance=Read<float>(limb,0x28);
        if(chance!=chance) {  // the timeout averages the finished limbs: none here
            chance=0; memcpy(limb+0x28,&chance,4);
            if(menuLogs++<24) Log("VATS_MENU_NAN_CHANCE_CLEARED frame=%u",heldFrames);
        }
        if(target[0x10]) {
            if(menuLogs++<24)
                Log("VATS_MENU_LIMB_FINALIZED frames=%u blind=%u visibility=%.3f chance=%.3f flags=%02X%02X%02X%02X",heldFrames,blindResults,
                    Read<float>(limb,0x24),chance,limb[0x2C],limb[0x2D],limb[0x2E],limb[0x2F]);
            heldLimb=nullptr;
        }
    }
    return result;
}

// v1.2 theme pause. His theme is his creature Conscious Loop sound (CSDT 11). The actor sound update (0x8899F5)
// creates that loop with the SOUN record's loop flag plus 3D only (flags 0x12). Entering menu mode
// (InterfaceManager 0x70B8F0: pause menu, Pip-Boy and the other menus) pauses only playing sounds whose flags
// contain 0x40000000 (audio request 0x1C needs every mask bit), and leaving menu mode (0x70C4A0) resumes them.
// Vanilla projectile loops are created with it (0x9BD224: 0x40000012); creature conscious loops never are, so his
// theme played on in every menu. This redirect adds that one flag when the game creates his theme's sound; the
// engine's own pause and resume do the rest. Evidence: projects/salvatore/handoff/V1.2-THEME-PAUSE-20261103.md.
constexpr uintptr_t kLoopSoundCall = 0x889EDF, kGetSoundHandle = 0xAD7480;
constexpr uint32_t kPauseInMenus = 0x40000000, kNoSound = 0xFFFFFFFF;  // kNoSound: a failed sound handle's ID
typedef void* (__fastcall* GetSoundFn)(void*, void*, void*, const char*, uint32_t, Form*);
static Form* theme;  // SalvatoreGanacciMusicSound, his CREA Conscious Loop
static volatile uint32_t themeSoundId=kNoSound;  // his latest theme sound (sound+4); also read on the audio thread
static bool themeHooked;
static uint8_t themeBytes[5];
static unsigned themeLogs;

static uint32_t ThemeFlags(const Form* sound, uint32_t flags) {
    return sound&&sound==theme?flags|kPauseInMenus:flags;
}
// Thiscall GetSoundHandle(out, path, flags, sound) via fastcall (ecx = audio callbacks; callee pops 0x10).
// Sound creation (0xAE5A50) writes the new sound's ID, which it also keeps at sound+4, to out (0xFFFFFFFF on failure).
extern "C" __attribute__((used)) void* __fastcall sg_theme_sound(void* callbacks, void* edx, void* out,
        const char* path, uint32_t flags, Form* sound) {
    const uint32_t pausable=ThemeFlags(sound,flags);
    void* handle=reinterpret_cast<GetSoundFn>(kGetSoundHandle)(callbacks,edx,out,path,pausable,sound);
    if(pausable!=flags) {
        const uint32_t id=out?Read<uint32_t>(out,0):kNoSound;
        if(id!=kNoSound) themeSoundId=id;
        if(themeLogs++<8) Log("THEME_PAUSES_IN_MENUS flags=%08X->%08X id=%08X",flags,pausable,id);
    }
    return handle;
}

// v1.2 theme position (candidate8). BSWin32GameSound::Play (vtable 0x010A3BF4 +0x30, 0xAED080) never asks whether
// the sound is already playing: for a loop sound (+8 & 0x10) it always moves its DirectSound buffer (+0x19C) to the
// game's CRT rand() % (size-1) (call at 0xAED0DA, then SetCurrentPosition; rand < 0x8000 bytes, his theme's first
// 0.372 s) before playing the buffer. After a menu his theme can get two Plays: leaving menu mode resumes it (audio
// request 0x1D), and the actor sound update (0x889F56) can still read the sound status cache as "not playing" and
// ask again (0xAD8830 -> request 1 -> Play). Pause (+0x34, 0xAED420) only stops the buffer, which keeps its play
// cursor, and flags the sound paused (+0x10 & 0x40); a successful Play flags it playing (0x20) and clears paused;
// Stop (+0x38, 0xAED4C0) rewinds it and clears both. For his paused or playing theme this redirect of that rand call
// skips Play's whole reposition, returning to its last instruction (`jmp 0xAED30F` at 0xAED104), so Play only
// (re)starts the buffer where it is. Candidate7 moved a paused theme to its own cursor instead, and a second Play then
// sent the playing theme back near the top (once in the 4 Nov owner test). A new or stopped theme, and every other
// sound, get the game's own rand. Play runs on the audio thread: nothing is logged there; MSG_LOOP logs the counts.
constexpr uintptr_t kLoopStartCall = 0xAED0DA, kGameRand = 0xECADB8, kRepositionJump = 0xAED104;
constexpr uint32_t kSoundPlaying = 0x20, kSoundPaused = 0x40;
// Play's reposition, from the rand call's return to the jmp at kRepositionJump. Skipping it needs these exact bytes.
constexpr uint8_t kReposition[42]={0x8B,0x4D,0xF0,0x83,0xE9,0x01,0x33,0xD2,0xF7,0xF1,0x52,0x8B,0x55,0xEC,0x8B,0x82,
    0x9C,0x01,0x00,0x00,0x8B,0x4D,0xEC,0x8B,0x91,0x9C,0x01,0x00,0x00,0x8B,0x00,0x52,0x8B,0x48,0x34,0xFF,0xD1,
    0xE9,0x06,0x02,0x00,0x00};
static_assert(kLoopStartCall+5+sizeof kReposition==kRepositionJump+5,"the reposition ends with its 5-byte jmp");
static_assert((kLoopStartCall+1)%64<=60,"Redirect must patch this call's displacement with one atomic store");
typedef HRESULT (__stdcall* CursorFn)(void* buffer, DWORD* play, DWORD* write);  // IDirectSoundBuffer +0x10
static bool resumeHooked;
static volatile bool keepPositions;  // set once the redirect is in; cleared if Play's reposition bytes change
static uint8_t resumeBytes[5];
static volatile LONG themeResumes, themeKeeps, themeStarts, cursorFailures;  // counted on the audio thread
static volatile uint32_t themeCursor;  // his theme's latest kept position, bytes; for the log only
static LONG resumesLogged, keepsLogged, startsLogged, failuresLogged;
static unsigned positionLogs;
// Read by sg_loop_start (globals so the host test can point them at stand-ins): where Play's rand call returns, where
// skipping the reposition returns instead, and the game's rand.
extern "C" {
__attribute__((used)) uintptr_t sg_loop_return=kLoopStartCall+5, sg_loop_skip=kRepositionJump, sg_game_rand=kGameRand;
}

enum ThemeStart {kOtherSound, kFreshStart, kResumed, kKeptPlaying};
// The theme's sound ID is an argument: no global state here.
static ThemeStart ThemeStartOf(const uint8_t* sound, uint32_t themeId) {
    if(themeId==kNoSound||!sound||Read<uint32_t>(sound,4)!=themeId) return kOtherSound;
    const uint32_t state=Read<uint32_t>(sound,0x10);
    return state&kSoundPaused?kResumed:state&kSoundPlaying?kKeptPlaying:kFreshStart;
}
// Returns 1 to skip Play's reposition, 0 for the game's rand. Only a call straight from Play's rand call can skip.
extern "C" __attribute__((used)) int sg_keep_position(const uint8_t* sound, uintptr_t returnTo) {
    if(returnTo!=sg_loop_return||!keepPositions) return 0;
    const ThemeStart start=ThemeStartOf(sound,themeSoundId);
    if(start==kFreshStart) InterlockedIncrement(&themeStarts);
    if(start!=kResumed&&start!=kKeptPlaying) return 0;
    InterlockedIncrement(start==kResumed?&themeResumes:&themeKeeps);
    auto* buffer=Read<void*>(sound,0x19C);  // IDirectSoundBuffer; Play has already checked it is there
    DWORD play=0, write=0;
    if(buffer&&SUCCEEDED(Read<CursorFn>(Read<void*>(buffer,0),0x10)(buffer,&play,&write))) themeCursor=play;
    else InterlockedIncrement(&cursorFailures);
    return 1;
}
// Replaces Play's `call rand` (edx = sound; eax, ecx and edx are free there). Skipping returns to sg_loop_skip;
// otherwise the game's rand runs with Play's own return address.
extern "C" __attribute__((naked,used)) void sg_loop_start() {
    __asm__ volatile(
        "pushl (%esp)\n\tpushl %edx\n\tcall _sg_keep_position\n\taddl $8,%esp\n\ttestl %eax,%eax\n\tjz 1f\n\t"
        "movl _sg_loop_skip,%eax\n\tmovl %eax,(%esp)\n\tret\n\t"
        "1: jmp *_sg_game_rand\n\t");
}

// Redirect one exact `call original` to replacement. Existing hooks are refused, never overwritten.
typedef uint32_t Unaligned32 __attribute__((aligned(1)));
static bool Redirect(uintptr_t site, uintptr_t original, uintptr_t replacement, uint8_t* bytes, const char* tag) {
    auto* p=reinterpret_cast<uint8_t*>(site);
    int32_t expected=static_cast<int32_t>(original-(site+5));
    if(p[0]!=0xE8||memcmp(p+1,&expected,4)) {
        Log("%s_HOOK_REFUSED site=%08X bytes=%02X%02X%02X%02X%02X",tag,(unsigned)site,p[0],p[1],p[2],p[3],p[4]);
        return false;
    }
    bytes[0]=0xE8;
    int32_t relative=static_cast<int32_t>(replacement-(site+5));
    memcpy(bytes+1,&relative,4);
    DWORD oldProtect;
    if(!VirtualProtect(p,5,PAGE_EXECUTE_READWRITE,&oldProtect)) return false;
    // p[0] is already E8, so only the displacement changes: one 32-bit store, atomic on x86 within a cache line. A
    // thread running this call meanwhile (the audio thread at 0xAED0DA) reaches the old or the new target, never a mix.
    *reinterpret_cast<volatile Unaligned32*>(p+1)=static_cast<uint32_t>(relative);
    FlushInstructionCache(GetCurrentProcess(),p,5);
    DWORD ignored; VirtualProtect(p,5,oldProtect,&ignored);
    Log("%s_HOOK_INSTALLED site=%08X original=%08X",tag,(unsigned)site,(unsigned)original);
    return true;
}

static Form* Lookup(uint32_t id) {
    auto* map=*reinterpret_cast<FormMap**>(0x11C54C0);
    unsigned guard=0;
    for(auto* b=map&&map->count?map->buckets[id%map->count]:nullptr;b&&guard++<8192;b=b->next)
        if(b->key==id&&b->form&&b->form->id==id) return b->form;
    return nullptr;
}
static double Number(void* expression) {
    Element r{};
    if(expression&&scripts->CallFunction(expression,nullptr,nullptr,&r,0)&&r.type==1&&isfinite(r.number)) return r.number;
    return -999;
}
static void* Expression(const char* format,uint32_t id) {
    char text[160]; snprintf(text,sizeof text,format,id);
    return scripts->CompileExpression(text);
}
static void Resolve() {
    Element result{};
    target=nullptr; quest=nullptr; salvatore=nullptr; theme=nullptr;
    if(targetExpression && scripts->CallFunction(targetExpression,nullptr,nullptr,&result,0) && result.type==1
       &&result.number>=0&&result.number<255&&floor(result.number)==result.number) {
        uint32_t prefix=static_cast<uint32_t>(result.number)<<24;
        target=Lookup(prefix|0x80B);
        if(target&&target->type!=kWEAP) target=nullptr;
        salvatore=Lookup(prefix|0x80E);
        if(salvatore&&salvatore->type!=kCREA) salvatore=nullptr;
        theme=Lookup(prefix|0x800);
        if(theme&&theme->type!=kSOUN) theme=nullptr;
        quest=Lookup(prefix|0x80F);
        // TESQuest.scriptable starts at18; script pointer at1C (pinned SDK).
        if(quest&&(quest->type!=71||Read<Form*>(quest,0x1C)!=Lookup(prefix|0x804))) quest=nullptr;
        if(expressionPrefix!=prefix) {
            countExpression=Expression("player.GetItemCount %08X",prefix|0x80B);
            stateExpression=Expression("GetVariable \"State\" %08X",prefix|0x80F);
            versionExpression=Expression("GetVariable \"NativeVersion\" %08X",prefix|0x80F);
            latchExpression=Expression("GetVariable \"NativeDeploy\" %08X",prefix|0x80F);
            expressionPrefix=prefix;
        }
    }
    Log("RESOLVE plugin=SalvatoreGanacci.esp target=%08X quest=%08X creature=%08X theme=%08X transaction=%d",
        target?target->id:0,quest?quest->id:0,salvatore?salvatore->id:0,theme?theme->id:0,transaction!=nullptr);
}
static Entry* OwnedEntry() {
    auto* p=*reinterpret_cast<uint8_t**>(kPlayer);
    if(!p) return nullptr;
    auto* x=Find(reinterpret_cast<ExtraList*>(p+0x44),0x15);
    auto* changes=x?Read<Changes*>(x,12):nullptr;
    unsigned guard=0;
    for(auto* n=changes?changes->entries:nullptr;n&&guard++<8192;n=n->next)
        if(n->data&&n->data->form==target) return n->data;
    return nullptr;
}
static void Refuse(const char* reason,uint32_t message=0) {
    Log("DEPLOY_REFUSED reason=%s",reason);
    if(message&&consoleAPI) {
        char line[64];snprintf(line,sizeof line,"ShowMessage %08X",(target->id&0xFF000000)|message);
        consoleAPI->RunScriptLine(line,nullptr);
    }
}
static bool Eligible(double count,double state,double version,double latch,const Health& h) {
    return count==1&&state==3&&version==1&&latch==0&&h.valid&&h.percent>=1;
}
static void SelectionFeedback() {
    if(!selectionFeedbackPending||!loaded) return;
    selectionFeedbackPending=false;
    const double count=Number(countExpression);
    Entry* e=OwnedEntry();
    ExtraList* list=e&&e->lists?e->lists->data:nullptr;
    const Health h=GetHealth(target,list);
    const char* message="Salvatore selected. Close the Pip-Boy to deploy.";
    bool accepted=true;
    if(count>1) {message="Carry only one Salvatore to deploy him.";accepted=false;}
    else if(h.valid&&h.percent<1) {message="Salvatore is too battered to deploy. Repair him first.";accepted=false;}
    else if(!hookInstalled||!quest||!transaction||!e||reinterpret_cast<uintptr_t>(list)!=clickedIdentity||
            !Eligible(count,Number(stateExpression),Number(versionExpression),Number(latchExpression),h)) {
        message="Salvatore could not be selected. Close the Pip-Boy and try again.";accepted=false;
    }
    if(!accepted) deployQueued=false;
    // Run outside the inventory click stack, but BEFORE waiting for MenuMode=0.
    // MessageBoxEx is visible over the Pip-Boy, unlike the HUD MessageEx queue.
    // UIPipBoySelect is vanilla SOUN0006FE3F, verified from FalloutNV.esm index.
    bool sound=consoleAPI->RunScriptLine("PlaySound 0006FE3F",nullptr);
    char line[256];snprintf(line,sizeof line,"MessageBoxEx \"%s\"",message);
    bool popup=consoleAPI->RunScriptLine(line,nullptr);
    Log("SELECTION_FEEDBACK queued=%d menu=%.9g soundAccepted=%d popupAccepted=%d",accepted,Number(menuExpression),sound,popup);
}
static void ProcessDeployment() {
    if(!deployQueued||!loaded) return;
    // Inventory/repair/barter remain read-only until the UI has closed.
    if(Number(menuExpression)!=0) return;
    deployQueued=false;
    if(!hookInstalled||!target||!quest||!transaction) {Refuse("unsupported_plugin_or_runtime");return;}
    if(GetTickCount()-queuedAt>120000) {Refuse("request_expired");return;}
    double count=Number(countExpression), state=Number(stateExpression), version=Number(versionExpression), latch=Number(latchExpression);
    if(count>1) {Refuse("duplicate",0x839);return;}
    Entry* e=OwnedEntry();
    ExtraList* list=e&&e->lists?e->lists->data:nullptr;
    if(!e||reinterpret_cast<uintptr_t>(list)!=clickedIdentity) {Refuse("inventory_identity_changed");return;}
    const Health h=GetHealth(target,list);
    if(!Eligible(count,state,version,latch,h)) {
        Log("VALIDATION count=%.9g state=%.9g version=%.9g latch=%.9g health=%.9g",count,state,version,latch,h.percent);
        Refuse(h.valid&&h.percent<1?"too_battered":"state_or_contract",h.valid&&h.percent<1?0x80C:0);return;
    }
    committedGun=Snapshot();
    // xNVSE InternalFunctionCaller transports float args as 32-bit float bits,
    // not C varargs doubles (FunctionScripts.cpp PopulateArgs/vSetArgs).
    float percent=static_cast<float>(h.percent);uint32_t bits;memcpy(&bits,&percent,4);
    Element result{};
    bool called=scripts->CallFunction(transaction,nullptr,nullptr,&result,3,quest,target,reinterpret_cast<void*>(bits));
    double remaining=Number(countExpression), pending=Number(latchExpression);
    Log("COMMIT call=%d result=%.9g condition=%.9g remaining=%.9g latch=%.9g sameGun=%d",called,result.type==1?result.number:-1,h.percent,remaining,pending,Same(committedGun,Snapshot()));
    if(remaining==0&&pending==1) {deploymentAudit=true;committedAt=GetTickCount();}
    // No retry after a possibly mutating call. Saved quest latch owns completion.
}
static void OnMessage(Message* msg) {
    if(msg->type==MSG_DEFERRED) {
        targetExpression=scripts->CompileExpression("GetModIndex \"SalvatoreGanacci.esp\"");
        menuExpression=scripts->CompileExpression("MenuMode");
        transaction=scripts->CompileScript(R"(ref q
ref item
float hp
Begin Function {q, item, hp}
    SetFunctionValue 0
    if (GetVariable "NativeVersion" q) == 1 && (GetVariable "State" q) == 3 && (GetVariable "NativeDeploy" q) == 0 && player.GetItemCount item == 1 && hp >= 1
        SetVariable "Condition" hp q
        SetVariable "RestoreWeaponPending" 0 q
        SetVariable "DropPending" 0 q
        SetVariable "NativeDeploy" 1 q
        player.RemoveItem item 1 1
        if player.GetItemCount item == 0
            SetFunctionValue 1
        else
            SetVariable "NativeDeploy" 0 q
        endif
    endif
End)");
        hookInstalled=InstallHook(); Resolve();
        // The extra cull first: without it the query-pass redirects would have nothing to do.
        cullHooked=Redirect(kRootCull,kCullScene,reinterpret_cast<uintptr_t>(&sg_vats_root_cull),cullBytes,"VATS");
        for(int i=0;i<2&&cullHooked;++i) queryHooked[i]=Redirect(kQueryCalls[i],kQueryPass,reinterpret_cast<uintptr_t>(&sg_vats_query_pass),queryBytes[i],"VATS");
        // Holding his limb only helps when the proxy can be counted.
        if(queryHooked[0]&&queryHooked[1]) menuHooked=Redirect(kMenuUpdateCall,kMenuUpdate,reinterpret_cast<uintptr_t>(&sg_vats_menu_update),menuBytes,"VATS");
        // Independent of VATS.
        themeHooked=Redirect(kLoopSoundCall,kGetSoundHandle,reinterpret_cast<uintptr_t>(&sg_theme_sound),themeBytes,"THEME");
        // Carrying on where it paused only matters for a theme that pauses.
        if(themeHooked) {
            if(memcmp(reinterpret_cast<void*>(kLoopStartCall+5),kReposition,sizeof kReposition))
                Log("THEME_HOOK_REFUSED site=%08X: Play's reposition is not the game's own",(unsigned)(kLoopStartCall+5));
            else keepPositions=resumeHooked=Redirect(kLoopStartCall,kGameRand,reinterpret_cast<uintptr_t>(&sg_loop_start),resumeBytes,"THEME");
        }
    }
    if(msg->type==MSG_PRELOAD||msg->type==MSG_MENU||msg->type==MSG_NEWGAME) {
        loaded=false; auditPending=false; deployQueued=false; deploymentAudit=false; selectionFeedbackPending=false; target=nullptr; quest=nullptr; salvatore=nullptr; theme=nullptr; themeSoundId=kNoSound;
    }
    if(msg->type==MSG_POSTLOAD||msg->type==MSG_NEWGAME) {loaded=true;Resolve();proxyDraws=0;proxyMisses=0;menuLogs=0;heldLimb=nullptr;themeLogs=0;positionLogs=0;}
    if(msg->type==MSG_LOOP) {
        if(hookInstalled&&memcmp(reinterpret_cast<void*>(kClickCall),installedBytes,5)) {
            Log("HOOK_CHANGED_AFTER_INSTALL: incompatible runtime hook; probe no longer guaranteed");
            hookInstalled=false; target=nullptr;
        }
        for(int i=0;i<2;++i) if(queryHooked[i]&&memcmp(reinterpret_cast<void*>(kQueryCalls[i]),queryBytes[i],5)) {
            Log("VATS_HOOK_CHANGED_AFTER_INSTALL site=%08X: left to its new owner",(unsigned)kQueryCalls[i]);
            queryHooked[i]=false;
        }
        if(cullHooked&&memcmp(reinterpret_cast<void*>(kRootCull),cullBytes,5)) {
            Log("VATS_HOOK_CHANGED_AFTER_INSTALL site=%08X: left to its new owner",(unsigned)kRootCull);
            cullHooked=false;
        }
        if(menuHooked&&memcmp(reinterpret_cast<void*>(kMenuUpdateCall),menuBytes,5)) {
            Log("VATS_HOOK_CHANGED_AFTER_INSTALL site=%08X: left to its new owner",(unsigned)kMenuUpdateCall);
            menuHooked=false;
        }
        if(themeHooked&&memcmp(reinterpret_cast<void*>(kLoopSoundCall),themeBytes,5)) {
            Log("THEME_HOOK_CHANGED_AFTER_INSTALL site=%08X: left to its new owner",(unsigned)kLoopSoundCall);
            themeHooked=false;
        }
        if(resumeHooked&&(memcmp(reinterpret_cast<void*>(kLoopStartCall),resumeBytes,5)||
                          memcmp(reinterpret_cast<void*>(kLoopStartCall+5),kReposition,sizeof kReposition))) {
            Log("THEME_HOOK_CHANGED_AFTER_INSTALL site=%08X: left to its new owner",(unsigned)kLoopStartCall);
            resumeHooked=false; keepPositions=false;
        }
        const LONG resumes=themeResumes, keeps=themeKeeps, starts=themeStarts, failures=cursorFailures;
        if((resumes!=resumesLogged||keeps!=keepsLogged||starts!=startsLogged||failures!=failuresLogged)&&positionLogs++<32)
            Log("THEME_POSITION resumed=%ld keptPlaying=%ld freshStarts=%ld cursor=%lu cursorReadFailures=%ld",
                resumes,keeps,starts,(unsigned long)themeCursor,failures);
        resumesLogged=resumes; keepsLogged=keeps; startsLogged=starts; failuresLogged=failures;
        if(auditPending) {
            auto after=Snapshot(); auditPending=false;
            Log("NEXT_LOOP click=%u sameGun=%d gun=%08X gunStack=%08X gunHealth=%.9g gunMods=%u",
                clicks,Same(before,after),after.id,(unsigned)after.stack,after.current,after.mods);
        }
        SelectionFeedback();
        ProcessDeployment();
        if(deploymentAudit&&loaded&&Number(menuExpression)==0) {
            double state=Number(stateExpression), latch=Number(latchExpression);
            if(state==4&&latch==0) {
                Log("DEPLOY_COMPLETE state=4 remaining=%.9g sameGun=%d",Number(countExpression),Same(committedGun,Snapshot()));deploymentAudit=false;
                // xNVSE MessageEx queues a two-second HUD message. Emit once only
                // after the existing quest confirms deployment, never on refusal.
                bool notified=consoleAPI->RunScriptLine("MessageEx \"Salvatore deployed.\"",nullptr);
                Log("DEPLOY_NOTIFICATION accepted=%d",notified);
            } else if(GetTickCount()-committedAt>10000) {
                Log("DEPLOY_INCOMPLETE state=%.9g latch=%.9g remaining=%.9g NO_RETRY",state,latch,Number(countExpression));deploymentAudit=false;
            }
        }
    }
}
extern "C" __declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse,PluginInfo* info) {
    info->infoVersion=1; info->name="SalvatoreGanacci"; info->version=8;
    char path[MAX_PATH]; snprintf(path,sizeof path,"%sSalvatoreGanacci-native.log",nvse->GetRuntimeDirectory());
    logFile=fopen(path,"w");
    Log("QUERY build=v12-theme-keep-position-candidate8-20261104 nvse=%u runtime=%08X editor=%u nogore=%u",nvse->nvseVersion,nvse->runtimeVersion,nvse->isEditor,nvse->isNogore);
    bool ok=!nvse->isEditor&&!nvse->isNogore&&nvse->runtimeVersion==kRuntime&&nvse->nvseVersion>=0x06040090&&nvse->nvseVersion<0x07000000;
    if(!ok) Log("UNSUPPORTED runtime/editor/xNVSE");
    return ok;
}
extern "C" __declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface* nvse) {
    scripts=static_cast<ScriptAPI*>(nvse->QueryInterface(6));
    consoleAPI=static_cast<ConsoleAPI*>(nvse->QueryInterface(1));
    auto* messaging=static_cast<Messaging*>(nvse->QueryInterface(2));
    if(!scripts||!scripts->CompileScript||!scripts->CompileExpression||!consoleAPI||consoleAPI->version<2||!messaging||messaging->version<4) return false;
    return messaging->RegisterListener(nvse->GetPluginHandle(),"NVSE",OnMessage);
}

#ifdef SG_UNIT_TEST
#undef NDEBUG  // zig c++ -O2 defines NDEBUG, which silently removed every check below until v1.2 candidate6
#include <assert.h>
static HRESULT fakeResult; static DWORD fakeCursor; static unsigned fakeReads;
// A DirectSound buffer's GetCurrentPosition; it fills both cursors even when it fails, as a careless driver might.
static HRESULT __stdcall FakeGetCurrentPosition(void*, DWORD* play, DWORD* write) {
    ++fakeReads; *play=fakeCursor; *write=fakeCursor+4410; return fakeResult;
}
// Writes `call to` at `at`, as Redirect leaves Play's rand call, then `code` after it.
static void PutCall(uint8_t* at, uintptr_t to, const uint8_t* code, size_t size) {
    const int32_t relative=static_cast<int32_t>(to-(reinterpret_cast<uintptr_t>(at)+5));
    at[0]=0xE8; memcpy(at+1,&relative,4); memcpy(at+5,code,size);
}
int main() {
    alignas(8) uint8_t weapon[0x388]{};
    auto* form=reinterpret_cast<Form*>(weapon); form->type=kWEAP;
    uint32_t maximum=100; memcpy(weapon+0x98,&maximum,4);
    auto h=GetHealth(form,nullptr); assert(h.valid&&!h.explicitHealth&&h.percent==100);
    struct HealthExtra { Extra header; float health; } hx{};
    hx.header.type=0x25;
    ExtraList list{}; list.head=&hx.header;
    hx.health=0; h=GetHealth(form,&list); assert(h.valid&&h.explicitHealth&&h.percent==0);
    hx.health=0.5f; h=GetHealth(form,&list); assert(h.percent==0.5);
    hx.health=66; h=GetHealth(form,&list); assert(h.percent==66);
    hx.health=90; h=GetHealth(form,&list); assert(h.percent==90); // merchant repair changes the same data
    assert(Eligible(1,3,1,0,h));assert(!Eligible(2,3,1,0,h));assert(!Eligible(0,3,1,0,h));
    assert(!Eligible(1,4,1,0,h));assert(!Eligible(1,3,0,0,h));assert(!Eligible(1,3,1,1,h));
    hx.health=0.5f;assert(!Eligible(1,3,1,0,GetHealth(form,&list)));hx.health=90;
    struct ModExtra { Extra header; uint8_t flags; } mx{};
    mx.header.type=0x8D; mx.flags=1; hx.header.next=&mx.header;
    uint32_t effect=10; float bonus=50; memcpy(weapon+0x180,&effect,4); memcpy(weapon+0x18C,&bonus,4);
    hx.health=75; h=GetHealth(form,&list); assert(h.maximum==150&&h.percent==50);
    list.head=&mx.header; h=GetHealth(form,&list); assert(h.current==150&&h.percent==100);
    list.head=&hx.header; hx.health=NAN; assert(!GetHealth(form,&list).valid);
    hx.health=-1; assert(!GetHealth(form,&list).valid);
    Gun a{1,2,33,4}, b=a; assert(Same(a,b)); b.stack=3; assert(!Same(a,b));
    b=a;b.mods=0;assert(!Same(a,b));b=a;b.current=34;assert(!Same(a,b));
    alignas(4) uint8_t limb[0x34]{}, fx[0x240]{};
    uint32_t samples=0;
    assert(HoldLimb(limb,fx)==1&&limb[0x2C]==1&&!limb[0x2E]);                  // not yet requested: menu requests it
    limb[0x2C]=0; limb[0x2E]=1;
    assert(HoldLimb(limb,fx)==1&&limb[0x2C]==1&&limb[0x2E]==1);                // in flight: menu waits
    fx[0x1AC]=1; samples=500; memcpy(fx+0x1BC,&samples,4);
    assert(HoldLimb(limb,fx)==1&&limb[0x2E]==1&&fx[0x1AC]==1);                 // real result: left for the engine
    samples=0; memcpy(fx+0x1BC,&samples,4);
    assert(HoldLimb(limb,nullptr)==1&&limb[0x2E]==1&&fx[0x1AC]==1);            // not the proxy's query: kept
    assert(HoldLimb(limb,fx)==2&&!limb[0x2E]&&!fx[0x1AC]&&limb[0x2C]==1);      // blind: discarded, asked again
    uint32_t other=3; memcpy(limb+0x20,&other,4); limb[0x2E]=1; fx[0x1AC]=1; fx[0x1AF]=1;
    assert(HoldLimb(limb,fx)==2&&fx[0x1AC]==1&&!fx[0x1AF]);                    // uses its own limb slot
    other=16; memcpy(limb+0x20,&other,4); limb[0x2E]=1;
    assert(HoldLimb(limb,fx)==1&&limb[0x2E]==1);                               // out-of-range limb: no read
    limb[0x2D]=1; limb[0x2C]=0;
    assert(HoldLimb(limb,fx)==0&&!limb[0x2C]&&limb[0x2E]==1);                  // finished: untouched
    alignas(8) uint8_t themeSound[0x68]{}, otherSound[0x68]{};
    auto* mine=reinterpret_cast<Form*>(themeSound); auto* stranger=reinterpret_cast<Form*>(otherSound);
    assert(ThemeFlags(mine,0x12)==0x12);                                       // not resolved: unchanged
    theme=mine;
    assert(ThemeFlags(mine,0x12)==0x40000012);                                 // his theme: pauses in menus
    assert(ThemeFlags(stranger,0x12)==0x12&&ThemeFlags(nullptr,0x12)==0x12);   // any other sound: unchanged
    assert(ThemeFlags(mine,0x40000012)==0x40000012);
    alignas(8) uint8_t loop[0x1A4]{};
    void* vtable[5]{}; vtable[4]=reinterpret_cast<void*>(&FakeGetCurrentPosition);  // IDirectSoundBuffer +0x10
    void* object[1]={vtable}; void* buffer=object;
    uint32_t id=0x1234, state=kSoundPaused;
    auto store=[&]{memcpy(loop+4,&id,4); memcpy(loop+0x10,&state,4); memcpy(loop+0x19C,&buffer,4);};
    store();
    assert(ThemeStartOf(loop,kNoSound)==kOtherSound);                          // no theme sound yet
    assert(ThemeStartOf(loop,0x1234)==kResumed);                               // his theme, paused
    assert(ThemeStartOf(loop,0x1235)==kOtherSound&&ThemeStartOf(nullptr,0x1234)==kOtherSound);
    state=kSoundPlaying; store(); assert(ThemeStartOf(loop,0x1234)==kKeptPlaying);  // his theme, already playing
    state=0; store(); assert(ThemeStartOf(loop,0x1234)==kFreshStart);              // his theme, new or stopped
    const uintptr_t fromPlay=kLoopStartCall+5;
    fakeResult=S_OK; fakeCursor=600000; state=kSoundPaused; store();
    assert(themeSoundId==kNoSound&&!keepPositions);
    themeSoundId=0x1234;
    assert(!sg_keep_position(loop,fromPlay)&&!themeResumes&&!fakeReads);       // redirect not in: the game's rand
    keepPositions=true;
    assert(sg_keep_position(loop,fromPlay)&&themeResumes==1&&themeCursor==600000&&fakeReads==1);  // paused: kept
    assert(!sg_keep_position(loop,fromPlay+1)&&themeResumes==1&&fakeReads==1); // not straight from Play: rand
    state=kSoundPlaying; store(); fakeCursor=0;
    assert(sg_keep_position(loop,fromPlay)&&themeKeeps==1&&themeCursor==0&&fakeReads==2);  // playing: kept (the fix)
    state=0; store();
    assert(!sg_keep_position(loop,fromPlay)&&themeStarts==1&&fakeReads==2);   // new or stopped: rand
    id=0x1235; state=kSoundPlaying; store();
    assert(!sg_keep_position(loop,fromPlay)&&themeKeeps==1&&themeStarts==1&&fakeReads==2);  // other sound: rand
    id=0x1234; store(); fakeResult=E_FAIL; fakeCursor=600000;
    assert(sg_keep_position(loop,fromPlay)&&themeKeeps==2&&cursorFailures==1&&themeCursor==0&&fakeReads==3);  // log only
    buffer=nullptr; store();
    assert(sg_keep_position(loop,fromPlay)&&themeKeeps==3&&cursorFailures==2&&fakeReads==3);  // no buffer: no call
    keepPositions=false;
    assert(!sg_keep_position(loop,fromPlay)&&themeKeeps==3);                  // reposition bytes changed: rand
    keepPositions=true; id=kNoSound; store(); themeSoundId=kNoSound;
    assert(!sg_keep_position(loop,fromPlay)&&themeKeeps==3&&themeStarts==1);  // a failed handle's ID never matches
    // The real thunk between stand-ins for Play's rand call and its way on, the reposition's jmp, another caller and
    // the game's rand.
    assert(sg_loop_return==0xAED0DF&&sg_loop_skip==0xAED104&&sg_game_rand==0xECADB8);
    auto* code=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    assert(code);
    const uint8_t playOn[]={0x05,0x00,0x00,0x00,0x10,0xC3};      // add eax,0x10000000; ret: Play's reposition
    const uint8_t skipped[]={0xB8,0x22,0x22,0x22,0x22,0xC3};     // mov eax,0x22222222; ret: the jmp after it
    const uint8_t otherOn[]={0x05,0x00,0x00,0x00,0x30,0xC3};     // add eax,0x30000000; ret
    const uint8_t randStub[]={0xB8,0x34,0x12,0x00,0x00,0xC3};    // mov eax,0x1234; ret
    PutCall(code,reinterpret_cast<uintptr_t>(&sg_loop_start),playOn,6);
    memcpy(code+0x20,skipped,6);
    PutCall(code+0x40,reinterpret_cast<uintptr_t>(&sg_loop_start),otherOn,6);
    memcpy(code+0x60,randStub,6);
    sg_loop_return=reinterpret_cast<uintptr_t>(code)+5; sg_loop_skip=reinterpret_cast<uintptr_t>(code)+0x20;
    sg_game_rand=reinterpret_cast<uintptr_t>(code)+0x60;
    typedef uint32_t (__fastcall* SiteFn)(void*, const uint8_t* sound);  // ecx unused, edx = sound
    auto atPlay=reinterpret_cast<SiteFn>(code), atOther=reinterpret_cast<SiteFn>(code+0x40);
    id=0x1234; state=kSoundPlaying; buffer=object; store(); themeSoundId=0x1234; fakeResult=S_OK; fakeCursor=4410;
    assert(atPlay(nullptr,loop)==0x22222222&&themeKeeps==4&&themeCursor==4410);  // his playing theme: reposition skipped
    assert(atOther(nullptr,loop)==0x30001234&&themeKeeps==4);                    // another caller: rand, back to it
    state=kSoundPaused; store();
    assert(atPlay(nullptr,loop)==0x22222222&&themeResumes==2);                   // his paused theme: skipped
    state=0; store();
    assert(atPlay(nullptr,loop)==0x10001234&&themeStarts==2);                    // new or stopped: rand, back to Play
    VirtualFree(code,0,MEM_RELEASE);
    puts("PASS: condition absent/zero/partial/repaired/effective-max/invalid; exact-stack comparison; "
         "VATS limb hold/blind-result discard; theme pause flag; theme position kept when paused or playing, "
         "with the real thunk skipping or running rand. NOT a gameplay test.");
}
#endif
