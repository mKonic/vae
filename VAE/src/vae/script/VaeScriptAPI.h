/* VaeScriptAPI — the ABI between the engine and a component script.
 *
 * Plain C, no includes, no engine types. That is the whole point: a script's translation unit
 * costs ~30 ms to compile against this header, where including the engine's own headers costs
 * ~3 s (spdlog and fmt alone are ~2 s of it). It is also what makes the boundary stable — a
 * function-pointer table with a version number cannot be broken by an inlined std::string
 * changing shape, and a script built against v1 either runs or is refused, never crashes.
 *
 * Everything a script touches is addressed by name: the node inside its component, the property
 * on that node. Names survive a reorder, a restyle and a reload; indices do not.
 */
#ifndef VAE_SCRIPT_API_H
#define VAE_SCRIPT_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever anything below changes shape. The host refuses a script that does not match. */
#define VAE_SCRIPT_ABI_VERSION 4u

/* The script's "self": one component instance. Opaque — the engine owns what is behind it. */
typedef struct VaeInstanceOpaque* VaeInstance;

typedef struct VaeVec2  { float x, y; } VaeVec2;
typedef struct VaeColor { float r, g, b, a; } VaeColor;

typedef enum VaeLogLevel {
    VAE_LOG_TRACE = 0, VAE_LOG_INFO = 1, VAE_LOG_WARN = 2, VAE_LOG_ERROR = 3
} VaeLogLevel;

/* What reaches on_event. Mirrors ui::ActionKind, plus timers, which the engine raises itself. */
typedef enum VaeEventKind {
    VAE_EVENT_CLICKED = 0,
    VAE_EVENT_VALUE_CHANGED,
    VAE_EVENT_TEXT_CHANGED,
    VAE_EVENT_SUBMITTED,
    VAE_EVENT_SELECTION_CHANGED,
    VAE_EVENT_OPENED,
    VAE_EVENT_CLOSED,
    VAE_EVENT_DISMISSED,
    VAE_EVENT_NAVIGATED,
    VAE_EVENT_SCROLLED,
    VAE_EVENT_TIMER,
    VAE_EVENT_SIGNAL,
    /* An answer from the network. `name` is the tag the request was sent with, `number` is the HTTP
     * status (0 when it never got an answer), `text` is the body — or the reason, when status is 0. */
    VAE_EVENT_HTTP,
    /* A live connection. `name` is the socket's name; `text` is the message, or the reason it went
     * away. A socket that closed cleanly and one that failed are the same event with `number` 0
     * and 1 — what a script does about either is usually the same thing. */
    VAE_EVENT_SOCKET_OPEN,
    VAE_EVENT_SOCKET_MESSAGE,
    VAE_EVENT_SOCKET_CLOSED
} VaeEventKind;

typedef struct VaeEvent {
    int         kind;      /* VaeEventKind */
    const char* source;    /* the node that produced it, "" when the engine did */
    const char* name;      /* timer or signal name, "" otherwise */
    double      number;    /* the numeric payload, 0 when there is none */
    const char* text;      /* the text payload, "" when there is none */
} VaeEvent;

/* Every pointer is non-null for a valid host. Strings returned by the engine point at storage the
 * instance owns and stay valid until the next call on that instance. */
