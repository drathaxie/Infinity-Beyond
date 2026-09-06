/*
 * Beyond Android loader shim.
 *
 * Ships in the patched APK as lib/<abi>/libmain.so, with Unity's original moved
 * aside to libmain_orig.so. Unity's Java side calls System.loadLibrary("main"),
 * so this is the earliest in-process entry point that needs no manifest edit,
 * no dex edit and no hard-coded offsets - which is the point: a new game
 * release can be patched with the same shim, unchanged.
 *
 * Unity's libmain.so exports exactly one symbol, JNI_OnLoad, so forwarding that
 * is the entire compatibility surface.
 *
 * Startup order, which matters:
 *   JNI_OnLoad -> background thread -> wait for libil2cpp.so to LOAD
 *              -> hook il2cpp_init -> wait for it to RETURN
 *              -> resolve and hook AEC.GetResponse, log packets.
 */

#include <android/log.h>
#include <ctype.h>
#include <dlfcn.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "Beyond"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* il2cpp's C API. Every one of these is resolved by name from libil2cpp.so, so
   nothing here depends on the game build, the IL2CPP metadata version, or an
   address. That is what makes the shim survive game updates. */
typedef void *(*il2cpp_domain_get_t)(void);
typedef void *(*il2cpp_thread_attach_t)(void *domain);
typedef void *(*il2cpp_domain_assembly_open_t)(void *domain, const char *name);
typedef void *(*il2cpp_assembly_get_image_t)(void *assembly);
typedef void *(*il2cpp_class_from_name_t)(void *image, const char *ns, const char *name);
typedef void *(*il2cpp_class_get_method_from_name_t)(void *klass, const char *name, int argc);
typedef void *(*il2cpp_object_get_class_t)(void *obj);
typedef const char *(*il2cpp_class_get_name_t)(void *klass);
typedef void *(*il2cpp_runtime_invoke_t)(void *method, void *obj, void **params, void **exc);
typedef uint16_t *(*il2cpp_string_chars_t)(void *str);
typedef int32_t (*il2cpp_string_length_t)(void *str);
typedef void **(*il2cpp_domain_get_assemblies_t)(void *domain, size_t *size);
typedef size_t (*il2cpp_image_get_class_count_t)(void *image);
typedef void *(*il2cpp_image_get_class_t)(void *image, size_t index);
typedef void *(*il2cpp_class_get_methods_t)(void *klass, void **iter);
typedef const char *(*il2cpp_method_get_name_t)(void *method);
typedef const char *(*il2cpp_class_get_namespace_t)(void *klass);
typedef uint32_t (*il2cpp_method_get_param_count_t)(void *method);

static il2cpp_object_get_class_t il2cpp_object_get_class;
static il2cpp_class_get_name_t il2cpp_class_get_name;
static il2cpp_class_get_method_from_name_t il2cpp_class_get_method_from_name;
static il2cpp_runtime_invoke_t il2cpp_runtime_invoke;
static il2cpp_string_chars_t il2cpp_string_chars;
static il2cpp_string_length_t il2cpp_string_length;
static il2cpp_class_get_methods_t il2cpp_class_get_methods;
static il2cpp_method_get_name_t il2cpp_method_get_name;
static il2cpp_class_get_namespace_t il2cpp_class_get_namespace;
static il2cpp_method_get_param_count_t il2cpp_method_get_param_count;

/* Object creation and overload resolution, for the on-device menu. */
typedef void *(*il2cpp_object_new_t)(void *klass);
typedef void (*il2cpp_runtime_object_init_t)(void *obj);
typedef void *(*il2cpp_class_get_type_t)(void *klass);
typedef void *(*il2cpp_type_get_object_t)(void *type);
typedef void *(*il2cpp_string_new_t)(const char *str);
typedef void *(*il2cpp_method_get_param_t)(void *method, uint32_t index);
typedef char *(*il2cpp_type_get_name_t)(void *type);
typedef void (*il2cpp_free_t)(void *ptr);

static il2cpp_object_new_t il2cpp_object_new;
static il2cpp_runtime_object_init_t il2cpp_runtime_object_init;
static il2cpp_class_get_type_t il2cpp_class_get_type;
static il2cpp_type_get_object_t il2cpp_type_get_object;
static il2cpp_string_new_t il2cpp_string_new;
static il2cpp_method_get_param_t il2cpp_method_get_param;
static il2cpp_type_get_name_t il2cpp_type_get_name;
static il2cpp_free_t il2cpp_free;

typedef void *(*il2cpp_object_unbox_t)(void *obj);
typedef bool (*il2cpp_class_is_valuetype_t)(void *klass);
typedef void *(*il2cpp_gchandle_new_t)(void *obj, bool pinned);
typedef void *(*il2cpp_gchandle_get_target_t)(void *handle);
typedef void (*il2cpp_gchandle_free_t)(void *handle);
typedef void *(*il2cpp_class_get_field_from_name_t)(void *klass, const char *name);
typedef void (*il2cpp_field_get_value_t)(void *obj, void *field, void *out);
typedef void (*il2cpp_field_static_get_value_t)(void *field, void *out);

static il2cpp_object_unbox_t il2cpp_object_unbox;
static il2cpp_class_is_valuetype_t il2cpp_class_is_valuetype;
static il2cpp_gchandle_new_t il2cpp_gchandle_new;
static il2cpp_gchandle_get_target_t il2cpp_gchandle_get_target;
static il2cpp_gchandle_free_t il2cpp_gchandle_free;
static il2cpp_class_get_field_from_name_t il2cpp_class_get_field_from_name;
static il2cpp_field_get_value_t il2cpp_field_get_value;
static il2cpp_field_static_get_value_t il2cpp_field_static_get_value;

/* The component whose OnGUI we borrow, found by probe_imgui. */
static void *g_host_class;
static void *g_host_ongui;
static void *g_cs_image;   /* Assembly-CSharp image */
static void *g_aec_class;

/* -------------------------------------------------------------------------
 * Managed call helpers
 * ---------------------------------------------------------------------- */

/* Swallows managed exceptions: every call site here is optional UI or a
   best-effort game call, and a throw must never take the game down. */
static void *inv(void *method, void *self, void **args)
{
    if (method == NULL) {
        return NULL;
    }
    void *exc = NULL;
    void *r = il2cpp_runtime_invoke(method, self, args, &exc);
    return exc != NULL ? NULL : r;
}

static bool inv_bool(void *method, void *self, void **args)
{
    void *boxed = inv(method, self, args);
    if (boxed == NULL || !il2cpp_object_unbox) {
        return false;
    }
    void *raw = il2cpp_object_unbox(boxed);
    return raw != NULL && *(uint8_t *)raw != 0;
}

static float inv_float(void *method, void *self, void **args)
{
    void *boxed = inv(method, self, args);
    if (boxed == NULL || !il2cpp_object_unbox) {
        return 0.0f;
    }
    void *raw = il2cpp_object_unbox(boxed);
    return raw != NULL ? *(float *)raw : 0.0f;
}

static int inv_int(void *method, void *self, void **args)
{
    void *boxed = inv(method, self, args);
    if (boxed == NULL || !il2cpp_object_unbox) {
        return -1;
    }
    void *raw = il2cpp_object_unbox(boxed);
    return raw != NULL ? *(int32_t *)raw : -1;
}

/* Managed strings move and are collected, so anything held across frames needs
   a GC handle rather than a raw pointer. */
static void *mstr_hold(void *old_handle, void *obj)
{
    if (!il2cpp_gchandle_new) {
        return NULL;
    }
    if (old_handle != NULL && il2cpp_gchandle_free) {
        il2cpp_gchandle_free(old_handle);
    }
    return obj != NULL ? il2cpp_gchandle_new(obj, false) : NULL;
}

static void *mstr_get(void *handle)
{
    if (handle == NULL || !il2cpp_gchandle_get_target) {
        return NULL;
    }
    return il2cpp_gchandle_get_target(handle);
}

/* Managed strings are UTF-16. Every packet command and type name in this game
   is ASCII, so flatten rather than drag in a converter. */
static void to_ascii(const uint16_t *src, int32_t len, char *dst, size_t cap)
{
    size_t n = (size_t)(len < 0 ? 0 : len);
    if (n > cap - 1) {
        n = cap - 1;
    }
    for (size_t i = 0; i < n; i++) {
        dst[i] = (src[i] > 0 && src[i] < 0x80) ? (char)src[i] : '?';
    }
    dst[n] = '\0';
}

static void mstr_to_utf8(void *managed_string, char *out, size_t cap)
{
    out[0] = '\0';
    if (managed_string == NULL || !il2cpp_string_chars || !il2cpp_string_length) {
        return;
    }
    to_ascii(il2cpp_string_chars(managed_string), il2cpp_string_length(managed_string), out, cap);
}

/* -------------------------------------------------------------------------
 * Minimal ARM64 inline hook.
 *
 * The obvious dependency - ShadowHook 2.0.1 - cannot initialise on Android 17
 * (SHADOWHOOK_ERRNO_INIT_LINKER): it parses dynamic-linker internals that
 * moved, and 2.0.1 is the newest release. Dobby ships no Android prebuilt. So
 * this does the single thing we need, and refuses loudly rather than
 * relocating an instruction it does not understand.
 *
 * The patch is one 4-byte B, so exactly one instruction is displaced and the
 * overwrite is a single aligned store - no torn-prologue window. A 16-byte
 * absolute jump would displace four, and IL2CPP prologues routinely carry an
 * ADRP within the first four (AEC.GetResponse does, at word 3).
 *
 * libil2cpp.so carries no BTI marking (checked with readelf -n), so the
 * trampoline can be branched into with no landing pad.
 * ---------------------------------------------------------------------- */
#if defined(__aarch64__)

#define INSN_LDR_X17 0x58000051u /* LDR X17, #8 */
#define INSN_BR_X17  0xD61F0220u /* BR  X17     */
#define B_RANGE      (1 << 27)   /* +/-128MB, the reach of a B */

/* Copying a PC-relative instruction to a trampoline silently changes what it
   addresses, so refuse instead of guessing. */
static bool is_pc_relative(uint32_t insn)
{
    if ((insn & 0x1F000000u) == 0x10000000u) return true; /* ADR / ADRP             */
    if ((insn & 0x7C000000u) == 0x14000000u) return true; /* B / BL                 */
    if ((insn & 0xFE000000u) == 0x54000000u) return true; /* B.cond                 */
    if ((insn & 0x7E000000u) == 0x34000000u) return true; /* CBZ / CBNZ             */
    if ((insn & 0x7E000000u) == 0x36000000u) return true; /* TBZ / TBNZ             */
    if ((insn & 0x3B000000u) == 0x18000000u) return true; /* LDR/LDRSW/PRFM literal */
    return false;
}

/* X17 is the linker's own veneer scratch register, so clobbering it at a
   function entry is what the ABI already expects. */
static void write_abs_jump(uint32_t *out, const void *dest)
{
    uint64_t addr = (uint64_t)dest;
    out[0] = INSN_LDR_X17;
    out[1] = INSN_BR_X17;
    memcpy(&out[2], &addr, sizeof(addr));
}

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* Android maps libmain.so gigabytes away from libil2cpp.so (measured: 2.8GB),
   so a B cannot reach our hook directly. Place an island within reach of the
   target instead and branch to that; the island does the absolute jump.
   MAP_FIXED_NOREPLACE fails rather than relocating when the address is taken,
   and we re-check the result so an older kernel that treats it as a plain hint
   cannot silently hand us something out of range. */
static void *alloc_near(void *target, size_t size)
{
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t base = (uintptr_t)target & ~(uintptr_t)(page - 1);
    const uintptr_t step = 64 * 1024;

    for (uintptr_t off = step; off < (uintptr_t)B_RANGE; off += step) {
        for (int back = 0; back < 2; back++) {
            uintptr_t cand = back ? base - off : base + off;
            if (back && off > base) {
                continue;
            }
            void *p = mmap((void *)cand, size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) {
                continue;
            }
            if ((uintptr_t)p == cand) {
                return p;
            }
            munmap(p, size); /* kernel ignored the hint; keep looking */
        }
    }
    return NULL;
}

static bool hook_func(const char *what, void *code, void *replacement, void **orig_out)
{
    uint32_t *target = (uint32_t *)code;

    /* Log the entry instruction unconditionally: if we decline below, this is
       the only thing that says why, and it costs one line. */
    LOGI("hook %s: entry %08x at %p", what, target[0], (void *)target);

    if (is_pc_relative(target[0])) {
        LOGE("hook %s: entry instruction %08x is PC-relative - refusing", what, target[0]);
        return false;
    }

    long page = sysconf(_SC_PAGESIZE);
    /* One page holds both halves: the island we branch to, and the trampoline
       that runs the displaced instruction and jumps back. */
    uint32_t *mem = alloc_near(target, (size_t)page);
    if (mem == NULL) {
        LOGE("hook %s: no free page within B reach of %p", what, (void *)target);
        return false;
    }
    uint32_t *island = mem;     /* LDR X17,#8 ; BR X17 ; .quad replacement */
    uint32_t *tramp = mem + 4;  /* displaced insn ; absolute jump back     */
    write_abs_jump(island, replacement);
    tramp[0] = target[0];
    write_abs_jump(tramp + 1, target + 1);
    if (mprotect(mem, (size_t)page, PROT_READ | PROT_EXEC) != 0) {
        LOGE("hook %s: island mprotect failed", what);
        munmap(mem, (size_t)page);
        return false;
    }
    __builtin___clear_cache((char *)mem, (char *)mem + page);

    int64_t delta = (int64_t)(uintptr_t)island - (int64_t)(uintptr_t)target;
    if (delta < -B_RANGE || delta >= B_RANGE) {
        LOGE("hook %s: island landed %lld bytes away - out of reach", what, (long long)delta);
        munmap(mem, (size_t)page);
        return false;
    }

    uintptr_t start = (uintptr_t)target & ~(uintptr_t)(page - 1);
    size_t span = (uintptr_t)target + sizeof(uint32_t) - start;
    if (mprotect((void *)start, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("hook %s: could not make text writable", what);
        munmap(mem, (size_t)page);
        return false;
    }
    /* One aligned 4-byte store: a thread mid-call sees either the old
       instruction or the branch, never a mixture. */
    *target = 0x14000000u | ((uint32_t)(delta >> 2) & 0x03FFFFFFu);
    mprotect((void *)start, span, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)target, (char *)target + sizeof(uint32_t));

    *orig_out = tramp;
    return true;
}

#else /* !__aarch64__ */

/* armeabi-v7a: the shim still loads and forwards JNI_OnLoad, it just does not
   hook. Every device this targets is arm64; writing a second instruction
   rewriter for a dead ABI is not worth it. */
static bool hook_func(const char *what, void *code, void *replacement, void **orig_out)
{
    (void)code;
    (void)replacement;
    (void)orig_out;
    LOGE("hook %s: only implemented for arm64-v8a", what);
    return false;
}

#endif

/* -------------------------------------------------------------------------
 * Runtime readiness
 *
 * il2cpp_domain_get() is NOT safe to call before il2cpp_init has run: it
 * dereferences runtime state that does not exist yet, and polling it segfaults
 * the game on a cold start (SIGSEGV in il2cpp_domain_get, reading 0x135). So
 * the trigger is il2cpp_init returning, not the domain becoming non-null.
 * ---------------------------------------------------------------------- */
static volatile int g_runtime_ready = 0;
static int (*orig_il2cpp_init)(const char *name);

static int hook_il2cpp_init(const char *name)
{
    int rc = orig_il2cpp_init(name);
    /* Stays cheap: this is Unity's startup thread, and the real work belongs
       on ours. */
    g_runtime_ready = 1;
    return rc;
}

/* dlopen with RTLD_NOLOAD only asks the linker whether the library is mapped;
   it never calls into it, so this is safe long before the runtime exists. */
