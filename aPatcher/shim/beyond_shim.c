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
#include <dlfcn.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
typedef void *(*il2cpp_gchandle_new_t)(void *obj, bool pinned);
typedef void *(*il2cpp_gchandle_get_target_t)(void *handle);
typedef void (*il2cpp_gchandle_free_t)(void *handle);
typedef void *(*il2cpp_class_get_field_from_name_t)(void *klass, const char *name);
typedef void (*il2cpp_field_get_value_t)(void *obj, void *field, void *out);
typedef void (*il2cpp_field_static_get_value_t)(void *field, void *out);

static il2cpp_object_unbox_t il2cpp_object_unbox;
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

/* Quest-farm state, read/written by quest_tick() further down. No ID entry -
   it acts on whatever UIQuestTracker.CurrentQuest already is; see that
   function's own comment for why. */
static int g_quest_farm;
static float g_next_quest_tick;
static int g_quest_accept_sent;
static int g_quest_turnin_sent;
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
static void *g_time_get_time;
static int g_autoskills;
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
static void *find_nearest_hostile(void *player, const float me[3])
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
    void *get_enumerator = il2cpp_class_get_method_from_name(dict_class, "GetEnumerator", 0);
    void *boxed_enum = get_enumerator ? inv(get_enumerator, dict, NULL) : NULL;
    void *enum_raw = boxed_enum ? il2cpp_object_unbox(boxed_enum) : NULL;
    if (enum_raw == NULL) {
        return NULL;
    }
    void *enum_class = il2cpp_object_get_class(boxed_enum);
    void *move_next = il2cpp_class_get_method_from_name(enum_class, "MoveNext", 0);
    void *get_current = il2cpp_class_get_method_from_name(enum_class, "get_Current", 0);
    if (move_next == NULL || get_current == NULL) {
        return NULL;
    }

    void *best = NULL;
    float best_dist2 = 0.0f;
    for (int i = 0; i < 500 && inv_bool(move_next, enum_raw, NULL); i++) {
        void *boxed_kv = inv(get_current, enum_raw, NULL);
        void *kv_raw = boxed_kv ? il2cpp_object_unbox(boxed_kv) : NULL;
        if (kv_raw == NULL) {
            continue;
        }
        void *kv_class = il2cpp_object_get_class(boxed_kv);
        void *get_value = il2cpp_class_get_method_from_name(kv_class, "get_Value", 0);
        void *mon = get_value ? inv(get_value, kv_raw, NULL) : NULL;
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
    return best;
}

