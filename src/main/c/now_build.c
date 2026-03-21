/*
 * now_build.c — Build and link phases
 *
 * Compiles source files to objects, then links into final output.
 */
#include "now_build.h"
#include "now_manifest.h"
#include "now_repro.h"
#include "now_module.h"
#include "now_cache.h"
#include "now_remote.h"
#include "now_tui.h"
#include "now.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <windows.h>
  #include <process.h>
#else
  #include <sys/wait.h>
  #include <unistd.h>
  #ifdef __APPLE__
    #include <sys/sysctl.h>
  #endif
#endif

/* ---- CPU count detection ---- */

NOW_API int now_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
    return n > 0 ? n : 1;
#elif defined(__APPLE__)
    int n = 1;
    size_t sz = sizeof(n);
    if (sysctlbyname("hw.logicalcpu", &n, &sz, NULL, 0) == 0 && n > 0)
        return n;
    return 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* ---- Subprocess execution ---- */

/* Build a Windows command-line string from argv.
 * Caller must free the returned string. */
#ifdef _WIN32
static char *build_cmdline(const char *const *argv) {
    size_t len = 0;
    for (const char *const *a = argv; *a; a++)
        len += strlen(*a) + 3;

    char *cmdline = malloc(len + 1);
    if (!cmdline) return NULL;
    cmdline[0] = '\0';

    for (const char *const *a = argv; *a; a++) {
        if (a != argv) strcat(cmdline, " ");
        if (strchr(*a, ' ')) {
            strcat(cmdline, "\"");
            strcat(cmdline, *a);
            strcat(cmdline, "\"");
        } else {
            strcat(cmdline, *a);
        }
    }
    return cmdline;
}
#endif

NOW_API int now_exec(const char *const *argv, int verbose) {
    if (!argv || !argv[0]) return -1;

    if (verbose) {
        fprintf(stderr, " ");
        for (const char *const *a = argv; *a; a++)
            fprintf(stderr, " %s", *a);
        fprintf(stderr, "\n");
    }

#ifdef _WIN32
    char *cmdline = build_cmdline(argv);
    if (!cmdline) return -1;

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags    = STARTF_USESTDHANDLES;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                        0, NULL, NULL, &si, &pi)) {
        free(cmdline);
        return -1;
    }
    free(cmdline);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;

#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

/* ---- Parallel process pool ---- */

/* A slot in the parallel compilation pool */
typedef struct {
    int            active;      /* 1 if a process is running in this slot */
    size_t         source_idx;  /* index into the source/work list */
#ifdef _WIN32
    HANDLE         hProcess;
    HANDLE         hPipeRead;   /* read end of captured stderr pipe */
#else
    pid_t          pid;
    int            pipe_fd;     /* read end of captured stderr pipe */
#endif
} NowWorkerSlot;

/* Captured output from a completed worker */
typedef struct {
    char  *data;
    size_t len;
} NowCapturedOutput;

/* Read all available data from a pipe/handle into a buffer.
 * Returns a malloc'd string (caller frees). */
static NowCapturedOutput read_pipe_all(
#ifdef _WIN32
    HANDLE h
#else
    int fd
#endif
) {
    NowCapturedOutput out = {NULL, 0};
    size_t cap = 1024;
    out.data = malloc(cap);
    if (!out.data) return out;
    out.data[0] = '\0';

    for (;;) {
        if (out.len + 512 > cap) {
            cap *= 2;
            char *tmp = realloc(out.data, cap);
            if (!tmp) break;
            out.data = tmp;
        }
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(h, out.data + out.len, (DWORD)(cap - out.len - 1), &n, NULL) || n == 0)
            break;
        out.len += n;
#else
        ssize_t n = read(fd, out.data + out.len, cap - out.len - 1);
        if (n <= 0) break;
        out.len += (size_t)n;
#endif
    }
    out.data[out.len] = '\0';
    return out;
}

/* Spawn a process with stderr+stdout captured to a pipe.
 * Returns 0 on success; fills slot with process info. */
static int spawn_captured(const char *const *argv, NowWorkerSlot *slot) {
#ifdef _WIN32
    char *cmdline = build_cmdline(argv);
    if (!cmdline) return -1;

    /* Create pipe for capturing output */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        free(cmdline);
        return -1;
    }
    /* Ensure read handle is not inherited */
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.dwFlags    = STARTF_USESTDHANDLES;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                        0, NULL, NULL, &si, &pi)) {
        free(cmdline);
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return -1;
    }
    free(cmdline);
    CloseHandle(pi.hThread);
    CloseHandle(hWrite);  /* close write end in parent */

    slot->hProcess  = pi.hProcess;
    slot->hPipeRead = hRead;
    return 0;

#else
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child: redirect stdout+stderr to pipe write end */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    /* Parent */
    close(pipefd[1]);
    slot->pid     = pid;
    slot->pipe_fd = pipefd[0];
    return 0;
#endif
}

/* Wait for any one of the active workers to finish.
 * Returns the slot index, sets *exit_code and *captured. */
static int wait_any_worker(NowWorkerSlot *slots, int nslots,
                           int *exit_code, NowCapturedOutput *captured) {
#ifdef _WIN32
    /* Collect active process handles */
    HANDLE handles[64];
    int    indices[64];
    int    nactive = 0;
    for (int i = 0; i < nslots && nactive < 64; i++) {
        if (slots[i].active) {
            handles[nactive] = slots[i].hProcess;
            indices[nactive] = i;
            nactive++;
        }
    }
    if (nactive == 0) return -1;

    DWORD which = WaitForMultipleObjects((DWORD)nactive, handles, FALSE, INFINITE);
    if (which < WAIT_OBJECT_0 || which >= WAIT_OBJECT_0 + (DWORD)nactive)
        return -1;

    int slot_idx = indices[which - WAIT_OBJECT_0];
    NowWorkerSlot *s = &slots[slot_idx];

    DWORD code;
    GetExitCodeProcess(s->hProcess, &code);
    *exit_code = (int)code;

    *captured = read_pipe_all(s->hPipeRead);

    CloseHandle(s->hProcess);
    CloseHandle(s->hPipeRead);
    s->active = 0;
    return slot_idx;

#else
    int status;
    pid_t pid = waitpid(-1, &status, 0);
    if (pid <= 0) return -1;

    /* Find the slot */
    int slot_idx = -1;
    for (int i = 0; i < nslots; i++) {
        if (slots[i].active && slots[i].pid == pid) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0) return -1;

    NowWorkerSlot *s = &slots[slot_idx];
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    *captured = read_pipe_all(s->pipe_fd);

    close(s->pipe_fd);
    s->active = 0;
    return slot_idx;
#endif
}

/* ---- Toolchain resolution (§7.1) ---- */

/* Find the full path to a tool by searching PATH (like `which`).
 * Returns a malloc'd absolute path, or strdup(name) if not found. */
static char *find_in_path(const char *name) {
#ifdef _WIN32
    /* If already absolute or contains path separator, use as-is */
    if (strchr(name, '/') || strchr(name, '\\') || (name[0] && name[1] == ':'))
        return strdup(name);

    const char *path = getenv("PATH");
    if (!path) return strdup(name);

    /* Try each PATH entry with common Windows extensions.
     * Handle both ; (native Windows) and : (MSYS2/MinGW) separators.
     * Heuristic: if PATH contains forward-slash dirs, use ':' */
    char sep = ';';
    if (strchr(path, '/') && !strchr(path, ';'))
        sep = ':';

    const char *exts[] = { "", ".exe", ".cmd", ".bat", NULL };
    char buf[1024];
    const char *p = path;
    while (*p) {
        const char *end = p;
        while (*end && *end != sep) end++;
        size_t dir_len = (size_t)(end - p);
        if (dir_len > 0 && dir_len < sizeof(buf) - 260) {
            memcpy(buf, p, dir_len);
            /* Normalize separator */
            if (buf[dir_len - 1] != '/' && buf[dir_len - 1] != '\\')
                buf[dir_len++] = '/';
            for (int e = 0; exts[e]; e++) {
                snprintf(buf + dir_len, sizeof(buf) - dir_len, "%s%s", name, exts[e]);
                /* Check if file exists */
                struct stat st;
                if (stat(buf, &st) == 0 && !(st.st_mode & S_IFDIR))
                    return strdup(buf);
            }
        }
        p = *end ? end + 1 : end;
    }
#else
    (void)name;
#endif
    return strdup(name);
}

static char *resolve_tool(const char *env_var, const char *fallback) {
    const char *val = getenv(env_var);
    if (val && *val) return find_in_path(val);
    return find_in_path(fallback);
}

/* Ensure the compiler's directory is on the Windows PATH so that
 * child processes (cc1.exe, as.exe, etc.) can find their DLLs.
 * This is critical for MinGW gcc when invoked via absolute path. */
#ifdef _WIN32
static void ensure_tool_dir_in_path(const char *tool_path) {
    if (!tool_path) return;

    /* Extract directory from tool path */
    const char *last_sep = strrchr(tool_path, '/');
    const char *last_bsep = strrchr(tool_path, '\\');
    if (last_bsep && (!last_sep || last_bsep > last_sep))
        last_sep = last_bsep;
    if (!last_sep) return;

    size_t dir_len = (size_t)(last_sep - tool_path);
    char *tool_dir = malloc(dir_len + 1);
    if (!tool_dir) return;
    memcpy(tool_dir, tool_path, dir_len);
    tool_dir[dir_len] = '\0';

    /* Check if already in PATH (case-insensitive on Windows) */
    const char *cur_path = getenv("PATH");
    if (cur_path) {
        /* Quick check: is the dir already a substring? */
        const char *found = cur_path;
        while ((found = strstr(found, tool_dir)) != NULL) {
            /* Verify it's a complete path entry (at start or after ;) */
            if (found == cur_path || *(found - 1) == ';') {
                char after = found[dir_len];
                if (after == '\0' || after == ';') {
                    free(tool_dir);
                    return;  /* already in PATH */
                }
            }
            found++;
        }
    }

    /* Prepend tool_dir to PATH */
    const char *old_path = cur_path ? cur_path : "";
    size_t new_len = dir_len + 1 + strlen(old_path) + 6; /* "PATH=" + dir + ";" + old */
    char *new_path = malloc(new_len);
    if (new_path) {
        snprintf(new_path, new_len, "PATH=%s;%s", tool_dir, old_path);
        _putenv(new_path);
        free(new_path);
    }
    free(tool_dir);
}
#endif

/* Check if Java is an active language in the project */
static int has_lang(const NowProject *p, const char *lang_id) {
    if (!p || !lang_id) return 0;
    for (size_t i = 0; i < p->langs.count; i++) {
        if (strcmp(p->langs.items[i], lang_id) == 0) return 1;
    }
    return 0;
}

static int has_java_lang(const NowProject *p) {
    return has_lang(p, "java");
}

NOW_API void now_toolchain_resolve(NowToolchain *tc, const NowProject *p) {
#ifdef _WIN32
    /* On Windows with MSVC, detect cl.exe; otherwise assume gcc/mingw */
    const char *cc_env = getenv("CC");
    if (cc_env && strstr(cc_env, "cl")) {
        tc->cc     = strdup(cc_env);
        tc->cxx    = resolve_tool("CXX", "cl.exe");
        tc->ar     = resolve_tool("AR",  "lib.exe");
        tc->as     = resolve_tool("AS",  "ml64.exe");
        tc->ld     = NULL;
        tc->is_msvc = 1;
    } else {
        tc->cc     = resolve_tool("CC",  "gcc");
        tc->cxx    = resolve_tool("CXX", "g++");
        tc->ar     = resolve_tool("AR",  "ar");
        tc->as     = resolve_tool("AS",  "as");
        tc->ld     = NULL;
        tc->is_msvc = 0;
    }
    /* Ensure compiler dir is on PATH for cc1.exe/libssp-0.dll */
    ensure_tool_dir_in_path(tc->cc);
#else
    tc->cc     = resolve_tool("CC",  "cc");
    tc->cxx    = resolve_tool("CXX", "c++");
    tc->ar     = resolve_tool("AR",  "ar");
    tc->as     = resolve_tool("AS",  "as");
    tc->ld     = NULL;
    tc->is_msvc = 0;
#endif

    /* Java tools (only if project uses Java) */
    if (p && has_java_lang(p)) {
        tc->javac = resolve_tool("JAVAC", "javac");
        tc->jar   = resolve_tool("JAR",   "jar");
        tc->java  = resolve_tool("JAVA",  "java");
    }

    /* Rust — resolve rustc for mixed C/Rust projects */
    if (has_lang(p, "rust"))
        tc->rustc = resolve_tool("RUSTC", "rustc");

    /* Go — resolve go for mixed C/Go projects */
    if (has_lang(p, "go"))
        tc->go = resolve_tool("GO", "go");

    /* Julia — resolve julia for embedded Julia */
    if (has_lang(p, "julia"))
        tc->julia = resolve_tool("JULIA", "julia");
}

