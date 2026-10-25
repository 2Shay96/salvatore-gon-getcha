// Salvatore-specific xNVSE plugin. Copyright (c) 2026 2Shay. MIT.
// Engine layouts: pinned xNVSE SDK; interception location: ShowOff reverse engineering.
// Pre-equip cancellation, then a revalidated game-loop transaction into the existing quest.
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
constexpr uint8_t kWEAP = 40;

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
    target=nullptr; quest=nullptr;
    if(targetExpression && scripts->CallFunction(targetExpression,nullptr,nullptr,&result,0) && result.type==1
       &&result.number>=0&&result.number<255&&floor(result.number)==result.number) {
        uint32_t prefix=static_cast<uint32_t>(result.number)<<24;
        target=Lookup(prefix|0x80B);
        if(target&&target->type!=kWEAP) target=nullptr;
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
    Log("RESOLVE plugin=SalvatoreGanacci.esp target=%08X quest=%08X transaction=%d",target?target->id:0,quest?quest->id:0,transaction!=nullptr);
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
    }
    if(msg->type==MSG_PRELOAD||msg->type==MSG_MENU||msg->type==MSG_NEWGAME) {
        loaded=false; auditPending=false; deployQueued=false; deploymentAudit=false; selectionFeedbackPending=false; target=nullptr; quest=nullptr;
    }
    if(msg->type==MSG_POSTLOAD||msg->type==MSG_NEWGAME) {loaded=true;Resolve();}
    if(msg->type==MSG_LOOP) {
        if(hookInstalled&&memcmp(reinterpret_cast<void*>(kClickCall),installedBytes,5)) {
            Log("HOOK_CHANGED_AFTER_INSTALL: incompatible runtime hook; probe no longer guaranteed");
            hookInstalled=false; target=nullptr;
        }
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
    info->infoVersion=1; info->name="SalvatoreGanacci"; info->version=2;
    char path[MAX_PATH]; snprintf(path,sizeof path,"%sSalvatoreGanacci-native.log",nvse->GetRuntimeDirectory());
    logFile=fopen(path,"w");
    Log("QUERY build=candidate1-pipboy-feedback-20261024 nvse=%u runtime=%08X editor=%u nogore=%u",nvse->nvseVersion,nvse->runtimeVersion,nvse->isEditor,nvse->isNogore);
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
#include <assert.h>
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
    puts("PASS: condition absent/zero/partial/repaired/effective-max/invalid; exact-stack comparison. NOT a gameplay test.");
}
#endif
