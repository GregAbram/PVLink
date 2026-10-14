/* DataManager client API (SPEC.md section 10).
 *
 * A dm_client delivers the same event stream whether it is connected to a
 * DataManager over the network (dm_connect) or playing a store in-process
 * (dm_open_store). Engines call dm_poll from their main thread; the library
 * never calls back into engine code from its own threads.
 *
 * Every event obtained from dm_poll must be given back with dm_release. Its
 * pointers stay valid until then.
 */
#ifndef DM_DM_H
#define DM_DM_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && !defined(DM_STATIC_INTERNAL)
#  ifdef DM_BUILDING_LIBRARY
#    define DM_API __declspec(dllexport)
#  else
#    define DM_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__)
#  define DM_API __attribute__((visibility("default")))
#else
#  define DM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dm_client dm_client;

enum dm_event_type {
    DM_EV_NONE           = 0,
    DM_EV_CONNECTED      = 1,   /* message: project attached to ("" if none yet) */
    DM_EV_DISCONNECTED   = 2,   /* message: reason; the client reconnects by itself */
    DM_EV_REJECTED       = 3,   /* code, message (REJECT codes, SPEC.md 5.1) */
    DM_EV_ERROR          = 4,   /* code, message */
    DM_EV_PLAYBACK_STATE = 5,   /* state */
    DM_EV_BATCH_BEGIN    = 6,   /* batch_id, time */
    DM_EV_MANIFEST       = 7,   /* count, keys, hashes: the complete snapshot */
    DM_EV_ITEM           = 8,   /* key, hash, meta, data */
    DM_EV_BATCH_END      = 9,   /* batch_id: apply the batch, then dm_ack */
    DM_EV_PROJECT_LIST   = 10,  /* count, keys: project names */
    DM_EV_PUT_STORED     = 11   /* batch_id: the request id from dm_put / dm_remove_untimed;
                                   code: 0 stored (or unchanged), 1 refused; message: reason */
};

typedef struct dm_playback_state {
    const char* project;
    int32_t playing;
    int32_t loop;
    int32_t follow_live;
    int32_t source_connected;
    double time;
    double rate;        /* timesteps per second; 0 = paced by the slowest client */
    double min_time;
    double max_time;
} dm_playback_state;

typedef struct dm_event {
    int32_t type;            /* enum dm_event_type */
    int32_t code;            /* REJECTED / ERROR */
    uint64_t batch_id;       /* BATCH_BEGIN / BATCH_END */
    double time;             /* BATCH_BEGIN */

    const char* key;         /* ITEM: "<Type>/<Name>" */
    uint8_t hash[16];        /* ITEM */
    const char* meta;        /* ITEM: UTF-8 JSON, NUL-terminated */
    uint32_t meta_length;
    const uint8_t* data;     /* ITEM: binary payload */
    uint64_t data_length;

    uint32_t count;              /* MANIFEST / PROJECT_LIST */
    const char* const* keys;     /* count entries */
    const uint8_t* hashes;       /* MANIFEST: count * 16 bytes */

    dm_playback_state state;     /* PLAYBACK_STATE */
    const char* message;         /* CONNECTED / DISCONNECTED / REJECTED / ERROR */

    void* internal_;             /* owned by the library */
} dm_event;

/* Connection flags for dm_connect. */
#define DM_WANT_COMPRESSION 1u   /* ask for compressed items (Wi-Fi, cellular) */
#define DM_CONTROL_ONLY     2u   /* control role: playback state only, no batches */

DM_API const char* dm_version(void);

/* sizeof(dm_event), so bindings in other languages can check their layout. */
DM_API uint32_t dm_event_size(void);

/* Connects to a DataManager; reconnects automatically after a disconnect.
 * project may be NULL or "" for the DataManager's default. Never returns NULL
 * for valid arguments: connection results arrive as events. */
DM_API dm_client* dm_connect(const char* host, uint16_t port, const char* project,
                             const char* peer_name, const char* token, uint32_t flags);

/* Plays a project from a local store, in-process, with no network.
 * Returns NULL and fills err (if given) on failure. */
DM_API dm_client* dm_open_store(const char* store_path, const char* project,
                                double rate, char* err, size_t err_len);

/* Returns 1 and fills *ev if an event was pending, 0 otherwise. */
DM_API int dm_poll(dm_client* c, dm_event* ev);
DM_API void dm_release(dm_client* c, dm_event* ev);

/* Acknowledges a batch once it is shown (SPEC.md 5.3). dropped lists keys
 * that failed to decode or could not be kept; they are resent. */
DM_API int dm_ack(dm_client* c, uint64_t batch_id, const char* const* dropped, uint32_t n_dropped);

/* Playback control (SPEC.md 5.5). Return 0 on success, -1 if not connected. */
DM_API int dm_play(dm_client* c);
DM_API int dm_pause(dm_client* c);
DM_API int dm_seek(dm_client* c, double time);
DM_API int dm_step(dm_client* c, int32_t delta);
DM_API int dm_set_rate(dm_client* c, double timesteps_per_second);
DM_API int dm_set_loop(dm_client* c, int loop);
DM_API int dm_follow_live(dm_client* c, int follow);
DM_API int dm_select_project(dm_client* c, const char* project);
DM_API int dm_list_projects(dm_client* c);

/* Untimed items (SPEC.md 5.8): writes the current version of `key` in the
 * attached project, e.g. "State/abr" or "VisAsset/<uuid>". Returns a request
 * id (> 0) matched by a later DM_EV_PUT_STORED event, or 0 if not connected. */
DM_API uint64_t dm_put(dm_client* c, const char* key, const char* meta, uint32_t meta_length,
                       const uint8_t* data, uint64_t data_length);
DM_API uint64_t dm_remove_untimed(dm_client* c, const char* key);

DM_API void dm_close(dm_client* c);

/* Listens for DataManager announcements (SPEC.md 5.6) for timeout_ms.
 * Writes one line per DataManager into buf: "address\tport\tname\tprojects\n"
 * (projects comma-separated). Returns the number found. */
DM_API int dm_discover(uint16_t discovery_port, int timeout_ms, char* buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* DM_DM_H */