NOW_API void now_toolchain_free(NowToolchain *tc) {
    free(tc->cc);
    free(tc->cxx);
    free(tc->ar);
    free(tc->as);
    free(tc->ld);
    free(tc->javac);
    free(tc->jar);
    free(tc->java);
    free(tc->rustc);
    free(tc->go);
    free(tc->julia);
    memset(tc, 0, sizeof(*tc));
}

/* ---- Build context ---- */

NOW_API int now_build_init(NowBuildCtx *ctx, const NowProject *project,
                   const char *basedir, NowResult *result) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->project    = project;
    ctx->basedir    = basedir;
    ctx->target_dir = "target";
    now_filelist_init(&ctx->sources);
    now_filelist_init(&ctx->objects);
    now_filelist_init(&ctx->dep_includes);
    now_filelist_init(&ctx->dep_libdirs);
    now_filelist_init(&ctx->dep_libs);

    now_lang_registry_init();
    now_toolchain_resolve(&ctx->toolchain, project);

    /* Create target directories */
    char *obj_dir = now_path_join(basedir, "target/obj/main");
    if (now_mkdir_p(obj_dir) != 0) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot create directory: %s", obj_dir);
        }
        free(obj_dir);
        return -1;
    }
    free(obj_dir);

    char *bin_dir = now_path_join(basedir, "target/bin");
    now_mkdir_p(bin_dir);
    free(bin_dir);

    /* Discover source files */
    if (project->langs.count == 0) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "no languages declared in project");
        }
        return -1;
    }

    const char **exts = now_lang_source_exts(
        (const char *const *)project->langs.items, project->langs.count);
    if (!exts) {
        if (result) {
            result->code = NOW_ERR_ALLOC;
            snprintf(result->message, sizeof(result->message), "out of memory");
        }
        return -1;
    }

    const char *src_dir = project->sources.dir;
    if (!src_dir) src_dir = "src/main/c";

    int rc = now_discover_sources(basedir, src_dir, exts, &ctx->sources);
    free(exts);

    if (rc != 0) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot scan source directory: %s", src_dir);
        }
        return -1;
    }

    /* Append explicit sources.include entries */
    for (size_t i = 0; i < project->sources.include.count; i++)
        now_filelist_push(&ctx->sources, project->sources.include.items[i]);

    if (ctx->sources.count == 0) {
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "no source files found in %s", src_dir);
        }
        return -1;
    }

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return 0;
}

/* ---- Compile phase (§2.3) ---- */

/* Filter out MSVC /showIncludes lines from captured output.
 * Prints remaining lines to stderr. */
static void print_filtered_output(const char *data, size_t len, int is_msvc) {
    if (!data || len == 0) return;
    if (!is_msvc) {
        fprintf(stderr, "%s", data);
        return;
    }
    const char *pos = data;
    const char *end = data + len;
    while (pos < end) {
        const char *eol = pos;
        while (eol < end && *eol != '\n') eol++;
        size_t line_len = (size_t)(eol - pos);
        /* Skip "Note: including file:" lines */
        if (line_len < 21 ||
            memcmp(pos, "Note: including file:", 21) != 0) {
            if (line_len > 0)
                fwrite(pos, 1, line_len, stderr);
            if (eol < end) fputc('\n', stderr);
        }
        pos = eol < end ? eol + 1 : end;
    }
}

/* A compile job — everything needed to compile one source file */
typedef struct {
    char  *src_rel;     /* source path (relative) */
    char  *obj_path;    /* output object path */
    char **argv;        /* NULL-terminated argument list (all strings owned) */
    int    argc;
    char  *source_hash; /* SHA-256 of source file (for cache/manifest reuse) */
    char  *cache_key;   /* content-addressable cache key */
    char  *dep_path;    /* path to .d depfile (GCC/Clang) or NULL (MSVC) */
} NowCompileJob;

static void compile_job_free(NowCompileJob *job) {
    free(job->src_rel);
    free(job->obj_path);
    if (job->argv) {
        for (int i = 0; i < job->argc; i++)
            free(job->argv[i]);
        free(job->argv);
    }
    free(job->source_hash);
    free(job->cache_key);
    free(job->dep_path);
    memset(job, 0, sizeof(*job));
}

/* Build the argument list for compiling one source file (MSVC cl.exe).
 * Returns 0 on success, fills job with owned copies of all strings. */
static int build_compile_job_msvc(NowBuildCtx *ctx, const char *src_rel,
                                  const NowLangType *type, const NowLangDef *lang,
                                  NowCompileJob *job) {
    const NowProject *p = ctx->project;
    const char *basedir = ctx->basedir;
    memset(job, 0, sizeof(*job));

    /* Resolve tool */
    const char *tool = ctx->toolchain.cc;
    if (type->tool_var && strcmp(type->tool_var, "${cxx}") == 0)
        tool = ctx->toolchain.cxx;

    /* Derive object path with .obj extension */
    char *obj = now_obj_path_ex(basedir, src_rel, p->sources.dir,
                                ctx->target_dir, ".obj");
    if (!obj) return -1;

    /* Ensure object directory exists */
    char *obj_dir = strdup(obj);
    char *last_sep = strrchr(obj_dir, '/');
    if (!last_sep) last_sep = strrchr(obj_dir, '\\');
    if (last_sep) {
        *last_sep = '\0';
        now_mkdir_p(obj_dir);
    }
    free(obj_dir);

    /* Build argument list */
    const char *tmp_argv[128];
    int tmp_argc = 0;

    tmp_argv[tmp_argc++] = tool;
    tmp_argv[tmp_argc++] = "/nologo";

    /* Standard flag: /std:c11, /std:c17, /std:c++17, etc. */
    char std_buf[32] = {0};
    const char *std = p->compile.std ? p->compile.std : p->std;
    if (std && lang->std_flag) {
        snprintf(std_buf, sizeof(std_buf), "/std:%s", std);
        tmp_argv[tmp_argc++] = std_buf;
    }

    /* Warnings: translate GCC-style to MSVC
     * Wall → /W4 (MSVC /Wall is too verbose)
     * Wextra → /W4
     * Werror → /WX
     * Wpedantic → /W4
     * Otherwise: pass through as /Wnnnn if numeric, else skip */
    for (size_t i = 0; i < p->compile.warnings.count; i++) {
        const char *w = p->compile.warnings.items[i];
        if (strcmp(w, "Wall") == 0 || strcmp(w, "Wextra") == 0 ||
            strcmp(w, "Wpedantic") == 0)
            tmp_argv[tmp_argc++] = "/W4";
        else if (strcmp(w, "Werror") == 0)
            tmp_argv[tmp_argc++] = "/WX";
    }

    /* Defines: /D */
    char *def_bufs[64];
    size_t ndef = 0;
    for (size_t i = 0; i < p->compile.defines.count && ndef < 64; i++) {
        size_t dlen = strlen(p->compile.defines.items[i]) + 4;
        def_bufs[ndef] = malloc(dlen);
        if (def_bufs[ndef]) {
            snprintf(def_bufs[ndef], dlen, "/D%s", p->compile.defines.items[i]);
            tmp_argv[tmp_argc++] = def_bufs[ndef];
            ndef++;
        }
    }

    /* Include paths: /I */
    char *inc_bufs[32];
    size_t ninc = 0;
    if (p->sources.headers) {
        char *hdr_full = now_path_join(basedir, p->sources.headers);
        if (hdr_full) {
            size_t ilen = strlen(hdr_full) + 4;
            inc_bufs[ninc] = malloc(ilen);
            if (inc_bufs[ninc]) {
                snprintf(inc_bufs[ninc], ilen, "/I%s", hdr_full);
                tmp_argv[tmp_argc++] = inc_bufs[ninc];
                ninc++;
            }
            free(hdr_full);
        }
    }
    if (p->sources.private_headers && ninc < 32) {
        char *prv_full = now_path_join(basedir, p->sources.private_headers);
        if (prv_full) {
            size_t ilen = strlen(prv_full) + 4;
            inc_bufs[ninc] = malloc(ilen);
            if (inc_bufs[ninc]) {
                snprintf(inc_bufs[ninc], ilen, "/I%s", prv_full);
                tmp_argv[tmp_argc++] = inc_bufs[ninc];
                ninc++;
            }
            free(prv_full);
        }
    }
    for (size_t i = 0; i < p->compile.includes.count && ninc < 32; i++) {
        char *inc_full = now_path_join(basedir, p->compile.includes.items[i]);
        if (inc_full) {
            size_t ilen = strlen(inc_full) + 4;
            inc_bufs[ninc] = malloc(ilen);
            if (inc_bufs[ninc]) {
                snprintf(inc_bufs[ninc], ilen, "/I%s", inc_full);
                tmp_argv[tmp_argc++] = inc_bufs[ninc];
                ninc++;
            }
            free(inc_full);
        }
    }

    /* Dependency include paths (from procure) — already have /I or -I prefix */
    for (size_t i = 0; i < ctx->dep_includes.count; i++)
        tmp_argv[tmp_argc++] = ctx->dep_includes.paths[i];

    /* Extra compile flags */
    for (size_t i = 0; i < p->compile.flags.count; i++)
        tmp_argv[tmp_argc++] = p->compile.flags.items[i];

    /* Optimization:
     * none  → /Od
     * debug → /Od /Zi
     * size  → /O1
     * speed → /O2
     * lto   → /O2 /GL */
    if (p->compile.opt) {
        if (strcmp(p->compile.opt, "none") == 0)
            tmp_argv[tmp_argc++] = "/Od";
        else if (strcmp(p->compile.opt, "debug") == 0) {
            tmp_argv[tmp_argc++] = "/Od";
            tmp_argv[tmp_argc++] = "/Zi";
        }
        else if (strcmp(p->compile.opt, "size") == 0)
            tmp_argv[tmp_argc++] = "/O1";
        else if (strcmp(p->compile.opt, "speed") == 0)
            tmp_argv[tmp_argc++] = "/O2";
        else if (strcmp(p->compile.opt, "lto") == 0) {
            tmp_argv[tmp_argc++] = "/O2";
            tmp_argv[tmp_argc++] = "/GL";
        }
    }

    /* Header dependency tracking */
    tmp_argv[tmp_argc++] = "/showIncludes";

    /* Compile only */
    tmp_argv[tmp_argc++] = "/c";

    /* Source file */
    char *src_full = now_path_join(basedir, src_rel);
    tmp_argv[tmp_argc++] = src_full;

    /* Output: /Fo"path" */
    char fo_buf[520];
    snprintf(fo_buf, sizeof(fo_buf), "/Fo%s", obj);
    tmp_argv[tmp_argc++] = fo_buf;

    /* Copy everything into owned strings */
    job->argv = (char **)malloc((tmp_argc + 1) * sizeof(char *));
    if (!job->argv) {
        free(src_full);
        free(obj);
        for (size_t i = 0; i < ndef; i++) free(def_bufs[i]);
        for (size_t i = 0; i < ninc; i++) free(inc_bufs[i]);
        return -1;
    }
    for (int i = 0; i < tmp_argc; i++)
        job->argv[i] = strdup(tmp_argv[i]);
    job->argv[tmp_argc] = NULL;
    job->argc = tmp_argc;

    job->src_rel  = strdup(src_rel);
    job->obj_path = strdup(obj);

    free(src_full);
    free(obj);
    for (size_t i = 0; i < ndef; i++) free(def_bufs[i]);
    for (size_t i = 0; i < ninc; i++) free(inc_bufs[i]);

    return 0;
}

/* Build the argument list for compiling one source file (GCC/Clang).
 * Returns 0 on success, fills job with owned copies of all strings. */
/* ---- Rust compile job ---- */

static int build_compile_job_rust(NowBuildCtx *ctx, const char *src_rel,
                                    const NowLangType *type, const NowLangDef *lang,
                                    NowCompileJob *job) {
    const NowProject *p = ctx->project;
    const char *basedir = ctx->basedir;
    (void)type;
    memset(job, 0, sizeof(*job));

    const char *rustc = ctx->toolchain.rustc;
    if (!rustc) return -1;

    /* Object output path */
    char *obj = now_obj_path(basedir, src_rel, p->sources.dir, ctx->target_dir);
    if (!obj) return -1;

    /* Ensure object directory exists */
    char *obj_dir = strdup(obj);
    char *sep = strrchr(obj_dir, '/');
    if (!sep) sep = strrchr(obj_dir, '\\');
    if (sep) { *sep = '\0'; now_mkdir_p(obj_dir); }
    free(obj_dir);

    /* Full source path */
    char *src_full = now_path_join(basedir, src_rel);
    if (!src_full) { free(obj); return -1; }

    /* Build argv: rustc --emit obj --crate-type staticlib --edition XXXX -o obj src */
    const char *tmp_argv[32];
    int tmp_argc = 0;

    tmp_argv[tmp_argc++] = rustc;
    tmp_argv[tmp_argc++] = "--crate-type";
    tmp_argv[tmp_argc++] = "staticlib";

    /* Edition (default 2021) */
    char edition_buf[32] = {0};
    const char *std = p->compile.std ? p->compile.std : p->std;
    if (std && (strcmp(std, "2015") == 0 || strcmp(std, "2018") == 0 ||
                strcmp(std, "2021") == 0 || strcmp(std, "2024") == 0)) {
        snprintf(edition_buf, sizeof(edition_buf), "%s", std);
    } else {
        snprintf(edition_buf, sizeof(edition_buf), "2021");
    }
    tmp_argv[tmp_argc++] = "--edition";
    tmp_argv[tmp_argc++] = edition_buf;

    tmp_argv[tmp_argc++] = "-o";
    tmp_argv[tmp_argc++] = obj;
    tmp_argv[tmp_argc++] = src_full;
    tmp_argv[tmp_argc] = NULL;

    /* Copy to job */
    job->argv = (char **)malloc((tmp_argc + 1) * sizeof(char *));
    if (!job->argv) { free(obj); free(src_full); return -1; }
    for (int i = 0; i < tmp_argc; i++)
        job->argv[i] = strdup(tmp_argv[i]);
    job->argv[tmp_argc] = NULL;
    job->argc = tmp_argc;
    job->src_rel = strdup(src_rel);
    job->obj_path = obj;

    free(src_full);
    return 0;
}