typedef struct VaeScriptAPI {
    unsigned int abiVersion;

    /* --- the component's own tree. `node` is a node name inside the component; "" is its root. */
    int         (*has_node)   (VaeInstance, const char* node);
    double      (*get_number) (VaeInstance, const char* node, const char* prop, double fallback);
    void        (*set_number) (VaeInstance, const char* node, const char* prop, double value);
    int         (*get_bool)   (VaeInstance, const char* node, const char* prop, int fallback);
    void        (*set_bool)   (VaeInstance, const char* node, const char* prop, int value);
    const char* (*get_text)   (VaeInstance, const char* node, const char* prop, const char* fallback);
    void        (*set_text)   (VaeInstance, const char* node, const char* prop, const char* value);
    VaeColor    (*get_color)  (VaeInstance, const char* node, const char* prop, VaeColor fallback);
    void        (*set_color)  (VaeInstance, const char* node, const char* prop, VaeColor value);
    void        (*set_visible)(VaeInstance, const char* node, int visible);
    void        (*set_enabled)(VaeInstance, const char* node, int enabled);

    /* --- per-instance state that outlives a hot reload.
     * A native script cannot keep its own state across a dlclose, and a Lua one should not have to
     * think about it, so the durable half of "self" lives here, on the engine's side of the line. */
    double      (*state_number)    (VaeInstance, const char* key, double fallback);
    void        (*set_state_number)(VaeInstance, const char* key, double value);
    const char* (*state_text)      (VaeInstance, const char* key, const char* fallback);
    void        (*set_state_text)  (VaeInstance, const char* key, const char* value);
    int         (*has_state)       (VaeInstance, const char* key);

    /* --- the app around it */
    void   (*emit)     (VaeInstance, const char* name, double number, const char* text);
    void   (*navigate) (VaeInstance, const char* route);
    int    (*back)     (VaeInstance);
    void   (*toast)    (VaeInstance, const char* text, double seconds);
    /* Raises VAE_EVENT_TIMER with this name after `seconds`. One pending timer per name. */
    void   (*after)    (VaeInstance, double seconds, const char* name);
    void   (*cancel)   (VaeInstance, const char* name);
    double (*time)     (VaeInstance);
    void   (*log)      (VaeInstance, int level, const char* text);

    /* --- identity, mostly for logs and for a script that drives several components */
    const char* (*component_name)(VaeInstance);
    const char* (*instance_name) (VaeInstance);

    /* --- services: what the app can reach outside itself ------------------------------------- */

    /* Durable key-value, shared by the whole app and written to disk. Distinct from per-instance
     * state, which belongs to one copy of one component and is not meant to outlive the run. */
    double      (*store_number)    (VaeInstance, const char* key, double fallback);
    void        (*set_store_number)(VaeInstance, const char* key, double value);
    const char* (*store_text)      (VaeInstance, const char* key, const char* fallback);
    void        (*set_store_text)  (VaeInstance, const char* key, const char* value);
    int         (*has_stored)      (VaeInstance, const char* key);
    void        (*forget)          (VaeInstance, const char* key);

    /* Files, inside the app's own folders. A path that escapes them reads as absent. */
    const char* (*read_file)  (VaeInstance, const char* path);
    int         (*write_file) (VaeInstance, const char* path, const char* text);
    int         (*file_exists)(VaeInstance, const char* path);

    /* The network. The answer arrives later as VAE_EVENT_HTTP tagged with `name`, on the thread the
     * script runs on — a script never sees a worker and never needs a lock. */
    void (*http_get) (VaeInstance, const char* url, const char* name);
    void (*http_post)(VaeInstance, const char* url, const char* body, const char* contentType,
                      const char* name);

    /* Wall-clock seconds since the epoch, and a formatted date in strftime's vocabulary. `time`
     * above is the app's own clock, which is what an animation or a cooldown means by "now". */
    double      (*clock)(VaeInstance);
    const char* (*date) (VaeInstance, const char* format);

    /* --- rows for a list or a table -----------------------------------------------------------
     * `cells` is row-major and `columns` wide. The widget virtualizes: a million rows cost the one
     * template node the designer styled, so this is a pointer handed over, not a tree built. */
    /* --- a live connection ----------------------------------------------------------------------
     * `ws://` only for now. Everything it produces arrives as a VAE_EVENT_SOCKET_* tagged with the
     * name it was opened under, on the thread the script runs on. */
    void (*socket_open) (VaeInstance, const char* url, const char* name);
    void (*socket_send) (VaeInstance, const char* name, const char* text);
    void (*socket_close)(VaeInstance, const char* name);
    int  (*socket_live) (VaeInstance, const char* name);

    void (*set_rows)  (VaeInstance, const char* node, const char* const* cells,
                       int rows, int columns);
    void (*clear_rows)(VaeInstance, const char* node);
    int  (*row_count) (VaeInstance, const char* node);

    /* --- sound ----------------------------------------------------------------------------------
     * By the name the sound was imported under, because that is what the Assets panel shows and
     * what someone will type; a relative path works too, for a file that was never imported.
     * Returns a voice, or 0 when there is nothing to play — a handle is never reused, so keeping
     * one past the end of its sound is safe and reads as stopped. */
    unsigned long long (*play_sound)   (VaeInstance, const char* asset, double volume, int loop);
    void               (*stop_sound)   (VaeInstance, unsigned long long voice);
    void               (*stop_sounds)  (VaeInstance);
    int                (*sound_playing)(VaeInstance, unsigned long long voice);

    /* Everything at once, which is what a volume slider means. */
    double (*sound_volume)    (VaeInstance);
    void   (*set_sound_volume)(VaeInstance, double volume);
} VaeScriptAPI;

/* One script class: the lifecycle for every instance of one component. Any entry may be null. */
typedef struct VaeScriptClass {
    const char* component;                          /* the component this drives, by name */
    void (*on_mount)  (VaeInstance);
    void (*on_update) (VaeInstance, double dt);
    void (*on_event)  (VaeInstance, const VaeEvent*);
    void (*on_unmount)(VaeInstance);
} VaeScriptClass;

/* What a native script module must export. The host reads them in this order and gives up loudly
 * at the first one missing or mismatched. */
typedef unsigned int         (*VaeScriptAbiFn)     (void);
typedef void                 (*VaeScriptRegisterFn)(const VaeScriptAPI*);
typedef const VaeScriptClass* (*VaeScriptClassesFn)(int* count);

#define VAE_SCRIPT_ABI_SYMBOL      "vae_script_abi"
#define VAE_SCRIPT_REGISTER_SYMBOL "vae_script_register"
#define VAE_SCRIPT_CLASSES_SYMBOL  "vae_script_classes"

#ifdef __cplusplus
}
#endif

#endif /* VAE_SCRIPT_API_H */