static void *wait_for_library(void)
{
    for (int i = 0; i < 6000; i++) { /* ~60s at 10ms */
        void *lib = dlopen("libil2cpp.so", RTLD_NOLOAD | RTLD_NOW);
        if (lib != NULL) {
            LOGI("libil2cpp.so mapped after %d ms", i * 10);
            return lib;
        }
        usleep(10 * 1000); /* tight, to shrink the gap before il2cpp_init runs */
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * AEC.GetResponse hook
 *
 * IL2CPP appends a trailing `const MethodInfo*` to every compiled method, and
 * passes `this` first for instance methods. Declaring two opaque arguments
 * covers both shapes on ARM: a static 0-arg method reads only the first, an
 * instance 0-arg method reads both, and either way the return lands in the
 * same register. So this works without knowing whether AEC.GetResponse is
 * static - which the desktop Harmony patch does not tell us either.
 * ---------------------------------------------------------------------- */
static void *(*orig_get_response)(void *a0, void *a1);
static void *g_time_get_time;   /* moved up from its original spot near the skill globals -
                                    log_packet below needs it and runs long before that point */

/* Quest-progress signals lifted straight off the wire, mirroring what the
   desktop's RuntimeEvents (a Harmony patch on ResponseQuestComplete.Execute)
   captures - reading the SAME already-populated fields directly off the
   response object GetResponse hands back is simpler here since this hook
   already intercepts every response, no separate patch needed.
   - QComp (ResponseQuestComplete): ID + Success confirm a turn-in actually
     landed, instead of advancing the chain the instant we SEND the request -
     a dropped/rejected send used to silently desync chain progress from the
     server's actual state.
   - rNotify (ResponseNotify): "Spam Detected" means back off and retry, not
     failure; anything else after a turn-in is a real rejection.
   - mKill: any kill counts as forward progress for the hunt-stall check,
     independent of which quest/objective it happened to credit. */
static void *g_qcomp_id_field, *g_qcomp_success_field;
static void *g_notify_msg_field;
static int32_t g_qcomp_qid = -1;
static float g_qcomp_at = -1.0f;
static char g_notify_msg[128];
static float g_notify_at = -1.0f;
static float g_quest_last_activity_at;

/* Packet log shown in the menu. Written from the GetResponse/sendRequest hooks
   and read in OnGUI - all three run on Unity's main thread, so no lock. */
#define PKT_ROWS 12
#define PKT_LEN 56
static char g_pkt[PKT_ROWS][PKT_LEN];
static int g_pkt_head;
static int g_pkt_count;
static int g_pkt_total;
static int g_block_incoming; /* interceptor: drop received packets */

static void pkt_push(const char *line)
{
    size_t n = strlen(line);
    if (n > PKT_LEN - 1) {
        n = PKT_LEN - 1;
    }
    memcpy(g_pkt[g_pkt_head], line, n);
    g_pkt[g_pkt_head][n] = '\0';
    g_pkt_head = (g_pkt_head + 1) % PKT_ROWS;
    if (g_pkt_count < PKT_ROWS) {
        g_pkt_count++;
    }
    g_pkt_total++;
}

/* Newest first, index 0 .. g_pkt_count-1. */
static const char *pkt_row(int i)
{
    return g_pkt[(g_pkt_head - 1 - i + 2 * PKT_ROWS) % PKT_ROWS];
}

static void log_packet(void *response, int blocked)
{
    void *klass = il2cpp_object_get_class(response);
    if (klass == NULL) {
        return;
    }
    const char *type_name = il2cpp_class_get_name(klass);

    /* GetCommand() is the same call the desktop patch makes. Resolving it per
       packet is a short linear scan; the packet rate does not justify a cache. */
    char cmd[40] = "?";
    void *m = il2cpp_class_get_method_from_name(klass, "GetCommand", 0);
    if (m != NULL) {
        mstr_to_utf8(inv(m, response, NULL), cmd, sizeof(cmd));
    }
    char line[PKT_LEN];
    /* Mirrors the desktop interceptor's BLOCKED/ALLOWED log. */
    snprintf(line, sizeof(line), "%s%s", blocked ? "x " : "< ", cmd[0] ? cmd : "?");
    pkt_push(line);
    LOGI("packet %s%s (%s)", blocked ? "[BLOCKED] " : "", type_name ? type_name : "?", cmd);

    float now_ts = g_time_get_time ? inv_float(g_time_get_time, NULL, NULL) : 0.0f;
    if (strcmp(cmd, "QComp") == 0) {
        if (g_qcomp_id_field == NULL) {
            g_qcomp_id_field = il2cpp_class_get_field_from_name(klass, "ID");
            g_qcomp_success_field = il2cpp_class_get_field_from_name(klass, "Success");
        }
        int32_t id = -1;
        uint8_t ok = 0;
        if (g_qcomp_id_field != NULL) {
            il2cpp_field_get_value(response, g_qcomp_id_field, &id);
        }
        if (g_qcomp_success_field != NULL) {
            il2cpp_field_get_value(response, g_qcomp_success_field, &ok);
        }
        if (ok) {
            g_qcomp_qid = id;
            g_qcomp_at = now_ts;
            g_quest_last_activity_at = now_ts;
        }
    } else if (strcmp(cmd, "rNotify") == 0) {
        if (g_notify_msg_field == NULL) {
            g_notify_msg_field = il2cpp_class_get_field_from_name(klass, "msg");
        }
        void *msg_str = NULL;
        if (g_notify_msg_field != NULL) {
            il2cpp_field_get_value(response, g_notify_msg_field, &msg_str);
        }
        mstr_to_utf8(msg_str, g_notify_msg, sizeof(g_notify_msg));
        g_notify_at = now_ts;
    } else if (strcmp(cmd, "mKill") == 0) {
        g_quest_last_activity_at = now_ts;
    }
}

static void *hook_get_response(void *a0, void *a1)
{
    void *response = orig_get_response(a0, a1);
    /* GetResponse is polled and returns null when nothing is queued, so the
       null case is the common one and must stay cheap. */
    if (response != NULL) {
        log_packet(response, g_block_incoming);
        if (g_block_incoming) {
            /* The response is consumed from the queue but never handed to the
               game - which is exactly what blocking means here. */
            return NULL;
        }
    }
    return response;
}

/* -------------------------------------------------------------------------
 * Outgoing packets: AEC.sendRequest(Request)
 *
 * Instance method, so the native shape is (this, request, MethodInfo*). Gives
 * the send-side half of the sniffer, and captures the live AEC instance that
 * the menu's Send button needs.
 * ---------------------------------------------------------------------- */
static void *(*orig_send_request)(void *self, void *req, void *method);
static void *g_aec_instance;
static void *g_request_cmd_field;

static void *hook_send_request(void *self, void *req, void *method)
{
    g_aec_instance = self;
    if (req != NULL && g_request_cmd_field != NULL && il2cpp_field_get_value) {
        void *cmd_str = NULL;
        il2cpp_field_get_value(req, g_request_cmd_field, &cmd_str);
        char cmd[40];
        mstr_to_utf8(cmd_str, cmd, sizeof(cmd));
        char line[PKT_LEN];
        snprintf(line, sizeof(line), "> %s", cmd[0] ? cmd : "?");
        pkt_push(line);
        LOGI("sent %s", cmd[0] ? cmd : "?");
    }
    return orig_send_request(self, req, method);
}

/* MethodInfo.methodPointer is the struct's first field in every IL2CPP version
   to date, but that is a layout assumption rather than a documented API, so
   verify the result actually points into libil2cpp.so before hooking it. */
static void *method_code_ptr(void *method)
{
    void *code = *(void **)method;
    if (code == NULL) {
        LOGE("method has a null code pointer - not compiled?");
        return NULL;
    }
    Dl_info info;
    if (dladdr(code, &info) == 0 || info.dli_fname == NULL ||
        strstr(info.dli_fname, "libil2cpp.so") == NULL) {
        LOGE("code pointer %p is not inside libil2cpp.so - MethodInfo layout changed", code);
        return NULL;
    }
    return code;
}

/* -------------------------------------------------------------------------
 * IMGUI probe (one-shot, diagnostic)
 *
 * An on-device IMGUI menu needs two things: the IMGUI types to have survived
 * `unity.strip-engine-code`, and a live OnGUI to hook, because GUILayout calls
 * are only legal inside one. Both are cheap to answer from reflection and
 * expensive to guess at, so answer them once and log the result.
 * ---------------------------------------------------------------------- */
static void probe_imgui(void *domain, void *lib,
                        il2cpp_domain_assembly_open_t assembly_open,
                        il2cpp_assembly_get_image_t assembly_image,
                        il2cpp_class_from_name_t class_from_name)
{
    il2cpp_domain_get_assemblies_t get_assemblies =
        (il2cpp_domain_get_assemblies_t)dlsym(lib, "il2cpp_domain_get_assemblies");
    il2cpp_image_get_class_count_t class_count =
        (il2cpp_image_get_class_count_t)dlsym(lib, "il2cpp_image_get_class_count");
    il2cpp_image_get_class_t image_class =
        (il2cpp_image_get_class_t)dlsym(lib, "il2cpp_image_get_class");
    if (!get_assemblies || !class_count || !image_class || !il2cpp_class_get_methods ||
        !il2cpp_method_get_name || !il2cpp_class_get_namespace) {
        LOGE("imgui probe: reflection exports missing, skipping");
        return;
    }

    /* 1. Did the IMGUI types survive stripping, at runtime and not just in the
          metadata? GUILayout is what a menu is actually built from. */
    void *imgui = assembly_open(domain, "UnityEngine.IMGUIModule");
    void *gui_layout = NULL;
    if (imgui != NULL) {
        gui_layout = class_from_name(assembly_image(imgui), "UnityEngine", "GUILayout");
    }
    LOGI("imgui probe: IMGUIModule=%p GUILayout=%p", imgui, gui_layout);

    /* 2. Which types actually declare OnGUI? Each one is a hook point that puts
          us inside a valid GUI context. */
    size_t n_asm = 0;
    void **assemblies = get_assemblies(domain, &n_asm);
    if (assemblies == NULL) {
        LOGE("imgui probe: could not enumerate assemblies");
        return;
    }
    int hits = 0;
    size_t scanned = 0;
    for (size_t a = 0; a < n_asm; a++) {
        void *image = assembly_image(assemblies[a]);
        if (image == NULL) {
            continue;
        }
        size_t n_cls = class_count(image);
        for (size_t c = 0; c < n_cls; c++) {
            void *klass = image_class(image, c);
            if (klass == NULL) {
                continue;
            }
            scanned++;
            void *iter = NULL, *m;
            while ((m = il2cpp_class_get_methods(klass, &iter)) != NULL) {
                const char *nm = il2cpp_method_get_name(m);
                if (nm != NULL && strcmp(nm, "OnGUI") == 0) {
                    const char *ns = il2cpp_class_get_namespace(klass);
                    LOGI("imgui probe: OnGUI on %s%s%s", (ns && *ns) ? ns : "",
                         (ns && *ns) ? "." : "", il2cpp_class_get_name(klass));
                    /* First hit becomes the menu's host: GUILayout/GUI calls are
                       only legal inside an OnGUI, and the game declares none of
                       its own, so we borrow a shipped component's. */
                    if (hits == 0) {
                        g_host_class = klass;
                        g_host_ongui = m;
                    }
                    hits++;
                }
            }
        }
    }
    LOGI("imgui probe: scanned %zu classes in %zu assemblies, %d OnGUI method(s)", scanned,
         n_asm, hits);
}

/* -------------------------------------------------------------------------
 * API probe (one-shot, diagnostic)
 *
 * Reflection only - reads the method tables, creates nothing and calls
 * nothing. The point is to stop guessing at the next two features:
 *   - what AEC exposes for SENDING, which is the other half of the packet
 *     tools and needs no new hook, just il2cpp_runtime_invoke;
 *   - which AddComponent overload survives, since an IMGUI menu has to attach
 *     one of the two OnGUI-declaring components to a GameObject to get drawn.
 * ---------------------------------------------------------------------- */
static void log_methods(const char *label, void *klass, const char *filter)
{
    if (klass == NULL) {
        LOGE("api probe: %s - class not found", label);
        return;
    }
    void *iter = NULL, *m;
    int n = 0;
    while ((m = il2cpp_class_get_methods(klass, &iter)) != NULL) {
        const char *nm = il2cpp_method_get_name(m);
        if (nm == NULL || (filter != NULL && strstr(nm, filter) == NULL)) {
            continue;
        }
        uint32_t pc = il2cpp_method_get_param_count ? il2cpp_method_get_param_count(m) : 0u;
        /* Parameter types, so overload choices stop being guesswork. */
        char sig[192];
        size_t used = 0;
        sig[0] = '\0';
        for (uint32_t i = 0; i < pc && il2cpp_method_get_param && il2cpp_type_get_name; i++) {
            char *tn = il2cpp_type_get_name(il2cpp_method_get_param(m, i));
            if (tn == NULL) {
                continue;
            }
            const char *shortname = strrchr(tn, '.');
            shortname = shortname ? shortname + 1 : tn;
            int w = snprintf(sig + used, sizeof(sig) - used, "%s%s", used ? "," : "", shortname);
            if (w > 0 && (size_t)w < sizeof(sig) - used) {
                used += (size_t)w;
            }
            if (il2cpp_free) {
                il2cpp_free(tn);
            }
        }
        LOGI("api probe: %s.%s/%u(%s)", label, nm, pc, sig);
        if (++n >= 200) {
            LOGI("api probe: %s - truncated at 200", label);
            break;
        }
    }
    LOGI("api probe: %s - %d method(s)", label, n);
}

static void probe_api(void *domain, void *aec,
                      il2cpp_domain_assembly_open_t assembly_open,
                      il2cpp_assembly_get_image_t assembly_image,
                      il2cpp_class_from_name_t class_from_name)
{
    if (!il2cpp_class_get_methods || !il2cpp_method_get_name) {
        return;
    }
    log_methods("AEC", aec, NULL);

    void *core = assembly_open(domain, "UnityEngine.CoreModule");
    if (core == NULL) {
        LOGE("api probe: UnityEngine.CoreModule not found");
        return;
    }
    void *core_image = assembly_image(core);
    log_methods("GameObject", class_from_name(core_image, "UnityEngine", "GameObject"),
                "AddComponent");
    log_methods("Object", class_from_name(core_image, "UnityEngine", "Object"),
                "DontDestroyOnLoad");

    /* strip-engine-code keeps only what the game actually calls, so which IMGUI
       entry points survive is a property of this build, not of Unity. Dump both
       classes rather than assuming the usual ones exist. */
    void *imgui = assembly_open(domain, "UnityEngine.IMGUIModule");
    if (imgui != NULL) {
        void *img = assembly_image(imgui);
        log_methods("GUI", class_from_name(img, "UnityEngine", "GUI"), "Button");
        log_methods("GUISkin", class_from_name(img, "UnityEngine", "GUISkin"), "get_");
    }
    /* IMGUI has no surviving TextField, so the Send box needs the OS keyboard.
       TouchScreenKeyboard is the mobile-native way in and is better UX anyway. */
    log_methods("TouchScreenKeyboard",
                class_from_name(core_image, "UnityEngine", "TouchScreenKeyboard"), NULL);

    /* Wall-aware movement needs exact overload signatures before committing
       to a disambiguation rule - Unity ships several Raycast/OverlapCircle
       overloads (ContactFilter2D-based ones included), and guessing which
       one find_method's (argc, param_index, type substring) match lands on
       is exactly the kind of thing that goes wrong silently. Dump the real
       ones this build has. */
    void *phys2d = assembly_open(domain, "UnityEngine.Physics2DModule");
    if (phys2d != NULL) {
        void *p2d_image = assembly_image(phys2d);
        log_methods("Physics2D", class_from_name(p2d_image, "UnityEngine", "Physics2D"),
                    "Raycast");
        log_methods("Physics2D", class_from_name(p2d_image, "UnityEngine", "Physics2D"),
                    "Overlap");
        log_methods("Physics2D", class_from_name(p2d_image, "UnityEngine", "Physics2D"),
                    "Cast");
        log_methods("BoxCollider2D",
                    class_from_name(p2d_image, "UnityEngine", "BoxCollider2D"), NULL);
        log_methods("Collider2D", class_from_name(p2d_image, "UnityEngine", "Collider2D"),
                    "ounds");
        log_methods("RaycastHit2D", class_from_name(p2d_image, "UnityEngine", "RaycastHit2D"),
                    NULL);
    } else {
        LOGE("api probe: UnityEngine.Physics2DModule not found");
    }
    log_methods("LayerMask", class_from_name(core_image, "UnityEngine", "LayerMask"), NULL);
    log_methods("Transform", class_from_name(core_image, "UnityEngine", "Transform"),
                "TransformPoint");
    log_methods("Transform", class_from_name(core_image, "UnityEngine", "Transform"),
                "ossyScale");
}

/* -------------------------------------------------------------------------
 * On-device menu
 *
 * Two constraints shape this:
 *   - GUI calls are only legal inside an OnGUI, and the game declares none, so
 *     we attach a shipped component that does and hook its OnGUI.
 *   - Unity refuses GameObject creation off the main thread, so the setup runs
 *     from a hook on AEC.Update rather than from our background thread.
 *     That hook doubles as Beyond's per-frame tick.
 * ---------------------------------------------------------------------- */
static void *g_gui_box;   /* UnityEngine.GUI.Box(Rect, string) */
static int g_ui_ready;
static int g_draw_logged;

/* il2cpp_class_get_method_from_name returns the first name+argc match, which
   for an overloaded UnityEngine method is usually the wrong one - GUI.Box has
   a (Rect, Texture) sibling. Match a parameter's type name too. */
static void *find_method(void *klass, const char *name, int argc, uint32_t param_index,
                         const char *param_type_substr)
{
    if (klass == NULL || !il2cpp_class_get_methods) {
        return NULL;
    }
    void *iter = NULL, *m;
    while ((m = il2cpp_class_get_methods(klass, &iter)) != NULL) {
        const char *nm = il2cpp_method_get_name(m);
        if (nm == NULL || strcmp(nm, name) != 0) {
            continue;
        }
        if (il2cpp_method_get_param_count && (int)il2cpp_method_get_param_count(m) != argc) {
            continue;
        }
        if (param_type_substr == NULL) {
            return m;
        }
        void *pt = il2cpp_method_get_param ? il2cpp_method_get_param(m, param_index) : NULL;
        if (pt == NULL) {
            continue;
        }
        char *tn = il2cpp_type_get_name ? il2cpp_type_get_name(pt) : NULL;
        int ok = (tn != NULL && strstr(tn, param_type_substr) != NULL);
        if (tn != NULL && il2cpp_free) {
            il2cpp_free(tn);
        }
        if (ok) {
            return m;
        }
    }
    return NULL;
}

static void *g_gui_button;      /* GUI.Button(Rect, GUIContent, GUIStyle) */
static void *g_gui_label;
static void *g_gui_set_matrix;
static void *g_gui_get_skin;
static void *g_skin_get_button;
static void *g_content_class;
static void *g_content_ctor;
static void *g_request_class;
static void *g_request_ctor;
static void *g_send_request;
/* TouchScreenKeyboard: IMGUI's TextField did not survive strip-engine-code, so
   text input comes from the OS keyboard - which is the right control on a
   phone regardless. */
static void *g_tsk_open;
static void *g_tsk_get_text;
static void *g_tsk_get_status;
static void *g_kb;         /* GC handle: live TouchScreenKeyboard, NULL when closed */
static char g_input[96];   /* command text; a C buffer needs no GC handle at all */
static char g_spoof[40];   /* nameplate name spoof; empty = off */
static int g_kb_target;    /* which buffer the open keyboard writes into */
#define KB_CMD 0
#define KB_SPOOF 1
#define KB_TITLE 2

/* Named quest chains: starting quest ID + map/frame/pad, and the full
   ID sequence a chain follows (quest N+1 = whichever quest has
   prevQuest == N), pulled directly from InfinityServer's own questdb via
   the same logic scripts/export_beyond_chains.py uses - not guessed, and
   not re-derived on-device (no prevQuest-walking here at all; the whole
   sequence is just baked in, which is simpler and does not depend on
   whether the client happens to auto-track the next storyline quest).
   Zard Killer's pad in the DB carries a stray leading tab - normalized to
   "Spawn" here rather than reproduced literally. */
typedef struct {
    const char *name;
    const char *map;
    const char *frame;
    const char *pad;
    const int32_t *ids;
    int count;
} ChainDef;

static const int32_t CHAIN_LAIR[] = {19, 20, 40, 41, 42, 43, 44, 45, 46, 47, 59};
static const int32_t CHAIN_BLUDRUT[] = {157, 120, 149, 121, 122, 119, 123, 124, 125, 164, 127,
                                        158, 150, 129, 130, 163, 132, 151, 152, 133, 134, 135,
                                        136, 137, 138, 153, 154, 139, 140, 166, 142, 143, 144,
                                        146, 148};
static const int32_t CHAIN_ZARD[] = {185, 193, 194, 195, 196, 197, 198, 199};
static const int32_t CHAIN_FOREST[] = {236, 237, 238, 239, 240, 241, 242, 243, 244};

static const ChainDef CHAINS[] = {
    {"Lair", "lair", "Enter", "Down", CHAIN_LAIR, 11},
    {"Bludrut Keep", "bludrut", "Enter", "Spawn", CHAIN_BLUDRUT, 35},
    {"Zard Killer", "riverquest", "Enter", "Spawn", CHAIN_ZARD, 8},
    {"Forest Zards", "forest", "Enter", "Spawn", CHAIN_FOREST, 9},
};
#define CHAIN_COUNT ((int)(sizeof(CHAINS) / sizeof(CHAINS[0])))

/* Quest-farm state, read/written by quest_tick() further down.
   g_quest_selected: 0 = follow whatever UIQuestTracker.CurrentQuest already
   is (no ID entry - track/accept normally in-game), 1..N = the Nth entry of
   CHAINS, run start to finish off the baked-in ID list rather than the live
   tracker. Selecting is separate from running (g_quest_running) - a select
   panel to change the choice, plus one Start/Stop button, instead of either
   a button per chain or one button doing double duty as both selector and
   on/off switch. */
static int g_quest_selected;
static int g_quest_running;
static int g_quest_select_open;
static int g_chain_index;
static int g_chain_tfer_sent;
static float g_next_quest_tick;
static int g_quest_accept_sent;
static int g_quest_turnin_sent;
static float g_quest_turnin_sent_at;  /* gates chain-advance on confirmed QComp, not just send -
                                          see the QComp/rNotify tracking near hook_get_response */
static char g_quest_status[80] = "idle";

static float g_scale = 2.0f;
static int g_menu_open;
static int g_log_open;
static int g_help_open;
static int g_help_page;

/* Autoskills */
static void *g_skillslots; /* live UISkillSlots, captured from its Register hook */
static void *g_get_slot;
static void *g_use_skill;
static int g_autoskills;
static int g_autoskip_cutscenes; /* default off, matching BeyondAgentClass.autoSkipCutscenes */
static void *g_dialogger_endpressed;
static void *g_pending_cutscene_mgr; /* one-shot: set by the hook, consumed by cutscene_skip_tick */
static void *(*orig_start_cutscene)(void *self, void *method);
static int g_skill_slot;
static float g_next_skill;

/* Skill gating: mirrors BeyondAgentClass.IsSkillSlotButtonDisabled /
   IsSkillOnCooldown. Without this the loop is a blind spammer - it fires into
   greyed-out or cooling-down slots every tick, each one a rejected round trip
   to the server for nothing. The overlay's concrete type is only knowable
   once we have a live instance, so - like the desktop reflection - resolve
   cooldownActive()/cdRemain lazily on first sight and cache by class. */
static void *g_slotbtn_disabled_field;    /* SkillSlotButton.disabled        (bool)   */
static void *g_slotbtn_pendingcd_field;   /* SkillSlotButton.pendingCooldown (bool)   */
static void *g_slotbtn_cooldown_field;    /* SkillSlotButton.cooldown        (object) */
static void *g_cdoverlay_class;
static void *g_cdoverlay_active_method;   /* overlay.cooldownActive()  (bool),  tried first  */
static void *g_cdoverlay_remain_field;    /* overlay.cdRemain          (float), fallback     */

static bool skill_disabled(void *btn)
{
    if (btn == NULL || g_slotbtn_disabled_field == NULL || !il2cpp_field_get_value) {
        return false;
    }
    uint8_t v = 0;
    il2cpp_field_get_value(btn, g_slotbtn_disabled_field, &v);
    return v != 0;
}

static bool skill_on_cooldown(void *btn)
{
    if (btn == NULL || !il2cpp_field_get_value) {
        return false;
    }
    if (g_slotbtn_pendingcd_field != NULL) {
        uint8_t v = 0;
        il2cpp_field_get_value(btn, g_slotbtn_pendingcd_field, &v);
        if (v != 0) {
            return true;
        }
    }
    if (g_slotbtn_cooldown_field == NULL || !il2cpp_object_get_class) {
        return false;
    }
    void *overlay = NULL;
    il2cpp_field_get_value(btn, g_slotbtn_cooldown_field, &overlay);
    if (overlay == NULL) {
        return false;
    }
    void *klass = il2cpp_object_get_class(overlay);
    if (klass != g_cdoverlay_class) {
        g_cdoverlay_class = klass;
        g_cdoverlay_active_method = il2cpp_class_get_method_from_name
            ? il2cpp_class_get_method_from_name(klass, "cooldownActive", 0) : NULL;
        g_cdoverlay_remain_field = il2cpp_class_get_field_from_name
            ? il2cpp_class_get_field_from_name(klass, "cdRemain") : NULL;
    }
    if (g_cdoverlay_active_method != NULL) {
        return inv_bool(g_cdoverlay_active_method, overlay, NULL);
    }
    if (g_cdoverlay_remain_field != NULL) {
        float remain = 0.0f;
        il2cpp_field_get_value(overlay, g_cdoverlay_remain_field, &remain);
        return remain > 0.0f;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * Auto-hunt
 *
 * Farms whatever hostile is nearest in the loaded map: target it the same
 * way a player click does (Targetable.ClickMe() x2 - first assigns target,
 * second triggers chargeAuto() -> Charge(0) -> RequestStartCharge), walk
 * toward it if out of engage range, and leave autoskills (already gated on
 * cooldown/disabled) to handle the actual casting once in range.
 *
 * Deliberately no A* here yet - straight-line EntityMovementUpdater.walkTo,
 * same as the desktop agent before PathWalker existed. A charge that dies on
 * a Blocker collider is a known, accepted gap for this first pass; wall-aware
 * pathing is a separate, larger follow-on (PathWalker.cs's own A* + Physics2D
 * raycasts), not a blocker for hunting in an open cell.
 * ---------------------------------------------------------------------- */
static void *g_entity_getgameobject;    /* Entity.getGameObject()          (0-arg) */
static void *g_entity_get_target;       /* Entity.get_target()             (0-arg) */
static void *g_go_get_transform;        /* GameObject.get_transform()      (0-arg) */
static void *g_go_getcomponent;         /* GameObject.GetComponent(Type)   (1-arg) */
static void *g_transform_get_localpos;  /* Transform.get_localPosition()   (0-arg) */
static void *g_component_get_transform; /* Component.get_transform() - MapCell is a Component,
                                           not a GameObject, so this is a separate resolution
                                           from GameObject.get_transform. Moved up from its
                                           original spot - component_local_in_player_frame needs
                                           it and runs long before that point. */
static void *g_transform_get_position;   /* Transform.get_position() - WORLD position, unlike
                                             get_localPosition (relative to the object's own
                                             immediate parent). Also moved up from its original
                                             spot near TransformPoint, for the same reason. */
static void *g_transform_inversetransformpoint; /* Transform.InverseTransformPoint(Vector3) -
                                             world->local, the other direction from TransformPoint */
static void *g_targetable_class;
static void *g_targetable_type_obj;     /* cached Type object for GetComponent(Type) */
static void *g_targetable_clickme;
static void *g_emu_class;
static void *g_emu_type_obj;
static void *g_emu_walkto;              /* EntityMovementUpdater.walkTo(Vector3,float) */
static void *g_emu_cellspeed_field;     /* static int */
static int g_hunt;
static float g_next_hunt;
static float g_hunt_settle_until;       /* pause hunting until this Time.time, post-revive */
static float g_max_engage_dist = 9.0f;  /* matches QuestRunner.MaxEngageDist */
#define QUEST_HUNT_TIMEOUT_SEC 90.0f     /* matches QuestRunner.HuntTimeoutNoProgress */
#define INTERACT_REACH_DIST 2.5f         /* matches QuestRunner.InteractReachDist. Widening this
                                             to 6.0 as a first guess did not fix "just approaching
                                             forever" - the real cause was the missing give-up
                                             fallback in approach_target, not this threshold. */
/* The objective hunting is currently serving, published by quest_tick (which
   runs at 1Hz) for hunt_tick (which runs at ~3Hz) so target selection can
   filter on what the quest actually asks for instead of grabbing whatever is
   nearest. Holding a raw managed pointer between ticks is safe here on two
   counts: il2cpp's Boehm GC is non-moving, and the item stays reachable the
   whole time via Quest.Turnins on the cached quest, so it cannot be
   collected out from under us. Cleared whenever hunting is not objective-
   driven, which reverts targeting to "any hostile". */
static void *g_hunt_obj;
static int32_t g_hunt_qotype = -1;
static int32_t g_hunt_qid = -1;   /* current quest id, for the QUEST_MON_HINTS lookup */
static void *g_req_movecell_class;
static void *g_req_movecell_ctor;  /* RequestMoveToCell(string Frame, string Pad) - resolved
                                       alongside the other Request subclasses in setup_menu */
static char g_hunt_nav_frame[40];  /* frame a moveToCell was last sent for, and when -
                                       mirrors QuestRunner.GoToFrame's resend throttle */
static float g_hunt_nav_sent_at;
/* Interact/Apop approach handoff: quest_tick (1Hz) publishes WHERE to walk,
   hunt_tick (~3Hz) actually walks there and reports back whether it is time
   to click. See hunt_tick's top and approach_target. */
static int g_interact_approach_active;
static float g_interact_approach_target[3];
static float g_interact_reach_dist_pub;
static int g_interact_ready;
static float g_next_interact_approach;

/* Shared with the nameplate spoof further down, which also needs the local
   player - resolved once in setup_menu. */
static void *g_get_main_player;

/* Local-space player/target position, as a 3-float unboxed struct - same
   unbox-a-boxed-return pattern already proven for KeyValuePair in
   probe_monsters. NULL on any failure (no GameObject yet, no Transform, …). */
static bool read_local_pos(void *entity, float out[3])
{
    if (entity == NULL || g_entity_getgameobject == NULL || g_go_get_transform == NULL ||
        g_transform_get_localpos == NULL || !il2cpp_object_unbox) {
        return false;
    }
    void *go = inv(g_entity_getgameobject, entity, NULL);
    void *tr = go ? inv(g_go_get_transform, go, NULL) : NULL;
    void *boxed_pos = tr ? inv(g_transform_get_localpos, tr, NULL) : NULL;
    float *raw = boxed_pos ? (float *)il2cpp_object_unbox(boxed_pos) : NULL;
    if (raw == NULL) {
        return false;
    }
    out[0] = raw[0];
    out[1] = raw[1];
    out[2] = raw[2];
    return true;
}

/* A Transform's WORLD position, converted into `player_parent`'s local
   space via InverseTransformPoint - exactly TickInteract/TickApop's
   `me.transform.parent.InverseTransformPoint(target.transform.position)`.

   The first cut of this (read_component_local_pos, since removed) used the
   target's own localPosition directly, on the assumption that a machine or
   NPC shares the player's parent the same way a hostile Monster does during
   combat - true for monsters (they spawn as flat siblings in the cell), but
   NOT for level-design machines, which can sit under arbitrary nested
   sub-groups in the prefab hierarchy. localPosition is relative to a
   transform's OWN immediate parent, not the player's - comparing it
   directly against the player's local position was comparing two different
   coordinate spaces. On device this meant the bot walked toward a point
   offset from the real machine and never converged - reported as "running
   at the armor pieces rather than clicking them." Going through world space
   first sidesteps the mismatch regardless of nesting depth. Falls back to
   the raw world position when there is no parent (matches PC's `?? world`
   fallback for a rare parentless player). */
static bool transform_local_in_player_frame(void *tr, void *player_parent, float out[3])
{
    if (tr == NULL || g_transform_get_position == NULL || !il2cpp_object_unbox) {
        return false;
    }
    void *boxed_world = inv(g_transform_get_position, tr, NULL);
    float *world = boxed_world ? (float *)il2cpp_object_unbox(boxed_world) : NULL;
    if (world == NULL) {
        return false;
    }
    if (player_parent != NULL && g_transform_inversetransformpoint != NULL) {
        void *args[1] = {world};
        void *boxed_local = inv(g_transform_inversetransformpoint, player_parent, args);
        float *local = boxed_local ? (float *)il2cpp_object_unbox(boxed_local) : NULL;
        if (local != NULL) {
            out[0] = local[0];
            out[1] = local[1];
            out[2] = local[2];
            return true;
        }
    }
    out[0] = world[0];
    out[1] = world[1];
    out[2] = world[2];
    return true;
}

/* Component (MapMachine, NPCButton) variant - goes through
   Component.get_transform rather than Entity.getGameObject(). */
static bool component_local_in_player_frame(void *component, void *player_parent, float out[3])
{
    void *tr = component && g_component_get_transform
                  ? inv(g_component_get_transform, component, NULL)
                  : NULL;
    return transform_local_in_player_frame(tr, player_parent, out);
}

/* Entity (Monster/NPC) variant. */
static bool entity_local_in_player_frame(void *entity, void *player_parent, float out[3])
{
    void *go = entity && g_entity_getgameobject ? inv(g_entity_getgameobject, entity, NULL) : NULL;
    void *tr = go && g_go_get_transform ? inv(g_go_get_transform, go, NULL) : NULL;
    return transform_local_in_player_frame(tr, player_parent, out);
}

/* -------------------------------------------------------------------------
 * Wall-aware movement (native port of PathWalker.cs)
 *
 * A straight walkTo toward a distant point drags the character along every
 * curved wall between here and there - the game's own charge-walk gives up
 * on a Blocker collider (blockedMoveTimer) and just stops. The desktop fix
 * is: raycast the direct line first (cheap, the common case in an open
 * cell), and only when that is blocked, plan a route with A* over a coarse
 * grid sampled from the Blocker layer, then string-pull down to the corner
 * waypoints that matter and feed those to the same walkTo() hunting already
 * uses.
 *
 * One substitution from the desktop version: PathWalker.cs samples each
 * grid cell with Physics2D.OverlapCircle, which strip-engine-code removed
 * entirely from this build (confirmed empty via probe_api - 0 methods).
 * Physics2D.OverlapBox survived, and a small square footprint is a fine
 * stand-in for a circular clearance check at grid resolution. Everything
 * else - Raycast, RaycastHit2D.collider, Transform.TransformPoint,
 * get_lossyScale, LayerMask.NameToLayer - is confirmed present on this
 * build too (probe_api, run before any of this was written on top of it).
 *
 * Grid is smaller than the desktop's (80x80 vs 160x160, 1.0 vs 0.5 cell
 * size) because each sampled cell costs one native->managed round trip
 * through OverlapBox - the desktop pays that cost in-process, this shim
 * pays it across the IL2CPP call boundary, so coarsening the grid trades a
 * little precision for a lot fewer round trips. Static arrays sized to the
 * cap rather than malloc, since this runs on Unity's main thread, on a
 * timer, and a plan that would exceed the cap fails cleanly (falls back to
 * a direct walk) the same way the desktop's own MaxGridCells check does.
 * ---------------------------------------------------------------------- */
static void *g_p2d_raycast;              /* Physics2D.Raycast(Vector2,Vector2,float,int)   */
static void *g_p2d_overlapbox;           /* Physics2D.OverlapBox(Vector2,Vector2,float,int) */
static void *g_raycasthit2d_get_collider;
static void *g_transform_transformpoint; /* Transform.TransformPoint(Vector3) - local->world */
static void *g_transform_get_lossyscale;
static void *g_transform_get_parent;
static int32_t g_blocker_mask = -1;      /* 1 << LayerMask.NameToLayer("Blocker") */

#define PATH_CELL_SIZE 1.0f
#define PATH_CLEARANCE 0.35f
#define PATH_MARGIN 6.0f
#define PATH_W 80
#define PATH_H 80
#define PATH_CELLS (PATH_W * PATH_H)
#define PATH_MAX_WAYPOINTS 48
#define PATH_WAYPOINT_REACHED 0.45f
#define PATH_STALL_SEC 1.2f

static bool g_path_blocked[PATH_CELLS];
static float g_path_gscore[PATH_CELLS];
static int32_t g_path_from[PATH_CELLS];   /* predecessor cell index, -1 = none */
static uint8_t g_path_state[PATH_CELLS];  /* 0 unvisited, 1 open, 2 closed */

static float g_path_waypoints[PATH_MAX_WAYPOINTS][3];
static int g_path_count;
static int g_path_idx;
static int g_path_active;   /* currently following a planned route */
static int g_path_replanned_once;
static float g_path_issued_at;
static float g_path_last_move_at;
static float g_path_last_pos[3];
static float g_path_goal[3];
static int g_path_walk_issued;

/* World-space player parent transform, cached per hunt_tick call - every
   local<->world conversion below needs it, and it does not change mid-tick. */
static void *path_player_parent(void *player_go)
{
    if (player_go == NULL || g_go_get_transform == NULL || g_transform_get_parent == NULL) {
        return NULL;
    }
    void *tr = inv(g_go_get_transform, player_go, NULL);
    return tr ? inv(g_transform_get_parent, tr, NULL) : NULL;
}

static bool path_to_world(void *parent, const float local[3], float world[3])
{
    if (parent == NULL || g_transform_transformpoint == NULL || !il2cpp_object_unbox) {
        return false;
    }
    void *args[1] = {(void *)local};
    void *boxed = inv(g_transform_transformpoint, parent, args);
    float *raw = boxed ? (float *)il2cpp_object_unbox(boxed) : NULL;
    if (raw == NULL) {
        return false;
    }
    world[0] = raw[0];
    world[1] = raw[1];
    world[2] = raw[2];
    return true;
}

/* Blocker-layer raycast between two WORLD points. True (clear) on any
   resolution failure - the desktop's own LineClear does the same, since a
   pathing feature failing open into "just walk straight" is a much smaller
   problem than it silently refusing to move at all. */
static bool path_line_clear_world(const float a[3], const float b[3])
{
    if (g_p2d_raycast == NULL || g_raycasthit2d_get_collider == NULL || g_blocker_mask < 0 ||
        !il2cpp_object_unbox) {
        return true;
    }
    float dx = b[0] - a[0], dy = b[1] - a[1];
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 0.001f) {
        return true;
    }
    float origin[2] = {a[0], a[1]};
    float dir[2] = {dx / dist, dy / dist};
    int32_t mask = g_blocker_mask;
    void *args[4] = {origin, dir, &dist, &mask};
    void *boxed_hit = inv(g_p2d_raycast, NULL, args);
    void *hit_raw = boxed_hit ? il2cpp_object_unbox(boxed_hit) : NULL;
    if (hit_raw == NULL) {
        return true;
    }
    void *collider = inv(g_raycasthit2d_get_collider, hit_raw, NULL);
    return collider == NULL;
}

static bool path_line_clear_local(void *parent, const float a[3], const float b[3])
{
    float wa[3], wb[3];
    if (!path_to_world(parent, a, wa) || !path_to_world(parent, b, wb)) {
        return true;
    }
    return path_line_clear_world(wa, wb);
}

/* Is a WORLD point inside a Blocker-layer collider? OverlapBox stands in for
   the desktop's OverlapCircle (stripped from this build - see the module
   comment above); a square footprint at grid resolution is a fine trade. */
static bool path_is_blocked_world(const float point[3], float half_size)
{
    if (g_p2d_overlapbox == NULL || g_blocker_mask < 0) {
        return false;
    }
    float p[2] = {point[0], point[1]};
    float size[2] = {half_size * 2.0f, half_size * 2.0f};
    float angle = 0.0f;
    int32_t mask = g_blocker_mask;
    void *args[4] = {p, size, &angle, &mask};
    return inv(g_p2d_overlapbox, NULL, args) != NULL;
}

static int path_cell_index(int x, int y)
{
    return y * PATH_W + x;
}

/* 8-directional A* over a grid covering start+goal (+margin), string-pulled
   down to the waypoints a straight raycast actually needs. Writes into
   g_path_waypoints/g_path_count; returns false (no waypoints written) if the
   grid is too big or genuinely unpathable - the caller falls back to a
   direct walk either way, so "no path found" is a normal outcome, not an
   error to surface. */
static bool path_plan(void *parent, const float start_local[3], const float goal_local[3])
{
    g_path_count = 0;

    float min_x = fminf(start_local[0], goal_local[0]) - PATH_MARGIN;
    float max_x = fmaxf(start_local[0], goal_local[0]) + PATH_MARGIN;
    float min_y = fminf(start_local[1], goal_local[1]) - PATH_MARGIN;
    float max_y = fmaxf(start_local[1], goal_local[1]) + PATH_MARGIN;
    int w = (int)ceilf((max_x - min_x) / PATH_CELL_SIZE);
    int h = (int)ceilf((max_y - min_y) / PATH_CELL_SIZE);
    if (w <= 0 || h <= 0 || w > PATH_W || h > PATH_H) {
        return false;
    }

    float lossy_scale = 1.0f;
    if (g_transform_get_lossyscale != NULL && il2cpp_object_unbox) {
        void *boxed = inv(g_transform_get_lossyscale, parent, NULL);
        float *raw = boxed ? (float *)il2cpp_object_unbox(boxed) : NULL;
        if (raw != NULL) {
            lossy_scale = raw[0];
        }
    }
    float world_half = PATH_CLEARANCE * lossy_scale;

    memset(g_path_blocked, 0, (size_t)(w * h) * sizeof(g_path_blocked[0]));
    for (int gx = 0; gx < w; gx++) {
        for (int gy = 0; gy < h; gy++) {
            float local[3] = {min_x + (gx + 0.5f) * PATH_CELL_SIZE,
                              min_y + (gy + 0.5f) * PATH_CELL_SIZE, start_local[2]};
            float world[3];
            bool blocked = true;
            if (path_to_world(parent, local, world)) {
                blocked = path_is_blocked_world(world, world_half);
            }
            g_path_blocked[path_cell_index(gx, gy)] = blocked;
        }
    }
    int blocked_count = 0;
    for (int i = 0; i < w * h; i++) {
        if (g_path_blocked[i]) {
            blocked_count++;
        }
    }

    int sx = (int)((start_local[0] - min_x) / PATH_CELL_SIZE);
    int sy = (int)((start_local[1] - min_y) / PATH_CELL_SIZE);
    int gxg = (int)((goal_local[0] - min_x) / PATH_CELL_SIZE);
    int gyg = (int)((goal_local[1] - min_y) / PATH_CELL_SIZE);
    sx = sx < 0 ? 0 : (sx >= w ? w - 1 : sx);
    sy = sy < 0 ? 0 : (sy >= h ? h - 1 : sy);
    gxg = gxg < 0 ? 0 : (gxg >= w ? w - 1 : gxg);
    gyg = gyg < 0 ? 0 : (gyg >= h ? h - 1 : gyg);

    int start_idx = path_cell_index(sx, sy);
    int goal_idx = path_cell_index(gxg, gyg);
    LOGI("path: grid %dx%d (%d/%d blocked), start=(%d,%d) goal=(%d,%d)", w, h, blocked_count,
         w * h, sx, sy, gxg, gyg);
    if (g_path_blocked[start_idx] || g_path_blocked[goal_idx]) {
        LOGI("path: start or goal cell itself is blocked - not our problem to fix");
        return false;
    }

    memset(g_path_state, 0, (size_t)(w * h) * sizeof(g_path_state[0]));
    for (int i = 0; i < w * h; i++) {
        g_path_gscore[i] = 1e9f;
        g_path_from[i] = -1;
    }
    g_path_gscore[start_idx] = 0.0f;
    g_path_state[start_idx] = 1;

    int dxs[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    int dys[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    bool found = false;
    int guard = w * h * 9 + 16;

    while (guard-- > 0) {
        /* Linear-scan open list: grids here are small (typically well under
           1000 cells for a hunt-range replan), so a real heap is not worth
           the extra code - this is O(cells) per pop, not O(cells^2) overall
           since each cell closes at most once. */
        int best = -1;
        float best_f = 0.0f;
        for (int i = 0; i < w * h; i++) {
            if (g_path_state[i] != 1) {
                continue;
            }
            int cx = i % w, cy = i / w;
            float hx = (float)(cx - gxg), hy = (float)(cy - gyg);
            float f = g_path_gscore[i] + sqrtf(hx * hx + hy * hy);
            if (best < 0 || f < best_f) {
                best = i;
                best_f = f;
            }
        }
        if (best < 0) {
            break; /* open set exhausted - unreachable */
        }
        if (best == goal_idx) {
            found = true;
            break;
        }
        g_path_state[best] = 2;
        int bx = best % w, by = best / w;
        for (int d = 0; d < 8; d++) {
            int nx = bx + dxs[d], ny = by + dys[d];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                continue;
            }
            int ni = path_cell_index(nx, ny);
            if (g_path_blocked[ni] || g_path_state[ni] == 2) {
                continue;
            }
            if (d >= 4 && (g_path_blocked[path_cell_index(bx, ny)] ||
                          g_path_blocked[path_cell_index(nx, by)])) {
                continue; /* no cutting a blocked corner diagonally */
            }
            float step = d >= 4 ? 1.41421f : 1.0f;
            float ng = g_path_gscore[best] + step;
            if (ng < g_path_gscore[ni]) {
                g_path_gscore[ni] = ng;
                g_path_from[ni] = best;
                g_path_state[ni] = 1;
            }
        }
    }
    if (!found) {
        LOGI("path: A* found no route - blocked grid or unreachable goal");
        return false;
    }

    /* Rebuild the raw cell-center path, then string-pull: keep only the
       corners a raycast from the current anchor cannot see past. */
    float raw[PATH_MAX_WAYPOINTS][3];
    int raw_count = 0;
    int walk = goal_idx;
    while (walk != start_idx && raw_count < PATH_MAX_WAYPOINTS - 1) {
        int cx = walk % w, cy = walk / w;
        raw[raw_count][0] = min_x + (cx + 0.5f) * PATH_CELL_SIZE;
        raw[raw_count][1] = min_y + (cy + 0.5f) * PATH_CELL_SIZE;
        raw[raw_count][2] = start_local[2];
        raw_count++;
        walk = g_path_from[walk];
        if (walk < 0) {
            return false; /* corrupt chain - fail safe rather than loop forever */
        }
    }
    /* raw is goal->start order; reverse in place, then append the true goal
       (the cell center is not exactly the target's actual position). */
    for (int i = 0; i < raw_count / 2; i++) {
        float tmp[3];
        memcpy(tmp, raw[i], sizeof(tmp));
        memcpy(raw[i], raw[raw_count - 1 - i], sizeof(tmp));
        memcpy(raw[raw_count - 1 - i], tmp, sizeof(tmp));
    }
    if (raw_count < PATH_MAX_WAYPOINTS) {
        memcpy(raw[raw_count], goal_local, sizeof(raw[raw_count]));
        raw_count++;
    }

    float anchor[3];
    memcpy(anchor, start_local, sizeof(anchor));
    int k = 0;
    while (k < raw_count && g_path_count < PATH_MAX_WAYPOINTS) {
        int next = k;
        for (int j = raw_count - 1; j > k; j--) {
            if (path_line_clear_local(parent, anchor, raw[j])) {
                next = j;
                break;
            }
        }
        memcpy(g_path_waypoints[g_path_count], raw[next], sizeof(g_path_waypoints[0]));
        memcpy(anchor, raw[next], sizeof(anchor));
        g_path_count++;
        k = next + 1;
    }
    LOGI("path: planned %d waypoint(s) (%d raw cells, string-pulled)", g_path_count, raw_count);
    return g_path_count > 0;
}

/* Drives an in-progress route: advances past reached waypoints (with the
   same string-pull skip-ahead PathWalker.cs does, so the walk cuts corners
   instead of visiting every grid cell), reissues walkTo when the current
   waypoint changes, and replans once on a stall before giving up for this
   attempt. Returns true while still navigating (caller should not also
   issue a direct walkTo this tick); false once done, failed, or never
   started, so the caller falls through to hunting's normal direct-engage
   path.
   done_out is set true only on "arrived" (path complete). */
static bool path_tick(void *parent, void *emu, const float goal_local[3], float speed,
                      bool *done_out)
{
    *done_out = false;
    float now = inv_float(g_time_get_time, NULL, NULL);
    float here[3];
    if (!read_local_pos(inv(g_get_main_player, NULL, NULL), here)) {
        return false;
    }

    bool need_replan = !g_path_active ||
                       (fabsf(g_path_goal[0] - goal_local[0]) +
                            fabsf(g_path_goal[1] - goal_local[1]) >
                        1.5f);
    if (need_replan) {
        memcpy(g_path_goal, goal_local, sizeof(g_path_goal));
        g_path_active = path_plan(parent, here, goal_local);
        g_path_idx = 0;
        g_path_walk_issued = 0;
        g_path_replanned_once = 0;
        memcpy(g_path_last_pos, here, sizeof(g_path_last_pos));
        g_path_last_move_at = now;
        if (!g_path_active) {
            return false; /* no route found - let the caller try a direct walk */
        }
    }

    float dx = here[0] - g_path_last_pos[0], dy = here[1] - g_path_last_pos[1];
    if (dx * dx + dy * dy > 0.001f) {
        memcpy(g_path_last_pos, here, sizeof(g_path_last_pos));
        g_path_last_move_at = now;
    }

    while (g_path_idx < g_path_count) {
        float *wp = g_path_waypoints[g_path_idx];
        float wdx = here[0] - wp[0], wdy = here[1] - wp[1];
        if (sqrtf(wdx * wdx + wdy * wdy) > PATH_WAYPOINT_REACHED) {
            break;
        }
        g_path_idx++;
        g_path_walk_issued = 0;
    }
    if (g_path_idx < g_path_count) {
        int furthest = g_path_idx;
        for (int i = g_path_count - 1; i > g_path_idx; i--) {
            if (path_line_clear_local(parent, here, g_path_waypoints[i])) {
                furthest = i;
                break;
            }
        }
        if (furthest != g_path_idx) {
            g_path_idx = furthest;
            g_path_walk_issued = 0;
        }
    }

    if (g_path_idx >= g_path_count) {
        g_path_active = 0;
        *done_out = true;
        return false;
    }

    bool stalled = g_path_walk_issued && (now - g_path_issued_at > 0.6f) &&
                   (now - g_path_last_move_at > PATH_STALL_SEC);
    if (stalled) {
        if (g_path_replanned_once) {
            g_path_active = 0; /* stalled twice - give up this attempt */
            return false;
        }
        g_path_replanned_once = 1;
        g_path_active = path_plan(parent, here, goal_local);
        g_path_idx = 0;
        g_path_walk_issued = 0;
        if (!g_path_active) {
            return false;
        }
    }

    if (!g_path_walk_issued && g_emu_walkto != NULL) {
        void *args[2] = {g_path_waypoints[g_path_idx], &speed};
        inv(g_emu_walkto, emu, args);
        g_path_walk_issued = 1;
        g_path_issued_at = now;
    }
    return true;
}

/* Shared with probe_monsters() below, which does the actual resolution (it
   runs first every tick - see hook_aec_update) and the same generic-
   dictionary walk technique, just counting instead of picking a winner. */
static void *g_area_currentarea_field;
static void *g_area_monsters_field;
static void *g_monster_reaction_field;
static void *g_entity_get_name;           /* resolved on Monster - see probe_monsters */
static void *g_entity_get_currentstate;   /* resolved on Entity - Monster doesn't override it */
/* Entity.get_ID - Monster's ctor assigns it from monBranch.MonID, which is
   what a Killcount objective's RefArray entries hold. Only Player overrides
   ID, so resolving on Entity is correct for monsters (unlike get_Name). */
static void *g_entity_get_id;
/* Declared here rather than with the rest of the quest bindings further down
   because target filtering (matches_objective) needs it, and that runs above
   them. */
static void *g_qti_getrefint_method;   /* QuestTurninItem.GetRefInt(int) - avoids ever
                                          touching RefArray's own array storage directly */
static void *g_entity_frame_field;     /* Entity.Frame (string) - moved up from the quest
                                          bindings block below; find_nearest_hostile's cell
                                          filter needs it and runs before that block. */

/* GameObject.GetComponent(Type) - the same non-generic overload setup_menu
   already uses for AddComponent(Type), since the generic GetComponent<T>()
   needs a resolved generic instantiation this shim has no path to. */
static void *get_component(void *go, void *type_obj)
{
    if (go == NULL || type_obj == NULL || g_go_getcomponent == NULL) {
        return NULL;
    }
    void *args[1] = {type_obj};
    return inv(g_go_getcomponent, go, args);
}

/* Nearest live hostile in Area.currentArea.Monsters, by 2D distance to the
   player. Reuses the same field/method handles probe_monsters resolved -
   this is the same generic-dictionary walk, just picking a winner instead of
   just counting. Returns NULL if nothing hostile is loaded. */
/* The correct `this` pointer for invoking an instance method on whatever a
   managed call just handed back.

   il2cpp_runtime_invoke wants the UNBOXED payload for a value type but the
   object pointer itself for a reference type, and getting this backwards is
   silent: unboxing a reference type just returns a pointer past the object
   header, so the callee reads its fields at the wrong offsets and behaves
   like a garbage-but-valid instance rather than crashing.

   That is exactly what broke objective dispatch. Dictionary<K,V>.Enumerator
   is a STRUCT, so the Monsters walk unboxing it was right - and that made
   "unbox the enumerator" look like the house rule. But System.Array's
   GetEnumerator returns IEnumerator, whose concrete type (SZArrayEnumerator)
   is a CLASS. Unboxing it handed MoveNext a bogus self, MoveNext read a
   nonsense index/length and returned false on the first call, so the loop
   body never ran once and next_incomplete_objective returned NULL for every
   quest - reported as "no actionable objective visible, hunting" with no
   error anywhere. Deciding per-object off is_valuetype is right for both. */
static void *self_ptr(void *obj)
{
    if (obj == NULL || !il2cpp_object_get_class) {
        return NULL;
    }
    void *klass = il2cpp_object_get_class(obj);
    if (klass != NULL && il2cpp_class_is_valuetype && il2cpp_class_is_valuetype(klass)) {
        return il2cpp_object_unbox ? il2cpp_object_unbox(obj) : NULL;
    }
    return obj;
}

/* Drives a foreach over any managed collection. `get_enumerator` must be
   resolved on the class that actually DECLARES it (a generic collection
   declares its own; an array inherits System.Array's), since
   il2cpp_class_get_method_from_name never searches base classes. MoveNext /
   get_Current then come off the returned enumerator's own concrete class. */
typedef struct {
    void *self;
    void *move_next;
    void *get_current;
} EnumWalk;

static bool enum_open(void *collection, void *get_enumerator, EnumWalk *w)
{
    w->self = NULL;
    w->move_next = NULL;
    w->get_current = NULL;
    if (collection == NULL || get_enumerator == NULL) {
        return false;
    }
    void *e = inv(get_enumerator, collection, NULL);
    if (e == NULL) {
        return false;
    }
    void *enum_class = il2cpp_object_get_class(e);
    if (enum_class == NULL) {
        return false;
    }
    w->self = self_ptr(e);
    w->move_next = il2cpp_class_get_method_from_name(enum_class, "MoveNext", 0);
    w->get_current = il2cpp_class_get_method_from_name(enum_class, "get_Current", 0);
    return w->self != NULL && w->move_next != NULL && w->get_current != NULL;
}

/* Per-quest monster-ID hints, baked in from InfinityServer's own questdb
   (server/questdb.py, itself synced from live AE captures - see
   scripts/export_beyond_chains.py, which generates the equivalent hint data
   the desktop Beyond consumes through a hand-authored chains.json).

   This exists because QuestTurninItem.RefArray - the ONLY per-objective
   target signal matches_objective() originally had - is empty on live AE for
   these quests. Confirmed by pulling every Lair/Bludrut/Zard/Forest quest out
   of the same questdb: e.g. quest 20 "The Wyverns" has refIds:[] but
   objectives[].monsters:[17]. With RefArray empty, the old signal-less logic
   fell through to "any hostile is fair game" for every Killcount objective -
   exactly the "keeps killing water draconians regardless of the active
   quest" behavior reported after the dispatch fix landed, since dispatch
   working correctly just meant it now REACHED the targeting code instead of
   never getting there. */
typedef struct {
    int32_t qid;
    const int32_t *mons;
    int count;
} QuestMonHint;

/* Covers every quest in server/db.py's questdb (180 quests, not just the 4
   named chains) that has at least one Killcount objective with a resolved
   monster - 78 of them. Same generation approach as the original 4-chain
   table (union of objectives[].monsters per quest, via
   scripts/export_beyond_chains.py's underlying questdb.build()), extended
   to hand-verified overrides for objectives the harvester never resolved a
   monster for at all (via:"none") - found by name-matching each one against
   server/db.py's monsters table and individually confirmed (some auto
   fuzzy-matches were wrong, e.g. "Cursed Cardboard Box" spuriously matching
   a monster named "Card" - nothing here was accepted without checking).
   Genuinely generic zone-wide objectives (e.g. "Undead Defeated" x50) were
   deliberately left out: any hostile IS the correct match for those, not a
   gap. This is what makes Track Current mode (not just the 4 baked chains)
   correctly target the right monster for whatever quest is tracked. */
static const int32_t MONS_Q1[] = {1, 7, 8};
static const int32_t MONS_Q11[] = {126};
static const int32_t MONS_Q14[] = {17};
static const int32_t MONS_Q19[] = {206};
static const int32_t MONS_Q23[] = {1};
static const int32_t MONS_Q40[] = {14, 15, 204, 205};
static const int32_t MONS_Q41[] = {12, 202};
static const int32_t MONS_Q42[] = {11, 12, 13, 14, 15, 16, 199, 201, 202, 203, 204, 205, 206};
static const int32_t MONS_Q43[] = {11, 201};
static const int32_t MONS_Q44[] = {13, 203};
static const int32_t MONS_Q45[] = {199};
static const int32_t MONS_Q46[] = {207};
static const int32_t MONS_Q47[] = {223};
static const int32_t MONS_Q55[] = {1, 105};
static const int32_t MONS_Q68[] = {103};
static const int32_t MONS_Q69[] = {388};
static const int32_t MONS_Q71[] = {17, 103};
static const int32_t MONS_Q85[] = {131, 139};
static const int32_t MONS_Q93[] = {186};
static const int32_t MONS_Q94[] = {185};
static const int32_t MONS_Q99[] = {183};
static const int32_t MONS_Q102[] = {184};
static const int32_t MONS_Q123[] = {190, 249};
static const int32_t MONS_Q124[] = {236};
static const int32_t MONS_Q132[] = {238};
static const int32_t MONS_Q138[] = {237};
static const int32_t MONS_Q139[] = {239};
static const int32_t MONS_Q142[] = {305};
static const int32_t MONS_Q144[] = {299};
static const int32_t MONS_Q148[] = {241, 278};
static const int32_t MONS_Q160[] = {196};
static const int32_t MONS_Q162[] = {149, 388};
static const int32_t MONS_Q168[] = {240};
static const int32_t MONS_Q171[] = {343, 344, 354};
static const int32_t MONS_Q185[] = {388, 389};
static const int32_t MONS_Q186[] = {263};
static const int32_t MONS_Q193[] = {161, 384};
static const int32_t MONS_Q194[] = {151};
static const int32_t MONS_Q195[] = {163}; /* "Sketchy Zard" - via:"none", found by hand */
static const int32_t MONS_Q196[] = {231, 232};
static const int32_t MONS_Q197[] = {228};
static const int32_t MONS_Q198[] = {1, 105, 149, 388};
static const int32_t MONS_Q199[] = {8};
static const int32_t MONS_Q205[] = {189};
static const int32_t MONS_Q208[] = {188};
static const int32_t MONS_Q209[] = {188, 189, 263};
static const int32_t MONS_Q210[] = {208};
static const int32_t MONS_Q211[] = {133};
static const int32_t MONS_Q212[] = {200};
static const int32_t MONS_Q222[] = {404};
static const int32_t MONS_Q227[] = {406};
static const int32_t MONS_Q228[] = {407, 413};
static const int32_t MONS_Q229[] = {405};
static const int32_t MONS_Q232[] = {407};
static const int32_t MONS_Q234[] = {70};
static const int32_t MONS_Q237[] = {1, 7, 105}; /* "Zardman Spearman Defeated" - via:"none",
   found by hand (Zardman Spear=7); 1/105 from this quest's other, already-mapped objective */
static const int32_t MONS_Q238[] = {412};
static const int32_t MONS_Q240[] = {153}; /* "BaconZard Rescued?" -> Bacon Zard, found by hand */
static const int32_t MONS_Q242[] = {415, 421, 422}; /* "Find the Lucky Zard" - 3 named variants
   (West/North/East), found by hand; any should satisfy it */
static const int32_t MONS_Q243[] = {414};
static const int32_t MONS_Q244[] = {423};

static const QuestMonHint QUEST_MON_HINTS[] = {
    {1, MONS_Q1, 3}, {11, MONS_Q11, 1}, {14, MONS_Q14, 1}, {19, MONS_Q19, 1},
    {20, MONS_Q14, 1}, {23, MONS_Q23, 1}, {24, MONS_Q14, 1}, {40, MONS_Q40, 4},
    {41, MONS_Q41, 2}, {42, MONS_Q42, 13}, {43, MONS_Q43, 2}, {44, MONS_Q44, 2},
    {45, MONS_Q45, 1}, {46, MONS_Q46, 1}, {47, MONS_Q47, 1}, {52, MONS_Q14, 1},
    {55, MONS_Q55, 2}, {68, MONS_Q68, 1}, {69, MONS_Q69, 1}, {71, MONS_Q71, 2},
    {85, MONS_Q85, 2}, {93, MONS_Q93, 1}, {94, MONS_Q94, 1}, {95, MONS_Q14, 1},
    {98, MONS_Q93, 1}, {99, MONS_Q99, 1}, {100, MONS_Q94, 1}, {102, MONS_Q102, 1},
    {123, MONS_Q123, 2}, {124, MONS_Q124, 1}, {127, MONS_Q124, 1}, {132, MONS_Q132, 1},
    {135, MONS_Q123, 2}, {138, MONS_Q138, 1}, {139, MONS_Q139, 1}, {142, MONS_Q142, 1},
    {144, MONS_Q144, 1}, {148, MONS_Q148, 2}, {157, MONS_Q123, 2}, {160, MONS_Q160, 1},
    {162, MONS_Q162, 2}, {168, MONS_Q168, 1}, {169, MONS_Q148, 2}, {171, MONS_Q171, 3},
    {185, MONS_Q185, 2}, {186, MONS_Q186, 1}, {190, MONS_Q171, 3}, {193, MONS_Q193, 2},
    {194, MONS_Q194, 1}, {195, MONS_Q195, 1}, {196, MONS_Q196, 2}, {197, MONS_Q197, 1},
    {198, MONS_Q198, 4}, {199, MONS_Q199, 1}, {205, MONS_Q205, 1}, {207, MONS_Q186, 1},
    {208, MONS_Q208, 1}, {209, MONS_Q209, 3}, {210, MONS_Q210, 1}, {211, MONS_Q211, 1},
    {212, MONS_Q212, 1}, {221, MONS_Q99, 1}, {222, MONS_Q222, 1}, {227, MONS_Q227, 1},
    {228, MONS_Q228, 2}, {229, MONS_Q229, 1}, {232, MONS_Q232, 1}, {234, MONS_Q234, 1},
    {235, MONS_Q234, 1}, {236, MONS_Q69, 1}, {237, MONS_Q237, 3}, {238, MONS_Q238, 1},
    {239, MONS_Q237, 3}, {240, MONS_Q240, 1}, {241, MONS_Q55, 2}, {242, MONS_Q242, 3},
    {243, MONS_Q243, 1}, {244, MONS_Q244, 1},
};
#define QUEST_MON_HINT_COUNT ((int)(sizeof(QUEST_MON_HINTS) / sizeof(QUEST_MON_HINTS[0])))

static bool quest_mon_hint(int32_t qid, const int32_t **out_mons, int *out_count)
{
    for (int i = 0; i < QUEST_MON_HINT_COUNT; i++) {
        if (QUEST_MON_HINTS[i].qid == qid) {
            *out_mons = QUEST_MON_HINTS[i].mons;
            *out_count = QUEST_MON_HINTS[i].count;
            return true;
        }
    }
    return false;
}

/* Per-quest turn-in location, baked in from the same questdb source as
   QUEST_MON_HINTS - mirrors QuestRunner.AtTurnInLocation. Every entry here
   turns in on the SAME MAP the quest is hunted on (confirmed against the DB
   for all four chains), just not always the same FRAME - e.g. quest 20 hunts
   in Enter but turns in at R3, quest 59 hunts at R8 but turns in back at
   Enter. Without this, RequestTryQuestComplete fired from wherever combat
   happened to end, which works by accident when hunt and turn-in frames
   match and silently no-ops (or worse, looks like a kick-worthy location
   mismatch to live AE) when they don't. */
typedef struct {
    int32_t qid;
    const char *frame;
    const char *pad;
} QuestTurnin;

/* Every quest in server/db.py's questdb (180 entries), not just the 4 named
   chains - same source/generation as QUEST_MON_HINTS above. turnInMap never
   differs from the hunt map for any quest in our DB, only turnInFrame/Pad
   sometimes do, which is why this table (like the RequestMoveToCell it
   feeds) only ever needs a frame+pad, never a full-area transfer. */
static const QuestTurnin QUEST_TURNINS[] = {
    {1, "Enter", "Spawn"},       {2, "Enter", "Spawn"},        {4, "Enter", "Spawn"},
    {5, "R4", "Spawn"},          {6, "R4", "Spawn"},           {7, "R4", "Spawn"},
    {8, "Enter", "Spawn"},       {9, "Enter", "Spawn"},        {10, "Enter", "Spawn"},
    {11, "Enter", "Spawn"},      {12, "Enter", "Spawn"},       {13, "Enter", "Spawn"},
    {14, "Enter", "Spawn"},      {15, "Enter", "Spawn"},       {16, "Enter", "Spawn"},
    {17, "Enter", "Spawn"},      {18, "Enter", "Spawn"},       {19, "Enter", "Down"},
    {20, "R3", "Spawn"},         {21, "Enter", "Spawn"},       {22, "Enter", "Spawn"},
    {23, "Enter", "Spawn"},      {24, "Enter", "Spawn"},       {40, "R4", "Spawn"},
    {41, "R5", "Spawn"},         {42, "R4", "Spawn"},          {43, "R4", "Spawn"},
    {44, "R4", "Spawn"},         {45, "R6", "Spawn"},          {46, "R6", "Spawn"},
    {47, "R7", "Spawn"},         {49, "Enter", "Spawn"},       {50, "Enter", "Spawn"},
    {51, "Enter", "Spawn"},      {52, "Enter", "Spawn"},       {55, "Enter", "Spawn"},
    {56, "16", "17"},            {57, "Enter", "Spawn"},       {58, "Enter", "Spawn"},
    {59, "Enter", "Spawn"},      {63, "Enter", "Spawn"},       {64, "Enter", "Spawn"},
    {65, "Enter", "Spawn"},      {66, "Enter", "Spawn"},       {67, "Enter", "Spawn"},
    {68, "Enter", "Spawn"},      {69, "Enter", "Spawn"},       {70, "Enter", "Spawn"},
    {71, "Enter", "Spawn"},      {72, "Enter", "Spawn"},       {85, "R12", "Spawn"},
    {93, "Enter", "Spawn"},      {94, "Enter", "Spawn"},       {95, "Enter", "Spawn"},
    {96, "Enter", "Spawn"},      {97, "Enter", "Spawn"},       {98, "Enter", "Spawn"},
    {99, "Enter", "Spawn"},      {100, "Enter", "Spawn"},      {101, "Enter", "Spawn"},
    {102, "Enter", "Spawn"},     {103, "Enter", "Spawn"},      {104, "Enter", "Spawn"},
    {105, "Enter", "Spawn"},     {106, "Enter", "Spawn"},      {107, "Enter", "Spawn"},
    {108, "R10", "Spawn"},       {109, "R2", "Spawn"},         {118, "Enter", "Spawn"},
    {119, "R2", "Down"},         {120, "Enter", "Spawn"},      {121, "R2", "Down"},
    {122, "R2", "Down"},         {123, "R2", "Down"},          {124, "R2", "Down"},
    {125, "R2", "Down"},         {127, "R15", "Up"},           {128, "Enter", "Spawn"},
    {129, "Enter", "Spawn"},     {130, "Enter", "Spawn"},      {132, "R16", "Left"},
    {133, "Enter", "Spawn"},     {134, "Enter", "Spawn"},      {135, "Enter", "Spawn"},
    {136, "R17", "Spawn"},       {137, "R14", "Spawn"},        {138, "R14", "Spawn"},
    {139, "Enter", "Spawn"},     {140, "Enter", "Spawn"},      {142, "R17", "Spawn"},
    {143, "R17-empty", "Spawn"}, {144, "R12-empty", "Spawn"},  {146, "R12-empty", "Spawn"},
    {148, "R16-empty", "Spawn"}, {149, "R2", "Spawn"},         {150, "Enter", "Spawn"},
    {151, "Enter", "Down"},      {152, "Enter", "Spawn"},      {153, "Enter", "Spawn"},
    {154, "Enter", "Spawn"},     {156, "R15", "Spawn"},        {157, "Enter", "Spawn"},
    {158, "Enter", "Down"},      {159, "R2", "Down"},          {160, "R2", "Down"},
    {161, "Enter", "Spawn"},     {162, "Enter", "Maya"},       {163, "R16", "Down"},
    {164, "R15", "Down"},        {166, "R8", "Spawn"},         {167, "Enter", "Spawn"},
    {168, "Enter", "Spawn"},     {169, "Enter", "Spawn"},      {170, "R6", "Spawn"},
    {171, "R6", "Up"},           {172, "R4", "Right"},         {173, "R5", "Spawn"},
    {174, "R4", "Right"},        {175, "PrincessFight", "Spawn"}, {177, "PrincessFight", "Spawn"},
    {178, "R5", "Spawn"},        {179, "R2", "Right"},         {182, "Enter", "Spawn"},
    {183, "Enter", "Spawn"},     {184, "Enter", "Spawn"},      {185, "Enter", "Spawn"},
    {186, "Enter", "spawn"},     {187, "Enter", "Spawn"},      {188, "Enter", "Spawn"},
    {189, "Enter", "Spawn"},     {190, "Enter", "Spawn"},      {191, "Enter", "Spawn"},
    {192, "Enter", "Spawn"},     {193, "Enter", "Spawn"},      {194, "Enter", "Spawn"},
    {195, "Enter", "Spawn"},     {196, "Enter", "Spawn"},      {197, "Enter", "Spawn"},
    {198, "Enter", "Right"},     {199, "Enter", "Spawn"},      {203, "Enter", "spawn"},
    {204, "Enter", "spawn"},     {205, "Enter", "spawn"},      {207, "Enter", "spawn"},
    {208, "Enter", "spawn"},     {209, "Enter", "spawn"},      {210, "Enter", "spawn"},
    {211, "Enter", "spawn"},     {212, "Enter", "spawn"},      {213, "Enter", "Spawn"},
    {214, "Enter", "Spawn"},     {215, "Enter", "Spawn"},      {216, "Enter", "Spawn"},
    {217, "Enter", "Spawn"},     {218, "Enter", "Spawn"},      {220, "Enter", "spawn"},
    {221, "Enter", "spawn"},     {222, "Enter", "spawn"},      {223, "Enter", "spawn"},
    {225, "Enter", "spawn"},     {226, "Enter", "spawn"},      {227, "Enter", "spawn"},
    {228, "Enter", "spawn"},     {229, "Enter", "spawn"},      {230, "Enter", "spawn"},
    {232, "Enter", "spawn"},     {233, "Enter", "spawn"},      {234, "R13-Petshop", "right"},
    {235, "R13-Petshop", "right"}, {236, "Enter", "spawn"},    {237, "Enter", "spawn"},
    {238, "Enter", "spawn"},     {239, "Enter", "spawn"},      {240, "Enter", "spawn"},
    {241, "Enter", "spawn"},     {242, "Enter", "spawn"},      {243, "Enter", "spawn"},
    {244, "Enter", "spawn"},     {6942, "Enter", "spawn"},     {6943, "Enter", "spawn"},
};
#define QUEST_TURNIN_COUNT ((int)(sizeof(QUEST_TURNINS) / sizeof(QUEST_TURNINS[0])))

static const QuestTurnin *quest_turnin_loc(int32_t qid)
{
    for (int i = 0; i < QUEST_TURNIN_COUNT; i++) {
        if (QUEST_TURNINS[i].qid == qid) {
            return &QUEST_TURNINS[i];
        }
    }
    return NULL;
}

/* True when `mon` is something the current objective actually wants killed.

   PERMISSIVE UNION of two signals, mirroring QuestRunner.MatchesTarget:
   the baked per-quest hint table above (works even when RefArray is empty,
   which on live AE it always is for these quests), and RefArray MonIDs
   directly (in case some quest somewhere DOES ship them - costs nothing to
   still check). Only when NEITHER offers anything does any hostile qualify -
   an unmapped quest should still let the bot grind the room rather than
   stall dead. */
static bool matches_objective(void *mon, void *obj, int32_t qotype, int32_t qid)
{
    if (obj == NULL || qotype != 1 /* Killcount */ || g_entity_get_id == NULL) {
        return true; /* no signal to filter on - any hostile is fair game */
    }
    int32_t mon_id = inv_int(g_entity_get_id, mon, NULL);
    bool has_signal = false;

    const int32_t *hint_mons = NULL;
    int hint_count = 0;
    if (quest_mon_hint(qid, &hint_mons, &hint_count)) {
        has_signal = true;
        for (int i = 0; i < hint_count; i++) {
            if (hint_mons[i] == mon_id) {
                return true;
            }
        }
    }

    if (g_qti_getrefint_method != NULL) {
        for (int32_t i = 0; i < 8; i++) {
            void *ref_args[1] = {&i};
            int32_t want = inv_int(g_qti_getrefint_method, obj, ref_args);
            if (want <= 0) {
                continue; /* -1/0 = absent or non-numeric ref; keep scanning */
            }
            has_signal = true;
            if (want == mon_id) {
                return true;
            }
        }
    }

    return !has_signal;
}

static void *find_nearest_hostile(void *player, const float me[3], void *obj, int32_t qotype,
                                   int32_t qid)
{
    if (g_area_currentarea_field == NULL || g_area_monsters_field == NULL ||
        g_monster_reaction_field == NULL || !il2cpp_field_static_get_value) {
        return NULL;
    }
    void *area = NULL;
    il2cpp_field_static_get_value(g_area_currentarea_field, &area);
    if (area == NULL) {
        return NULL;
    }
    void *dict = NULL;
    il2cpp_field_get_value(area, g_area_monsters_field, &dict);
    if (dict == NULL) {
        return NULL;
    }
    void *dict_class = il2cpp_object_get_class(dict);
    EnumWalk w;
    if (!enum_open(dict, il2cpp_class_get_method_from_name(dict_class, "GetEnumerator", 0), &w)) {
        return NULL;
    }

    /* Only consider monsters standing in the player's own cell. The game
       spawns every cell's monsters into one Monsters dict, so without this
       the "nearest" hostile can be one in a completely different room that
       happens to sit close in world coordinates - unreachable, and it stalls
       the hunt on a target that can never be engaged. Compared case-
       insensitively on purpose: the game's own GetMonstersInFrame does an
       ordinal compare and consequently misses monsters whose Frame casing
       differs from the player's. */
    char my_frame[40] = "";
    if (g_entity_frame_field != NULL) {
        void *fs = NULL;
        il2cpp_field_get_value(player, g_entity_frame_field, &fs);
        mstr_to_utf8(fs, my_frame, sizeof(my_frame));
    }

    void *best = NULL;
    float best_dist2 = 0.0f;
    void *current = g_entity_get_target ? inv(g_entity_get_target, player, NULL) : NULL;
    bool current_ok = false;

    for (int i = 0; i < 500 && inv_bool(w.move_next, w.self, NULL); i++) {
        void *boxed_kv = inv(w.get_current, w.self, NULL);
        void *kv_self = self_ptr(boxed_kv);
        if (kv_self == NULL) {
            continue;
        }
        void *kv_class = il2cpp_object_get_class(boxed_kv);
        void *get_value = il2cpp_class_get_method_from_name(kv_class, "get_Value", 0);
        void *mon = get_value ? inv(get_value, kv_self, NULL) : NULL;
        if (mon == NULL || mon == player) {
            continue;
        }
        int32_t reaction = 0;
        il2cpp_field_get_value(mon, g_monster_reaction_field, &reaction);
        if (reaction != 1) { /* not Hostile */
            continue;
        }
        if (g_entity_get_currentstate != NULL &&
            inv_int(g_entity_get_currentstate, mon, NULL) == 0) { /* State.Dead */
            continue;
        }
        if (my_frame[0] != '\0' && g_entity_frame_field != NULL) {
            char mf[40] = "";
            void *fs = NULL;
            il2cpp_field_get_value(mon, g_entity_frame_field, &fs);
            mstr_to_utf8(fs, mf, sizeof(mf));
            if (mf[0] != '\0' && strcasecmp(mf, my_frame) != 0) {
                continue;
            }
        }
        if (!matches_objective(mon, obj, qotype, qid)) {
            continue;
        }
        /* Sticky targeting: if what we are already fighting still qualifies,
           keep it. Re-picking purely by distance every 0.3s makes the bot
           oscillate between two equally-close mobs and land almost no hits -
           the desktop agent hit this exact failure against the pair of water
           draconians that flank the player on Lair. */
        if (mon == current) {
            current_ok = true;
        }
        float pos[3];
        if (!read_local_pos(mon, pos)) {
            continue;
        }
        float dx = pos[0] - me[0], dy = pos[1] - me[1];
        float d2 = dx * dx + dy * dy;
        if (best == NULL || d2 < best_dist2) {
            best = mon;
            best_dist2 = d2;
        }
    }
    return current_ok ? current : best;
}

/* The frame (map-wide, every cell - not just the player's own) holding the
   most matching hostiles for the given objective. Mirrors
   QuestRunner.FindHostileFrame: Area.currentArea.Monsters carries every
   monster in the WHOLE MAP regardless of which cell is currently active, so
   this is a pure lookup, not a scan-as-you-walk. Needed because a chain's
   fixed entry frame does not always hold the objective's target - Lair's
   Wyverns (quest 20) live in R2/R3 while the chain parks the player in
   Enter, which is full of Water Draconians (quest 19's target). Returns
   false if nothing anywhere matches. */
static bool find_hostile_frame(void *obj, int32_t qotype, int32_t qid, char *out_frame,
                                size_t out_cap)
{
    out_frame[0] = '\0';
    if (g_area_currentarea_field == NULL || g_area_monsters_field == NULL ||
        g_monster_reaction_field == NULL || g_entity_frame_field == NULL ||
        !il2cpp_field_static_get_value) {
        return false;
    }
    void *area = NULL;
    il2cpp_field_static_get_value(g_area_currentarea_field, &area);
    if (area == NULL) {
        return false;
    }
    void *dict = NULL;
    il2cpp_field_get_value(area, g_area_monsters_field, &dict);
    if (dict == NULL) {
        return false;
    }
    void *dict_class = il2cpp_object_get_class(dict);
    EnumWalk w;
    if (!enum_open(dict, il2cpp_class_get_method_from_name(dict_class, "GetEnumerator", 0), &w)) {
        return false;
    }

    /* Tally match counts per frame (case-insensitive), same as the desktop's
       byFrame dictionary, so a frame with 3 Wyverns wins over one with 1. A
       small fixed table beats pulling in a hash map for at most a
       handful of distinct frames per map. */
    char frames[16][40];
    int counts[16];
    int nframes = 0;

    for (int i = 0; i < 500 && inv_bool(w.move_next, w.self, NULL); i++) {
        void *boxed_kv = inv(w.get_current, w.self, NULL);
        void *kv_self = self_ptr(boxed_kv);
        if (kv_self == NULL) {
            continue;
        }
        void *kv_class = il2cpp_object_get_class(boxed_kv);
        void *get_value = il2cpp_class_get_method_from_name(kv_class, "get_Value", 0);
        void *mon = get_value ? inv(get_value, kv_self, NULL) : NULL;
        if (mon == NULL) {
            continue;
        }
        int32_t reaction = 0;
        il2cpp_field_get_value(mon, g_monster_reaction_field, &reaction);
        if (reaction != 1) {
            continue;
        }
        if (g_entity_get_currentstate != NULL &&
            inv_int(g_entity_get_currentstate, mon, NULL) == 0) {
            continue;
        }
        if (!matches_objective(mon, obj, qotype, qid)) {
            continue;
        }
        char mf[40] = "";
        void *fs = NULL;
        il2cpp_field_get_value(mon, g_entity_frame_field, &fs);
        mstr_to_utf8(fs, mf, sizeof(mf));
        if (mf[0] == '\0') {
            continue;
        }
        int slot = -1;
        for (int j = 0; j < nframes; j++) {
            if (strcasecmp(frames[j], mf) == 0) {
                slot = j;
                break;
            }
        }
        if (slot < 0 && nframes < 16) {
            slot = nframes++;
            snprintf(frames[slot], sizeof(frames[slot]), "%s", mf);
            counts[slot] = 0;
        }
        if (slot >= 0) {
            counts[slot]++;
        }
    }

    int best = -1;
    for (int j = 0; j < nframes; j++) {
        if (best < 0 || counts[j] > counts[best]) {
            best = j;
        }
    }
    if (best < 0) {
        return false;
    }
    snprintf(out_frame, out_cap, "%s", frames[best]);
    return true;
}

/* approach_target's give-up clock: how long we have been trying to reach the
   CURRENT target without success. Reset whenever the target moves (a new
   machine/NPC, or the same one at a materially different spot) so switching
   targets does not inherit a stale clock. */
static float g_approach_target_last[3] = {1e9f, 1e9f, 1e9f};
static float g_approach_started_at;
#define APPROACH_GIVEUP_SEC 1.5f /* was 4.0 - a target the pathing genuinely cannot reach (e.g. a
                                     DSPiece across a lava pit the A* grid correctly refuses to
                                     route through) fails on every attempt regardless of how long
                                     we wait, so a shorter timeout costs nothing there and just
                                     gets to the "click anyway" fallback faster; a real walk at
                                     ~14 units/sec covers far more ground than any of these rooms
                                     need in 1.5s anyway */

/* Wall-aware approach shared by combat engage (hunt_tick) and machine/NPC
   interact (quest_tick's Interact/Apop branches). Returns true once `me` is
   within reach_dist of `target` - the caller clicks on true, keeps waiting
   on false. Before this, Interact/Apop clicked the moment a machine/NPC was
   FOUND, with no regard for whether the player was anywhere near it - PC
   requires <=2.5 units and a clear line before clicking, and paths there
   (same A* used for combat) if not.

   PC's other half of this - clicking anyway once PathWalker.Failed, since "a
   machine click needs no proximity" - has no clean equivalent here: path_tick
   folds "no route at all", "stalled twice", and "reached the goal" into the
   same false return, so the caller cannot tell failure from arrival. A
   give-up timer sidesteps that distinction entirely and generalizes better
   anyway: whatever the actual cause (an interactable embedded in geometry
   the A* grid cannot reach, a planning bug, bad terrain), 4 seconds of not
   converging on a STATIONARY target means it is not going to converge.
   Without this, a DSPiece embedded in a wall walked the character into it
   forever - visually "just approaching", reported as looking like the
   character was clipping against something it should have been able to
   reach ("can we turn on no clip?"). */
static bool approach_target(void *player, const float me[3], const float target[3],
                            float reach_dist)
{
    float dx = target[0] - me[0], dy = target[1] - me[1];
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist <= reach_dist) {
        g_path_active = 0; /* in reach - no route left to maintain */
        g_approach_started_at = 0.0f;
        return true;
    }

    float now = inv_float(g_time_get_time, NULL, NULL);
    float tdx = target[0] - g_approach_target_last[0], tdy = target[1] - g_approach_target_last[1];
    if (tdx * tdx + tdy * tdy > 1.0f) {
        /* New target (or this one moved meaningfully) - fresh clock. */
        memcpy(g_approach_target_last, target, sizeof(g_approach_target_last));
        g_approach_started_at = now;
    } else if (g_approach_started_at <= 0.0f) {
        g_approach_started_at = now;
    } else if (now - g_approach_started_at > APPROACH_GIVEUP_SEC) {
        LOGI("hunt: approach gave up after %.1fs (dist=%.1f) - interacting anyway", now -
             g_approach_started_at, dist);
        g_approach_started_at = 0.0f;
        return true;
    }

    if (g_emu_type_obj == NULL || g_emu_walkto == NULL) {
        return false; /* can't walk - let the caller decide (click anyway, or wait) */
    }
    void *player_go = inv(g_entity_getgameobject, player, NULL);
    void *emu = get_component(player_go, g_emu_type_obj);
    if (emu == NULL) {
        return false;
    }
    float speed = 14.0f;
    if (g_emu_cellspeed_field != NULL && il2cpp_field_static_get_value) {
        int32_t cs = 14;
        il2cpp_field_static_get_value(g_emu_cellspeed_field, &cs);
        speed = (float)cs;
    }
    void *parent = path_player_parent(player_go);
    bool clear = parent != NULL && path_line_clear_local(parent, me, target);
    if (clear) {
        g_path_active = 0;
        void *args[2] = {(void *)target, &speed};
        inv(g_emu_walkto, emu, args);
    } else {
        bool done = false;
        bool navigating = parent != NULL && path_tick(parent, emu, target, speed, &done);
        if (!navigating) {
            void *args[2] = {(void *)target, &speed};
            inv(g_emu_walkto, emu, args);
        }
    }
    return false;
}

static void hunt_tick(void)
{
    float now0 = inv_float(g_time_get_time, NULL, NULL);

    /* Interact/Apop approach, published by quest_tick's Interact/Talk/Apop
       branches. This runs regardless of g_hunt (which those branches set to
       0 - a machine click needs the player still, not chasing a mob) and at
       hunt_tick's own ~3Hz cadence, NOT quest_tick's 1Hz decision cadence.
       Calling approach_target from inside quest_tick directly re-issued the
       walk only once a second, which is far too sparse to sustain the
       continuous movement combat's own approach relies on at this same
       cadence - the character advanced in tiny, second-apart nudges that
       looked and reported as "stuck approaching" even though the logic was
       otherwise correct. quest_tick still decides WHAT to approach and
       fires the actual click; this just keeps the walk moving in between. */
    if (g_interact_approach_active && now0 >= g_next_interact_approach) {
        g_next_interact_approach = now0 + 0.3f;
        void *iplayer = g_get_main_player ? inv(g_get_main_player, NULL, NULL) : NULL;
        float ime[3];
        if (iplayer != NULL && read_local_pos(iplayer, ime)) {
            g_interact_ready = approach_target(iplayer, ime, g_interact_approach_target,
                                               g_interact_reach_dist_pub)
                                  ? 1
                                  : 0;
        }
    }

    if (!g_hunt || g_get_main_player == NULL) {
        return;
    }
    float now = now0;
    if (now < g_next_hunt) {
        return;
    }
    g_next_hunt = now + 0.3f;

    void *player = inv(g_get_main_player, NULL, NULL);
    if (player == NULL) {
        return;
    }

    /* Death/respawn: mirrors QuestRunner.PlayerIsDead()/TickRespawning().
       Targeting or walking while dead is nonsensical - the click has nothing
       to act on and the walk goes nowhere - so pause outright rather than
       let find_nearest_hostile spin on it. On revive, give the game a beat
       to actually place the character (same 1.5s the desktop agent waits)
       before resuming: position/area reads immediately after a respawn can
       still reflect the pre-death state for a frame or two. */
    bool dead = g_entity_get_currentstate != NULL &&
                inv_int(g_entity_get_currentstate, player, NULL) == 0; /* State.Dead */
    if (dead) {
        g_hunt_settle_until = now + 1.5f;
        return;
    }
    if (now < g_hunt_settle_until) {
        return;
    }

    float me[3];
    if (!read_local_pos(player, me)) {
        return;
    }

    void *tgt = find_nearest_hostile(player, me, g_hunt_obj, g_hunt_qotype, g_hunt_qid);
    if (tgt == NULL) {
        /* No live target to engage this tick - mirrors QuestRunner calling
           StopAutoskills() as soon as PickBestHostile returns null, whether
           that's because we're mid cross-cell travel or nothing anywhere
           matches yet. Re-armed below once engage actually happens. */
        g_autoskills = 0;
    }
    if (tgt == NULL && g_hunt_obj != NULL) {
        /* Nothing matching in THIS cell, but the objective does name a real
           target (Killcount with a hint/RefArray hit) - search the whole map
           before giving up on it. Dropping the filter here (old behavior)
           defeated the entire point: Lair's chain always parks the player in
           Enter, but quest 20's Wyverns live in R2/R3, so "any hostile
           nearby" just meant fighting whatever quest 19 wanted instead. */
        char here[40] = "";
        if (g_entity_frame_field != NULL) {
            void *fs = NULL;
            il2cpp_field_get_value(player, g_entity_frame_field, &fs);
            mstr_to_utf8(fs, here, sizeof(here));
        }
        char want_frame[40];
        if (find_hostile_frame(g_hunt_obj, g_hunt_qotype, g_hunt_qid, want_frame,
                                sizeof(want_frame)) &&
            strcasecmp(want_frame, here) != 0) {
            bool need_send = strcasecmp(g_hunt_nav_frame, want_frame) != 0 ||
                              (now - g_hunt_nav_sent_at > 8.0f); /* matches QuestRunner.NavResendSec */
            if (need_send && g_req_movecell_class != NULL && g_req_movecell_ctor != NULL &&
                il2cpp_object_new && il2cpp_string_new && g_send_request != NULL) {
                void *req = il2cpp_object_new(g_req_movecell_class);
                if (req != NULL) {
                    void *ctor_args[2] = {il2cpp_string_new(want_frame),
                                          il2cpp_string_new("Spawn")};
                    inv(g_req_movecell_ctor, req, ctor_args);
                    void *send_args[1] = {req};
                    inv(g_send_request, g_aec_instance, send_args);
                    LOGI("hunt: nothing matching in '%s' - moving to '%s' for quest %d", here,
                         want_frame, g_hunt_qid);
                }
                snprintf(g_hunt_nav_frame, sizeof(g_hunt_nav_frame), "%s", want_frame);
                g_hunt_nav_sent_at = now;
            }
            return; /* wait for the cell to load before engaging anything */
        }
        /* Nowhere on the map has a match either - the hint/RefArray may just
           not cover this quest. Fall back to any hostile rather than
           stalling forever. */
        tgt = find_nearest_hostile(player, me, NULL, -1, -1);
    }
    if (tgt == NULL) {
        return; /* nothing hostile loaded - wait for one to spawn/appear */
    }

    float tp[3];
    if (!read_local_pos(tgt, tp)) {
        return;
    }
    float dx = tp[0] - me[0], dy = tp[1] - me[1];
    float dist = sqrtf(dx * dx + dy * dy);

    /* Engage: mirrors a player click on the target - first ClickMe() assigns
       target, second fires the charge. Only re-issue when the target actually
       changed; the setter no-ops on an unchanged/dead value anyway, but
       skipping the two managed calls entirely when nothing changed is cheap
       and avoids re-triggering the charge animation every 0.3s. */
    void *current_target = g_entity_get_target ? inv(g_entity_get_target, player, NULL) : NULL;
    if (current_target != tgt) {
        int32_t tgt_id = g_entity_get_id ? inv_int(g_entity_get_id, tgt, NULL) : -999;
        char tgt_name[40] = "";
        if (g_entity_get_name != NULL) {
            mstr_to_utf8(inv(g_entity_get_name, tgt, NULL), tgt_name, sizeof(tgt_name));
        }
        LOGI("hunt: retarget -> id=%d name='%s' (hunt_qid=%d hunt_qotype=%d hunt_obj=%p)",
             tgt_id, tgt_name, g_hunt_qid, g_hunt_qotype, g_hunt_obj);
    }
    if (current_target != tgt && g_targetable_type_obj != NULL) {
        void *tgt_go = inv(g_entity_getgameobject, tgt, NULL);
        void *targetable = get_component(tgt_go, g_targetable_type_obj);
        if (targetable != NULL && g_targetable_clickme != NULL) {
            inv(g_targetable_clickme, targetable, NULL);
            inv(g_targetable_clickme, targetable, NULL);
        }
    }

    if (dist > g_max_engage_dist && g_emu_type_obj != NULL && g_emu_walkto != NULL) {
        void *player_go = inv(g_entity_getgameobject, player, NULL);
        void *emu = get_component(player_go, g_emu_type_obj);
        if (emu != NULL) {
            float speed = 14.0f;
            if (g_emu_cellspeed_field != NULL && il2cpp_field_static_get_value) {
                int32_t cs = 14;
                il2cpp_field_static_get_value(g_emu_cellspeed_field, &cs);
                speed = (float)cs;
            }

            /* Open ground is the common case and cheap to confirm - one
               raycast beats planning a route we do not need. Blocked or no
               parent transform (fresh spawn, still settling): fall to the
               A* route, and if THAT fails too (grid too big, unreachable,
               stalled twice), a direct walk at least tries - the game's own
               blockedMoveTimer stops it rather than looping forever, same
               failure mode hunting already had before this feature existed. */
            void *parent = path_player_parent(player_go);
            bool clear = parent != NULL && path_line_clear_local(parent, me, tp);
            if (clear) {
                if (g_path_active) {
                    LOGI("path: line to target opened up - dropping route, walking direct");
                }
                g_path_active = 0; /* drop any stale route once the line is open */
                void *args[2] = {tp, &speed};
                inv(g_emu_walkto, emu, args);
            } else {
                if (!g_path_active) {
                    LOGI("path: direct line blocked - planning a route");
                }
                bool done = false;
                bool navigating = parent != NULL && path_tick(parent, emu, tp, speed, &done);
                if (!navigating) {
                    LOGI("path: no route available - falling back to a direct walk anyway");
                    void *args[2] = {tp, &speed};
                    inv(g_emu_walkto, emu, args);
                }
            }
        }
    } else {
        g_path_active = 0; /* in engage range - no route left to maintain */
    }

    /* Combat is autoskills' job; hunting just keeps a live target in range. */
    g_autoskills = 1;
}

/* Nameplate spoof.
   RefreshNameplate() is a dead end: it looks for a TextMeshProUGUI on the
   nameplate root, but the root carries a NameplateView and the text lives on
   children, so it finds nothing and silently does nothing. The live path is
   Player.nameTagView.SetName()/SetTitle(), which is what createNameTag itself
   calls. */
static void *g_nametagview_field;
static void *g_view_set_name;
static void *g_view_set_title;
static void *g_view_set_title_visible;
static void *g_get_name;
static void *g_name_field;
static void *g_title_field;
static char g_title[40];
static float g_next_spoof;
/* The nameplate is only one of several places the name is drawn. The top-left
   HUD panel rewrites it from target.Name every Update, so it needs its own
   hook rather than a one-shot write. */
static void *g_panel_target_field;
static void *g_panel_nametext_field;
static void *g_tmp_set_text;
static void *g_spoof_str; /* GC handle: cached managed spoof name */

/* Every command the client can send, read off the Request subclasses in the
   decompiled Assembly-CSharp. Tapping one loads it into the send box, which
   beats having to remember the wire names. */
static const char *const CMDS[] = {
    "acceptQuest", "tryQuestComplete", "qabandon", "trackQuest", "getQuests",
    "loadShop", "buyItem", "sellItem", "loadHairShop", "getItemSponsors",
    "loadBank", "bankToInv", "bankFromInv", "bankSwapInv", "bulkOperation",
    "equipItem", "unequipItem", "getEquip", "equipEnh", "removeEnh",
    "gar", "gai", "gas", "gimp", "getClassSkills",
    "moveToCell", "tfer", "mv", "mvtgt", "stopWalk",
    "message", "emotea", "inspectPlayer", "itemQuery", "getPlayerTitles",
    "savePlayerTitle", "getDrop", "discardDrop", "getenhloot", "dustenhloot",
    "getCutscene", "watchCutscene", "getDialog", "getApop", "openApopQO",
    "house", "housesave", "savePortrait", "savePrefs", "firstJoin",
    "spawnMob", "spawnMapMob", "tKill", "resPlayerTimed", "machineInteract",
    "mapCapture", "generateStatue", "equipPattern", "dustPattern", "useSpellstone",
    "startCharge", "cancelCharge", "upgradeSync", "resetsaga", "removeItem",
    "hitboxes", "cmd", "c",
};
#define CMD_COUNT ((int)(sizeof(CMDS) / sizeof(CMDS[0])))
#define HELP_ROWS 10

/* Rect is a value type, so runtime_invoke wants a pointer to the raw floats. */
static void gui_text(void *method, float x, float y, float w, float h, const char *text)
{
    if (method == NULL || !il2cpp_string_new) {
        return;
    }
    float r[4] = {x, y, w, h};
    void *args[2] = {r, il2cpp_string_new(text)};
    inv(method, NULL, args);
}

/* GUI.Button(Rect, string) was stripped; only the GUIContent+GUIStyle overload
   survives, so wrap the text and borrow the skin's button style. */
static bool gui_button(float x, float y, float w, float h, const char *text)
{
    if (g_gui_button == NULL || g_content_ctor == NULL || g_gui_get_skin == NULL ||
        g_skin_get_button == NULL || !il2cpp_object_new) {
        return false;
    }
    /* GUI.skin only exists inside a GUI context, and the style object it hands
       back is owned by the skin - so read it per call rather than caching a GC
       handle to it. */
    void *skin = inv(g_gui_get_skin, NULL, NULL);
    void *style = skin ? inv(g_skin_get_button, skin, NULL) : NULL;
    if (style == NULL) {
        return false;
    }
    void *content = il2cpp_object_new(g_content_class);
    if (content == NULL) {
        return false;
    }
    void *ctor_args[1] = {il2cpp_string_new(text)};
    inv(g_content_ctor, content, ctor_args);

    float r[4] = {x, y, w, h};
    void *args[3] = {r, content, style};
    return inv_bool(g_gui_button, NULL, args);
}

/* Opens the OS keyboard, seeded with whichever buffer we are editing. */
static void keyboard_open(int target)
{
    if (g_tsk_open == NULL) {
        LOGE("keyboard: TouchScreenKeyboard.Open unavailable");
        return;
    }
    g_kb_target = target;
    const char *seed = target == KB_SPOOF ? g_spoof : (target == KB_TITLE ? g_title : g_input);
    int32_t kb_type = 0; /* TouchScreenKeyboardType.Default */
    uint8_t no = 0;
    void *args[5] = {il2cpp_string_new(seed), &kb_type, &no, &no, &no};
    void *kb = inv(g_tsk_open, NULL, args);
    if (kb != NULL) {
        g_kb = mstr_hold(g_kb, kb);
    }
}

static void apply_spoof(int verbose);

/* Mirrors the keyboard's text into the target buffer each frame, and closes out
   when the user dismisses it. Status: 0 = Visible, anything else = finished. */
static void keyboard_poll(void)
{
    void *kb = mstr_get(g_kb);
    if (kb == NULL) {
        return;
    }
    void *text = inv(g_tsk_get_text, kb, NULL);
    if (text != NULL) {
        if (g_kb_target == KB_SPOOF) {
            mstr_to_utf8(text, g_spoof, sizeof(g_spoof));
        } else if (g_kb_target == KB_TITLE) {
            mstr_to_utf8(text, g_title, sizeof(g_title));
        } else {
            mstr_to_utf8(text, g_input, sizeof(g_input));
        }
    }
    if (inv_int(g_tsk_get_status, kb, NULL) != 0) {
        g_kb = mstr_hold(g_kb, NULL); /* finished or dismissed */
        if (g_kb_target == KB_SPOOF || g_kb_target == KB_TITLE) {
            apply_spoof(1);
        }
    }
}

/* Builds Request(cmd) and hands it to the live AEC instance - the same path the
   desktop Packet Sender uses: AEC.Instance.sendRequest(new Request(...)). Goes
   through our own sendRequest hook, so it lands in the log too. */
static void send_typed_packet(void)
{
    void *text = g_input[0] ? il2cpp_string_new(g_input) : NULL;
    if (text == NULL || g_aec_instance == NULL || g_request_class == NULL ||
        g_request_ctor == NULL || g_send_request == NULL || !il2cpp_object_new) {
        LOGE("send: not ready (instance=%p Request=%p)", g_aec_instance, g_request_class);
        return;
    }
    void *req = il2cpp_object_new(g_request_class);
    if (req == NULL) {
        return;
    }
    void *ctor_args[1] = {text};
    inv(g_request_ctor, req, ctor_args);
    void *send_args[1] = {req};
    inv(g_send_request, g_aec_instance, send_args);
}

/* -------------------------------------------------------------------------
 * Quest farming (narrow native slice of QuestRunner)
 *
 * No quest ID entry - remembering IDs while working through a chain is
 * exactly the tedium this should remove. Instead it reads
 * UIQuestTracker.CurrentQuest, the same static the client itself sets in
 * ResponseQuestAccept.CurrentQuest = quest (confirmed in the decomp - NOT
 * Quest.CurrentQuest, a same-named but effectively dead property on a
 * different class that a reset path nulls alongside the real one). That
 * means Farm ON acts on whatever is currently tracked, however it got
 * tracked: something the player tracked from the quest log but has not
 * accepted yet gets accepted; something already accepted and mid-chain
 * gets hunted and turned in; and once turned in, whatever the client
 * tracks NEXT (a chain auto-advancing, or the player tracking a new one by
 * hand) picks up automatically on the next tick, with no re-toggle needed.
 *
 * Accept a quest, keep auto-hunt running so kill-count objectives progress,
 * and turn in once the quest's own IsReadyForTurnin() says every objective
 * is satisfied. That method is CALLED, not reimplemented - it already walks
 * the quest's Requirements (item counts, per-objective completion flags,
 * all of it) server-synced, the same way the desktop agent calls
 * q.IsReadyForTurnin() rather than replicating what is behind it.
 *
 * Deliberately narrow: this does not act on Interact/Apop/Talk/Cutscene
 * objectives (QuestRunner's TickInteract/TickApop/TickCutscene), and does
 * not travel across cells to find them (MapNav). A pure killcount quest
 * completes end-to-end below; a quest that also needs a machine click or an
 * NPC conversation will sit at "hunting, not ready yet" forever once the
 * kill-only objectives are done - a known, logged gap, not a silent one.
 * ---------------------------------------------------------------------- */
static void *g_uiquesttracker_get_currentquest; /* static UIQuestTracker.get_CurrentQuest() */
static void *g_quest_get;                       /* static Quest.Get(int) - chain mode only */
static void *g_quest_get_id;                    /* Quest.get_ID()                          */
static void *g_quest_npcid_field;               /* Quest.NPCID - Apop/Talk NPC fallback when
                                                    the objective's own apopID isn't usable  */
static void *g_req_apopqo_class;
static void *g_req_apopqo_ctor;                 /* RequestOpenApopQO(int apopid, int monMapID) */
static void *g_monster_get_monmapid;            /* Monster.get_monMapID()                  */
static void *g_quest_is_ready_turnin;           /* Quest.IsReadyForTurnin()                */
static void *g_player_is_quest_accepted;        /* Player.IsQuestAccepted(int)             */
static void *g_req_accept_class;
static void *g_req_accept_ctor;                 /* RequestQuestAccept(int)                 */
static void *g_req_turnin_class;
static void *g_req_turnin_ctor;                 /* RequestTryQuestComplete(int,int)        */
static void *g_req_transfer_class;
static void *g_req_transfer_ctor;               /* RequestMoveToArea(string,string,string,string,string) */
static void *g_req_cutscene_class;
static void *g_req_cutscene_ctor;               /* RequestWatchCutscene(int)                */
static float g_next_interact;   /* throttles machine/NPC clicks and cutscene requests -
                                    see quest_tick's Interact/Talk/Apop/Cutscene branches */
static void *g_area_get_mapname;                /* Area.get_mapName() - confirms chain-mode arrival
                                                    before accepting; see quest_tick's chain branch */
static float g_chain_tfer_sent_at;
static float g_chain_arrived_at;                /* 0 = not yet confirmed arrived */
static int g_quest_last_id;                     /* detects the target moving to a new quest */
static float g_next_getquests_request;          /* chain mode: retry cadence while Quest.Get(id) is null */

/* Objective dispatch: which incomplete QuestTurninItem to act on, and how.
   QuestObjectiveType (decomp): Turnin=0, Killcount=1, Interact=2, Talk=3,
   Apop=4, Cutscene=5. Killcount already works (existing hunt/autoskills);
   this adds Interact and Apop/Talk. Cutscene is not covered - logged as
   such rather than silently stalling. */
static void *g_quest_turnins_field;    /* Quest.Turnins (QuestTurninItem[]) */
static void *g_system_array_class;     /* System.Array - see next_incomplete_objective()'s own
                                          comment for why GetEnumerator has to be resolved here
                                          rather than on the array's own concrete class */
static void *g_qti_qoid_field;
static void *g_qti_qotype_field;
static void *g_qti_refscontains_method;/* QuestTurninItem.RefsContains(string) - exact match
                                          only; kept for the Killcount RefInt path, but NOT used
                                          for machine matching any more - see g_qti_refarray_field */
static void *g_qti_refarray_field;     /* QuestTurninItem.RefArray (string[]) - read directly so
                                          machine names can be tiered exact/prefix/contains
                                          matched like MapNav.MatchTier, instead of RefsContains'
                                          exact-only Array.Contains (which is why quest 59's
                                          "DSPiece" ref never matched actual pieces named
                                          "DSPiece1".."DSPiece6") */
static void *g_player_quests_field;    /* Player.Quests (PlayerQuestData) */
static void *g_pqd_is_objective_complete; /* PlayerQuestData.IsObjectiveComplete(int) */
static void *g_pqd_is_quest_complete;     /* PlayerQuestData.isQuestComplete(int) - whether the
                                              WHOLE quest has ever been turned in, not just one
                                              objective; used to resume a chain instead of
                                              replaying it from the top every time */
static void *g_area_cells_field;       /* Area.Cells (Dictionary<string,MapCell>) */
static void *g_entity_apopid_field;    /* Entity.apopID (int, default -1) */
static void *g_transform_get_childcount;
static void *g_transform_get_child;     /* Transform.GetChild(int) */
static void *g_transform_get_gameobject;
static void *g_object_get_name;         /* UnityEngine.Object.get_name - not overridden by
                                           MapMachine/NPCButton, safe to resolve once here */
static void *g_mapmachine_type_obj;
static void *g_mapmachine_interact;     /* MapMachine.Interact() */
static void *g_npcbutton_type_obj;
static void *g_npcbutton_interact;      /* NPCButton.Interact() */

/* A QuestTurninItem's RefArray tokens, read once up front rather than
   re-walked per candidate machine. Fixed-size: RefArray is authored data
   (a handful of names/ids at most), not a dynamic list. */
typedef struct {
    char tok[16][40];
    int count;
} RefTokens;

/* Reads QuestTurninItem.RefArray (string[]) directly instead of going
   through RefsContains(). RefArray is a plain array of strings, same
   enumerator-walk technique as every other collection in this file. */
static void qti_ref_tokens(void *qti, RefTokens *out)
{
    out->count = 0;
    if (qti == NULL || g_qti_refarray_field == NULL || g_system_array_class == NULL) {
        return;
    }
    void *arr = NULL;
    il2cpp_field_get_value(qti, g_qti_refarray_field, &arr);
    if (arr == NULL) {
        return;
    }
    void *get_enumerator =
        il2cpp_class_get_method_from_name(g_system_array_class, "GetEnumerator", 0);
    EnumWalk w;
    if (!enum_open(arr, get_enumerator, &w)) {
        return;
    }
    for (int i = 0; i < 16 && inv_bool(w.move_next, w.self, NULL); i++) {
        void *s = inv(w.get_current, w.self, NULL);
        mstr_to_utf8(s, out->tok[out->count], sizeof(out->tok[out->count]));
        if (out->tok[out->count][0] != '\0') {
            out->count++;
        }
    }
}

/* Case-insensitive "does hay contain needle anywhere" - no strcasestr
   dependency (a GNU extension not guaranteed present in bionic libc). */
static bool ci_contains(const char *hay, const char *needle)
{
    size_t hn = strlen(hay), nn = strlen(needle);
    if (nn == 0 || nn > hn) {
        return false;
    }
    for (size_t i = 0; i + nn <= hn; i++) {
        if (strncasecmp(hay + i, needle, nn) == 0) {
            return true;
        }
    }
    return false;
}

/* Tiered exact/prefix/contains match, mirroring MapNav.MatchTier. RefsContains
   (Array.Contains, exact-only) is why quest 59 never found anything: its ref
   is the bare prefix "DSPiece", but the actual machines are individually
   named "DSPiece1".."DSPiece6" - no exact match ever exists for any of them,
   only a prefix one. */
static bool machine_name_matches(const char *name, const RefTokens *refs)
{
    for (int i = 0; i < refs->count; i++) {
        const char *tok = refs->tok[i];
        if (tok[0] == '\0') {
            continue;
        }
        if (strcasecmp(name, tok) == 0) {
            return true;
        }
        if (strncasecmp(name, tok, strlen(tok)) == 0) {
            return true;
        }
        if (ci_contains(name, tok) || ci_contains(tok, name)) {
            return true;
        }
    }
    return false;
}

/* Interact objectives: recursive search of a cell's transform subtree for a
   MapMachine whose GameObject name matches the objective's RefArray tokens.
   No FindObjectsByType here (the generic overload this shim has no path to
   resolve) - walking a cell's subtree one at a time (see find_machine_frame
   for map-wide) stands in for the desktop's single whole-map MapNav scan. */
static void *find_machine_in_subtree(void *transform, const RefTokens *refs, int depth)
{
    if (transform == NULL || depth > 14 || g_mapmachine_type_obj == NULL ||
        g_object_get_name == NULL || refs->count == 0) {
        return NULL;
    }
    void *go = g_transform_get_gameobject ? inv(g_transform_get_gameobject, transform, NULL)
                                          : NULL;
    void *machine = go ? get_component(go, g_mapmachine_type_obj) : NULL;
    if (machine != NULL) {
        void *name_str = inv(g_object_get_name, go, NULL);
        char name[64] = "";
        mstr_to_utf8(name_str, name, sizeof(name));
        if (name[0] != '\0' && machine_name_matches(name, refs)) {
            return machine;
        }
    }
    if (g_transform_get_childcount == NULL || g_transform_get_child == NULL) {
        return NULL;
    }
    int32_t count = inv_int(g_transform_get_childcount, transform, NULL);
    for (int32_t i = 0; i < count; i++) {
        void *idx_args[1] = {&i};
        void *child = inv(g_transform_get_child, transform, idx_args);
        void *found = find_machine_in_subtree(child, refs, depth + 1);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* Map-wide machine search, mirroring MapNav.FindMachine: walks every cell in
   Area.Cells (not just the current one) and returns the frame name of the
   first one whose subtree holds a matching MapMachine. Needed for the same
   reason find_hostile_frame is: find_machine_in_subtree only sees the
   CURRENT cell, but a chain's fixed entry frame does not always hold the
   objective's target - Lair's DragonSlayer armor pieces (quest 59, "DSPiece")
   are scattered across several rooms, not the Enter cell chains park the
   player in. Confirmed on device: without this, quest 59 just reported
   "interact target not in current cell" forever. */
static bool find_machine_frame(const RefTokens *refs, char *out_frame, size_t out_cap)
{
    out_frame[0] = '\0';
    if (g_area_currentarea_field == NULL || g_area_cells_field == NULL ||
        g_component_get_transform == NULL || !il2cpp_field_static_get_value) {
        return false;
    }
    void *area = NULL;
    il2cpp_field_static_get_value(g_area_currentarea_field, &area);
    if (area == NULL) {
        return false;
    }
    void *cells = NULL;
    il2cpp_field_get_value(area, g_area_cells_field, &cells);
    if (cells == NULL) {
        return false;
    }
    void *cells_class = il2cpp_object_get_class(cells);
    EnumWalk w;
    if (!enum_open(cells, il2cpp_class_get_method_from_name(cells_class, "GetEnumerator", 0),
                   &w)) {
        return false;
    }
    for (int i = 0; i < 64 && inv_bool(w.move_next, w.self, NULL); i++) {
        void *boxed_kv = inv(w.get_current, w.self, NULL);
        void *kv_self = self_ptr(boxed_kv);
        if (kv_self == NULL) {
            continue;
        }
        void *kv_class = il2cpp_object_get_class(boxed_kv);
        void *get_key = il2cpp_class_get_method_from_name(kv_class, "get_Key", 0);
        void *get_value = il2cpp_class_get_method_from_name(kv_class, "get_Value", 0);
        void *key = get_key ? inv(get_key, kv_self, NULL) : NULL;
        void *cell = get_value ? inv(get_value, kv_self, NULL) : NULL;
        if (key == NULL || cell == NULL) {
            continue;
        }
        void *tr = inv(g_component_get_transform, cell, NULL);
        if (find_machine_in_subtree(tr, refs, 0) != NULL) {
            mstr_to_utf8(key, out_frame, out_cap);
            return true;
        }
    }
    return false;
}

/* Apop/Talk objectives: the friendly Monster carrying the wanted apopID (or,
   when that is not usable, the wanted catalog ID) - mirrors
   MapNav.FindNpc(wantApop, wantNpcId) taking both signals, not apopID alone.
   Reuses the exact Area.currentArea.Monsters walk find_nearest_hostile does
   (down to the field/method handles) - same generic Dictionary<int,Monster>
   technique, filtering on apopID/ID instead of reactionType==Hostile. */
static void *find_apop_npc(int32_t want_apop, int32_t want_npc_id)
{
    if (want_apop <= 0 && want_npc_id <= 0) {
        return NULL;
    }
    if (g_area_currentarea_field == NULL || g_area_monsters_field == NULL ||
        g_entity_apopid_field == NULL || !il2cpp_field_static_get_value) {
        return NULL;
    }
    void *area = NULL;
    il2cpp_field_static_get_value(g_area_currentarea_field, &area);
    if (area == NULL) {
        return NULL;
    }
    void *dict = NULL;
    il2cpp_field_get_value(area, g_area_monsters_field, &dict);
    if (dict == NULL) {
        return NULL;
    }
    void *dict_class = il2cpp_object_get_class(dict);
    EnumWalk w;
    if (!enum_open(dict, il2cpp_class_get_method_from_name(dict_class, "GetEnumerator", 0), &w)) {
        return NULL;
    }
    for (int i = 0; i < 500 && inv_bool(w.move_next, w.self, NULL); i++) {
        void *boxed_kv = inv(w.get_current, w.self, NULL);
        void *kv_self = self_ptr(boxed_kv);
        if (kv_self == NULL) {
            continue;
        }
        void *kv_class = il2cpp_object_get_class(boxed_kv);
        void *get_value = il2cpp_class_get_method_from_name(kv_class, "get_Value", 0);
        void *mon = get_value ? inv(get_value, kv_self, NULL) : NULL;
        if (mon == NULL) {
            continue;
        }
        /* Skip hostiles, as MapNav.FindNpc does: a quest giver is always
           friendly/neutral, and apopID defaults to -1 on things that have
           none, so an unset field can never collide with a real want_apop. */
        if (g_monster_reaction_field != NULL) {
            int32_t reaction = 0;
            il2cpp_field_get_value(mon, g_monster_reaction_field, &reaction);
            if (reaction == 1) {
                continue;
            }
        }
        int32_t apop = -1;
        il2cpp_field_get_value(mon, g_entity_apopid_field, &apop);
        if (want_apop > 0 && apop == want_apop) {
            return mon;
        }
        if (want_npc_id > 0 && g_entity_get_id != NULL &&
            inv_int(g_entity_get_id, mon, NULL) == want_npc_id) {
            return mon;
        }
    }
    return NULL;
}

/* First QuestTurninItem in Quest.Turnins the player has not completed, via
   PlayerQuestData.IsObjectiveComplete(QOID) - same array-of-reference-type
   walk as the Monsters dictionary (GetEnumerator/MoveNext/get_Current on the
   array instance's own class), just picking the first non-complete entry
   instead of aggregating. NULL if every visible objective reads complete
   (including if Turnins/Quests themselves are not resolvable). */
static void *next_incomplete_objective(void *quest, void *player, int32_t *qotype_out)
{
    *qotype_out = -1;
    if (g_quest_turnins_field == NULL || g_player_quests_field == NULL ||
        g_pqd_is_objective_complete == NULL || g_qti_qoid_field == NULL) {
        return NULL;
    }
    void *turnins = NULL;
    il2cpp_field_get_value(quest, g_quest_turnins_field, &turnins);
    void *pq = NULL;
    il2cpp_field_get_value(player, g_player_quests_field, &pq);
    if (turnins == NULL || pq == NULL) {
        return NULL;
    }

    /* QuestTurninItem[]'s own concrete class does not declare GetEnumerator
       itself - arrays get it from System.Array, and il2cpp_class_get_method
       _from_name only searches a class's OWN declared methods, not inherited
       ones (the same rule that made resolving get_Name on Entity read the
       wrong backing field for Monster earlier). Confirmed on device: this
       returned NULL for every quest, silently treating a quest with a real
       incomplete Killcount objective as having nothing actionable - the bot
       fell back to blind hunting and only completed by luck (monsters
       happened to be in the starting room). Resolving GetEnumerator on
       System.Array instead - the class that actually declares it - is half
       the fix; the other half is NOT unboxing what it returns, since unlike
       Dictionary's struct enumerator this one is a reference type. See
       self_ptr for why that distinction was silent rather than fatal. */
    void *get_enumerator = g_system_array_class != NULL
                              ? il2cpp_class_get_method_from_name(g_system_array_class,
                                                                  "GetEnumerator", 0)
                              : NULL;
    EnumWalk w;
    if (!enum_open(turnins, get_enumerator, &w)) {
        return NULL;
    }
    for (int i = 0; i < 32 && inv_bool(w.move_next, w.self, NULL); i++) {
        void *item = inv(w.get_current, w.self, NULL);
        if (item == NULL) {
            continue;
        }
        int32_t qoid = 0;
        il2cpp_field_get_value(item, g_qti_qoid_field, &qoid);
        void *qoid_args[1] = {&qoid};
        if (!inv_bool(g_pqd_is_objective_complete, pq, qoid_args)) {
            if (g_qti_qotype_field != NULL) {
                il2cpp_field_get_value(item, g_qti_qotype_field, qotype_out);
            }
            return item;
        }
    }
    return NULL;
}

/* Current MapCell's Transform, via Area.Cells[Entity.Frame] - a dictionary
   lookup by key (get_Item), not the enumerator walk the other dictionary
   uses elsewhere; the key is already known, so indexing it directly is both
   simpler and cheaper. NULL on any miss (no area, no cell for this frame,
   dictionary indexer threw - inv() swallows exceptions and returns NULL,
   which doubles as "key not found" here). */
static void *current_cell_transform(void *player)
{
    if (g_area_currentarea_field == NULL || g_area_cells_field == NULL ||
        g_entity_frame_field == NULL || g_component_get_transform == NULL ||
        !il2cpp_field_static_get_value) {
        return NULL;
    }
    void *area = NULL;
    il2cpp_field_static_get_value(g_area_currentarea_field, &area);
    if (area == NULL) {
        return NULL;
    }
    void *cells = NULL;
    il2cpp_field_get_value(area, g_area_cells_field, &cells);
    if (cells == NULL) {
        return NULL;
    }
    void *frame = NULL;
    il2cpp_field_get_value(player, g_entity_frame_field, &frame);
    if (frame == NULL) {
        return NULL;
    }
    void *cells_class = il2cpp_object_get_class(cells);
    void *get_item = il2cpp_class_get_method_from_name(cells_class, "get_Item", 1);
    if (get_item == NULL) {
        return NULL;
    }
    void *args[1] = {frame};
    void *cell = inv(get_item, cells, args);
    return cell ? inv(g_component_get_transform, cell, NULL) : NULL;
}

static void quest_tick(void)
{
    if (!g_quest_running) {
        return;
    }
    float now = inv_float(g_time_get_time, NULL, NULL);
    if (now < g_next_quest_tick) {
        return;
    }
    g_next_quest_tick = now + 1.0f;

    /* Stall detection, mirroring CheckHuntTimeout/Fail: g_quest_last_activity_at
       advances on every mKill and every confirmed QComp (see the hook_get_
       response tracking), reset whenever the active quest changes or the run
       (re)starts. No kills and no completion for 90s means something this
       bot doesn't handle - a wrong zone, an unmapped mob, an objective type
       with no dispatch - not a quest to keep grinding forever in silence. */
    if (g_quest_last_activity_at > 0.0f &&
        now - g_quest_last_activity_at > QUEST_HUNT_TIMEOUT_SEC) {
        LOGE("quest: no kills or progress in %.0fs - stopping", QUEST_HUNT_TIMEOUT_SEC);
        snprintf(g_quest_status, sizeof(g_quest_status), "STALLED: no progress in %.0fs - stopped",
                QUEST_HUNT_TIMEOUT_SEC);
        g_quest_running = 0;
        return;
    }

    void *player = g_get_main_player ? inv(g_get_main_player, NULL, NULL) : NULL;
    if (player == NULL) {
        return;
    }

    /* Target resolution: Track reads whatever is live-tracked in-game; a
       selected chain reads a fixed ID off the baked-in CHAINS table. Once
       target_id/quest are set, everything below (accept/dispatch/turn-in)
       is identical for both. */
    int32_t qid = 0;
    void *quest = NULL;
    const ChainDef *chain = g_quest_selected >= 1 ? &CHAINS[g_quest_selected - 1] : NULL;

    if (chain == NULL) {
        quest = inv(g_uiquesttracker_get_currentquest, NULL, NULL);
        if (quest == NULL) {
            snprintf(g_quest_status, sizeof(g_quest_status),
                    "no quest tracked - track one in-game");
            g_quest_last_id = 0;
            return;
        }
        qid = g_quest_get_id != NULL ? inv_int(g_quest_get_id, quest, NULL) : 0;
    } else {
        if (g_chain_index >= chain->count) {
            snprintf(g_quest_status, sizeof(g_quest_status), "%s complete!", chain->name);
            g_quest_running = 0;
            return;
        }

        /* Get to the chain's map before doing anything else - and, critically,
           WAIT for confirmed arrival before accepting. The first version sent
           the accept request in the same tick as the transfer (or the very
           next one) with no confirmation the character had actually landed;
           live AE's servers are far stricter than our own about validating
           that the client's real server-side location matches what a request
           claims, and an accept for a location-gated quest while still
           mid-transfer reads exactly like invalid/bot behavior - which is
           what was getting players kicked, not a wrong quest ID (the whole
           baked-in ID/prevQuest sequence for all four chains was cross-checked
           against real live-AE packet captures and matches exactly). */
        char current_map[40] = "";
        void *area_for_map = NULL;
        if (g_area_currentarea_field != NULL && il2cpp_field_static_get_value) {
            il2cpp_field_static_get_value(g_area_currentarea_field, &area_for_map);
        }
        if (area_for_map != NULL && g_area_get_mapname != NULL) {
            mstr_to_utf8(inv(g_area_get_mapname, area_for_map, NULL), current_map,
                        sizeof(current_map));
        }
        bool arrived = current_map[0] != '\0' && strcasecmp(current_map, chain->map) == 0;

        if (!arrived) {
            g_chain_arrived_at = 0.0f;
            bool need_send = !g_chain_tfer_sent || (now - g_chain_tfer_sent_at > 10.0f);
            if (need_send && g_req_transfer_class != NULL && g_req_transfer_ctor != NULL &&
                il2cpp_object_new && g_get_name != NULL && il2cpp_string_new) {
                void *req = il2cpp_object_new(g_req_transfer_class);
                if (req != NULL) {
                    void *player_name = inv(g_get_name, player, NULL);
                    void *ctor_args[5] = {player_name, il2cpp_string_new(chain->map),
                                          il2cpp_string_new(""), il2cpp_string_new(chain->frame),
                                          il2cpp_string_new(chain->pad)};
                    inv(g_req_transfer_ctor, req, ctor_args);
                    void *send_args[1] = {req};
                    inv(g_send_request, g_aec_instance, send_args);
                    LOGI("quest: chain '%s' - transferring to %s/%s/%s", chain->name, chain->map,
                         chain->frame, chain->pad);
                }
                g_chain_tfer_sent = 1;
                g_chain_tfer_sent_at = now;
            }
            snprintf(g_quest_status, sizeof(g_quest_status), "%s - traveling to %s (at %s)",
                    chain->name, chain->map, current_map[0] ? current_map : "?");
            return;
        }
        if (g_chain_arrived_at <= 0.0f) {
            g_chain_arrived_at = now;
        }
        if (now - g_chain_arrived_at < 1.5f) {
            /* Same settle window the death/respawn handling uses - a map
               transfer that just landed can still report stale area/position
               state for a frame or two. */
            snprintf(g_quest_status, sizeof(g_quest_status), "%s - arrived, settling",
                    chain->name);
            return;
        }

        /* Resume support: don't blindly restart every chain at index 0. If
           quest(s) at the front of the baked-in list are already turned in -
           the player ran this chain before, whether through the bot or by
           hand - skip forward to the first one that is not, mirroring
           QuestRunner.TickAccept's isQuestComplete check. Cheap to run every
           tick: once resumed past, the current index's quest is never
           complete so the loop body runs zero times. */
        if (g_player_quests_field != NULL && g_pqd_is_quest_complete != NULL) {
            void *pq_resume = NULL;
            il2cpp_field_get_value(player, g_player_quests_field, &pq_resume);
            if (pq_resume != NULL) {
                while (g_chain_index < chain->count) {
                    int32_t check_qid = chain->ids[g_chain_index];
                    void *check_args[1] = {&check_qid};
                    if (!inv_bool(g_pqd_is_quest_complete, pq_resume, check_args)) {
                        break;
                    }
                    g_chain_index++;
                }
                if (g_chain_index >= chain->count) {
                    snprintf(g_quest_status, sizeof(g_quest_status), "%s complete!", chain->name);
                    g_quest_running = 0;
                    return;
                }
            }
        }

        qid = chain->ids[g_chain_index];
        void *id_args[1] = {&qid};
        quest = g_quest_get != NULL ? inv(g_quest_get, NULL, id_args) : NULL;
        if (quest == NULL) {
            /* Not cached client-side yet - most likely still loading into
               the chain's map, or the quest def just has not been sent.
               getQuests populates Quest.Get() the same as opening the quest
               log once would. */
            if (now >= g_next_getquests_request && g_request_class != NULL &&
                g_request_ctor != NULL && g_send_request != NULL && il2cpp_object_new &&
                il2cpp_string_new) {
                g_next_getquests_request = now + 5.0f;
                void *req = il2cpp_object_new(g_request_class);
                if (req != NULL) {
                    void *ctor_args[1] = {il2cpp_string_new("getQuests")};
                    inv(g_request_ctor, req, ctor_args);
                    void *send_args[1] = {req};
                    inv(g_send_request, g_aec_instance, send_args);
                }
                snprintf(g_quest_status, sizeof(g_quest_status),
                        "%s - loading quest %d (%d/%d)", chain->name, qid, g_chain_index + 1,
                        chain->count);
            }
            return;
        }
    }

    if (qid != g_quest_last_id) {
        /* Target moved to a different quest - fresh attempt, whether that's
           the tracker changing, a chain advancing, or the player tracking a
           new one by hand while in track mode. */
        g_quest_last_id = qid;
        g_quest_accept_sent = 0;
        g_quest_turnin_sent = 0;
        g_quest_last_activity_at = now; /* fresh quest - don't inherit a stale stall clock */
        LOGI("quest: now working on quest %d", qid);
    }
    void *id_args[1] = {&qid};

    bool accepted = g_player_is_quest_accepted != NULL &&
                    inv_bool(g_player_is_quest_accepted, player, id_args);
    if (!accepted) {
        if (!g_quest_accept_sent && g_req_accept_class != NULL && g_req_accept_ctor != NULL &&
            il2cpp_object_new) {
            void *req = il2cpp_object_new(g_req_accept_class);
            if (req != NULL) {
                inv(g_req_accept_ctor, req, id_args);
                void *send_args[1] = {req};
                inv(g_send_request, g_aec_instance, send_args);
                g_quest_accept_sent = 1;
                snprintf(g_quest_status, sizeof(g_quest_status), "accepting quest %d", qid);
                LOGI("quest: sent RequestQuestAccept(%d)", qid);
            }
        }
        return;
    }

    bool ready = g_quest_is_ready_turnin != NULL &&
                inv_bool(g_quest_is_ready_turnin, quest, NULL);
    if (ready) {
        g_hunt = 0;
        g_autoskills = 0; /* mirrors QuestRunner's StopAutoskills() on nextObjective==null */
        g_interact_approach_active = 0;
    }
    if (!ready) {
        int32_t qotype = -1;
        void *obj = next_incomplete_objective(quest, player, &qotype);
        /* QuestObjectiveType, per the decomp: Turnin=0 Killcount=1 Interact=2
           Talk=3 Apop=4 Cutscene=5. */
        /* Publish the objective for hunt_tick's target filter. Only a
           Killcount objective carries monster refs worth filtering on; for
           everything else this stays cleared so any incidental hunting is
           unconstrained. */
        g_hunt_obj = (obj != NULL && qotype == 1) ? obj : NULL;
        g_hunt_qotype = qotype;
        g_hunt_qid = qid;
        /* Default off; only the Interact/Apop branches below turn this back
           on. Without this reset, switching from an Interact/Apop objective
           to a Killcount one (or to no objective at all) left hunt_tick
           still walking toward a stale machine/NPC position from whatever
           was last published, indefinitely. */
        g_interact_approach_active = 0;

        /* Clicking a machine or an NPC every single tick is both pointless
           and the kind of request rate that trips live AE's spam detection -
           the desktop agent spaces these out and so do we. */
        bool may_click = now >= g_next_interact;

        if (obj == NULL) {
            /* Nothing we can see is incomplete, yet IsReadyForTurnin says
               not ready - most likely a Turnin-type item-count objective,
               which is server-tracked inventory, not something to act on
               here. Hunting is harmless if nothing needs it. */
            g_hunt = 1;
            snprintf(g_quest_status, sizeof(g_quest_status),
                    "quest %d - no actionable objective visible, hunting", qid);
        } else if (qotype == 2) { /* Interact */
            g_hunt = 0;           /* a machine click needs the player still, not chasing a mob */
            /* Mirrors QuestRunner: EnsureAutoskillsOn() only ever runs from
               inside a live-hostile engage; every non-combat objective path
               (this one included) calls StopAutoskills() instead. Android's
               hunt_tick sets g_autoskills=1 the same way on engage, but
               nothing turned it back off - since g_hunt=0 makes hunt_tick
               return before ever reaching that logic, autoskills firing from
               an earlier Killcount objective stayed stuck on with no target,
               which is what "spamming skills" at the DragonSlayer armor
               machines was: skills firing on a 0.6s cycle at nothing. */
            g_autoskills = 0;
            RefTokens machine_refs;
            qti_ref_tokens(obj, &machine_refs);
            void *cell_tr = current_cell_transform(player);
            void *machine = cell_tr ? find_machine_in_subtree(cell_tr, &machine_refs, 0) : NULL;
            float me_pos[3], mach_pos[3];
            void *player_parent_i = path_player_parent(inv(g_entity_getgameobject, player, NULL));
            bool have_pos = machine != NULL && read_local_pos(player, me_pos) &&
                            component_local_in_player_frame(machine, player_parent_i, mach_pos);
            if (have_pos) {
                /* Publish for hunt_tick to actually walk toward at its own
                   ~3Hz cadence - see hunt_tick's top for why this can't just
                   be called directly from here. */
                g_interact_approach_active = 1;
                memcpy(g_interact_approach_target, mach_pos, sizeof(mach_pos));
                g_interact_reach_dist_pub = INTERACT_REACH_DIST;
            } else {
                g_interact_approach_active = 0;
            }
            if (machine != NULL && g_mapmachine_interact != NULL && have_pos && g_interact_ready) {
                if (may_click) {
                    inv(g_mapmachine_interact, machine, NULL);
                    g_next_interact = now + 2.5f;
                }
                snprintf(g_quest_status, sizeof(g_quest_status),
                        "quest %d - clicking machine for current objective", qid);
            } else if (machine != NULL && have_pos) {
                /* Found it, but not close enough yet - hunt_tick is already
                   walking/pathing toward it between quest_tick's own ticks. */
                snprintf(g_quest_status, sizeof(g_quest_status),
                        "quest %d - approaching machine for current objective", qid);
            } else {
                /* Not in this cell - search the whole map (mirrors
                   MapNav.FindMachine + GoToFrame) and jump to whichever cell
                   actually holds it, same cross-cell travel as the Killcount
                   hunt path above. */
                char here[40] = "";
                if (g_entity_frame_field != NULL) {
                    void *fs = NULL;
                    il2cpp_field_get_value(player, g_entity_frame_field, &fs);
                    mstr_to_utf8(fs, here, sizeof(here));
                }
                char want_frame[40];
                if (find_machine_frame(&machine_refs, want_frame, sizeof(want_frame)) &&
                    strcasecmp(want_frame, here) != 0) {
                    bool need_send = strcasecmp(g_hunt_nav_frame, want_frame) != 0 ||
                                      (now - g_hunt_nav_sent_at > 8.0f); /* matches QuestRunner.NavResendSec */
                    if (need_send && g_req_movecell_class != NULL &&
                        g_req_movecell_ctor != NULL && il2cpp_object_new &&
                        il2cpp_string_new && g_send_request != NULL) {
                        void *req = il2cpp_object_new(g_req_movecell_class);
                        if (req != NULL) {
                            void *ctor_args[2] = {il2cpp_string_new(want_frame),
                                                  il2cpp_string_new("Spawn")};
                            inv(g_req_movecell_ctor, req, ctor_args);
                            void *send_args[1] = {req};
                            inv(g_send_request, g_aec_instance, send_args);
                            LOGI("quest: interact target not in '%s' - moving to '%s'", here,
                                 want_frame);
                        }
                        snprintf(g_hunt_nav_frame, sizeof(g_hunt_nav_frame), "%s", want_frame);
                        g_hunt_nav_sent_at = now;
                    }
                    snprintf(g_quest_status, sizeof(g_quest_status),
                            "quest %d - traveling to %s for interact target", qid, want_frame);
                } else {
                    snprintf(g_quest_status, sizeof(g_quest_status),
                            "quest %d - interact target not found anywhere in map", qid);
                }
            }
        } else if (qotype == 3 || qotype == 4) { /* Talk / Apop */
            g_hunt = 0;
            g_autoskills = 0; /* see the Interact branch above */
            int32_t zero = 0;
            void *ref_args[1] = {&zero};
            int32_t want_apop = g_qti_getrefint_method != NULL
                                    ? inv_int(g_qti_getrefint_method, obj, ref_args)
                                    : -1;
            int32_t want_npc_id = -1;
            if (g_quest_npcid_field != NULL) {
                il2cpp_field_get_value(quest, g_quest_npcid_field, &want_npc_id);
            }
            void *npc = find_apop_npc(want_apop, want_npc_id);
            /* find_apop_npc already scans the whole map (no frame filter),
               but that only means it CAN locate an NPC in another cell - it
               says nothing about whether NPCButton.Interact() works on one
               that is not in the player's currently active cell. Same class
               of bug as the Interact/Killcount cross-cell gaps: check the
               NPC's own Frame and travel there first if it differs. */
            char npc_frame[40] = "";
            if (npc != NULL && g_entity_frame_field != NULL) {
                void *fs = NULL;
                il2cpp_field_get_value(npc, g_entity_frame_field, &fs);
                mstr_to_utf8(fs, npc_frame, sizeof(npc_frame));
            }
            char here_apop[40] = "";
            if (g_entity_frame_field != NULL) {
                void *fs = NULL;
                il2cpp_field_get_value(player, g_entity_frame_field, &fs);
                mstr_to_utf8(fs, here_apop, sizeof(here_apop));
            }
            void *npc_go = (npc != NULL && (npc_frame[0] == '\0' ||
                                            strcasecmp(npc_frame, here_apop) == 0))
                              ? inv(g_entity_getgameobject, npc, NULL)
                              : NULL;
            void *npcbtn = npc_go ? get_component(npc_go, g_npcbutton_type_obj) : NULL;
            float me_pos_apop[3], npc_pos[3];
            void *player_parent_a = path_player_parent(inv(g_entity_getgameobject, player, NULL));
            bool have_apop_pos = npc_go != NULL && read_local_pos(player, me_pos_apop) &&
                                 entity_local_in_player_frame(npc, player_parent_a, npc_pos);
            if (have_apop_pos) {
                g_interact_approach_active = 1;
                memcpy(g_interact_approach_target, npc_pos, sizeof(npc_pos));
                g_interact_reach_dist_pub = INTERACT_REACH_DIST;
            } else {
                g_interact_approach_active = 0;
            }
            if (npcbtn != NULL && g_npcbutton_interact != NULL && have_apop_pos &&
                g_interact_ready) {
                if (may_click) {
                    inv(g_npcbutton_interact, npcbtn, NULL);
                    /* Belt-and-suspenders, mirroring ClickNpc: the button
                       click SHOULD credit the objective via ShowApop(), but
                       sending the same request the click's own dialog flow
                       would send is harmless if already credited and covers
                       the click silently not landing. */
                    if (want_apop > 0 && g_req_apopqo_class != NULL &&
                        g_req_apopqo_ctor != NULL && il2cpp_object_new &&
                        g_monster_get_monmapid != NULL && g_send_request != NULL) {
                        int32_t mon_map_id = inv_int(g_monster_get_monmapid, npc, NULL);
                        void *req = il2cpp_object_new(g_req_apopqo_class);
                        if (req != NULL) {
                            void *apopqo_args[2] = {&want_apop, &mon_map_id};
                            inv(g_req_apopqo_ctor, req, apopqo_args);
                            void *send_args[1] = {req};
                            inv(g_send_request, g_aec_instance, send_args);
                        }
                    }
                    g_next_interact = now + 2.5f;
                }
                snprintf(g_quest_status, sizeof(g_quest_status),
                        "quest %d - talking to NPC (apop %d)", qid, want_apop);
            } else if (npcbtn != NULL && have_apop_pos) {
                snprintf(g_quest_status, sizeof(g_quest_status),
                        "quest %d - approaching NPC (apop %d)", qid, want_apop);
            } else if (npc != NULL && npc_frame[0] != '\0' &&
                       strcasecmp(npc_frame, here_apop) != 0) {
                bool need_send = strcasecmp(g_hunt_nav_frame, npc_frame) != 0 ||
                                  (now - g_hunt_nav_sent_at > 8.0f); /* matches QuestRunner.NavResendSec */
                if (need_send && g_req_movecell_class != NULL && g_req_movecell_ctor != NULL &&
                    il2cpp_object_new && il2cpp_string_new && g_send_request != NULL) {
                    void *req = il2cpp_object_new(g_req_movecell_class);
                    if (req != NULL) {
                        void *ctor_args[2] = {il2cpp_string_new(npc_frame),
                                              il2cpp_string_new("Spawn")};
                        inv(g_req_movecell_ctor, req, ctor_args);
                        void *send_args[1] = {req};
                        inv(g_send_request, g_aec_instance, send_args);
                        LOGI("quest: apop %d not in '%s' - moving to '%s'", want_apop, here_apop,
                             npc_frame);
                    }
                    snprintf(g_hunt_nav_frame, sizeof(g_hunt_nav_frame), "%s", npc_frame);
                    g_hunt_nav_sent_at = now;
                }
                snprintf(g_quest_status, sizeof(g_quest_status),
                        "quest %d - traveling to %s for NPC (apop %d)", qid, npc_frame,
                        want_apop);
            } else {
                snprintf(g_quest_status, sizeof(g_quest_status),
                        "quest %d - apop %d not found in current map", qid, want_apop);
            }
        } else if (qotype == 5) { /* Cutscene */
            g_hunt = 0;
            g_autoskills = 0; /* see the Interact branch above */
            /* Ask the server for the cutscene directly rather than trying to
               find and click whatever triggers it in-world. The desktop
               agent does the same, and for the same reason: driving it from
               the trigger reliably lands on a black screen. */
            int32_t zero = 0;
            void *ref_args[1] = {&zero};
            int32_t csid = g_qti_getrefint_method != NULL
                               ? inv_int(g_qti_getrefint_method, obj, ref_args)
                               : -1;
            if (csid > 0 && may_click && g_req_cutscene_class != NULL &&
                g_req_cutscene_ctor != NULL && il2cpp_object_new) {
                void *req = il2cpp_object_new(g_req_cutscene_class);
                if (req != NULL) {
                    void *cs_args[1] = {&csid};
                    inv(g_req_cutscene_ctor, req, cs_args);
                    void *send_args[1] = {req};
                    inv(g_send_request, g_aec_instance, send_args);
                    g_next_interact = now + 5.0f; /* cutscenes take a while to play out */
                    LOGI("quest: sent RequestWatchCutscene(%d)", csid);
                }
            }
            if (csid > 0) {
                snprintf(g_quest_status, sizeof(g_quest_status),
                        "quest %d - watching cutscene %d", qid, csid);
            } else {
                g_hunt = 1;
                snprintf(g_quest_status, sizeof(g_quest_status),
                        "quest %d - cutscene objective has no id, hunting meanwhile", qid);
            }
        } else { /* Killcount, or unrecognized - hunting is always a safe default */
            g_hunt = 1;
            snprintf(g_quest_status, sizeof(g_quest_status), "hunting for quest %d", qid);
        }
        return;
    }

    /* Travel to the turn-in location first. Mirrors AtTurnInLocation/
       TickTurnIn: firing tryQuestComplete from wherever hunting happened to
       end works by luck when the hunt and turn-in frames coincide, and
       silently fails (or reads as a location mismatch to live AE) when they
       don't - quest 20 hunts in Enter but turns in at R3; quest 59 hunts at
       R8 but turns in back at Enter. */
    const QuestTurnin *tloc = quest_turnin_loc(qid);
    if (tloc != NULL) {
        char here_ti[40] = "";
        if (g_entity_frame_field != NULL) {
            void *fs = NULL;
            il2cpp_field_get_value(player, g_entity_frame_field, &fs);
            mstr_to_utf8(fs, here_ti, sizeof(here_ti));
        }
        if (strcasecmp(here_ti, tloc->frame) != 0) {
            bool need_send = strcasecmp(g_hunt_nav_frame, tloc->frame) != 0 ||
                              (now - g_hunt_nav_sent_at > 8.0f);
            if (need_send && g_req_movecell_class != NULL && g_req_movecell_ctor != NULL &&
                il2cpp_object_new && il2cpp_string_new && g_send_request != NULL) {
                void *req = il2cpp_object_new(g_req_movecell_class);
                if (req != NULL) {
                    void *ctor_args[2] = {il2cpp_string_new(tloc->frame),
                                          il2cpp_string_new(tloc->pad)};
                    inv(g_req_movecell_ctor, req, ctor_args);
                    void *send_args[1] = {req};
                    inv(g_send_request, g_aec_instance, send_args);
                    LOGI("quest: traveling to turn-in location '%s' for quest %d", tloc->frame,
                         qid);
                }
                snprintf(g_hunt_nav_frame, sizeof(g_hunt_nav_frame), "%s", tloc->frame);
                g_hunt_nav_sent_at = now;
            }
            snprintf(g_quest_status, sizeof(g_quest_status),
                    "quest %d - traveling to %s to turn in", qid, tloc->frame);
            return;
        }
    }

    if (!g_quest_turnin_sent) {
        if (g_req_turnin_class != NULL && g_req_turnin_ctor != NULL && il2cpp_object_new) {
            void *req = il2cpp_object_new(g_req_turnin_class);
            if (req != NULL) {
                int32_t choice = -1;
                void *turnin_args[2] = {&qid, &choice};
                inv(g_req_turnin_ctor, req, turnin_args);
                void *send_args[1] = {req};
                inv(g_send_request, g_aec_instance, send_args);
                g_quest_turnin_sent = 1;
                g_quest_turnin_sent_at = now;
                LOGI("quest: sent RequestTryQuestComplete(%d, -1)", qid);
                snprintf(g_quest_status, sizeof(g_quest_status),
                        "quest %d - turn-in sent, awaiting confirmation", qid);
            }
        }
        return;
    }

    /* Sent - do NOT advance the chain until the server actually confirms it
       (QComp/Success), mirroring TickAwaitComplete. Advancing on send alone
       let a dropped or rejected turn-in silently desync the chain index from
       the server's real quest state - the run would think it had moved on
       to the next quest while the server still considered the old one
       active. */
    if (g_qcomp_qid == qid && g_qcomp_at > g_quest_turnin_sent_at) {
        LOGI("quest: turn-in confirmed (QComp success) for quest %d", qid);
        if (chain != NULL) {
            /* Chain mode advances off our own baked-in list, not the live
               tracker - deterministic regardless of whether the client
               happens to auto-track the next storyline quest. */
            g_chain_index++;
            if (g_chain_index >= chain->count) {
                snprintf(g_quest_status, sizeof(g_quest_status), "%s complete!", chain->name);
            } else {
                snprintf(g_quest_status, sizeof(g_quest_status),
                        "%s - quest %d turned in (%d/%d)", chain->name, qid, g_chain_index + 1,
                        chain->count);
            }
        } else {
            snprintf(g_quest_status, sizeof(g_quest_status),
                    "quest %d turned in - waiting for next tracked quest", qid);
            /* Track mode stays on this ID until the tracker itself moves;
               the id-change check above resets accept/turnin state whenever
               that happens. */
        }
        return;
    }

    /* No confirmation yet - check for an rNotify that arrived after our
       send. "Spam Detected" is a rate limit, not a rejection: back off and
       let the resend-after-timeout path below retry. Anything else is a
       real rejection, and mirrors Fail() by stopping the run outright rather
       than looping on a turn-in that will never succeed. */
    if (g_notify_at > g_quest_turnin_sent_at && g_notify_msg[0] != '\0') {
        char lower[128];
        snprintf(lower, sizeof(lower), "%s", g_notify_msg);
        for (char *p = lower; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        bool is_spam = strstr(lower, "spam") != NULL || strstr(lower, "wait before") != NULL;
        if (!is_spam) {
            g_quest_running = 0;
            snprintf(g_quest_status, sizeof(g_quest_status), "quest %d - rejected: %s", qid,
                    g_notify_msg);
            LOGE("quest: turn-in for %d rejected by server: %s", qid, g_notify_msg);
            return;
        }
        snprintf(g_quest_status, sizeof(g_quest_status), "quest %d - rate-limited, retrying", qid);
    }

    /* Resend once nothing has confirmed or rejected within a reasonable
       window - the server does occasionally drop a request outright. */
    if (now - g_quest_turnin_sent_at > 6.0f) {
        g_quest_turnin_sent = 0;
    } else {
        snprintf(g_quest_status, sizeof(g_quest_status), "quest %d - awaiting turn-in confirmation",
                qid);
    }
}

/* Reads a managed string member, preferring the property getter and falling
   back to the field - which of the two a name lives behind varies. */
static void *read_str_member(void *obj, void *getter, void *field)
{
    if (getter != NULL) {
        void *v = inv(getter, obj, NULL);
        if (v != NULL) {
            return v;
        }
    }
    if (field != NULL && il2cpp_field_get_value) {
        void *v = NULL;
        il2cpp_field_get_value(obj, field, &v);
        return v;
    }
    return NULL;
}

/* Writes the spoof straight into the local player's NameplateView. Cheap
   enough to re-run on a timer, which is how it survives the nameplate being
   rebuilt on every map change (createNameTag). */
static void apply_spoof(int verbose)
{
    if (g_get_main_player == NULL || g_nametagview_field == NULL || !il2cpp_field_get_value) {
        return;
    }
    g_spoof_str = mstr_hold(g_spoof_str, g_spoof[0] ? il2cpp_string_new(g_spoof) : NULL);
    void *mp = inv(g_get_main_player, NULL, NULL);
    if (mp == NULL) {
        if (verbose) {
            LOGE("spoof: no local player yet");
        }
        return;
    }
    void *view = NULL;
    il2cpp_field_get_value(mp, g_nametagview_field, &view);
    if (view == NULL) {
        if (verbose) {
            LOGE("spoof: local player has no nameplate yet");
        }
        return;
    }

    if (g_view_set_name != NULL) {
        void *str = g_spoof[0] ? il2cpp_string_new(g_spoof)
                               : read_str_member(mp, g_get_name, g_name_field);
        if (str != NULL) {
            void *args[1] = {str};
            inv(g_view_set_name, view, args);
        }
    }

    if (g_view_set_title != NULL && g_view_set_title_visible != NULL) {
        void *str = g_title[0] ? il2cpp_string_new(g_title)
                               : read_str_member(mp, NULL, g_title_field);
        char real[40];
        mstr_to_utf8(str, real, sizeof(real));
        uint8_t visible = (g_title[0] || real[0]) ? 1 : 0;
        void *vis_args[1] = {&visible};
        inv(g_view_set_title_visible, view, vis_args);
        if (visible && str != NULL) {
            void *args[1] = {str};
            inv(g_view_set_title, view, args);
        }
    }

    if (verbose) {
        LOGI("spoof: applied name='%s' title='%s'", g_spoof[0] ? g_spoof : "(real)",
             g_title[0] ? g_title : "(real)");
    }
}

/* UIPlayerPanel.setText() runs every Update and does `nameText.text =
   target.Name`, so the only way to keep a spoof on the HUD is to overwrite it
   after the fact - the same postfix the desktop agent uses. Display only: the
   real Entity.Name is never touched, because other code still depends on it. */
static void *(*orig_panel_settext)(void *self, void *method);

static void *hook_panel_settext(void *self, void *method)
{
    void *r = orig_panel_settext(self, method);
    /* Cached managed string: this runs per frame, so allocating one here would
       be needless GC churn. */
    void *str = mstr_get(g_spoof_str);
    if (str == NULL || self == NULL || g_panel_target_field == NULL ||
        g_panel_nametext_field == NULL || g_tmp_set_text == NULL ||
        g_get_main_player == NULL || !il2cpp_field_get_value) {
        return r;
    }
    void *target = NULL;
    il2cpp_field_get_value(self, g_panel_target_field, &target);
    if (target == NULL || target != inv(g_get_main_player, NULL, NULL)) {
        return r; /* somebody else's panel */
    }
    void *label = NULL;
    il2cpp_field_get_value(self, g_panel_nametext_field, &label);
    if (label != NULL) {
        void *args[1] = {str};
        inv(g_tmp_set_text, label, args);
    }
    return r;
}

/* The game rebuilds nameplates on map change, so re-assert rather than relying
   on a one-shot apply. */
static void spoof_tick(void)
{
    if (g_spoof[0] == '\0' && g_title[0] == '\0') {
        return;
    }
    float now = inv_float(g_time_get_time, NULL, NULL);
    if (now < g_next_spoof) {
        return;
    }
    g_next_spoof = now + 1.0f;
    apply_spoof(0);
}

/* Replaces the host component's OnGUI. The original is deliberately not called:
   we created the only instance of it, nothing else in the game uses the type,
   and its own OnGUI would run against fields we never set. */
static void hook_host_ongui(void *self, void *method)
{
    (void)self;
    (void)method;
    if (g_gui_box == NULL || !il2cpp_string_new) {
        return;
    }

    /* Scale the whole UI, fonts included. Sizing rects up alone would leave the
       default font unreadable at this density. */
    if (g_gui_set_matrix != NULL) {
        float m[16] = {0};
        m[0] = g_scale;
        m[5] = g_scale;
        m[10] = 1.0f;
        m[15] = 1.0f;
        void *args[1] = {m};
        inv(g_gui_set_matrix, NULL, args);
    }

    keyboard_poll();

    /* Coordinates below are logical; GUI.matrix scales them to the screen. */
    if (gui_button(8, 8, 96, 30, g_menu_open ? "Beyond X" : "Beyond")) {
        g_menu_open = !g_menu_open;
    }
    if (!g_menu_open) {
        goto done;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "Beyond - packets %d", g_pkt_total);
    gui_text(g_gui_box, 8, 44, 344, 340, buf);

    if (gui_button(18, 78, 152, 30, g_block_incoming ? "Block: ON" : "Block: OFF")) {
        g_block_incoming = !g_block_incoming;
    }
    if (gui_button(176, 78, 92, 30, "Clear")) {
        g_pkt_count = 0;
        g_pkt_head = 0;
    }

    /* Row heights are generous on purpose: the scaled default font was clipping
       at the tighter spacing this started with. */
    snprintf(buf, sizeof(buf), "cmd: %s", g_input[0] ? g_input : "(tap Type)");
    gui_text(g_gui_label, 18, 112, 326, 24, buf);

    if (gui_button(18, 140, 84, 30, "Type")) {
        keyboard_open(KB_CMD);
    }
    if (gui_button(108, 140, 84, 30, "Send")) {
        send_typed_packet();
    }
    if (gui_button(198, 140, 84, 30, g_help_open ? "Help X" : "Help")) {
        g_help_open = !g_help_open;
        g_log_open = 0; /* one side window at a time - they share the same slot */
        g_quest_select_open = 0;
    }

    if (gui_button(18, 176, 84, 30, g_log_open ? "Log X" : "Log")) {
        g_log_open = !g_log_open;
        g_help_open = 0;
        g_quest_select_open = 0;
    }
    if (gui_button(108, 176, 160, 30,
                   g_autoskills ? "Autoskills: ON" : "Autoskills: OFF")) {
        g_autoskills = !g_autoskills;
        g_skill_slot = 0;
        g_next_skill = 0.0f;
    }
    if (gui_button(272, 176, 130, 30,
                   g_autoskip_cutscenes ? "Cutscene Skip: ON" : "Cutscene Skip: OFF")) {
        g_autoskip_cutscenes = !g_autoskip_cutscenes;
    }

    if (gui_button(18, 212, 90, 30, "Name")) {
        keyboard_open(KB_SPOOF);
    }
    if (gui_button(114, 212, 90, 30, "Title")) {
        keyboard_open(KB_TITLE);
    }
    if (gui_button(210, 212, 90, 30, "Reset")) {
        g_spoof[0] = '\0';
        g_title[0] = '\0';
        apply_spoof(1);
    }
    snprintf(buf, sizeof(buf), "as: %s / %s", g_spoof[0] ? g_spoof : "(real)",
             g_title[0] ? g_title : "(real)");
    gui_text(g_gui_label, 18, 248, 326, 24, buf);

    /* Auto-hunt: nearest hostile, wall-aware approach (direct walk when the
       line is clear, A* route when it isn't - see the wall-aware movement
       section for how). Shares the cadence with autoskills so combat starts
       as soon as a target is in range. */
    if (gui_button(18, 280, 160, 30, g_hunt ? "Hunt: ON" : "Hunt: OFF")) {
        g_hunt = !g_hunt;
        g_next_hunt = 0.0f;
    }

    /* Quest farming: a select panel (same "one side window" slot as Log/Help)
       picks WHAT to work on, a separate Start/Stop switches whether it is
       currently running - changing your pick no longer requires stopping
       first, and starting doesn't require re-picking. Track acts on
       whatever UIQuestTracker.CurrentQuest already is (track/accept
       normally in-game, no ID entry); a chain transfers to its map -
       waiting for confirmed arrival before accepting anything, see
       quest_tick's own comment on why that wait matters on live AE - and
       works a baked-in quest list start to finish. Either way this forces
       Hunt on for kill-count objectives and dispatches Interact/Apop
       objectives - see the quest farming section's own comment for what it
       does not cover (Cutscene, cross-cell travel to a machine/NPC). */
    const char *selected_label =
        g_quest_selected == 0 ? "Track Current" : CHAINS[g_quest_selected - 1].name;
    snprintf(buf, sizeof(buf), "Quest: %s", selected_label);
    if (gui_button(18, 316, 160, 30, buf)) {
        g_quest_select_open = !g_quest_select_open;
        g_log_open = 0;
        g_help_open = 0;
    }
    if (gui_button(184, 316, 90, 30, g_quest_running ? "Stop" : "Start")) {
        g_quest_running = !g_quest_running;
        g_quest_accept_sent = 0;
        g_quest_turnin_sent = 0;
        g_chain_index = 0;
        g_chain_tfer_sent = 0;
        g_chain_arrived_at = 0.0f;
        g_next_quest_tick = 0.0f;
        g_next_getquests_request = 0.0f;
        g_quest_last_id = 0;
        g_quest_last_activity_at = g_time_get_time ? inv_float(g_time_get_time, NULL, NULL) : 0.0f;
        g_interact_approach_active = 0;
        snprintf(g_quest_status, sizeof(g_quest_status), "%s",
                g_quest_running ? "starting" : "stopped");
    }
    snprintf(buf, sizeof(buf), "quest: %s", g_quest_status);
    gui_text(g_gui_label, 18, 352, 326, 24, buf);

    /* Packet log, in its own window rather than crowding the tools panel. */
    if (g_log_open) {
        snprintf(buf, sizeof(buf), "Packets (%d)   < in  > out  x blocked", g_pkt_total);
        gui_text(g_gui_box, 360, 44, 340, 320, buf);
        for (int i = 0; i < g_pkt_count; i++) {
            gui_text(g_gui_label, 370, 78.0f + (float)i * 20.0f, 320, 20, pkt_row(i));
        }
        if (gui_button(370, 326, 96, 30, "Close")) {
            g_log_open = 0;
        }
    }

    if (g_help_open) {
        gui_text(g_gui_box, 360, 44, 340, 356, "Tap a command to load it");
        int start = g_help_page * HELP_ROWS;
        for (int i = 0; i < HELP_ROWS && start + i < CMD_COUNT; i++) {
            if (gui_button(370, 78.0f + (float)i * 26.0f, 240, 24, CMDS[start + i])) {
                snprintf(g_input, sizeof(g_input), "%s", CMDS[start + i]);
            }
        }
        snprintf(buf, sizeof(buf), "%d/%d", g_help_page + 1,
                 (CMD_COUNT + HELP_ROWS - 1) / HELP_ROWS);
        gui_text(g_gui_label, 620, 78, 60, 24, buf);
        if (gui_button(370, 348, 76, 30, "Prev") && g_help_page > 0) {
            g_help_page--;
        }
        if (gui_button(452, 348, 76, 30, "Next") &&
            (g_help_page + 1) * HELP_ROWS < CMD_COUNT) {
            g_help_page++;
        }
        if (gui_button(534, 348, 86, 30, "Close")) {
            g_help_open = 0;
        }
    }

    if (g_quest_select_open) {
        gui_text(g_gui_box, 360, 44, 340, 356, "Tap what to work on");
        if (gui_button(370, 78, 240, 30,
                       g_quest_selected == 0 ? "> Track Current" : "Track Current")) {
            g_quest_selected = 0;
            g_quest_select_open = 0;
        }
        for (int i = 0; i < CHAIN_COUNT; i++) {
            char label[48];
            snprintf(label, sizeof(label), "%s%s", g_quest_selected == i + 1 ? "> " : "",
                    CHAINS[i].name);
            if (gui_button(370, 112.0f + (float)i * 34.0f, 240, 30, label)) {
                g_quest_selected = i + 1;
                g_quest_select_open = 0;
            }
        }
        if (gui_button(370, 348, 96, 30, "Close")) {
            g_quest_select_open = 0;
        }
    }

done:
    if (!g_draw_logged) {
        g_draw_logged = 1;
        LOGI("menu: first draw ok (scale %.2f)", (double)g_scale);
    }
}

/* -------------------------------------------------------------------------
 * Autoskills
 *
 * Mirrors BeyondAgentClass: UISkillSlots.GetSlot(i) then UseSkill(true) and
 * UseSkill(false), gated by skill_disabled()/skill_on_cooldown() the same way
 * the desktop agent's IsSkillSlotButtonDisabled/IsSkillOnCooldown gate it -
 * firing into a greyed-out or cooling-down slot just spends a server round
 * trip on a packet that gets rejected. Runs from the AEC.Update tick because
 * it must be on Unity's main thread.
 * ---------------------------------------------------------------------- */
static void autoskills_tick(void)
{
    if (!g_autoskills || g_skillslots == NULL || g_get_slot == NULL || g_use_skill == NULL) {
        return;
    }
    float now = inv_float(g_time_get_time, NULL, NULL);
    if (now < g_next_skill) {
        return;
    }

    /* Scan forward for a slot that is actually ready, same as the desktop's
       combo walk: skip disabled/cooling-down slots without wasting the cast
       cadence on them, but don't spin forever within one frame if nothing is
       up yet. */
    for (int tries = 0; tries < 5; tries++) {
        int32_t slot = g_skill_slot;
        void *slot_args[1] = {&slot};
        void *btn = inv(g_get_slot, g_skillslots, slot_args);
        g_skill_slot = (g_skill_slot + 1) % 5;

        if (btn == NULL) {
            continue;
        }
        if (skill_disabled(btn) || skill_on_cooldown(btn)) {
            continue;
        }

        uint8_t down = 1, up = 0;
        void *a_down[1] = {&down};
        void *a_up[1] = {&up};
        inv(g_use_skill, btn, a_down);
        inv(g_use_skill, btn, a_up);
        g_next_skill = now + 0.6f;
        return;
    }

    /* Nothing in the rotation was ready. Re-check soon rather than idling for
       a full cast cycle - matches the desktop agent's 100ms cooldown retry. */
    g_next_skill = now + 0.1f;
}

/* UISkillSlots derives from Singleton<T>, whose static Instance lives on an
   inflated generic type that is awkward to resolve from native. Capturing the
   instance from a method it calls anyway is simpler and just as reliable. */
static void *(*orig_register)(void *self, void *sb, void *method);

static void *hook_register_slot(void *self, void *sb, void *method)
{
    g_skillslots = self;
    return orig_register(self, sb, method);
}

/* Cutscene auto-skip, porting BeyondAgent.Patches.CutsceneSkipPatch: a
   Harmony postfix on Dialogger_Manager.StartCutscene that calls EndPressed()
   one frame later (deferred because StartCutscene kicks off async asset
   loads that EndPressed's page-state check expects to exist yet). EndPressed
   is the same call the in-game "End" button makes, so DoCompleteActions
   still runs - quest hooks, item grants, whatever the cutscene's own
   completeActions do - unlike just dropping the getDialog/getCutscene packet
   outright, which would skip those too and silently stall progression.
   There is no native inline-hook equivalent of "yield return null", so the
   defer here is "consumed on the next AEC.Update tick" instead of exactly
   one Unity frame - AEC.Update runs every frame anyway, so in practice this
   is the same wait, just not frame-exact. */
static void *hook_start_cutscene(void *self, void *method)
{
    void *r = orig_start_cutscene(self, method);
    if (g_autoskip_cutscenes && self != NULL) {
        g_pending_cutscene_mgr = self;
    }
    return r;
}

static void cutscene_skip_tick(void)
{
    if (g_pending_cutscene_mgr == NULL) {
        return;
    }
    void *mgr = g_pending_cutscene_mgr;
    g_pending_cutscene_mgr = NULL; /* consume before invoking - EndPressed must not re-trigger this */
    if (g_dialogger_endpressed != NULL) {
        inv(g_dialogger_endpressed, mgr, NULL);
        LOGI("cutscene: auto-skipped");
    }
}

/* Runs on Unity's main thread, from the AEC.Update hook. */
static void setup_menu(void *domain,
                       il2cpp_domain_assembly_open_t assembly_open,
                       il2cpp_assembly_get_image_t assembly_image,
                       il2cpp_class_from_name_t class_from_name)
{
    if (g_host_class == NULL || g_host_ongui == NULL) {
        LOGE("menu: no OnGUI host component found - cannot draw");
        return;
    }
    if (!il2cpp_object_new || !il2cpp_runtime_object_init || !il2cpp_class_get_type ||
        !il2cpp_type_get_object || !il2cpp_string_new) {
        LOGE("menu: object-creation exports missing");
        return;
    }

    void *core = assembly_open(domain, "UnityEngine.CoreModule");
    void *imgui = assembly_open(domain, "UnityEngine.IMGUIModule");
    if (core == NULL || imgui == NULL) {
        LOGE("menu: UnityEngine assemblies not found");
        return;
    }
    void *core_image = assembly_image(core);
    void *go_class = class_from_name(core_image, "UnityEngine", "GameObject");
    void *obj_class = class_from_name(core_image, "UnityEngine", "Object");
    void *gui_class = class_from_name(assembly_image(imgui), "UnityEngine", "GUI");
    if (go_class == NULL || obj_class == NULL || gui_class == NULL) {
        LOGE("menu: GameObject/Object/GUI not resolvable");
        return;
    }

    /* What survives strip-engine-code is a property of this build, not of
       Unity. Measured here: Box/2 and Label/2 keep their (Rect,string) forms,
       Button keeps only (Rect,GUIContent,GUIStyle), and TextField is gone
       entirely - hence the OS keyboard for input. */
    void *imgui_image = assembly_image(imgui);
    g_gui_box = find_method(gui_class, "Box", 2, 1, "String");
    g_gui_label = find_method(gui_class, "Label", 2, 1, "String");
    g_gui_button = find_method(gui_class, "Button", 3, 2, "GUIStyle");
    g_gui_set_matrix = find_method(gui_class, "set_matrix", 1, 0, NULL);
    g_gui_get_skin = find_method(gui_class, "get_skin", 0, 0, NULL);
    void *skin_class = class_from_name(imgui_image, "UnityEngine", "GUISkin");
    g_skin_get_button = find_method(skin_class, "get_button", 0, 0, NULL);
    g_content_class = class_from_name(imgui_image, "UnityEngine", "GUIContent");
    g_content_ctor = find_method(g_content_class, ".ctor", 1, 0, "String");
    LOGI("menu: Box=%p Label=%p Button=%p matrix=%p skin=%p style=%p content=%p/%p", g_gui_box,
         g_gui_label, g_gui_button, g_gui_set_matrix, g_gui_get_skin, g_skin_get_button,
         g_content_class, g_content_ctor);
    if (g_gui_box == NULL) {
        LOGE("menu: no GUI.Box(Rect, string) - nothing can be drawn");
        return;
    }
    if (g_gui_label == NULL) {
        g_gui_label = g_gui_box; /* Box reads fine as a row background */
    }

    void *tsk = class_from_name(core_image, "UnityEngine", "TouchScreenKeyboard");
    g_tsk_open = find_method(tsk, "Open", 5, 0, "String");
    g_tsk_get_text = find_method(tsk, "get_text", 0, 0, NULL);
    g_tsk_get_status = find_method(tsk, "get_status", 0, 0, NULL);
    LOGI("menu: keyboard Open=%p text=%p status=%p", g_tsk_open, g_tsk_get_text,
         g_tsk_get_status);

    /* Density-derived so the menu is the same physical size on any screen.
       Screen.dpi can legitimately return 0, hence the fallback and clamp. */
    void *screen = class_from_name(core_image, "UnityEngine", "Screen");
    float dpi = screen ? inv_float(find_method(screen, "get_dpi", 0, 0, NULL), NULL, NULL) : 0.0f;
    g_scale = dpi > 1.0f ? dpi / 160.0f : 2.0f;
    if (g_scale < 1.0f) {
        g_scale = 1.0f;
    }
    if (g_scale > 4.0f) {
        g_scale = 4.0f;
    }
    LOGI("menu: dpi %.0f -> scale %.2f", (double)dpi, (double)g_scale);

    /* Packet sender: Request(string) plus the field the send hook reads. */
    if (g_cs_image != NULL) {
        g_request_class = class_from_name(g_cs_image, "", "Request");
        if (g_request_class != NULL) {
            g_request_ctor = find_method(g_request_class, ".ctor", 1, 0, "String");
            if (il2cpp_class_get_field_from_name) {
                g_request_cmd_field = il2cpp_class_get_field_from_name(g_request_class, "Cmd");
            }
        }
    }
    if (g_aec_class != NULL) {
        g_send_request = il2cpp_class_get_method_from_name(g_aec_class, "sendRequest", 1);
        void *code = g_send_request ? method_code_ptr(g_send_request) : NULL;
        if (code != NULL) {
            hook_func("AEC.sendRequest", code, (void *)hook_send_request,
                      (void **)&orig_send_request);
        }
    }
    LOGI("menu: Request=%p ctor=%p Cmd=%p sendRequest=%p", g_request_class, g_request_ctor,
         g_request_cmd_field, g_send_request);

    /* Quest farming: Quest.Get/IsReadyForTurnin, Player.IsQuestAccepted, and
       the two concrete Request subclasses whose own constructors build the
       List<string> Params internally - sidesteps ever needing to construct
       a generic List<string> from native code ourselves. */
    if (g_cs_image != NULL) {
        void *quest_class = class_from_name(g_cs_image, "", "Quest");
        void *tracker_class = class_from_name(g_cs_image, "", "UIQuestTracker");
        void *player_class = class_from_name(g_cs_image, "", "Player");
        if (quest_class != NULL) {
            g_quest_get = il2cpp_class_get_method_from_name(quest_class, "Get", 1);
            g_quest_get_id = il2cpp_class_get_method_from_name(quest_class, "get_ID", 0);
            g_quest_is_ready_turnin =
                il2cpp_class_get_method_from_name(quest_class, "IsReadyForTurnin", 0);
            if (il2cpp_class_get_field_from_name) {
                g_quest_npcid_field = il2cpp_class_get_field_from_name(quest_class, "NPCID");
            }
        }
        if (tracker_class != NULL) {
            g_uiquesttracker_get_currentquest =
                il2cpp_class_get_method_from_name(tracker_class, "get_CurrentQuest", 0);
        }
        g_player_is_quest_accepted =
            il2cpp_class_get_method_from_name(player_class, "IsQuestAccepted", 1);
        g_req_accept_class = class_from_name(g_cs_image, "", "RequestQuestAccept");
        g_req_accept_ctor = find_method(g_req_accept_class, ".ctor", 1, 0, NULL);
        g_req_turnin_class = class_from_name(g_cs_image, "", "RequestTryQuestComplete");
        g_req_turnin_ctor = find_method(g_req_turnin_class, ".ctor", 2, 0, NULL);
        g_req_transfer_class = class_from_name(g_cs_image, "", "RequestMoveToArea");
        g_req_transfer_ctor = find_method(g_req_transfer_class, ".ctor", 5, 0, NULL);
        g_req_cutscene_class = class_from_name(g_cs_image, "", "RequestWatchCutscene");
        g_req_cutscene_ctor = find_method(g_req_cutscene_class, ".ctor", 1, 0, NULL);
        g_req_movecell_class = class_from_name(g_cs_image, "", "RequestMoveToCell");
        g_req_movecell_ctor = find_method(g_req_movecell_class, ".ctor", 2, 0, NULL);
        g_req_apopqo_class = class_from_name(g_cs_image, "", "RequestOpenApopQO");
        g_req_apopqo_ctor = find_method(g_req_apopqo_class, ".ctor", 2, 0, NULL);
        LOGI("menu: quest Get=%p CurrentQuest=%p get_ID=%p IsReadyForTurnin=%p "
             "IsQuestAccepted=%p Accept=%p/%p TurnIn=%p/%p Transfer=%p/%p Cutscene=%p/%p "
             "MoveCell=%p/%p",
             g_quest_get, g_uiquesttracker_get_currentquest, g_quest_get_id,
             g_quest_is_ready_turnin, g_player_is_quest_accepted, g_req_accept_class,
             g_req_accept_ctor, g_req_turnin_class, g_req_turnin_ctor, g_req_transfer_class,
             g_req_transfer_ctor, g_req_cutscene_class, g_req_cutscene_ctor,
             g_req_movecell_class, g_req_movecell_ctor);

        /* Objective dispatch: Interact (machine click) and Apop/Talk (NPC
           click). See next_incomplete_objective()/find_machine_in_subtree()/
           find_apop_npc() for how these get used. */
        void *entity_c = class_from_name(g_cs_image, "", "Entity");
        void *area_c = class_from_name(g_cs_image, "", "Area");
        void *qti_class = class_from_name(g_cs_image, "", "QuestTurninItem");
        void *pqd_class = class_from_name(g_cs_image, "", "PlayerQuestData");
        void *mapmachine_class = class_from_name(g_cs_image, "", "MapMachine");
        void *npcbutton_class = class_from_name(g_cs_image, "", "NPCButton");
        void *component_class = class_from_name(core_image, "UnityEngine", "Component");
        void *tf_class = class_from_name(core_image, "UnityEngine", "Transform");
        void *mscorlib_asm = assembly_open(domain, "mscorlib");
        if (mscorlib_asm != NULL) {
            g_system_array_class = class_from_name(assembly_image(mscorlib_asm), "System",
                                                   "Array");
        }
        LOGI("menu: objectives System.Array=%p", g_system_array_class);

        if (quest_class != NULL) {
            g_quest_turnins_field = il2cpp_class_get_field_from_name
                                        ? il2cpp_class_get_field_from_name(quest_class, "Turnins")
                                        : NULL;
        }
        if (qti_class != NULL && il2cpp_class_get_field_from_name) {
            g_qti_qoid_field = il2cpp_class_get_field_from_name(qti_class, "QOID");
            g_qti_qotype_field = il2cpp_class_get_field_from_name(qti_class, "QOType");
            g_qti_getrefint_method = il2cpp_class_get_method_from_name(qti_class, "GetRefInt", 1);
            g_qti_refscontains_method =
                il2cpp_class_get_method_from_name(qti_class, "RefsContains", 1);
            g_qti_refarray_field = il2cpp_class_get_field_from_name(qti_class, "RefArray");
        }
        if (player_class != NULL && il2cpp_class_get_field_from_name) {
            g_player_quests_field = il2cpp_class_get_field_from_name(player_class, "Quests");
        }
        if (pqd_class != NULL) {
            g_pqd_is_objective_complete =
                il2cpp_class_get_method_from_name(pqd_class, "IsObjectiveComplete", 1);
            g_pqd_is_quest_complete =
                il2cpp_class_get_method_from_name(pqd_class, "isQuestComplete", 1);
        }
        if (area_c != NULL) {
            if (il2cpp_class_get_field_from_name) {
                g_area_cells_field = il2cpp_class_get_field_from_name(area_c, "Cells");
            }
            g_area_get_mapname = il2cpp_class_get_method_from_name(area_c, "get_mapName", 0);
        }
        if (entity_c != NULL && il2cpp_class_get_field_from_name) {
            g_entity_frame_field = il2cpp_class_get_field_from_name(entity_c, "Frame");
            g_entity_apopid_field = il2cpp_class_get_field_from_name(entity_c, "apopID");
        }
        if (component_class != NULL) {
            g_component_get_transform =
                il2cpp_class_get_method_from_name(component_class, "get_transform", 0);
        }
        if (tf_class != NULL) {
            g_transform_get_childcount =
                il2cpp_class_get_method_from_name(tf_class, "get_childCount", 0);
            g_transform_get_child = il2cpp_class_get_method_from_name(tf_class, "GetChild", 1);
            g_transform_get_gameobject =
                il2cpp_class_get_method_from_name(tf_class, "get_gameObject", 0);
        }
        if (obj_class != NULL) {
            g_object_get_name = il2cpp_class_get_method_from_name(obj_class, "get_name", 0);
        }
        if (mapmachine_class != NULL) {
            g_mapmachine_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(mapmachine_class));
            g_mapmachine_interact = il2cpp_class_get_method_from_name(mapmachine_class, "Interact", 0);
        }
        if (npcbutton_class != NULL) {
            g_npcbutton_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(npcbutton_class));
            g_npcbutton_interact = il2cpp_class_get_method_from_name(npcbutton_class, "Interact", 0);
        }
        LOGI("menu: objectives Turnins=%p QOID=%p QOType=%p GetRefInt=%p RefsContains=%p "
             "Quests=%p IsObjComplete=%p Cells=%p Frame=%p apopID=%p",
             g_quest_turnins_field, g_qti_qoid_field, g_qti_qotype_field, g_qti_getrefint_method,
             g_qti_refscontains_method, g_player_quests_field, g_pqd_is_objective_complete,
             g_area_cells_field, g_entity_frame_field, g_entity_apopid_field);
        LOGI("menu: objectives Component.transform=%p childCount=%p GetChild=%p "
             "tf.gameObject=%p Object.name=%p MapMachine=%p/%p NPCButton=%p/%p mapName=%p",
             g_component_get_transform, g_transform_get_childcount, g_transform_get_child,
             g_transform_get_gameobject, g_object_get_name, g_mapmachine_type_obj,
             g_mapmachine_interact, g_npcbutton_type_obj, g_npcbutton_interact,
             g_area_get_mapname);
    }

    /* Autoskills: UISkillSlots.GetSlot(int) + SkillSlotButton.UseSkill(bool),
       with the singleton captured from its own Register call. */
    g_time_get_time = find_method(class_from_name(core_image, "UnityEngine", "Time"),
                                  "get_time", 0, 0, NULL);
    if (g_cs_image != NULL) {
        void *slots = class_from_name(g_cs_image, "", "UISkillSlots");
        void *slot_btn = class_from_name(g_cs_image, "", "SkillSlotButton");
        g_get_slot = find_method(slots, "GetSlot", 1, 0, NULL);
        g_use_skill = find_method(slot_btn, "UseSkill", 1, 0, NULL);
        void *reg = find_method(slots, "Register", 1, 0, NULL);
        void *reg_code = reg ? method_code_ptr(reg) : NULL;
        if (reg_code != NULL) {
            hook_func("UISkillSlots.Register", reg_code, (void *)hook_register_slot,
                      (void **)&orig_register);
        }
        if (il2cpp_class_get_field_from_name) {
            g_slotbtn_disabled_field = il2cpp_class_get_field_from_name(slot_btn, "disabled");
            g_slotbtn_pendingcd_field =
                il2cpp_class_get_field_from_name(slot_btn, "pendingCooldown");
            g_slotbtn_cooldown_field = il2cpp_class_get_field_from_name(slot_btn, "cooldown");
        }
        LOGI("menu: skills GetSlot=%p UseSkill=%p Register=%p time=%p disabled=%p "
             "pendingCooldown=%p cooldown=%p",
             g_get_slot, g_use_skill, reg, g_time_get_time, g_slotbtn_disabled_field,
             g_slotbtn_pendingcd_field, g_slotbtn_cooldown_field);

        /* Cutscene auto-skip: hook StartCutscene, resolve EndPressed to fire
           on the next tick. See hook_start_cutscene/cutscene_skip_tick. */
        void *dialogger = class_from_name(g_cs_image, "", "Dialogger_Manager");
        void *start_cs = find_method(dialogger, "StartCutscene", 0, 0, NULL);
        void *start_cs_code = start_cs ? method_code_ptr(start_cs) : NULL;
        if (start_cs_code != NULL) {
            hook_func("Dialogger_Manager.StartCutscene", start_cs_code,
                      (void *)hook_start_cutscene, (void **)&orig_start_cutscene);
        }
        g_dialogger_endpressed = find_method(dialogger, "EndPressed", 0, 0, NULL);
        LOGI("menu: cutscene StartCutscene=%p EndPressed=%p", start_cs, g_dialogger_endpressed);

        /* Nameplate spoof: replace what Player.ComposeNameplateText returns. */
        void *player = class_from_name(g_cs_image, "", "Player");
        void *entity = class_from_name(g_cs_image, "", "Entity");
        void *view = class_from_name(g_cs_image, "", "NameplateView");
        g_get_main_player = find_method(entity, "get_mainPlayer", 0, 0, NULL);
        /* find_method enumerates a class's OWN methods only, and Name is a
           virtual property declared on Entity - so look there. Invoking it on a
           Player still dispatches virtually. */
        g_get_name = find_method(entity, "get_Name", 0, 0, NULL);
        if (g_get_name == NULL) {
            g_get_name = find_method(player, "get_Name", 0, 0, NULL);
        }
        if (il2cpp_class_get_field_from_name) {
            g_nametagview_field = il2cpp_class_get_field_from_name(player, "nameTagView");
            g_title_field = il2cpp_class_get_field_from_name(player, "Title");
            /* Unlike get_method_from_name, field lookup does NOT walk base
               classes, and Name is declared on Entity rather than Player. */
            g_name_field = il2cpp_class_get_field_from_name(player, "Name");
            if (g_name_field == NULL) {
                g_name_field = il2cpp_class_get_field_from_name(entity, "Name");
            }
            if (g_title_field == NULL) {
                g_title_field = il2cpp_class_get_field_from_name(entity, "Title");
            }
        }
        /* Top-left HUD panel. */
        void *panel = class_from_name(g_cs_image, "", "UIPlayerPanel");
        if (il2cpp_class_get_field_from_name) {
            g_panel_target_field = il2cpp_class_get_field_from_name(panel, "target");
            g_panel_nametext_field = il2cpp_class_get_field_from_name(panel, "nameText");
        }
        void *tmp_asm = assembly_open(domain, "Unity.TextMeshPro");
        void *tmp_text = tmp_asm ? class_from_name(assembly_image(tmp_asm), "TMPro", "TMP_Text")
                                 : NULL;
        g_tmp_set_text = find_method(tmp_text, "set_text", 1, 0, "String");
        void *settext = find_method(panel, "setText", 0, 0, NULL);
        void *settext_code = settext ? method_code_ptr(settext) : NULL;
        if (settext_code != NULL) {
            hook_func("UIPlayerPanel.setText", settext_code, (void *)hook_panel_settext,
                      (void **)&orig_panel_settext);
        }
        LOGI("menu: hud panel=%p target=%p nameText=%p set_text=%p setText=%p", panel,
             g_panel_target_field, g_panel_nametext_field, g_tmp_set_text, settext);

        g_view_set_name = find_method(view, "SetName", 1, 0, "String");
        g_view_set_title = find_method(view, "SetTitle", 1, 0, "String");
        g_view_set_title_visible = find_method(view, "SetTitleVisible", 1, 0, NULL);
        LOGI("menu: spoof mainPlayer=%p view=%p field=%p SetName=%p SetTitle=%p vis=%p "
             "Name=%p/%p Title=%p",
             g_get_main_player, view, g_nametagview_field, g_view_set_name, g_view_set_title,
             g_view_set_title_visible, g_get_name, g_name_field, g_title_field);

        /* Auto-hunt bindings: targeting (Targetable.ClickMe), movement
           (EntityMovementUpdater.walkTo) and position reads (Transform).
           GetComponent(Type) needs a cached Type object per component type -
           the same AddComponent(Type) pattern the menu's own host component
           uses below. */
        g_entity_getgameobject = find_method(entity, "getGameObject", 0, 0, NULL);
        g_entity_get_target = find_method(entity, "get_target", 0, 0, NULL);
        void *transform_class = class_from_name(core_image, "UnityEngine", "Transform");
        g_go_get_transform = find_method(go_class, "get_transform", 0, 0, NULL);
        g_go_getcomponent = find_method(go_class, "GetComponent", 1, 0, "Type");
        g_transform_get_localpos = find_method(transform_class, "get_localPosition", 0, 0, NULL);

        g_targetable_class = class_from_name(g_cs_image, "", "Targetable");
        g_targetable_clickme = find_method(g_targetable_class, "ClickMe", 0, 0, NULL);
        if (g_targetable_class != NULL) {
            g_targetable_type_obj =
                il2cpp_type_get_object(il2cpp_class_get_type(g_targetable_class));
        }

        g_emu_class = class_from_name(g_cs_image, "", "EntityMovementUpdater");
        g_emu_walkto = find_method(g_emu_class, "walkTo", 2, 0, NULL);
        if (g_emu_class != NULL) {
            g_emu_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(g_emu_class));
            if (il2cpp_class_get_field_from_name) {
                g_emu_cellspeed_field = il2cpp_class_get_field_from_name(g_emu_class, "cellSpeed");
            }
        }
        LOGI("menu: hunt getGameObject=%p get_target=%p get_transform=%p GetComponent=%p "
             "get_localPosition=%p Targetable=%p/%p/%p EMU=%p/%p/%p/%p",
             g_entity_getgameobject, g_entity_get_target, g_go_get_transform, g_go_getcomponent,
             g_transform_get_localpos, g_targetable_class, g_targetable_type_obj,
             g_targetable_clickme, g_emu_class, g_emu_type_obj, g_emu_walkto,
             g_emu_cellspeed_field);

        /* Wall-aware movement bindings - see the module comment above
           path_player_parent() for why OverlapBox stands in for the
           (stripped) OverlapCircle, and which methods were confirmed
           present via probe_api before any of this was written. */
        void *phys2d = assembly_open(domain, "UnityEngine.Physics2DModule");
        if (phys2d != NULL) {
            void *p2d_image = assembly_image(phys2d);
            void *physics2d_class = class_from_name(p2d_image, "UnityEngine", "Physics2D");
            void *raycasthit2d_class =
                class_from_name(p2d_image, "UnityEngine", "RaycastHit2D");
            g_p2d_raycast = find_method(physics2d_class, "Raycast", 4, 2, "Single");
            g_p2d_overlapbox = find_method(physics2d_class, "OverlapBox", 4, 2, "Single");
            g_raycasthit2d_get_collider = find_method(raycasthit2d_class, "get_collider", 0, 0,
                                                      NULL);
        } else {
            LOGE("menu: UnityEngine.Physics2DModule not found - wall-aware movement disabled");
        }
        g_transform_transformpoint = find_method(transform_class, "TransformPoint", 1, 0, NULL);
        g_transform_get_lossyscale = find_method(transform_class, "get_lossyScale", 0, 0, NULL);
        g_transform_get_parent = find_method(transform_class, "get_parent", 0, 0, NULL);
        g_transform_get_position = find_method(transform_class, "get_position", 0, 0, NULL);
        g_transform_inversetransformpoint =
            find_method(transform_class, "InverseTransformPoint", 1, 0, NULL);

        void *layermask_class = class_from_name(core_image, "UnityEngine", "LayerMask");
        void *name_to_layer = find_method(layermask_class, "NameToLayer", 1, 0, "String");
        if (name_to_layer != NULL && il2cpp_string_new) {
            void *args[1] = {il2cpp_string_new("Blocker")};
            int layer = inv_int(name_to_layer, NULL, args);
            g_blocker_mask = layer >= 0 ? (1 << layer) : -1;
        }
        LOGI("menu: pathing Raycast=%p OverlapBox=%p get_collider=%p TransformPoint=%p "
             "lossyScale=%p get_parent=%p BlockerMask=%d",
             g_p2d_raycast, g_p2d_overlapbox, g_raycasthit2d_get_collider,
             g_transform_transformpoint, g_transform_get_lossyscale, g_transform_get_parent,
             g_blocker_mask);
    }

    void *go = il2cpp_object_new(go_class);
    if (go == NULL) {
        LOGE("menu: GameObject allocation failed");
        return;
    }
    il2cpp_runtime_object_init(go); /* runs the parameterless ctor */

    void *exc = NULL;
    void *keep = find_method(obj_class, "DontDestroyOnLoad", 1, 0, NULL);
    if (keep != NULL) {
        void *args[1] = {go};
        il2cpp_runtime_invoke(keep, NULL, args, &exc);
    }

    void *add = find_method(go_class, "AddComponent", 1, 0, "Type");
    if (add == NULL) {
        LOGE("menu: no AddComponent(Type) overload");
        return;
    }
    void *host_type = il2cpp_type_get_object(il2cpp_class_get_type(g_host_class));
    void *args[1] = {host_type};
    exc = NULL;
    void *component = il2cpp_runtime_invoke(add, go, args, &exc);
    if (exc != NULL || component == NULL) {
        LOGE("menu: AddComponent failed (%s)", exc ? "threw" : "returned null");
        return;
    }

    void *code = method_code_ptr(g_host_ongui);
    if (code == NULL) {
        return;
    }
    static void *orig_ongui;
    if (!hook_func("host OnGUI", code, (void *)hook_host_ongui, &orig_ongui)) {
        return;
    }
    LOGI("menu: host %s attached, OnGUI hooked", il2cpp_class_get_name(g_host_class));
}

/* -------------------------------------------------------------------------
 * Main-thread tick
 * ---------------------------------------------------------------------- */
static void *(*orig_aec_update)(void *a0, void *a1);
static void *g_domain;
static il2cpp_domain_assembly_open_t g_assembly_open;
static il2cpp_assembly_get_image_t g_assembly_image;
static il2cpp_class_from_name_t g_class_from_name;

/* -------------------------------------------------------------------------
 * Monster dictionary probe (one-shot, diagnostic)
 *
 * Load-bearing question for any hunt/quest logic: can this shim walk a
 * generic Dictionary<int, Monster> - Area.currentArea.Monsters, straight off
 * the PC decomp - from native code? Nothing else in the shim has touched a
 * generic collection; every field/method so far has been on a plain type.
 * IL2CPP compiles each closed generic instantiation as its own concrete
 * class with its own vtable, so the plan is: read the field to get the
 * dictionary object, ask ITS class (not Dictionary<,> itself) for
 * GetEnumerator, then invoke MoveNext/get_Current against the UNBOXED
 * struct pointer - il2cpp_runtime_invoke's convention for a value-type
 * instance method is the address of the unboxed value, not the boxed
 * object handed back by a prior invoke. Same reasoning applies to reading
 * Key/Value off the boxed KeyValuePair<int,Monster> that get_Current
 * returns. Logged at every step so a wrong guess is diagnosable instead of
 * a silent zero.
 *
 * Retried every 2s (Time.get_time-gated, like spoof_tick) since
 * Area.currentArea is null until a map is actually loaded - failing before
 * that is expected, not a bug. Resolution (classes/fields/methods) happens
 * once; the walk itself retries indefinitely so live monster count/roster
 * tracks reality instead of freezing at whatever the first successful read
 * saw.
 * ---------------------------------------------------------------------- */
static float g_next_monster_probe;
static int g_monster_probe_resolved;   /* class/field/method lookups - these don't change */
static int g_monster_probe_fatal;      /* a lookup genuinely failed; no point retrying */

static void probe_monsters(void)
{
    if (g_monster_probe_fatal || g_cs_image == NULL || g_class_from_name == NULL) {
        return;
    }
    float now = inv_float(g_time_get_time, NULL, NULL);
    if (now < g_next_monster_probe) {
        return;
    }
    g_next_monster_probe = now + 2.0f;

    if (!g_monster_probe_resolved) {
        void *area_class = g_class_from_name(g_cs_image, "", "Area");
        void *monster_class = g_class_from_name(g_cs_image, "", "Monster");
        void *entity_class = g_class_from_name(g_cs_image, "", "Entity");
        if (area_class == NULL || monster_class == NULL || entity_class == NULL ||
            !il2cpp_class_get_field_from_name || !il2cpp_field_static_get_value ||
            !il2cpp_object_unbox || !il2cpp_object_get_class) {
            LOGE("monster probe: Area=%p Monster=%p Entity=%p - class/API lookup incomplete",
                 area_class, monster_class, entity_class);
            g_monster_probe_fatal = 1;
            return;
        }
        g_area_currentarea_field = il2cpp_class_get_field_from_name(area_class, "currentArea");
        g_area_monsters_field = il2cpp_class_get_field_from_name(area_class, "Monsters");
        g_monster_reaction_field =
            il2cpp_class_get_field_from_name(monster_class, "reactionType");
        /* Name is declared virtual on Entity, but unlike Player (which
           inherits it as-is), Monster overrides it with its own backing
           field - per the decomp, `public override string Name { get; set; }`.
           il2cpp_runtime_invoke calls the exact MethodInfo handed to it; it
           does not walk the vtable the way a C# virtual call would. Resolving
           this on Entity and invoking it on a Monster instance compiles and
           returns cleanly, but silently reads Entity's own never-set backing
           field - empty string, no exception, no hint anything is wrong. So
           resolve on Monster's own class, where the override actually lives. */
        g_entity_get_name = il2cpp_class_get_method_from_name(monster_class, "get_Name", 0);
        g_monster_get_monmapid = il2cpp_class_get_method_from_name(monster_class, "get_monMapID", 0);
        /* currentState is NOT overridden by Monster (only Name is, per the
           decomp), so resolving it on Entity is safe here - unlike get_Name
           above, there's no derived-class override to miss. */
        g_entity_get_currentstate =
            il2cpp_class_get_method_from_name(entity_class, "get_currentState", 0);
        /* Same reasoning as currentState: ID is virtual on Entity but only
           Player overrides it, so Entity's is the one a Monster actually
           runs. This is the MonID that Killcount RefArray entries name. */
        g_entity_get_id = il2cpp_class_get_method_from_name(entity_class, "get_ID", 0);
        LOGI("monster probe: currentArea field=%p Monsters field=%p reactionType field=%p "
             "get_Name=%p get_currentState=%p",
             g_area_currentarea_field, g_area_monsters_field, g_monster_reaction_field,
             g_entity_get_name, g_entity_get_currentstate);
        if (g_area_currentarea_field == NULL || g_area_monsters_field == NULL) {
            g_monster_probe_fatal = 1;
            return;
        }
        g_monster_probe_resolved = 1;
    }

    void *area = NULL;
    il2cpp_field_static_get_value(g_area_currentarea_field, &area);
    if (area == NULL) {
        LOGI("monster probe: Area.currentArea is null - no map loaded, retrying");
        return;
    }

    void *dict = NULL;
    il2cpp_field_get_value(area, g_area_monsters_field, &dict);
    if (dict == NULL) {
        LOGI("monster probe: currentArea=%p but Monsters dict is null, retrying", area);
        return;
    }

    void *dict_class = il2cpp_object_get_class(dict);
    void *get_enumerator = il2cpp_class_get_method_from_name(dict_class, "GetEnumerator", 0);
    void *boxed_enum = get_enumerator ? inv(get_enumerator, dict, NULL) : NULL;
    if (boxed_enum == NULL) {
        LOGE("monster probe: dict class=%p GetEnumerator=%p returned null, retrying", dict_class,
             get_enumerator);
        return;
    }
    void *enum_raw = il2cpp_object_unbox(boxed_enum);
    void *enum_class = il2cpp_object_get_class(boxed_enum);
    void *move_next = il2cpp_class_get_method_from_name(enum_class, "MoveNext", 0);
    void *get_current = il2cpp_class_get_method_from_name(enum_class, "get_Current", 0);
    if (enum_raw == NULL || move_next == NULL || get_current == NULL) {
        LOGE("monster probe: enumerator raw=%p MoveNext=%p get_Current=%p - giving up", enum_raw,
             move_next, get_current);
        g_monster_probe_fatal = 1;
        return;
    }

    int count = 0, hostile = 0;
    char sample[96] = "";
    for (int i = 0; i < 500 && inv_bool(move_next, enum_raw, NULL); i++) {
        count++;
        void *boxed_kv = inv(get_current, enum_raw, NULL);
        if (boxed_kv == NULL) {
            continue;
        }
        void *kv_raw = il2cpp_object_unbox(boxed_kv);
        void *kv_class = il2cpp_object_get_class(boxed_kv);
        void *get_value = kv_raw ? il2cpp_class_get_method_from_name(kv_class, "get_Value", 0)
                                 : NULL;
        void *mon = get_value ? inv(get_value, kv_raw, NULL) : NULL;
        if (mon == NULL) {
            continue;
        }
        if (g_monster_reaction_field != NULL) {
            int32_t reaction = 0;
            il2cpp_field_get_value(mon, g_monster_reaction_field, &reaction);
            if (reaction == 1) { /* ReactionType.Hostile, per the decomp enum */
                hostile++;
            }
        }
        if (count <= 5 && g_entity_get_name != NULL) {
            char nm[40];
            mstr_to_utf8(inv(g_entity_get_name, mon, NULL), nm, sizeof(nm));
            size_t used = strlen(sample);
            snprintf(sample + used, sizeof(sample) - used, "%s%s", used ? ", " : "", nm);
        }
    }

    LOGI("monster probe: RESULT %d monster(s), %d hostile - sample: %s", count, hostile,
         sample[0] ? sample : "(none)");
}

static void *hook_aec_update(void *a0, void *a1)
{
    void *r = orig_aec_update(a0, a1);
    g_aec_instance = a0; /* AEC.Update is an instance method: a0 is the AEC */
    autoskills_tick();
    spoof_tick();
    cutscene_skip_tick();
    quest_tick(); /* may turn hunting on - runs before hunt_tick so this tick sees it */
    hunt_tick();
    probe_monsters();
    if (!g_ui_ready) {
        g_ui_ready = 1; /* set first: a failed setup must not retry every frame */
        setup_menu(g_domain, g_assembly_open, g_assembly_image, g_class_from_name);
    }
    return r;
}

/* -------------------------------------------------------------------------
 * Startup
 * ---------------------------------------------------------------------- */
static void *beyond_thread(void *arg)
{
    (void)arg;

    void *lib = wait_for_library();
    if (lib == NULL) {
        LOGE("libil2cpp.so never appeared - Beyond not starting (game unaffected)");
        return NULL;
    }

    void *init = dlsym(lib, "il2cpp_init");
    if (init == NULL || !hook_func("il2cpp_init", init, (void *)hook_il2cpp_init,
                                   (void **)&orig_il2cpp_init)) {
        LOGE("could not hook il2cpp_init - Beyond not starting (game unaffected)");
        return NULL;
    }
    for (int i = 0; i < 6000 && !g_runtime_ready; i++) {
        usleep(10 * 1000);
    }
    if (!g_runtime_ready) {
        LOGE("il2cpp_init never completed - Beyond not starting (game unaffected)");
        return NULL;
    }

    il2cpp_domain_get_t domain_get = (il2cpp_domain_get_t)dlsym(lib, "il2cpp_domain_get");
    il2cpp_thread_attach_t thread_attach =
        (il2cpp_thread_attach_t)dlsym(lib, "il2cpp_thread_attach");
    il2cpp_domain_assembly_open_t assembly_open =
        (il2cpp_domain_assembly_open_t)dlsym(lib, "il2cpp_domain_assembly_open");
    il2cpp_assembly_get_image_t assembly_image =
        (il2cpp_assembly_get_image_t)dlsym(lib, "il2cpp_assembly_get_image");
    il2cpp_class_from_name_t class_from_name =
        (il2cpp_class_from_name_t)dlsym(lib, "il2cpp_class_from_name");
    il2cpp_class_get_method_from_name =
        (il2cpp_class_get_method_from_name_t)dlsym(lib, "il2cpp_class_get_method_from_name");
    il2cpp_object_get_class = (il2cpp_object_get_class_t)dlsym(lib, "il2cpp_object_get_class");
    il2cpp_class_get_name = (il2cpp_class_get_name_t)dlsym(lib, "il2cpp_class_get_name");
    il2cpp_runtime_invoke = (il2cpp_runtime_invoke_t)dlsym(lib, "il2cpp_runtime_invoke");
    il2cpp_string_chars = (il2cpp_string_chars_t)dlsym(lib, "il2cpp_string_chars");
    il2cpp_string_length = (il2cpp_string_length_t)dlsym(lib, "il2cpp_string_length");
    il2cpp_class_get_methods =
        (il2cpp_class_get_methods_t)dlsym(lib, "il2cpp_class_get_methods");
    il2cpp_method_get_name = (il2cpp_method_get_name_t)dlsym(lib, "il2cpp_method_get_name");
    il2cpp_class_get_namespace =
        (il2cpp_class_get_namespace_t)dlsym(lib, "il2cpp_class_get_namespace");
    il2cpp_method_get_param_count =
        (il2cpp_method_get_param_count_t)dlsym(lib, "il2cpp_method_get_param_count");
    il2cpp_object_new = (il2cpp_object_new_t)dlsym(lib, "il2cpp_object_new");
    il2cpp_runtime_object_init =
        (il2cpp_runtime_object_init_t)dlsym(lib, "il2cpp_runtime_object_init");
    il2cpp_class_get_type = (il2cpp_class_get_type_t)dlsym(lib, "il2cpp_class_get_type");
    il2cpp_type_get_object = (il2cpp_type_get_object_t)dlsym(lib, "il2cpp_type_get_object");
    il2cpp_string_new = (il2cpp_string_new_t)dlsym(lib, "il2cpp_string_new");
    il2cpp_method_get_param = (il2cpp_method_get_param_t)dlsym(lib, "il2cpp_method_get_param");
    il2cpp_type_get_name = (il2cpp_type_get_name_t)dlsym(lib, "il2cpp_type_get_name");
    il2cpp_free = (il2cpp_free_t)dlsym(lib, "il2cpp_free");
    il2cpp_object_unbox = (il2cpp_object_unbox_t)dlsym(lib, "il2cpp_object_unbox");
    il2cpp_class_is_valuetype =
        (il2cpp_class_is_valuetype_t)dlsym(lib, "il2cpp_class_is_valuetype");
    il2cpp_gchandle_new = (il2cpp_gchandle_new_t)dlsym(lib, "il2cpp_gchandle_new");
    il2cpp_gchandle_get_target =
        (il2cpp_gchandle_get_target_t)dlsym(lib, "il2cpp_gchandle_get_target");
    il2cpp_gchandle_free = (il2cpp_gchandle_free_t)dlsym(lib, "il2cpp_gchandle_free");
    il2cpp_class_get_field_from_name =
        (il2cpp_class_get_field_from_name_t)dlsym(lib, "il2cpp_class_get_field_from_name");
    il2cpp_field_get_value = (il2cpp_field_get_value_t)dlsym(lib, "il2cpp_field_get_value");
    il2cpp_field_static_get_value =
        (il2cpp_field_static_get_value_t)dlsym(lib, "il2cpp_field_static_get_value");

    if (!domain_get || !thread_attach || !assembly_open || !assembly_image ||
        !class_from_name || !il2cpp_class_get_method_from_name || !il2cpp_object_get_class ||
        !il2cpp_class_get_name || !il2cpp_runtime_invoke || !il2cpp_string_chars ||
        !il2cpp_string_length) {
        LOGE("il2cpp export table incomplete - Beyond not starting");
        return NULL;
    }

    void *domain = domain_get();
    if (domain == NULL) {
        LOGE("no il2cpp domain after init - Beyond not starting");
        return NULL;
    }
    /* Managed calls are only legal on a thread the runtime knows about. */
    thread_attach(domain);

    void *assembly = assembly_open(domain, "Assembly-CSharp");
    if (assembly == NULL) {
        LOGE("Assembly-CSharp not found in the il2cpp domain");
        return NULL;
    }
    void *image = assembly_image(assembly);
    g_cs_image = image;

    /* AEC is the same global-namespace type the desktop agent hooks
       (Patches/AECPatch.cs). */
    void *aec = class_from_name(image, "", "AEC");
    if (aec == NULL) {
        LOGE("resolved Assembly-CSharp but not AEC - class renamed in this release?");
        return NULL;
    }
    g_aec_class = aec;
    void *method = il2cpp_class_get_method_from_name(aec, "GetResponse", 0);
    if (method == NULL) {
        LOGE("AEC has no 0-arg GetResponse - signature changed in this release?");
        return NULL;
    }
    void *code = method_code_ptr(method);
    if (code == NULL) {
        return NULL;
    }

    LOGI("attached: domain=%p image=%p AEC=%p GetResponse=%p code=%p", domain, image, aec,
         method, code);

    if (hook_func("AEC.GetResponse", code, (void *)hook_get_response,
                  (void **)&orig_get_response)) {
        LOGI("hooked AEC.GetResponse - logging packets");
    }

    /* Probes run after the hook so a probe failure cannot cost us packet
       logging. Both are read-only reflection, and probe_imgui also picks the
       component whose OnGUI the menu will borrow. */
    probe_imgui(domain, lib, assembly_open, assembly_image, class_from_name);
    probe_api(domain, aec, assembly_open, assembly_image, class_from_name);

    /* Unity refuses GameObject creation off the main thread, so hand the menu
       setup to AEC.Update - a MonoBehaviour tick that is main-thread by
       definition. Stash what it needs; it runs on the next frame. */
    g_domain = domain;
    g_assembly_open = assembly_open;
    g_assembly_image = assembly_image;
    g_class_from_name = class_from_name;
    void *update = il2cpp_class_get_method_from_name(aec, "Update", 0);
    void *update_code = update ? method_code_ptr(update) : NULL;
    if (update_code != NULL &&
        hook_func("AEC.Update", update_code, (void *)hook_aec_update,
                  (void **)&orig_aec_update)) {
        LOGI("hooked AEC.Update - menu setup queued for the main thread");
    } else {
        LOGE("could not hook AEC.Update - no menu (packet logging unaffected)");
    }

    LOGI("Beyond loader ready");
    return NULL;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    /* Start Beyond off the JNI thread: the game must not wait on us, and a
       failure in here must never keep Unity from booting. */
    pthread_t t;
    if (pthread_create(&t, NULL, beyond_thread, NULL) == 0) {
        pthread_detach(t);
    } else {
        LOGE("pthread_create failed - Beyond not starting (game unaffected)");
    }

    /* Hand control to Unity's real libmain.so, renamed by the patcher. */
    void *orig = dlopen("libmain_orig.so", RTLD_NOW);
    if (orig == NULL) {
        LOGE("libmain_orig.so missing (%s) - the game will not start", dlerror());
        return JNI_ERR;
    }
    jint (*orig_onload)(JavaVM *, void *) =
        (jint (*)(JavaVM *, void *))dlsym(orig, "JNI_OnLoad");
    if (orig_onload == NULL) {
        LOGE("libmain_orig.so has no JNI_OnLoad - the game will not start");
        return JNI_ERR;
    }
    return orig_onload(vm, reserved);
}
