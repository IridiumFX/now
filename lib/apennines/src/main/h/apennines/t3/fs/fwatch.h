#ifndef APENNINES_T3_FWATCH_H
#define APENNINES_T3_FWATCH_H

#include "apennines/export.h"
#include "apennines/types.h"

/* ---- event type constants ---- */

#define FWATCH_CREATE  ((u32)1)
#define FWATCH_MODIFY  ((u32)2)
#define FWATCH_DELETE  ((u32)3)
#define FWATCH_RENAME  ((u32)4)

/* ---- types ---- */

typedef struct fwatch fwatch;

typedef struct {
    u32 watch_id;
    u32 event_type;
    char path[512];
} fwatch_event;

/* ---- api ---- */

APENNINES_API unsigned long fwatch_create(fwatch **out);
APENNINES_API unsigned long fwatch_add(u32 *out_watch_id, fwatch *fw, const char *path);
APENNINES_API unsigned long fwatch_remove(fwatch *fw, u32 watch_id);
APENNINES_API unsigned long fwatch_poll(fwatch_event **out_events, u64 *out_count,
                                        fwatch *fw);
APENNINES_API unsigned long fwatch_destroy(fwatch *fw);

#endif /* APENNINES_T3_FWATCH_H */