/* ---- Go compile job (cgo c-archive) ---- */

static int build_compile_job_go(NowBuildCtx *ctx, const char *src_rel,
                                  const NowLangType *type, const NowLangDef *lang,
                                  NowCompileJob *job) {
    const NowProject *p = ctx->project;
    const char *basedir = ctx->basedir;
    (void)type; (void)lang;
    memset(job, 0, sizeof(*job));

    const char *go = ctx->toolchain.go;
    if (!go) return -1;

    /* Output .a path */
    char *obj = now_obj_path(basedir, src_rel, p->sources.dir, ctx->target_dir);
    if (!obj) return -1;

    char *obj_dir = strdup(obj);
    char *sep = strrchr(obj_dir, '/');
    if (!sep) sep = strrchr(obj_dir, '\\');
    if (sep) { *sep = '\0'; now_mkdir_p(obj_dir); }
    free(obj_dir);

    char *src_full = now_path_join(basedir, src_rel);
    if (!src_full) { free(obj); return -1; }

    /* go build -buildmode=c-archive -o obj src */
    const char *tmp_argv[16];
    int tmp_argc = 0;
    tmp_argv[tmp_argc++] = go;
    tmp_argv[tmp_argc++] = "build";
    tmp_argv[tmp_argc++] = "-buildmode=c-archive";
    tmp_argv[tmp_argc++] = "-o";
    tmp_argv[tmp_argc++] = obj;
    tmp_argv[tmp_argc++] = src_full;
    tmp_argv[tmp_argc] = NULL;

    job->argv = (char **)malloc((tmp_argc + 1) * sizeof(char *));
    if (!job->argv) { free(obj); free(src_full); return -1; }
    for (int i = 0; i < tmp_argc; i++)
        job->argv[i] = strdup(tmp_argv[i]);
    job->argv[tmp_argc] = NULL;
    job->argc = tmp_argc;
    job->src_rel = strdup(src_rel);
    job->obj_path = obj;

    free(src_full);
    return 0;
}

/* ---- GCC/Clang compile job ---- */

static int build_compile_job(NowBuildCtx *ctx, const char *src_rel,
                             const NowLangType *type, const NowLangDef *lang,
                             NowCompileJob *job) {
    const NowProject *p = ctx->project;
    const char *basedir = ctx->basedir;
    memset(job, 0, sizeof(*job));

    /* Resolve tool */
    const char *tool = ctx->toolchain.cc;
    if (type->tool_var && strcmp(type->tool_var, "${cxx}") == 0)
        tool = ctx->toolchain.cxx;
    else if (type->tool_var && strcmp(type->tool_var, "${as}") == 0)
        tool = ctx->toolchain.as;

    /* Derive object path */
    char *obj = now_obj_path(basedir, src_rel, p->sources.dir, ctx->target_dir);
    if (!obj) return -1;

    /* Ensure object directory exists */
    char *obj_dir = strdup(obj);
    char *last_sep = strrchr(obj_dir, '/');
    if (!last_sep) last_sep = strrchr(obj_dir, '\\');
    if (last_sep) {
        *last_sep = '\0';
        now_mkdir_p(obj_dir);
    }
    free(obj_dir);

    /* Build argument list into a temporary stack array, then copy */
    const char *tmp_argv[128];
    int tmp_argc = 0;

    tmp_argv[tmp_argc++] = tool;

    /* Standard flag */
    char std_buf[32] = {0};
    const char *std = p->compile.std ? p->compile.std : p->std;
    if (std && lang->std_flag) {
        snprintf(std_buf, sizeof(std_buf), "-std=%s", std);
        tmp_argv[tmp_argc++] = std_buf;
    }

    /* Warnings */
    char *warn_bufs[32];
    size_t nwarn = 0;
    for (size_t i = 0; i < p->compile.warnings.count && nwarn < 32; i++) {
        const char *w = p->compile.warnings.items[i];
        size_t wlen = strlen(w) + 3;
        warn_bufs[nwarn] = malloc(wlen);
        if (warn_bufs[nwarn]) {
            snprintf(warn_bufs[nwarn], wlen, "-%s", w);
            tmp_argv[tmp_argc++] = warn_bufs[nwarn];
            nwarn++;
        }
    }

    /* Defines */
    char *def_bufs[64];
    size_t ndef = 0;
    for (size_t i = 0; i < p->compile.defines.count && ndef < 64; i++) {
        size_t dlen = strlen(p->compile.defines.items[i]) + 3;
        def_bufs[ndef] = malloc(dlen);
        if (def_bufs[ndef]) {
            snprintf(def_bufs[ndef], dlen, "-D%s", p->compile.defines.items[i]);
            tmp_argv[tmp_argc++] = def_bufs[ndef];
            ndef++;
        }
    }

    /* Include paths */
    char *inc_bufs[32];
    size_t ninc = 0;
    if (p->sources.headers) {
        char *hdr_full = now_path_join(basedir, p->sources.headers);
        if (hdr_full) {
            size_t ilen = strlen(hdr_full) + 3;
            inc_bufs[ninc] = malloc(ilen);
            if (inc_bufs[ninc]) {
                snprintf(inc_bufs[ninc], ilen, "-I%s", hdr_full);
                tmp_argv[tmp_argc++] = inc_bufs[ninc];
                ninc++;
            }
            free(hdr_full);
        }
    }
    if (p->sources.private_headers && ninc < 32) {
        char *prv_full = now_path_join(basedir, p->sources.private_headers);
        if (prv_full) {
            size_t ilen = strlen(prv_full) + 3;
            inc_bufs[ninc] = malloc(ilen);
            if (inc_bufs[ninc]) {
                snprintf(inc_bufs[ninc], ilen, "-I%s", prv_full);
                tmp_argv[tmp_argc++] = inc_bufs[ninc];
                ninc++;
            }
            free(prv_full);
        }
    }
    for (size_t i = 0; i < p->compile.includes.count && ninc < 32; i++) {
        char *inc_full = now_path_join(basedir, p->compile.includes.items[i]);
        if (inc_full) {
            size_t ilen = strlen(inc_full) + 3;
            inc_bufs[ninc] = malloc(ilen);
            if (inc_bufs[ninc]) {
                snprintf(inc_bufs[ninc], ilen, "-I%s", inc_full);
                tmp_argv[tmp_argc++] = inc_bufs[ninc];
                ninc++;
            }
            free(inc_full);
        }
    }

    /* Dependency include paths (from procure) */
    for (size_t i = 0; i < ctx->dep_includes.count; i++)
        tmp_argv[tmp_argc++] = ctx->dep_includes.paths[i];

    /* Extra compile flags */
    for (size_t i = 0; i < p->compile.flags.count; i++)
        tmp_argv[tmp_argc++] = p->compile.flags.items[i];

    /* Optimization */
    const char *opt_flag = NULL;
    if (p->compile.opt) {
        if (strcmp(p->compile.opt, "none") == 0)       opt_flag = "-O0";
        else if (strcmp(p->compile.opt, "debug") == 0) opt_flag = "-Og";
        else if (strcmp(p->compile.opt, "size") == 0)  opt_flag = "-Os";
        else if (strcmp(p->compile.opt, "speed") == 0) opt_flag = "-O2";
        else if (strcmp(p->compile.opt, "lto") == 0)   opt_flag = "-O2";
        if (opt_flag) tmp_argv[tmp_argc++] = opt_flag;
        if (strcmp(p->compile.opt, "lto") == 0)
            tmp_argv[tmp_argc++] = "-flto";
    }

    /* Dependency file for header tracking */
    size_t obj_len = strlen(obj);
    char *dep_file = (char *)malloc(obj_len + 3);
    if (dep_file) {
        memcpy(dep_file, obj, obj_len);
        memcpy(dep_file + obj_len, ".d", 3);
        tmp_argv[tmp_argc++] = "-MD";
        tmp_argv[tmp_argc++] = "-MF";
        tmp_argv[tmp_argc++] = dep_file;
    }

    tmp_argv[tmp_argc++] = "-c";

    char *src_full = now_path_join(basedir, src_rel);
    tmp_argv[tmp_argc++] = src_full;

    tmp_argv[tmp_argc++] = "-o";
    tmp_argv[tmp_argc++] = obj;

    /* Now copy everything into owned strings */
    job->argv = (char **)malloc((tmp_argc + 1) * sizeof(char *));
    if (!job->argv) {
        free(dep_file);
        free(src_full);
        free(obj);
        for (size_t i = 0; i < nwarn; i++) free(warn_bufs[i]);
        for (size_t i = 0; i < ndef; i++)  free(def_bufs[i]);
        for (size_t i = 0; i < ninc; i++)  free(inc_bufs[i]);
        return -1;
    }
    for (int i = 0; i < tmp_argc; i++)
        job->argv[i] = strdup(tmp_argv[i]);
    job->argv[tmp_argc] = NULL;
    job->argc = tmp_argc;

    job->src_rel  = strdup(src_rel);
    job->obj_path = strdup(obj);
    job->dep_path = dep_file ? strdup(dep_file) : NULL;

    /* Free temporaries (the copies are in job->argv now) */
    free(dep_file);
    free(src_full);
    free(obj);
    for (size_t i = 0; i < nwarn; i++) free(warn_bufs[i]);
    for (size_t i = 0; i < ndef; i++)  free(def_bufs[i]);
    for (size_t i = 0; i < ninc; i++)  free(inc_bufs[i]);

    return 0;
}

/* Build a hash of the compile flags for a given source file.
 * Used for incremental rebuild detection. */
static char *link_flags_hash(const NowProject *p);
static char *compile_flags_hash(const NowProject *p) {
    /* Concatenate all flags that affect compilation */
    size_t cap = 256;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t len = 0;

    /* Helper to append */
    #define APPEND(s) do { \
        size_t sl = strlen(s); \
        while (len + sl + 2 > cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) return NULL; } \
        memcpy(buf + len, s, sl); len += sl; buf[len++] = '\n'; buf[len] = '\0'; \
    } while(0)

    if (p->std) APPEND(p->std);
    if (p->compile.std) APPEND(p->compile.std);
    if (p->compile.opt) APPEND(p->compile.opt);
    for (size_t i = 0; i < p->compile.warnings.count; i++)
        APPEND(p->compile.warnings.items[i]);
    for (size_t i = 0; i < p->compile.defines.count; i++)
        APPEND(p->compile.defines.items[i]);
    for (size_t i = 0; i < p->compile.includes.count; i++)
        APPEND(p->compile.includes.items[i]);
    for (size_t i = 0; i < p->compile.flags.count; i++)
        APPEND(p->compile.flags.items[i]);
    #undef APPEND

    char *hash = now_sha256_string(buf, len);
    free(buf);
    return hash;
}

/* ================================================================
 * Java build support
 *
 * Java compilation: all .java sources → javac → target/classes/
 * Java linking:     target/classes/ → jar → target/bin/{name}.jar
 * Java testing:     test .java → javac → target/test-classes/ → java
 * ================================================================ */

/* Compile all Java sources in one javac invocation.
 * javac --release {std} -d target/classes -cp {classpath} src1.java src2.java ...
 * Returns 0 on success, non-zero on error. */
