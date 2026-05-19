#include "apennines/t3/fs/fwatch.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ---- internal types ---- */

typedef struct {
    u32   id;
    char  path[512];
    time_t last_mtime;
    int    existed;       /* 1 if the file existed at last check */
} fwatch_entry;

struct fwatch {
    fwatch_entry *entries;
    u32           count;
    u32           capacity;
    u32           next_id;
};

/* ---- helpers ---- */

static int stat_mtime(const char *path, time_t *mtime) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *mtime = st.st_mtime;
    return 0;
}

static int path_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
}

/* ---- api ---- */

unsigned long fwatch_create(fwatch **out) {
    fwatch *fw;

    if (!out) return 1;

    fw = (fwatch *)calloc(1, sizeof(fwatch));
    if (!fw) return 2;

    fw->entries  = NULL;
    fw->count    = 0;
    fw->capacity = 0;
    fw->next_id  = 1;

    *out = fw;
    return 0;
}

unsigned long fwatch_add(u32 *out_watch_id, fwatch *fw, const char *path) {
    fwatch_entry *entry;
    size_t path_len;

    if (!out_watch_id) return 1;
    if (!fw) return 2;
    if (!path) return 3;

    path_len = strlen(path);
    if (path_len == 0 || path_len >= sizeof(((fwatch_entry *)0)->path)) return 4;

    /* grow entries array if needed */
    if (fw->count >= fw->capacity) {
        u32 new_cap = fw->capacity == 0 ? 8 : fw->capacity * 2;
        fwatch_entry *new_entries = (fwatch_entry *)realloc(
            fw->entries, (size_t)new_cap * sizeof(fwatch_entry));
        if (!new_entries) return 5;
        fw->entries  = new_entries;
        fw->capacity = new_cap;
    }

    entry = &fw->entries[fw->count];
    memset(entry, 0, sizeof(*entry));
    entry->id = fw->next_id++;
    memcpy(entry->path, path, path_len + 1);

    /* snapshot current state */
    if (stat_mtime(path, &entry->last_mtime) == 0) {
        entry->existed = 1;
    } else {
        entry->existed = 0;
        entry->last_mtime = 0;
    }

    *out_watch_id = entry->id;
    fw->count++;
    return 0;
}

unsigned long fwatch_remove(fwatch *fw, u32 watch_id) {
    u32 i;

    if (!fw) return 1;
    if (watch_id == 0) return 2;

    for (i = 0; i < fw->count; i++) {
        if (fw->entries[i].id == watch_id) {
            /* swap with last entry */
            if (i < fw->count - 1) {
                fw->entries[i] = fw->entries[fw->count - 1];
            }
            fw->count--;
            return 0;
        }
    }

    return 3; /* watch_id not found */
}

unsigned long fwatch_poll(fwatch_event **out_events, u64 *out_count,
                          fwatch *fw) {
    fwatch_event *events = NULL;
    u64 event_count = 0;
    u64 event_cap = 0;
    u32 i;

    if (!out_events) return 1;
    if (!out_count)  return 2;
    if (!fw)         return 3;

    *out_events = NULL;
    *out_count  = 0;

    for (i = 0; i < fw->count; i++) {
        fwatch_entry *e = &fw->entries[i];
        int now_exists = path_exists(e->path);
        time_t now_mtime = 0;
        u32 etype = 0;

        if (now_exists) {
            stat_mtime(e->path, &now_mtime);
        }

        if (!e->existed && now_exists) {
            etype = FWATCH_CREATE;
        } else if (e->existed && !now_exists) {
            etype = FWATCH_DELETE;
        } else if (e->existed && now_exists && now_mtime != e->last_mtime) {
            etype = FWATCH_MODIFY;
        }

        /* update snapshot */
        e->existed    = now_exists;
        e->last_mtime = now_mtime;

        if (etype == 0) continue;

        /* grow events array if needed */
        if (event_count >= event_cap) {
            u64 new_cap = event_cap == 0 ? 4 : event_cap * 2;
            fwatch_event *new_events = (fwatch_event *)realloc(
                events, (size_t)new_cap * sizeof(fwatch_event));
            if (!new_events) {
                free(events);
                return 4;
            }
            events    = new_events;
            event_cap = new_cap;
        }

        memset(&events[event_count], 0, sizeof(fwatch_event));
        events[event_count].watch_id   = e->id;
        events[event_count].event_type = etype;
        memcpy(events[event_count].path, e->path, strlen(e->path) + 1);
        event_count++;
    }

    *out_events = events;
    *out_count  = event_count;
    return 0;
}

unsigned long fwatch_destroy(fwatch *fw) {
    if (!fw) return 1;

    free(fw->entries);
    free(fw);
    return 0;
}