static void hunt_tick(void)
{
    if (!g_hunt || g_get_main_player == NULL) {
        return;
    }
    float now = inv_float(g_time_get_time, NULL, NULL);
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

    void *tgt = find_nearest_hostile(player, me);
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
static void *g_quest_get_id;                    /* Quest.get_ID()                          */
static void *g_quest_is_ready_turnin;           /* Quest.IsReadyForTurnin()                */
static void *g_player_is_quest_accepted;        /* Player.IsQuestAccepted(int)             */
static void *g_req_accept_class;
static void *g_req_accept_ctor;                 /* RequestQuestAccept(int)                 */
static void *g_req_turnin_class;
static void *g_req_turnin_ctor;                 /* RequestTryQuestComplete(int,int)        */
static int g_quest_last_id;                     /* detects the tracker moving to a new quest */

static void quest_tick(void)
{
    if (!g_quest_farm || g_uiquesttracker_get_currentquest == NULL) {
        return;
    }
    float now = inv_float(g_time_get_time, NULL, NULL);
    if (now < g_next_quest_tick) {
        return;
    }
    g_next_quest_tick = now + 1.0f;

    void *player = g_get_main_player ? inv(g_get_main_player, NULL, NULL) : NULL;
    if (player == NULL) {
        return;
    }

    void *quest = inv(g_uiquesttracker_get_currentquest, NULL, NULL);
    if (quest == NULL) {
        snprintf(g_quest_status, sizeof(g_quest_status), "no quest tracked - track one in-game");
        g_quest_last_id = 0;
        return;
    }

    int32_t qid = g_quest_get_id != NULL ? inv_int(g_quest_get_id, quest, NULL) : 0;
    if (qid != g_quest_last_id) {
        /* Tracker moved to a different quest - fresh attempt, whether that's
           chain auto-advance or the player tracking a new one by hand. */
        g_quest_last_id = qid;
        g_quest_accept_sent = 0;
        g_quest_turnin_sent = 0;
        LOGI("quest: now tracking quest %d", qid);
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
    if (!ready) {
        g_hunt = 1; /* accepted, not done - keep the kill-count objectives moving */
        snprintf(g_quest_status, sizeof(g_quest_status), "hunting for quest %d", qid);
        return;
    }

    if (!g_quest_turnin_sent && g_req_turnin_class != NULL && g_req_turnin_ctor != NULL &&
        il2cpp_object_new) {
        void *req = il2cpp_object_new(g_req_turnin_class);
        if (req != NULL) {
            int32_t choice = -1;
            void *turnin_args[2] = {&qid, &choice};
            inv(g_req_turnin_ctor, req, turnin_args);
            void *send_args[1] = {req};
            inv(g_send_request, g_aec_instance, send_args);
            g_quest_turnin_sent = 1;
            snprintf(g_quest_status, sizeof(g_quest_status), "quest %d turned in - waiting for "
                                                              "next tracked quest",
                    qid);
            LOGI("quest: sent RequestTryQuestComplete(%d, -1)", qid);
            /* Farm stays ON: whatever the tracker points to next (chain
               auto-advance or a manual re-track) picks up on its own. */
        }
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
    }

    if (gui_button(18, 176, 84, 30, g_log_open ? "Log X" : "Log")) {
        g_log_open = !g_log_open;
        g_help_open = 0;
    }
    if (gui_button(108, 176, 160, 30,
                   g_autoskills ? "Autoskills: ON" : "Autoskills: OFF")) {
        g_autoskills = !g_autoskills;
        g_skill_slot = 0;
        g_next_skill = 0.0f;
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

    /* Quest farming: no ID entry - acts on whatever UIQuestTracker.CurrentQuest
       already is. Track/accept a quest normally in-game, then just leave this
       on; it follows the tracker through however much of a chain keeps
       getting tracked. Forces Hunt on while a tracked quest isn't ready to
       turn in yet - kill-count objectives only, see the quest farming
       section's own comment for what this does not cover. */
    if (gui_button(18, 316, 160, 30, g_quest_farm ? "Farm: ON" : "Farm: OFF")) {
        g_quest_farm = !g_quest_farm;
        if (g_quest_farm) {
            g_quest_accept_sent = 0;
            g_quest_turnin_sent = 0;
            g_next_quest_tick = 0.0f;
        }
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
            g_quest_get_id = il2cpp_class_get_method_from_name(quest_class, "get_ID", 0);
            g_quest_is_ready_turnin =
                il2cpp_class_get_method_from_name(quest_class, "IsReadyForTurnin", 0);
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
        LOGI("menu: quest CurrentQuest=%p get_ID=%p IsReadyForTurnin=%p IsQuestAccepted=%p "
             "Accept=%p/%p TurnIn=%p/%p",
             g_uiquesttracker_get_currentquest, g_quest_get_id, g_quest_is_ready_turnin,
             g_player_is_quest_accepted, g_req_accept_class, g_req_accept_ctor,
             g_req_turnin_class, g_req_turnin_ctor);
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
        /* currentState is NOT overridden by Monster (only Name is, per the
           decomp), so resolving it on Entity is safe here - unlike get_Name
           above, there's no derived-class override to miss. */
        g_entity_get_currentstate =
            il2cpp_class_get_method_from_name(entity_class, "get_currentState", 0);
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