static int build_java_compile(NowBuildCtx *ctx, NowResult *result) {
    const NowProject *p = ctx->project;
    const char *javac = ctx->toolchain.javac;
    if (!javac) {
        if (result) {
            result->code = NOW_ERR_TOOL;
            snprintf(result->message, sizeof(result->message),
                     "javac not found (set JAVAC env var)");
        }
        return -1;
    }

    /* Create output directory: target/classes */
    char *classes_dir = now_path_join(ctx->basedir, "target/classes");
    if (!classes_dir) return -1;
    now_mkdir_p(classes_dir);

    /* Collect .java source files */
    size_t java_count = 0;
    for (size_t i = 0; i < ctx->sources.count; i++) {
        const char *ext = strrchr(ctx->sources.paths[i], '.');
        if (ext && strcmp(ext, ".java") == 0) java_count++;
    }

    if (java_count == 0) {
        free(classes_dir);
        if (result) {
            result->code = NOW_OK;
            result->message[0] = '\0';
        }
        return 0;
    }

    /* Build argv: javac --release N -d target/classes [-cp ...] [-encoding ...] src1.java ... */
    size_t argv_cap = java_count + 20 + p->java.classpath.count + p->compile.flags.count;
    const char **argv = (const char **)calloc(argv_cap, sizeof(char *));
    if (!argv) { free(classes_dir); return -1; }
    int argc = 0;

    argv[argc++] = javac;

    /* --release flag */
    char release_buf[32] = {0};
    const char *std = p->compile.std ? p->compile.std : p->std;
    if (std) {
        snprintf(release_buf, sizeof(release_buf), "--release");
        argv[argc++] = release_buf;
        argv[argc++] = std;
    }

    /* Output directory */
    argv[argc++] = "-d";
    argv[argc++] = classes_dir;

    /* Encoding */
    const char *encoding = p->java.encoding ? p->java.encoding : "UTF-8";
    argv[argc++] = "-encoding";
    argv[argc++] = encoding;

    /* Classpath: dep jars + user classpath entries */
    char cp_buf[4096] = {0};
    size_t cp_len = 0;

    /* Add dependency paths */
    for (size_t i = 0; i < ctx->dep_libdirs.count; i++) {
        if (cp_len > 0) {
#ifdef _WIN32
            cp_buf[cp_len++] = ';';
#else
            cp_buf[cp_len++] = ':';
#endif
        }
        size_t dlen = strlen(ctx->dep_libdirs.paths[i]);
        if (cp_len + dlen + 2 < sizeof(cp_buf)) {
            memcpy(cp_buf + cp_len, ctx->dep_libdirs.paths[i], dlen);
            cp_len += dlen;
        }
    }

    /* Add user classpath entries */
    for (size_t i = 0; i < p->java.classpath.count; i++) {
        if (cp_len > 0) {
#ifdef _WIN32
            cp_buf[cp_len++] = ';';
#else
            cp_buf[cp_len++] = ':';
#endif
        }
        size_t elen = strlen(p->java.classpath.items[i]);
        if (cp_len + elen + 2 < sizeof(cp_buf)) {
            memcpy(cp_buf + cp_len, p->java.classpath.items[i], elen);
            cp_len += elen;
        }
    }
    cp_buf[cp_len] = '\0';

    if (cp_len > 0) {
        argv[argc++] = "-cp";
        argv[argc++] = cp_buf;
    }

    /* Extra compiler flags */
    for (size_t i = 0; i < p->compile.flags.count; i++)
        argv[argc++] = p->compile.flags.items[i];

    /* Source files (full paths) */
    char **src_paths = NULL;
    size_t src_count = 0;
    src_paths = (char **)calloc(java_count, sizeof(char *));
    if (!src_paths) { free(argv); free(classes_dir); return -1; }

    for (size_t i = 0; i < ctx->sources.count; i++) {
        const char *ext = strrchr(ctx->sources.paths[i], '.');
        if (ext && strcmp(ext, ".java") == 0) {
            src_paths[src_count] = now_path_join(ctx->basedir, ctx->sources.paths[i]);
            if (src_paths[src_count])
                argv[argc++] = src_paths[src_count];
            src_count++;
        }
    }
    argv[argc] = NULL;

    if (ctx->verbose) {
        fprintf(stderr, " ");
        for (int i = 0; i < argc; i++)
            fprintf(stderr, " %s", argv[i]);
        fprintf(stderr, "\n");
    }

    int rc = now_exec((const char *const *)argv, 0);
    if (rc != 0) {
        if (result) {
            result->code = NOW_ERR_TOOL;
            snprintf(result->message, sizeof(result->message),
                     "javac failed (exit %d)", rc);
        }
    } else {
        /* Register the classes directory as an "object" for the link phase */
        now_filelist_push(&ctx->objects, classes_dir);

        if (ctx->verbose)
            fprintf(stderr, "  compiled %zu Java source(s) to %s\n",
                    java_count, classes_dir);
    }

    for (size_t i = 0; i < src_count; i++) free(src_paths[i]);
    free(src_paths);
    free((void *)argv);
    free(classes_dir);
    return rc;
}

/* Package .class files into a JAR.
 * jar cf target/bin/{name}.jar -C target/classes .
 * For executables, adds Main-Class manifest entry.
 * Returns 0 on success. */
static int build_java_link(NowBuildCtx *ctx, NowResult *result) {
    const NowProject *p = ctx->project;
    const char *jar_tool = ctx->toolchain.jar;
    if (!jar_tool) {
        if (result) {
            result->code = NOW_ERR_TOOL;
            snprintf(result->message, sizeof(result->message),
                     "jar not found (set JAR env var)");
        }
        return -1;
    }

    /* Ensure output directory */
    char *bin_dir = now_path_join(ctx->basedir, "target/bin");
    if (!bin_dir) return -1;
    now_mkdir_p(bin_dir);

    /* Output JAR path */
    const char *name = p->output.name ? p->output.name :
                       (p->artifact ? p->artifact : "output");
    char *jar_path = (char *)malloc(strlen(bin_dir) + strlen(name) + 8);
    if (!jar_path) { free(bin_dir); return -1; }
    sprintf(jar_path, "%s/%s.jar", bin_dir, name);

    /* Classes directory */
    char *classes_dir = now_path_join(ctx->basedir, "target/classes");
    if (!classes_dir) { free(jar_path); free(bin_dir); return -1; }

    const char *argv[16];
    int argc = 0;
    argv[argc++] = jar_tool;

    /* Check if executable with main class */
    const char *out_type = p->output.type ? p->output.type : "jar";
    int is_exec = (strcmp(out_type, "executable") == 0) && p->java.main_class;

    if (is_exec) {
        /* jar cfe output.jar MainClass -C target/classes . */
        argv[argc++] = "cfe";
        argv[argc++] = jar_path;
        argv[argc++] = p->java.main_class;
    } else {
        /* jar cf output.jar -C target/classes . */
        argv[argc++] = "cf";
        argv[argc++] = jar_path;
    }

    argv[argc++] = "-C";
    argv[argc++] = classes_dir;
    argv[argc++] = ".";
    argv[argc] = NULL;

    if (ctx->verbose) {
        fprintf(stderr, " ");
        for (int i = 0; i < argc; i++)
            fprintf(stderr, " %s", argv[i]);
        fprintf(stderr, "\n");
    }

    int rc = now_exec((const char *const *)argv, 0);
    if (rc != 0) {
        if (result) {
            result->code = NOW_ERR_TOOL;
            snprintf(result->message, sizeof(result->message),
                     "jar failed (exit %d)", rc);
        }
    } else {
        if (ctx->verbose)
            fprintf(stderr, "  packaged %s\n", jar_path);
    }

    free(jar_path);
    free(classes_dir);
    free(bin_dir);
    return rc;
}

/* Compile and run Java tests.
 * javac --release N -d target/test-classes -cp target/classes:deps src/test/java/*.java
 * java -cp target/test-classes:target/classes:deps MainTestClass
 * Returns 0 on success. */
static int build_java_test(NowBuildCtx *ctx, NowResult *result) {
    const NowProject *p = ctx->project;
    const char *javac = ctx->toolchain.javac;
    const char *java  = ctx->toolchain.java;
    if (!javac || !java) {
        if (result) {
            result->code = NOW_ERR_TOOL;
            snprintf(result->message, sizeof(result->message),
                     "javac/java not found for test execution");
        }
        return -1;
    }

    /* Check if test directory exists */
    const char *test_dir_rel = p->tests.dir ? p->tests.dir : "src/test/java";
    char *test_dir = now_path_join(ctx->basedir, test_dir_rel);
    if (!test_dir || !now_path_exists(test_dir)) {
        free(test_dir);
        if (result) {
            result->code = NOW_OK;
            snprintf(result->message, sizeof(result->message), "no test directory");
        }
        return 0;
    }

    /* Discover test .java files */
    NowFileList test_sources;
    memset(&test_sources, 0, sizeof(test_sources));
    const char *java_exts[] = { ".java", NULL };
    now_discover_sources(ctx->basedir, test_dir_rel, java_exts, &test_sources);

    size_t java_test_count = 0;
    for (size_t i = 0; i < test_sources.count; i++) {
        const char *ext = strrchr(test_sources.paths[i], '.');
        if (ext && strcmp(ext, ".java") == 0) java_test_count++;
    }

    if (java_test_count == 0) {
        now_filelist_free(&test_sources);
        free(test_dir);
        if (result) {
            result->code = NOW_OK;
            snprintf(result->message, sizeof(result->message), "no test sources");
        }
        return 0;
    }

    /* Create test-classes directory */
    char *test_classes = now_path_join(ctx->basedir, "target/test-classes");
    if (!test_classes) {
        now_filelist_free(&test_sources);
        free(test_dir);
        return -1;
    }
    now_mkdir_p(test_classes);

    /* Build classpath: target/classes + deps */
    char *classes_dir = now_path_join(ctx->basedir, "target/classes");
    char cp[4096];
#ifdef _WIN32
    const char *sep = ";";
#else
    const char *sep = ":";
#endif
    snprintf(cp, sizeof(cp), "%s", classes_dir ? classes_dir : ".");
    for (size_t i = 0; i < ctx->dep_libdirs.count; i++) {
        size_t cur = strlen(cp);
        snprintf(cp + cur, sizeof(cp) - cur, "%s%s", sep, ctx->dep_libdirs.paths[i]);
    }
    for (size_t i = 0; i < p->java.classpath.count; i++) {
        size_t cur = strlen(cp);
        snprintf(cp + cur, sizeof(cp) - cur, "%s%s", sep, p->java.classpath.items[i]);
    }

    /* Phase 1: Compile test sources */
    size_t argv_cap = java_test_count + 16;
    const char **argv = (const char **)calloc(argv_cap, sizeof(char *));
    if (!argv) {
        now_filelist_free(&test_sources);
        free(test_dir); free(test_classes); free(classes_dir);
        return -1;
    }
    int argc = 0;

    argv[argc++] = javac;

    const char *std = p->compile.std ? p->compile.std : p->std;
    char release_buf[32];
    if (std) {
        snprintf(release_buf, sizeof(release_buf), "--release");
        argv[argc++] = release_buf;
        argv[argc++] = std;
    }

    argv[argc++] = "-d";
    argv[argc++] = test_classes;
    argv[argc++] = "-cp";
    argv[argc++] = cp;

    /* Add test source files (full paths) */
    char **test_full_paths = (char **)calloc(java_test_count, sizeof(char *));
    size_t tfp_count = 0;
    for (size_t i = 0; i < test_sources.count; i++) {
        const char *ext = strrchr(test_sources.paths[i], '.');
        if (ext && strcmp(ext, ".java") == 0) {
            test_full_paths[tfp_count] = now_path_join(ctx->basedir, test_sources.paths[i]);
            if (test_full_paths[tfp_count])
                argv[argc++] = test_full_paths[tfp_count];
            tfp_count++;
        }
    }
    argv[argc] = NULL;

    if (ctx->verbose) {
        fprintf(stderr, " ");
        for (int i = 0; i < argc; i++)
            fprintf(stderr, " %s", argv[i]);
        fprintf(stderr, "\n");
    }

    int rc = now_exec((const char *const *)argv, 0);
    free((void *)argv);
    for (size_t i = 0; i < tfp_count; i++) free(test_full_paths[i]);
    free(test_full_paths);

    if (rc != 0) {
        if (result) {
            result->code = NOW_ERR_TOOL;
            snprintf(result->message, sizeof(result->message),
                     "javac (test) failed (exit %d)", rc);
        }
        now_filelist_free(&test_sources);
        free(test_dir); free(test_classes); free(classes_dir);
        return rc;
    }

    /* Phase 2: Run tests — find a class with main() */
    /* Build test classpath: test-classes:classes:deps */
    char test_cp[4096];
    snprintf(test_cp, sizeof(test_cp), "%s%s%s", test_classes, sep, cp);

    /* Look for a test runner class by scanning for *Test.java files */
    const char *test_class = NULL;
    char class_name[256] = {0};
    for (size_t i = 0; i < test_sources.count; i++) {
        const char *path = test_sources.paths[i];
        const char *ext = strrchr(path, '.');
        if (!ext || strcmp(ext, ".java") != 0) continue;

        /* Extract class name from filename */
        const char *slash = strrchr(path, '/');
        if (!slash) slash = strrchr(path, '\\');
        const char *fname = slash ? slash + 1 : path;
        size_t namelen = (size_t)(ext - fname);
        if (namelen >= sizeof(class_name)) continue;
        memcpy(class_name, fname, namelen);
        class_name[namelen] = '\0';
        test_class = class_name;
        break;  /* Use first test file */
    }

    if (test_class) {
        const char *run_argv[8];
        int run_argc = 0;
        run_argv[run_argc++] = java;
        run_argv[run_argc++] = "-cp";
        run_argv[run_argc++] = test_cp;
        run_argv[run_argc++] = test_class;
        run_argv[run_argc] = NULL;

        if (ctx->verbose) {
            fprintf(stderr, " ");
            for (int i = 0; i < run_argc; i++)
                fprintf(stderr, " %s", run_argv[i]);
            fprintf(stderr, "\n");
        }

        rc = now_exec((const char *const *)run_argv, 0);
        if (rc != 0) {
            if (result) {
                result->code = NOW_ERR_TOOL;
                snprintf(result->message, sizeof(result->message),
                         "test %s failed (exit %d)", test_class, rc);
            }
        }
    }

    now_filelist_free(&test_sources);
    free(test_dir);
    free(test_classes);
    free(classes_dir);
    return rc;
}

