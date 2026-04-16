/*
 * now_plugin.h — Plugin system (§10)
 *
 * Handles plugin invocation (stdin/stdout Pasta IPC), built-in plugins
 * (now:embed, now:version), and lifecycle hook dispatch.
 */
#ifndef NOW_PLUGIN_H
#define NOW_PLUGIN_H

#include "now_pom.h"
#include "now.h"

/* Hook names — must match spec §10.2 */
#define NOW_HOOK_PRE_PROCURE   "pre-procure"
#define NOW_HOOK_POST_PROCURE  "post-procure"
#define NOW_HOOK_GENERATE      "generate"
#define NOW_HOOK_PRE_COMPILE   "pre-compile"
#define NOW_HOOK_POST_COMPILE  "post-compile"
#define NOW_HOOK_PRE_LINK      "pre-link"
#define NOW_HOOK_POST_LINK     "post-link"
#define NOW_HOOK_PRE_TEST      "pre-test"
#define NOW_HOOK_POST_TEST     "post-test"
#define NOW_HOOK_PRE_PACKAGE   "pre-package"
#define NOW_HOOK_POST_PACKAGE  "post-package"
#define NOW_HOOK_PRE_PUBLISH   "pre-publish"
#define NOW_HOOK_POST_PUBLISH  "post-publish"

/* Result of a plugin invocation */
typedef struct {
    int         ok;         /* 1 = ok/warn, 0 = error */
    NowStrArray sources;    /* additional source files to compile */
    NowStrArray includes;   /* additional include paths */
    NowStrArray defines;    /* additional defines */
    NowStrArray messages;   /* diagnostic messages */
} NowPluginResult;

NOW_API void now_plugin_result_init(NowPluginResult *r);
NOW_API void now_plugin_result_free(NowPluginResult *r);

/* Run all plugins registered for the given hook.
 * Collects sources/includes/defines from generate plugins.
 * Returns 0 on success, -1 if any plugin fails. */
NOW_API int now_plugin_run_hook(const NowProject *project,
                                 const char *basedir,
                                 const char *hook,
                                 int verbose,
                                 NowPluginResult *out,
                                 NowResult *result);

/* Run a single plugin entry. For built-in plugins (now:*) this
 * executes inline; for external plugins it spawns a child process. */
NOW_API int now_plugin_invoke(const NowPlugin *plugin,
                               const NowProject *project,
                               const char *basedir,
                               const char *hook,
                               int verbose,
                               NowPluginResult *out,
                               NowResult *result);

/* Check if a plugin ID is a built-in (now:embed, now:version, etc.) */
NOW_API int now_plugin_is_builtin(const char *id);

#endif /* NOW_PLUGIN_H */
