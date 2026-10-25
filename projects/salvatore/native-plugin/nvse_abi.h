/* Minimal C declarations of the documented xNVSE plugin ABI.
 * Reference: xNVSE/NVSE nvse/nvse/PluginAPI.h, commit
 * 0ccd23ad885ddae533c1790a3fc56cd073e38de3.
 * No engine addresses, game structure layouts or patched game code.
 */
#pragma once
#include <stdint.h>

#include <stddef.h>
typedef struct {
    uint32_t nvseVersion, runtimeVersion, editorVersion, isEditor;
    bool (*RegisterCommand)(void *);
    void (*SetOpcodeBase)(uint32_t);
    void *(*QueryInterface)(uint32_t);
    uint32_t (*GetPluginHandle)(void);
    bool (*RegisterTypedCommand)(void *, uint32_t);
    const char *(*GetRuntimeDirectory)(void);
    uint32_t isNogore;
} NVSEInterface;
typedef struct { uint32_t infoVersion; const char *name; uint32_t version; } PluginInfo;
typedef struct { const char *sender; uint32_t type, dataLen; void *data; } Message;
typedef struct {
    uint32_t version;
    bool (*RegisterListener)(uint32_t, const char *, void (*)(Message *));
    bool (*Dispatch)(uint32_t, uint32_t, void *, uint32_t, const char *);
} Messaging;

/* Public Console interface: bool return requires version >=2 (xNVSE PluginAPI.h).
 * Engine acceptance is not independent verification of a gameplay effect.
 */
typedef struct {
    uint32_t version;
    bool (*RunScriptLine)(const char *, void *);
    bool (*RunScriptLine2)(const char *, void *, bool);
} ConsoleAPI;
/* Numeric Element is an eight-byte union, a one-byte type tag, and padding.
 * Only numeric results are accepted; strings/arrays/forms are never requested.
 */
typedef struct { double number; uint8_t type; uint8_t padding[7]; } Element;
typedef struct {
    bool (*CallFunction)(void *, void *, void *, Element *, uint8_t, ...);
    uint32_t (*GetFunctionParams)(void *, uint8_t *);
    bool (*ExtractArgsEx)(void *, void *, uint32_t *, void *, void *, ...);
    bool (*ExtractFormatStringArgs)(uint32_t, char *, void *, void *, uint32_t *, void *, void *, uint32_t, ...);
    bool (*CallFunctionAlt)(void *, void *, uint8_t, ...);
    void *(*CompileScript)(const char *);
    void *(*CompileExpression)(const char *);
} ScriptAPI;
enum { MSG_EXIT=1, MSG_MENU=2, MSG_PRELOAD=6, MSG_EXIT_CONSOLE=7,
       MSG_POSTLOAD=8, MSG_NEWGAME=14, MSG_DEFERRED=18, MSG_LOOP=20 };
static_assert(sizeof(void *) == 4, "FNV companion must be 32-bit");
static_assert(offsetof(NVSEInterface, QueryInterface)==24, "NVSE ABI");
static_assert(offsetof(ScriptAPI, CompileExpression)==24, "script ABI");
static_assert(offsetof(Element, type)==8, "numeric Element ABI");
static_assert(offsetof(ConsoleAPI, RunScriptLine)==4, "console ABI");