NOW_API int now_build_compile(NowBuildCtx *ctx, NowResult *result) {
    const NowProject *p = ctx->project;
    int errors = 0;

    /* Java projects use a completely different compilation model */
    if (has_java_lang(p) && p->langs.count > 0 &&
        strcmp(p->langs.items[0], "java") == 0) {
        return build_java_compile(ctx, result);
    }

    /* Reproducibility: parse config and resolve timebase */
    NowReproConfig repro;
    now_repro_from_project(&repro, p);

    char *repro_timestamp = NULL;
    char **repro_flags = NULL;
    size_t repro_flag_count = 0;

    if (repro.enabled) {
        repro_timestamp = now_repro_resolve_timebase(&repro, ctx->basedir, result);
        if (repro.path_prefix_map || repro.no_date_macros) {
            now_repro_compile_flags(&repro, ctx->basedir, repro_timestamp,
                                    ctx->toolchain.is_msvc,
                                    &repro_flags, &repro_flag_count);
        }
        /* Sort source list for deterministic ordering */
        if (repro.sort_inputs)
            now_repro_sort_filelist(&ctx->sources);
    }

    /* Module pre-scan: detect C++20 module declarations and reorder sources */
    NowModuleScan modscan;
    now_module_scan_init(&modscan);
    int has_modules = 0;

    /* Check if C++ is active and std >= c++20 */
    const char *std_check = p->compile.std ? p->compile.std : p->std;
    int cxx20_or_later = std_check && (
        strcmp(std_check, "c++20") == 0 || strcmp(std_check, "c++2a") == 0 ||
        strcmp(std_check, "c++23") == 0 || strcmp(std_check, "c++2b") == 0 ||
        strcmp(std_check, "c++26") == 0 || strcmp(std_check, "c++latest") == 0);

    if (cxx20_or_later) {
        /* Scan all sources for module declarations */
        for (size_t i = 0; i < ctx->sources.count; i++) {
            const char *src = ctx->sources.paths[i];
            const char *ext = strrchr(src, '.');
            if (ext && (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cppm") == 0 ||
                        strcmp(ext, ".ixx") == 0 || strcmp(ext, ".ccm") == 0 ||
                        strcmp(ext, ".cxx") == 0 || strcmp(ext, ".cc") == 0)) {
                char *full = now_path_join(ctx->basedir, src);
                if (full) {
                    now_module_scan_file(&modscan, full);
                    free(full);
                }
            }
        }
        has_modules = (int)modscan.unit_count > 0;

        /* Reorder sources: module interfaces first, then impl, then regular */
        if (has_modules) {
            NowModuleOrder morder;
            if (now_module_order(&modscan,
                                  (const char *const *)ctx->sources.paths,
                                  ctx->sources.count, &morder) == 0) {
                /* Replace source list with ordered list */
                for (size_t i = 0; i < ctx->sources.count; i++)
                    free(ctx->sources.paths[i]);
                free(ctx->sources.paths);
                ctx->sources.paths = morder.paths;
                ctx->sources.count = morder.count;
                /* Don't free morder — we took ownership of its paths */
                morder.paths = NULL;
                morder.count = 0;
                now_module_order_free(&morder);
            }

            /* Ensure BMI output directory exists */
            char *bmi_dir = now_path_join(ctx->target_dir, "bmi");
            if (bmi_dir) {
                now_mkdir_p(bmi_dir);
                free(bmi_dir);
            }
        }
    }

    /* Load manifest for incremental builds */
    NowManifest manifest;
    char *manifest_path = now_path_join(ctx->basedir, "target/.now-manifest");
    now_manifest_load(&manifest, manifest_path);

    /* Hash memoization cache — avoids re-hashing same headers across source files */
    NowHashMemo hash_memo;
    now_hash_memo_init(&hash_memo, 1024);
    now_hash_memo_global = &hash_memo;

    /* Compute flags hash once for all sources */
    char *fhash = compile_flags_hash(p);

    /* Determine job count */
    int max_jobs = ctx->jobs > 0 ? ctx->jobs : now_cpu_count();
    if (max_jobs < 1) max_jobs = 1;
    if (max_jobs > 64) max_jobs = 64;

    /* Phase 1: Classify sources, check manifest, build job list */
    size_t njobs = 0;
    size_t jobs_cap = ctx->sources.count > 0 ? ctx->sources.count : 1;
    NowCompileJob *jobs = (NowCompileJob *)calloc(jobs_cap, sizeof(NowCompileJob));
    if (!jobs) {
        free(fhash);
        free(manifest_path);
        now_manifest_free(&manifest);
        now_module_scan_free(&modscan);
        return -1;
    }

    int skipped = 0;
    int cache_hits = 0;
    int remote_hits = 0;

    /* Load remote cache config (optional, silent on failure) */
    NowRemoteCacheConfig remote_cfg;
    int has_remote = (now_remote_config_load(&remote_cfg) == 0);
    if (has_remote) now_remote_reset();  /* clear circuit breaker from prior build */

    for (size_t i = 0; i < ctx->sources.count; i++) {
        const char *src = ctx->sources.paths[i];

        const NowLangDef *lang = NULL;
        const NowLangType *type = now_lang_classify(
            src, (const char *const *)p->langs.items, p->langs.count, &lang);

        if (!type || type->role != NOW_ROLE_SOURCE) continue;

        /* Check manifest for incremental skip */
        const NowManifestEntry *entry = now_manifest_find(&manifest, src);
        if (!now_manifest_needs_rebuild(entry, ctx->basedir, src, fhash)) {
            if (entry->object)
                now_filelist_push(&ctx->objects, entry->object);
            skipped++;
            if (now_tui_global) now_tui_skip(now_tui_global);
            continue;
        }

        /* Check content-addressable cache (local, then remote) */
        {
            char *src_full = now_path_join(ctx->basedir, src);
            char *src_hash = src_full ? now_sha256_file_memo(src_full, &hash_memo) : NULL;
            if (src_hash) {
                /* Pick compiler based on language */
                const char *compiler = ctx->toolchain.cc;
                if (type->tool_var && strcmp(type->tool_var, "${cxx}") == 0)
                    compiler = ctx->toolchain.cxx;
                if (!compiler) compiler = "";

                char *ckey = now_cache_key(src_hash, fhash, compiler);
                if (ckey) {
                    const char *obj_ext = ctx->toolchain.is_msvc ? ".obj" : ".o";
                    char *obj = ctx->toolchain.is_msvc
                        ? now_obj_path_ex(ctx->basedir, src, p->sources.dir,
                                          ctx->target_dir, ".obj")
                        : now_obj_path(ctx->basedir, src, p->sources.dir,
                                       ctx->target_dir);

                    int restored = 0;

                    /* Try local cache first */
                    if (obj && now_cache_restore_ex(ckey, obj, obj_ext) == 0) {
                        restored = 1;
                        cache_hits++;
                        if (now_tui_global) now_tui_cache_hit(now_tui_global, 0);
                    }

                    /* Try remote cache on local miss */
                    if (!restored && obj && has_remote &&
                        now_remote_cache_restore(&remote_cfg, ckey, obj, obj_ext) == 0) {
                        restored = 1;
                        remote_hits++;
                        if (now_tui_global) now_tui_cache_hit(now_tui_global, 1);
                        /* Populate local cache for next time */
                        now_cache_store(ckey, obj, obj_ext);
                    }

                    if (restored) {
                        struct stat cst;
                        long long mtime = 0;
                        if (src_full && stat(src_full, &cst) == 0)
                            mtime = (long long)cst.st_mtime;
                        now_manifest_set(&manifest, src, obj,
                                         src_hash, fhash, mtime);
                        now_filelist_push(&ctx->objects, obj);
                        free(obj);
                        free(ckey);
                        free(src_hash);
                        free(src_full);
                        continue;
                    }
                    free(obj);
                    free(ckey);
                }
            }
            free(src_hash);
            free(src_full);
        }

        int jrc;
        if (type->tool_var && strcmp(type->tool_var, "${rustc}") == 0)
            jrc = build_compile_job_rust(ctx, src, type, lang, &jobs[njobs]);
        else if (type->tool_var && strcmp(type->tool_var, "${go}") == 0)
            jrc = build_compile_job_go(ctx, src, type, lang, &jobs[njobs]);
        else if (ctx->toolchain.is_msvc)
            jrc = build_compile_job_msvc(ctx, src, type, lang, &jobs[njobs]);
        else
            jrc = build_compile_job(ctx, src, type, lang, &jobs[njobs]);

        if (jrc == 0) {
            /* Inject reproducibility flags into the job argv */
            if (repro_flag_count > 0 && repro_flags) {
                NowCompileJob *job = &jobs[njobs];
                int new_argc = job->argc + (int)repro_flag_count;
                char **new_argv = realloc(job->argv,
                                          (new_argc + 1) * sizeof(char *));
                if (new_argv) {
                    /* Insert repro flags before the last 3 args
                     * (which are: -c src -o obj  OR  /c src /Fo...) */
                    int insert_at = job->argc >= 4 ? job->argc - 4 : 0;
                    /* Shift tail */
                    memmove(new_argv + insert_at + repro_flag_count,
                            new_argv + insert_at,
                            (job->argc - insert_at + 1) * sizeof(char *));
                    for (size_t rf = 0; rf < repro_flag_count; rf++)
                        new_argv[insert_at + rf] = strdup(repro_flags[rf]);
                    job->argv = new_argv;
                    job->argc = new_argc;
                }
            }
            /* Inject C++20 module flags if applicable */
            if (has_modules && !ctx->toolchain.is_msvc) {
                NowCompileJob *job = &jobs[njobs];
                /* Check if this source file is a module unit or imports modules */
                char *src_full = now_path_join(ctx->basedir, src);
                int is_mod = src_full ? now_module_is_module_file(&modscan, src_full) : 0;
                int imports_mod = 0;
                if (src_full) {
                    for (size_t mi = 0; mi < modscan.import_count; mi++) {
                        if (strcmp(modscan.imports[mi].importer_path, src_full) == 0) {
                            imports_mod = 1;
                            break;
                        }
                    }
                }
                free(src_full);

                if (is_mod || imports_mod) {
                    /* GCC/Clang: add -fmodules-ts (GCC) or no extra flag (Clang auto-detects) */
                    int nmod_flags = 1;  /* -fmodules-ts */
                    int new_argc = job->argc + nmod_flags;
                    char **new_argv = realloc(job->argv,
                                              (new_argc + 1) * sizeof(char *));
                    if (new_argv) {
                        /* Insert -fmodules-ts after the std flag (index 2) */
                        int insert_at = 2;
                        if (insert_at > job->argc) insert_at = job->argc;
                        memmove(new_argv + insert_at + nmod_flags,
                                new_argv + insert_at,
                                (job->argc - insert_at + 1) * sizeof(char *));
                        new_argv[insert_at] = strdup("-fmodules-ts");
                        job->argv = new_argv;
                        job->argc = new_argc;
                    }
                }
            } else if (has_modules && ctx->toolchain.is_msvc) {
                NowCompileJob *job = &jobs[njobs];
                char *src_full = now_path_join(ctx->basedir, src);
                int is_iface = 0;
                if (src_full) {
                    for (size_t mi = 0; mi < modscan.unit_count; mi++) {
                        if (modscan.units[mi].is_interface &&
                            strcmp(modscan.units[mi].source_path, src_full) == 0) {
                            is_iface = 1;
                            break;
                        }
                    }
                }
                free(src_full);

                if (is_iface) {
                    /* MSVC: add /interface /ifcOutput target/bmi/ */
                    char *bmi_dir = now_path_join(ctx->target_dir, "bmi/");
                    int nmod_flags = bmi_dir ? 3 : 1;
                    int new_argc = job->argc + nmod_flags;
                    char **new_argv = realloc(job->argv,
                                              (new_argc + 1) * sizeof(char *));
                    if (new_argv) {
                        int insert_at = 2;
                        if (insert_at > job->argc) insert_at = job->argc;
                        memmove(new_argv + insert_at + nmod_flags,
                                new_argv + insert_at,
                                (job->argc - insert_at + 1) * sizeof(char *));
                        new_argv[insert_at] = strdup("/interface");
                        if (bmi_dir) {
                            new_argv[insert_at + 1] = strdup("/ifcOutput");
                            new_argv[insert_at + 2] = bmi_dir;
                        }
                        job->argv = new_argv;
                        job->argc = new_argc;
                    } else {
                        free(bmi_dir);
                    }
                }
            }

            njobs++;
        }
        else
            errors++;
    }

    /* Phase 2: Execute jobs in parallel */
    int compiled = 0;

    if (njobs > 0 && errors == 0) {
        int pool_size = (int)njobs < max_jobs ? (int)njobs : max_jobs;

        /* For single job, skip the pool overhead */
        if (pool_size <= 1) {
            for (size_t i = 0; i < njobs; i++) {
                NowCompileJob *job = &jobs[i];

                if (now_tui_global)
                    now_tui_compile_start(now_tui_global, job->src_rel);
                else if (ctx->verbose) {
                    fprintf(stderr, " ");
                    for (int a = 0; a < job->argc; a++)
                        fprintf(stderr, " %s", job->argv[a]);
                    fprintf(stderr, "\n");
                }

                int rc = now_exec((const char *const *)job->argv, 0);
                if (rc != 0) {
                    if (result) {
                        result->code = NOW_ERR_TOOL;
                        snprintf(result->message, sizeof(result->message),
                                 "compiler failed on %s (exit %d)", job->src_rel, rc);
                    }
                    errors++;
                    if (now_tui_global) now_tui_compile_done(now_tui_global, job->src_rel, 0);
                    continue;
                }

                /* Parse deps from compiler depfile */
                char *src_full = now_path_join(ctx->basedir, job->src_rel);
                NowDepList deps = {0};
                if (job->dep_path)
                    now_depfile_parse(job->dep_path, src_full, &deps);

                /* Update manifest */
                char *src_hash = src_full ? now_sha256_file_memo(src_full, &hash_memo) : NULL;
                struct stat st;
                long long mtime = 0;
                if (src_full && stat(src_full, &st) == 0)
                    mtime = (long long)st.st_mtime;
                now_manifest_set(&manifest, job->src_rel, job->obj_path,
                                 src_hash, fhash, mtime);

                /* Store dep info in manifest */
                if (deps.count > 0) {
                    char **dhashes = (char **)calloc(deps.count, sizeof(char *));
                    if (dhashes) {
                        for (size_t d = 0; d < deps.count; d++)
                            dhashes[d] = now_sha256_file_memo(deps.paths[d], &hash_memo);
                        now_manifest_set_deps(&manifest, job->src_rel,
                            (const char **)deps.paths,
                            (const char **)dhashes, deps.count);
                        for (size_t d = 0; d < deps.count; d++)
                            free(dhashes[d]);
                        free(dhashes);
                    }
                }

                /* Store in content-addressable cache (with deps) */
                if (src_hash) {
                    const char *compiler = job->argv ? job->argv[0] : "";
                    char *ckey = now_cache_key(src_hash, fhash, compiler);
                    if (ckey) {
                        const char *ext = ctx->toolchain.is_msvc ? ".obj" : ".o";
                        now_cache_store_ex(ckey, job->obj_path, ext,
                                           deps.count > 0 ? &deps : NULL);
                        /* Push to remote cache */
                        if (has_remote)
                            now_remote_cache_store(&remote_cfg, ckey,
                                                    job->obj_path, ext);
                        free(ckey);
                    }
                }

                now_deplist_free(&deps);

                /* Clean up depfile */
                if (job->dep_path)
                    remove(job->dep_path);

                free(src_full);
                free(src_hash);

                now_filelist_push(&ctx->objects, job->obj_path);
                compiled++;
                if (now_tui_global) now_tui_compile_done(now_tui_global, job->src_rel, 1);
            }
        } else {
            /* Parallel execution with process pool */
            NowWorkerSlot *slots = (NowWorkerSlot *)calloc(pool_size,
                                                            sizeof(NowWorkerSlot));
            if (!slots) {
                for (size_t j = 0; j < njobs; j++) compile_job_free(&jobs[j]);
                free(jobs);
                free(fhash);
                free(manifest_path);
                now_manifest_free(&manifest);
                return -1;
            }

            size_t next_job = 0;  /* next job to dispatch */
            int running = 0;

            while (next_job < njobs || running > 0) {
                /* Fill available slots */
                while (running < pool_size && next_job < njobs) {
                    /* Find a free slot */
                    int slot_idx = -1;
                    for (int s = 0; s < pool_size; s++) {
                        if (!slots[s].active) { slot_idx = s; break; }
                    }
                    if (slot_idx < 0) break;

                    NowCompileJob *job = &jobs[next_job];

                    if (ctx->verbose) {
                        if (!now_tui_global)
                            fprintf(stderr, "  [%zu/%zu] %s\n",
                                    next_job + 1, njobs, job->src_rel);
                    }
                    if (now_tui_global)
                        now_tui_compile_start(now_tui_global, job->src_rel);

                    slots[slot_idx].source_idx = next_job;
                    if (spawn_captured((const char *const *)job->argv,
                                       &slots[slot_idx]) == 0) {
                        slots[slot_idx].active = 1;
                        running++;
                    } else {
                        /* Failed to spawn */
                        if (result) {
                            result->code = NOW_ERR_TOOL;
                            snprintf(result->message, sizeof(result->message),
                                     "failed to spawn compiler for %s",
                                     job->src_rel);
                        }
                        errors++;
                    }
                    next_job++;
                }

                /* Wait for any worker to finish */
                if (running > 0) {
                    int exit_code;
                    NowCapturedOutput captured;
                    int slot_idx = wait_any_worker(slots, pool_size,
                                                   &exit_code, &captured);
                    if (slot_idx < 0) {
                        errors++;
                        running--;
                        continue;
                    }

                    running--;
                    size_t job_idx = slots[slot_idx].source_idx;
                    NowCompileJob *job = &jobs[job_idx];

                    if (exit_code != 0) {
                        /* Print captured output (compiler errors) */
                        if (captured.data && captured.len > 0)
                            print_filtered_output(captured.data, captured.len,
                                                  ctx->toolchain.is_msvc);
                        if (result) {
                            result->code = NOW_ERR_TOOL;
                            snprintf(result->message, sizeof(result->message),
                                     "compiler failed on %s (exit %d)",
                                     job->src_rel, exit_code);
                        }
                        errors++;
                        if (now_tui_global) {
                            now_tui_compile_done(now_tui_global, job->src_rel, 0);
                            if (result) now_tui_error(now_tui_global, result->message);
                        }
                    } else {
                        /* Print captured output only if verbose */
                        if (ctx->verbose && captured.data && captured.len > 0)
                            print_filtered_output(captured.data, captured.len,
                                                  ctx->toolchain.is_msvc);

                        /* Parse deps from compiler output */
                        char *src_full = now_path_join(ctx->basedir, job->src_rel);
                        NowDepList deps = {0};
                        if (ctx->toolchain.is_msvc && captured.data)
                            now_depfile_parse_msvc(captured.data,
                                                   captured.len, &deps);
                        else if (job->dep_path)
                            now_depfile_parse(job->dep_path, src_full, &deps);

                        /* Update manifest */
                        char *src_hash = src_full ? now_sha256_file_memo(src_full, &hash_memo) : NULL;
                        struct stat st;
                        long long mtime = 0;
                        if (src_full && stat(src_full, &st) == 0)
                            mtime = (long long)st.st_mtime;
                        now_manifest_set(&manifest, job->src_rel, job->obj_path,
                                         src_hash, fhash, mtime);

                        /* Store dep info in manifest */
                        if (deps.count > 0) {
                            char **dhashes = (char **)calloc(deps.count, sizeof(char *));
                            if (dhashes) {
                                for (size_t d = 0; d < deps.count; d++)
                                    dhashes[d] = now_sha256_file_memo(deps.paths[d], &hash_memo);
                                now_manifest_set_deps(&manifest, job->src_rel,
                                    (const char **)deps.paths,
                                    (const char **)dhashes, deps.count);
                                for (size_t d = 0; d < deps.count; d++)
                                    free(dhashes[d]);
                                free(dhashes);
                            }
                        }

                        /* Store in content-addressable cache (with deps) */
                        if (src_hash) {
                            const char *compiler = job->argv ? job->argv[0] : "";
                            char *ckey = now_cache_key(src_hash, fhash, compiler);
                            if (ckey) {
                                const char *ext = ctx->toolchain.is_msvc ? ".obj" : ".o";
                                now_cache_store_ex(ckey, job->obj_path, ext,
                                                   deps.count > 0 ? &deps : NULL);
                                /* Push to remote cache */
                                if (has_remote)
                                    now_remote_cache_store(&remote_cfg, ckey,
                                                            job->obj_path, ext);
                                free(ckey);
                            }
                        }

                        now_deplist_free(&deps);

                        /* Clean up depfile */
                        if (job->dep_path)
                            remove(job->dep_path);

                        free(src_full);
                        free(src_hash);

                        now_filelist_push(&ctx->objects, job->obj_path);
                        compiled++;
                        if (now_tui_global) now_tui_compile_done(now_tui_global, job->src_rel, 1);
                    }

                    free(captured.data);
                }
            }

            free(slots);
        }
    }

    /* Free all jobs */
    for (size_t j = 0; j < njobs; j++)
        compile_job_free(&jobs[j]);
    free(jobs);

    /* Save manifest (include link flags hash for link-skip optimization) */
    if (manifest_path) {
        char *lfh = link_flags_hash(ctx->project);
        if (lfh) {
            free(manifest.link_flags_hash);
            manifest.link_flags_hash = lfh;
        }
        now_manifest_save(&manifest, manifest_path);
    }

    free(fhash);
    free(manifest_path);
    now_manifest_free(&manifest);
    now_hash_memo_global = NULL;
    now_hash_memo_free(&hash_memo);
    now_repro_free_flags(repro_flags, repro_flag_count);
    free(repro_timestamp);
    now_repro_free(&repro);
    now_module_scan_free(&modscan);
    if (has_remote) now_remote_config_free(&remote_cfg);

    if (ctx->verbose && !now_tui_global && (compiled > 0 || skipped > 0 || cache_hits > 0 || remote_hits > 0)) {
        int total_cached = cache_hits + remote_hits;
        if (total_cached > 0 && compiled > 0 && max_jobs > 1) {
            if (remote_hits > 0)
                fprintf(stderr, "  compiled %d (%d-way parallel), cached %d (local %d, remote %d), skipped %d (up to date)\n",
                        compiled, max_jobs, total_cached, cache_hits, remote_hits, skipped);
            else
                fprintf(stderr, "  compiled %d (%d-way parallel), cached %d, skipped %d (up to date)\n",
                        compiled, max_jobs, total_cached, skipped);
        } else if (total_cached > 0) {
            if (remote_hits > 0)
                fprintf(stderr, "  compiled %d, cached %d (local %d, remote %d), skipped %d (up to date)\n",
                        compiled, total_cached, cache_hits, remote_hits, skipped);
            else
                fprintf(stderr, "  compiled %d, cached %d, skipped %d (up to date)\n",
                        compiled, total_cached, skipped);
        } else if (max_jobs > 1 && compiled > 1)
            fprintf(stderr, "  compiled %d (%d-way parallel), skipped %d (up to date)\n",
                    compiled, max_jobs, skipped);
        else
            fprintf(stderr, "  compiled %d, skipped %d (up to date)\n", compiled, skipped);
    }

    if (errors) {
        if (result && result->code == NOW_OK) {
            result->code = NOW_ERR_TOOL;
            snprintf(result->message, sizeof(result->message),
                     "%d source file(s) failed to compile", errors);
        }
        return errors;
    }

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return 0;
}

/* Build a hash of the link flags */
static char *link_flags_hash(const NowProject *p) {
    size_t cap = 256;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t len = 0;

    #define APPEND(s) do { \
        size_t sl = strlen(s); \
        while (len + sl + 2 > cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) return NULL; } \
        memcpy(buf + len, s, sl); len += sl; buf[len++] = '\n'; buf[len] = '\0'; \
    } while(0)

    const char *out_type = p->output.type ? p->output.type : "executable";
    APPEND(out_type);
    for (size_t i = 0; i < p->link.flags.count; i++)
        APPEND(p->link.flags.items[i]);
    for (size_t i = 0; i < p->link.libs.count; i++)
        APPEND(p->link.libs.items[i]);
    for (size_t i = 0; i < p->link.libdirs.count; i++)
        APPEND(p->link.libdirs.items[i]);
    #undef APPEND

    char *hash = now_sha256_string(buf, len);
    free(buf);
    return hash;
}

/* ---- Link phase (§2.4, §7.6) ---- */

NOW_API int now_build_link(NowBuildCtx *ctx, NowResult *result) {
    const NowProject *p = ctx->project;
    const char *basedir = ctx->basedir;

    /* Java projects: package into JAR */
    if (has_java_lang(p) && p->langs.count > 0 &&
        strcmp(p->langs.items[0], "java") == 0) {
        return build_java_link(ctx, result);
    }

    if (ctx->objects.count == 0) {
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "no object files to link");
        }
        return -1;
    }

    /* Determine output type and name */
    const char *out_type = p->output.type ? p->output.type : "executable";
    const char *out_name = p->output.name ? p->output.name : p->artifact;
    if (!out_name) out_name = "a";

    /* Build output path */
    char *bin_dir = now_path_join(basedir, "target/bin");
    now_mkdir_p(bin_dir);

    char out_file[512];
    int is_static  = (strcmp(out_type, "static") == 0);
    int is_shared  = (strcmp(out_type, "shared") == 0);

    if (is_static) {
#ifdef _WIN32
        snprintf(out_file, sizeof(out_file), "%s/%s.lib", bin_dir, out_name);
#else
        snprintf(out_file, sizeof(out_file), "%s/lib%s.a", bin_dir, out_name);
#endif
    } else if (is_shared) {
#ifdef _WIN32
        snprintf(out_file, sizeof(out_file), "%s/%s.dll", bin_dir, out_name);
#elif defined(__APPLE__)
        snprintf(out_file, sizeof(out_file), "%s/lib%s.dylib", bin_dir, out_name);
#else
        snprintf(out_file, sizeof(out_file), "%s/lib%s.so", bin_dir, out_name);
#endif
    } else {
        /* executable */
#ifdef _WIN32
        snprintf(out_file, sizeof(out_file), "%s/%s.exe", bin_dir, out_name);
#else
        snprintf(out_file, sizeof(out_file), "%s/%s", bin_dir, out_name);
#endif
    }
    free(bin_dir);

    /* Skip link if output exists and is newer than all objects + link flags unchanged */
    {
        struct stat out_st;
        if (stat(out_file, &out_st) == 0) {
            long long out_mtime = (long long)out_st.st_mtime;
            int up_to_date = 1;
            for (size_t i = 0; i < ctx->objects.count; i++) {
                struct stat obj_st;
                if (stat(ctx->objects.paths[i], &obj_st) != 0 ||
                    (long long)obj_st.st_mtime > out_mtime) {
                    up_to_date = 0;
                    break;
                }
            }
            if (up_to_date) {
                /* Also check link flags hash */
                char *manifest_path = now_path_join(basedir, "target/.now-manifest");
                NowManifest manifest;
                now_manifest_load(&manifest, manifest_path);
                free(manifest_path);
                char *cur_lfh = link_flags_hash(p);
                if (cur_lfh && manifest.link_flags_hash &&
                    strcmp(cur_lfh, manifest.link_flags_hash) == 0) {
                    free(cur_lfh);
                    now_manifest_free(&manifest);
                    if (ctx->verbose)
                        fprintf(stderr, "  link: up to date\n");
                    if (result) { result->code = NOW_OK; result->message[0] = '\0'; }
                    return 0;
                }
                free(cur_lfh);
                now_manifest_free(&manifest);
            }
        }
    }

    if (ctx->toolchain.is_msvc) {
        /* ---- MSVC link path ---- */
        if (is_static) {
            /* Static library: lib.exe /OUT:file obj... */
            const char *argv[256];
            int argc = 0;
            argv[argc++] = ctx->toolchain.ar;  /* lib.exe */
            argv[argc++] = "/nologo";
            char out_buf[520];
            snprintf(out_buf, sizeof(out_buf), "/OUT:%s", out_file);
            argv[argc++] = out_buf;
            for (size_t i = 0; i < ctx->objects.count; i++)
                argv[argc++] = ctx->objects.paths[i];
            argv[argc] = NULL;

            int rc = now_exec(argv, ctx->verbose);
            if (rc != 0) {
                if (result) {
                    result->code = NOW_ERR_TOOL;
                    snprintf(result->message, sizeof(result->message),
                             "archiver (lib.exe) failed (exit %d)", rc);
                }
                return rc;
            }
        } else {
            /* Executable or DLL: link.exe /OUT:file obj... libs... */
            const char *argv[512];
            int argc = 0;
            argv[argc++] = "link.exe";
            argv[argc++] = "/nologo";

            if (is_shared) argv[argc++] = "/DLL";

            char out_buf[520];
            snprintf(out_buf, sizeof(out_buf), "/OUT:%s", out_file);
            argv[argc++] = out_buf;

            /* Link flags */
            for (size_t i = 0; i < p->link.flags.count; i++)
                argv[argc++] = p->link.flags.items[i];

            /* Object files */
            for (size_t i = 0; i < ctx->objects.count; i++)
                argv[argc++] = ctx->objects.paths[i];

            /* Library directories: /LIBPATH:dir */
            char *libdir_bufs[32];
            size_t nlibdir = 0;
            for (size_t i = 0; i < p->link.libdirs.count && nlibdir < 32; i++) {
                char *full = now_path_join(basedir, p->link.libdirs.items[i]);
                if (full) {
                    size_t len = strlen(full) + 12;
                    libdir_bufs[nlibdir] = malloc(len);
                    if (libdir_bufs[nlibdir]) {
                        snprintf(libdir_bufs[nlibdir], len, "/LIBPATH:%s", full);
                        argv[argc++] = libdir_bufs[nlibdir];
                        nlibdir++;
                    }
                    free(full);
                }
            }

            /* Dependency library directories (from procure) */
            for (size_t i = 0; i < ctx->dep_libdirs.count; i++)
                argv[argc++] = ctx->dep_libdirs.paths[i];

            /* Libraries: name.lib */
            char *lib_bufs[64];
            size_t nlib = 0;
            for (size_t i = 0; i < p->link.libs.count && nlib < 64; i++) {
                size_t len = strlen(p->link.libs.items[i]) + 5;
                lib_bufs[nlib] = malloc(len);
                if (lib_bufs[nlib]) {
                    snprintf(lib_bufs[nlib], len, "%s.lib",
                             p->link.libs.items[i]);
                    argv[argc++] = lib_bufs[nlib];
                    nlib++;
                }
            }

            /* Dependency libraries (from procure) */
            for (size_t i = 0; i < ctx->dep_libs.count; i++)
                argv[argc++] = ctx->dep_libs.paths[i];

            argv[argc] = NULL;

            int rc = now_exec(argv, ctx->verbose);

            for (size_t i = 0; i < nlibdir; i++) free(libdir_bufs[i]);
            for (size_t i = 0; i < nlib; i++)    free(lib_bufs[i]);

            if (rc != 0) {
                if (result) {
                    result->code = NOW_ERR_TOOL;
                    snprintf(result->message, sizeof(result->message),
                             "linker (link.exe) failed (exit %d)", rc);
                }
                return rc;
            }
        }
    } else {
        /* ---- GCC/Clang link path ---- */

        /* Reproducibility: sort objects for deterministic archive/link order */
        NowReproConfig link_repro;
        now_repro_from_project(&link_repro, p);
        char **link_repro_flags = NULL;
        size_t link_repro_nflags = 0;
        if (link_repro.enabled) {
            if (link_repro.sort_inputs)
                now_repro_sort_filelist(&ctx->objects);
            now_repro_link_flags(&link_repro, 0, &link_repro_flags, &link_repro_nflags);
        }

        if (is_static) {
            /* Static library: ar rcs */
            const char *argv[256];
            int argc = 0;
            argv[argc++] = ctx->toolchain.ar;
            argv[argc++] = "rcs";
            argv[argc++] = out_file;
            for (size_t i = 0; i < ctx->objects.count; i++)
                argv[argc++] = ctx->objects.paths[i];
            argv[argc] = NULL;

            int rc = now_exec(argv, ctx->verbose);
            if (rc != 0) {
                if (result) {
                    result->code = NOW_ERR_TOOL;
                    snprintf(result->message, sizeof(result->message),
                             "archiver failed (exit %d)", rc);
                }
                return rc;
            }
        } else {
            /* Executable or shared: link via compiler driver */
            int has_cxx = 0;
            for (size_t i = 0; i < p->langs.count; i++) {
                if (strcmp(p->langs.items[i], "c++") == 0) { has_cxx = 1; break; }
            }

            const char *argv[512];
            int argc = 0;
            argv[argc++] = has_cxx ? ctx->toolchain.cxx : ctx->toolchain.cc;

            if (is_shared) argv[argc++] = "-shared";

            /* Link flags */
            for (size_t i = 0; i < p->link.flags.count; i++)
                argv[argc++] = p->link.flags.items[i];

            /* Object files */
            for (size_t i = 0; i < ctx->objects.count; i++)
                argv[argc++] = ctx->objects.paths[i];

            /* Library directories: -L prepended */
            char *libdir_bufs[32];
            size_t nlibdir = 0;
            for (size_t i = 0; i < p->link.libdirs.count && nlibdir < 32; i++) {
                char *full = now_path_join(basedir, p->link.libdirs.items[i]);
                if (full) {
                    size_t len = strlen(full) + 3;
                    libdir_bufs[nlibdir] = malloc(len);
                    if (libdir_bufs[nlibdir]) {
                        snprintf(libdir_bufs[nlibdir], len, "-L%s", full);
                        argv[argc++] = libdir_bufs[nlibdir];
                        nlibdir++;
                    }
                    free(full);
                }
            }

            /* Dependency library directories (from procure) */
            for (size_t i = 0; i < ctx->dep_libdirs.count; i++)
                argv[argc++] = ctx->dep_libdirs.paths[i];

            /* Libraries: -l prepended */
            char *lib_bufs[64];
            size_t nlib = 0;
            for (size_t i = 0; i < p->link.libs.count && nlib < 64; i++) {
                size_t len = strlen(p->link.libs.items[i]) + 3;
                lib_bufs[nlib] = malloc(len);
                if (lib_bufs[nlib]) {
                    snprintf(lib_bufs[nlib], len, "-l%s", p->link.libs.items[i]);
                    argv[argc++] = lib_bufs[nlib];
                    nlib++;
                }
            }

            /* Rust stdlib deps (auto-injected when Rust sources present) */
            if (ctx->toolchain.rustc) {
#ifdef _WIN32
                argv[argc++] = "-lws2_32";
                argv[argc++] = "-luserenv";
                argv[argc++] = "-lbcrypt";
                argv[argc++] = "-lntdll";
#else
                argv[argc++] = "-ldl";
                argv[argc++] = "-lpthread";
                argv[argc++] = "-lm";
#endif
            }

            /* Go runtime deps */
            if (ctx->toolchain.go) {
#ifdef _WIN32
                argv[argc++] = "-lws2_32";
                argv[argc++] = "-lwinmm";
                argv[argc++] = "-lntdll";
#else
                argv[argc++] = "-lpthread";
                argv[argc++] = "-lm";
#endif
            }

            /* Dependency libraries (from procure) */
            for (size_t i = 0; i < ctx->dep_libs.count; i++)
                argv[argc++] = ctx->dep_libs.paths[i];

            /* Reproducibility link flags (e.g. --build-id=sha1) */
            for (size_t i = 0; i < link_repro_nflags; i++)
                argv[argc++] = link_repro_flags[i];

            argv[argc++] = "-o";
            argv[argc++] = out_file;
            argv[argc] = NULL;

            int rc = now_exec(argv, ctx->verbose);

            for (size_t i = 0; i < nlibdir; i++) free(libdir_bufs[i]);
            for (size_t i = 0; i < nlib; i++)    free(lib_bufs[i]);

            if (rc != 0) {
                if (result) {
                    result->code = NOW_ERR_TOOL;
                    snprintf(result->message, sizeof(result->message),
                             "linker failed (exit %d)", rc);
                }
                now_repro_free_flags(link_repro_flags, link_repro_nflags);
                now_repro_free(&link_repro);
                return rc;
            }
        }
        now_repro_free_flags(link_repro_flags, link_repro_nflags);
        now_repro_free(&link_repro);
    }

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return 0;
}

/* ---- Test phase (§9) ---- */

NOW_API int now_build_test(NowBuildCtx *ctx, NowResult *result) {
    const NowProject *p = ctx->project;
    const char *basedir = ctx->basedir;

    /* Java projects: compile and run test sources */
    if (has_java_lang(p) && p->langs.count > 0 &&
        strcmp(p->langs.items[0], "java") == 0) {
        return build_java_test(ctx, result);
    }

    /* Determine test source directory */
    const char *test_dir = p->tests.dir;
    if (!test_dir) test_dir = "src/test/c";

    /* Check test dir exists */
    char *test_full = now_path_join(basedir, test_dir);
    if (!test_full || !now_is_dir(test_full)) {
        free(test_full);
        if (result) {
            result->code = NOW_OK;
            snprintf(result->message, sizeof(result->message),
                     "no test directory found, skipping tests");
        }
        return 0; /* no tests = success */
    }
    free(test_full);

    /* Discover test source files */
    const char **exts = now_lang_source_exts(
        (const char *const *)p->langs.items, p->langs.count);
    if (!exts) return -1;

    NowFileList test_sources;
    now_filelist_init(&test_sources);
    int rc = now_discover_sources(basedir, test_dir, exts, &test_sources);
    free(exts);

    if (rc != 0 || test_sources.count == 0) {
        now_filelist_free(&test_sources);
        if (result) {
            result->code = NOW_OK;
            snprintf(result->message, sizeof(result->message),
                     "no test sources found, skipping tests");
        }
        return 0;
    }

    /* Create test obj dir */
    char *test_obj_dir = now_path_join(basedir, "target/obj/test");
    now_mkdir_p(test_obj_dir);
    free(test_obj_dir);

    /* Compile test sources */
    NowFileList test_objects;
    now_filelist_init(&test_objects);
    int errors = 0;

    for (size_t i = 0; i < test_sources.count; i++) {
        const char *src = test_sources.paths[i];
        const NowLangDef *lang = NULL;
        const NowLangType *type = now_lang_classify(
            src, (const char *const *)p->langs.items, p->langs.count, &lang);
        if (!type || type->role != NOW_ROLE_SOURCE) continue;

        /* Resolve tool */
        const char *tool = ctx->toolchain.cc;
        if (type->tool_var && strcmp(type->tool_var, "${cxx}") == 0)
            tool = ctx->toolchain.cxx;

        /* Derive object path under target/obj/test/ */
        const char *oext = ctx->toolchain.is_msvc ? ".obj" : ".o";
        char *obj = now_obj_path_ex(basedir, src, test_dir, "target", oext);
        if (!obj) { errors++; continue; }

        /* Replace obj/main with obj/test in path */
        char *main_marker = strstr(obj, "obj/main");
        if (main_marker) memcpy(main_marker, "obj/test", 8);

        /* Ensure directory */
        char *obj_dir_copy = strdup(obj);
        char *last = strrchr(obj_dir_copy, '/');
        if (!last) last = strrchr(obj_dir_copy, '\\');
        if (last) { *last = '\0'; now_mkdir_p(obj_dir_copy); }
        free(obj_dir_copy);

        /* Build argv — MSVC vs GCC/Clang */
        const char *argv[64];
        int argc = 0;
        char *inc_src = NULL, *inc_hdr = NULL;
        char *src_full = now_path_join(basedir, src);

        if (ctx->toolchain.is_msvc) {
            argv[argc++] = tool;
            argv[argc++] = "/nologo";

            char std_buf[32] = {0};
            const char *std_val = p->compile.std ? p->compile.std : p->std;
            if (std_val && lang->std_flag) {
                snprintf(std_buf, sizeof(std_buf), "/std:%s", std_val);
                argv[argc++] = std_buf;
            }

            const char *src_dir_str = p->sources.dir ? p->sources.dir : "src/main/c";
            inc_src = now_path_join(basedir, src_dir_str);
            if (inc_src) {
                char inc_flag[512];
                snprintf(inc_flag, sizeof(inc_flag), "/I%s", inc_src);
                char *inc_arg = strdup(inc_flag);
                argv[argc++] = inc_arg;
            }
            if (p->sources.headers) {
                inc_hdr = now_path_join(basedir, p->sources.headers);
                if (inc_hdr) {
                    char inc_flag[512];
                    snprintf(inc_flag, sizeof(inc_flag), "/I%s", inc_hdr);
                    char *inc_arg = strdup(inc_flag);
                    argv[argc++] = inc_arg;
                }
            }

            argv[argc++] = "/c";
            argv[argc++] = src_full;

            char fo_buf[520];
            snprintf(fo_buf, sizeof(fo_buf), "/Fo%s", obj);
            argv[argc++] = fo_buf;
        } else {
            argv[argc++] = tool;

            char std_buf[32] = {0};
            const char *std_val = p->compile.std ? p->compile.std : p->std;
            if (std_val && lang->std_flag) {
                snprintf(std_buf, sizeof(std_buf), "-std=%s", std_val);
                argv[argc++] = std_buf;
            }

            const char *src_dir_str = p->sources.dir ? p->sources.dir : "src/main/c";
            inc_src = now_path_join(basedir, src_dir_str);
            if (inc_src) {
                char inc_flag[512];
                snprintf(inc_flag, sizeof(inc_flag), "-I%s", inc_src);
                char *inc_arg = strdup(inc_flag);
                argv[argc++] = inc_arg;
            }
            if (p->sources.headers) {
                inc_hdr = now_path_join(basedir, p->sources.headers);
                if (inc_hdr) {
                    char inc_flag[512];
                    snprintf(inc_flag, sizeof(inc_flag), "-I%s", inc_hdr);
                    char *inc_arg = strdup(inc_flag);
                    argv[argc++] = inc_arg;
                }
            }

            argv[argc++] = "-c";
            argv[argc++] = src_full;
            argv[argc++] = "-o";
            argv[argc++] = obj;
        }
        argv[argc] = NULL;

        rc = now_exec(argv, ctx->verbose);

        /* Free include args */
        for (int a = 1; a < argc; a++) {
            if (argv[a] == src_full || argv[a] == obj) continue;
            /* Free the strdup'd -I / /I flags */
            const char *p_a = argv[a];
            if ((p_a[0] == '-' || p_a[0] == '/') && p_a[1] == 'I')
                free((char *)argv[a]);
        }
        free(inc_src);
        free(inc_hdr);
        free(src_full);

        if (rc != 0) {
            if (result) {
                result->code = NOW_ERR_TOOL;
                snprintf(result->message, sizeof(result->message),
                         "test compile failed on %s (exit %d)", src, rc);
            }
            free(obj);
            errors++;
            continue;
        }

        now_filelist_push(&test_objects, obj);
        free(obj);
    }

    now_filelist_free(&test_sources);

    if (errors) {
        now_filelist_free(&test_objects);
        return -1;
    }

    if (test_objects.count == 0) {
        now_filelist_free(&test_objects);
        if (result) {
            result->code = NOW_OK;
            result->message[0] = '\0';
        }
        return 0;
    }

    /* Link test binary: test objects + production objects */
    char *test_bin_dir = now_path_join(basedir, "target/bin");
    now_mkdir_p(test_bin_dir);

    char test_bin[512];
    const char *proj_name = p->artifact ? p->artifact : "test";
#ifdef _WIN32
    snprintf(test_bin, sizeof(test_bin), "%s/%s-test.exe", test_bin_dir, proj_name);
#else
    snprintf(test_bin, sizeof(test_bin), "%s/%s-test", test_bin_dir, proj_name);
#endif
    free(test_bin_dir);

    const char *argv[512];
    int argc = 0;
    char *lib_bufs[64];
    size_t nlib = 0;

    if (ctx->toolchain.is_msvc) {
        /* MSVC: link.exe /nologo /OUT:test.exe objs... libs... */
        argv[argc++] = "link.exe";
        argv[argc++] = "/nologo";

        char out_buf[520];
        snprintf(out_buf, sizeof(out_buf), "/OUT:%s", test_bin);
        argv[argc++] = out_buf;

        for (size_t i = 0; i < test_objects.count; i++)
            argv[argc++] = test_objects.paths[i];
        for (size_t i = 0; i < ctx->objects.count; i++)
            argv[argc++] = ctx->objects.paths[i];

        for (size_t i = 0; i < p->link.libs.count && nlib < 64; i++) {
            size_t len = strlen(p->link.libs.items[i]) + 5;
            lib_bufs[nlib] = malloc(len);
            if (lib_bufs[nlib]) {
                snprintf(lib_bufs[nlib], len, "%s.lib", p->link.libs.items[i]);
                argv[argc++] = lib_bufs[nlib];
                nlib++;
            }
        }
        argv[argc] = NULL;
    } else {
        /* GCC/Clang: cc objs... -llibs... -o test */
        int has_cxx = 0;
        for (size_t i = 0; i < p->langs.count; i++) {
            if (strcmp(p->langs.items[i], "c++") == 0) { has_cxx = 1; break; }
        }

        argv[argc++] = has_cxx ? ctx->toolchain.cxx : ctx->toolchain.cc;

        for (size_t i = 0; i < test_objects.count; i++)
            argv[argc++] = test_objects.paths[i];
        for (size_t i = 0; i < ctx->objects.count; i++)
            argv[argc++] = ctx->objects.paths[i];

        for (size_t i = 0; i < p->link.libs.count && nlib < 64; i++) {
            size_t len = strlen(p->link.libs.items[i]) + 3;
            lib_bufs[nlib] = malloc(len);
            if (lib_bufs[nlib]) {
                snprintf(lib_bufs[nlib], len, "-l%s", p->link.libs.items[i]);
                argv[argc++] = lib_bufs[nlib];
                nlib++;
            }
        }

        argv[argc++] = "-o";
        argv[argc++] = test_bin;
        argv[argc] = NULL;
    }

    rc = now_exec(argv, ctx->verbose);

    for (size_t i = 0; i < nlib; i++) free(lib_bufs[i]);
    now_filelist_free(&test_objects);

    if (rc != 0) {
        if (result) {
            result->code = NOW_ERR_TOOL;
            snprintf(result->message, sizeof(result->message),
                     "test link failed (exit %d)", rc);
        }
        return -1;
    }

    /* Execute the test binary */
    const char *test_argv[] = { test_bin, NULL };
    rc = now_exec(test_argv, ctx->verbose);

    if (rc != 0) {
        if (result) {
            result->code = NOW_ERR_TEST;
            snprintf(result->message, sizeof(result->message),
                     "test execution failed (exit %d)", rc);
        }
        return rc;
    }

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return 0;
}

/* ---- compile_commands.json generation (§22.1) ---- */

/* Escape a string for JSON output */
static void json_escape(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f);  break;
            case '\r': fputs("\\r", f);  break;
            case '\t': fputs("\\t", f);  break;
            default:   fputc(*s, f);     break;
        }
    }
    fputc('"', f);
}

NOW_API int now_compile_db(const NowProject *project, const char *basedir,
                            NowResult *result) {
    NowBuildCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (now_build_init(&ctx, project, basedir, result) != 0) return -1;

    /* Open compile_commands.json for writing */
    char *out_path = now_path_join(basedir, "compile_commands.json");
    if (!out_path) { now_build_free(&ctx); return -1; }

    FILE *f = fopen(out_path, "w");
    if (!f) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot write %s", out_path);
        }
        free(out_path);
        now_build_free(&ctx);
        return -1;
    }

    /* Get absolute basedir for "directory" field */
    char abs_basedir[1024];
#ifdef _WIN32
    if (!_fullpath(abs_basedir, basedir, sizeof(abs_basedir)))
        strncpy(abs_basedir, basedir, sizeof(abs_basedir) - 1);
#else
    if (!realpath(basedir, abs_basedir))
        strncpy(abs_basedir, basedir, sizeof(abs_basedir) - 1);
#endif
    abs_basedir[sizeof(abs_basedir) - 1] = '\0';

    /* Normalize backslashes to forward slashes */
    for (char *p = abs_basedir; *p; p++)
        if (*p == '\\') *p = '/';

    fputs("[\n", f);
    int count = 0;

    for (size_t i = 0; i < ctx.sources.count; i++) {
        const char *src = ctx.sources.paths[i];
        const NowLangDef *lang = NULL;
        const NowLangType *type = now_lang_classify(
            src, (const char *const *)project->langs.items,
            project->langs.count, &lang);

        if (!type || type->role != NOW_ROLE_SOURCE) continue;

        NowCompileJob job;
        int jrc;
        if (ctx.toolchain.is_msvc)
            jrc = build_compile_job_msvc(&ctx, src, type, lang, &job);
        else
            jrc = build_compile_job(&ctx, src, type, lang, &job);
        if (jrc != 0) continue;

        if (count > 0) fputs(",\n", f);
        fputs("  {\n", f);

        /* directory */
        fputs("    \"directory\": ", f);
        json_escape(f, abs_basedir);
        fputs(",\n", f);

        /* file (absolute path) */
        char *src_full = now_path_join(basedir, src);
        char abs_src[1024];
#ifdef _WIN32
        if (!_fullpath(abs_src, src_full ? src_full : src, sizeof(abs_src)))
            strncpy(abs_src, src_full ? src_full : src, sizeof(abs_src) - 1);
#else
        if (!realpath(src_full ? src_full : src, abs_src))
            strncpy(abs_src, src_full ? src_full : src, sizeof(abs_src) - 1);
#endif
        abs_src[sizeof(abs_src) - 1] = '\0';
        for (char *p = abs_src; *p; p++)
            if (*p == '\\') *p = '/';
        free(src_full);

        fputs("    \"file\": ", f);
        json_escape(f, abs_src);
        fputs(",\n", f);

        /* arguments (array form, preferred by clangd) */
        fputs("    \"arguments\": [", f);
        for (int a = 0; a < job.argc; a++) {
            if (a > 0) fputs(", ", f);

            /* Normalize backslashes in argument values too */
            char *arg = strdup(job.argv[a]);
            if (arg) {
                for (char *p = arg; *p; p++)
                    if (*p == '\\') *p = '/';
                json_escape(f, arg);
                free(arg);
            } else {
                json_escape(f, job.argv[a]);
            }
        }
        fputs("]\n", f);

        fputs("  }", f);
        count++;

        compile_job_free(&job);
    }

    fputs("\n]\n", f);
    fclose(f);

    if (result) {
        result->code = NOW_OK;
        snprintf(result->message, sizeof(result->message),
                 "wrote %d entries to %s", count, out_path);
    }

    free(out_path);
    now_build_free(&ctx);
    return count;
}

NOW_API void now_build_free(NowBuildCtx *ctx) {
    now_toolchain_free(&ctx->toolchain);
    now_filelist_free(&ctx->sources);
    now_filelist_free(&ctx->objects);
    now_filelist_free(&ctx->dep_includes);
    now_filelist_free(&ctx->dep_libdirs);
    now_filelist_free(&ctx->dep_libs);
}
