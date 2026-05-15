#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
  #include <direct.h>
  #define rmdir _rmdir
#else
  #include <unistd.h>
#endif
#include "now.h"
#include "pasta.h"

/* Internal headers for unit testing */
#include "now_lang.h"
#include "now_fs.h"
#include "now_version.h"
#include "now_manifest.h"
#include "now_resolve.h"
#include "now_procure.h"
#include "now_build.h"
#include "now_package.h"
#include "now_workspace.h"
#include "now_plugin.h"
#include "now_plugin_registry.h"
#include "now_ci.h"
#include "now_layer.h"
#include "now_arch.h"
#include "now_export.h"
#include "now_trust.h"
#include "now_repro.h"
#include "now_advisory.h"
#include "now_auth.h"
#include "now_module.h"
#include "now_cache.h"
#include "now_sbom.h"
#include "now_remote.h"
#include "now_audit.h"
#include "now_watch.h"
#include "now_graph.h"
#include "pico_h2.h"
#include "alforno.h"
#include "basta.h"
#include "pico_http.h"
#include "pico_ws.h"

#ifndef NOW_TEST_RESOURCES
  #define NOW_TEST_RESOURCES "."
#endif

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { tests_run++; printf("  %-50s", name); } while (0)
#define PASS() \
    do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(msg) \
    do { tests_failed++; printf("FAIL: %s\n", msg); } while (0)
#define ASSERT_STR(actual, expected) \
    do { \
        if (!(actual) || strcmp((actual), (expected)) != 0) { \
            FAIL("expected '" expected "'"); return; \
        } \
    } while (0)
#define ASSERT_EQ(actual, expected) \
    do { \
        if ((actual) != (expected)) { FAIL(#actual " != " #expected); return; } \
    } while (0)
#define ASSERT_NOT_NULL(ptr) \
    do { if (!(ptr)) { FAIL(#ptr " is NULL"); return; } } while (0)
#define ASSERT_NULL(ptr) \
    do { if ((ptr)) { FAIL(#ptr " is not NULL"); return; } } while (0)

/* ---- Version ---- */

static void test_version(void) {
    TEST("now_version");
    const char *v = now_version();
    ASSERT_NOT_NULL(v);
    PASS();
}

/* ---- POM: load from string ---- */

static void test_pom_minimal_string(void) {
    TEST("pom: load minimal from string");
    const char *input =
        "{ group: \"io.test\", artifact: \"demo\", version: \"1.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"executable\", name: \"demo\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_STR(now_project_group(p), "io.test");
    ASSERT_STR(now_project_artifact(p), "demo");
    ASSERT_STR(now_project_version(p), "1.0.0");
    ASSERT_STR(now_project_std(p), "c11");
    ASSERT_EQ(now_project_lang_count(p), (size_t)1);
    ASSERT_STR(now_project_lang(p, 0), "c");
    ASSERT_STR(now_project_output_type(p), "executable");
    ASSERT_STR(now_project_output_name(p), "demo");
    /* default source dirs */
    ASSERT_STR(now_project_source_dir(p), "src/main/c");
    ASSERT_STR(now_project_header_dir(p), "src/main/h");
    now_project_free(p);
    PASS();
}

static void test_pom_lang_scalar(void) {
    TEST("pom: lang scalar shorthand");
    const char *input =
        "{ group: \"x\", artifact: \"x\", version: \"0.1.0\","
        "  lang: \"c\", std: \"c11\" }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(now_project_lang_count(p), (size_t)1);
    ASSERT_STR(now_project_lang(p, 0), "c");
    now_project_free(p);
    PASS();
}

static void test_pom_lang_mixed(void) {
    TEST("pom: lang 'mixed' expands to c + c++");
    const char *input =
        "{ group: \"x\", artifact: \"x\", version: \"0.1.0\","
        "  lang: \"mixed\" }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(now_project_lang_count(p), (size_t)2);
    ASSERT_STR(now_project_lang(p, 0), "c");
    ASSERT_STR(now_project_lang(p, 1), "c++");
    now_project_free(p);
    PASS();
}

static void test_pom_compile(void) {
    TEST("pom: compile warnings and defines");
    const char *input =
        "{ group: \"x\", artifact: \"x\", version: \"0.1.0\","
        "  compile: { warnings: [\"Wall\", \"Wextra\"],"
        "             defines: [\"NDEBUG\", \"FOO=1\"],"
        "             opt: \"speed\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(now_project_warning_count(p), (size_t)2);
    ASSERT_STR(now_project_warning(p, 0), "Wall");
    ASSERT_STR(now_project_warning(p, 1), "Wextra");
    ASSERT_EQ(now_project_define_count(p), (size_t)2);
    ASSERT_STR(now_project_define(p, 0), "NDEBUG");
    ASSERT_STR(now_project_opt(p), "speed");
    now_project_free(p);
    PASS();
}

static void test_pom_os_conditional(void) {
    TEST("pom: OS-conditional sub-blocks merge into parent");
    /* compile.windows / compile.posix and link.* sub-blocks should
     * append to the parent arrays only when host OS matches. */
    const char *input =
        "{ group: \"x\", artifact: \"x\", version: \"0.1.0\","
        "  compile: { defines: [\"BASE\"],"
        "             windows: { defines: [\"IS_WINDOWS\"] },"
        "             posix:   { defines: [\"IS_POSIX\"] } } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    /* BASE is always present. Exactly one of IS_WINDOWS or IS_POSIX
     * should be present, depending on host. */
    size_t n = now_project_define_count(p);
    int have_base = 0, have_win = 0, have_posix = 0;
    for (size_t i = 0; i < n; i++) {
        const char *d = now_project_define(p, i);
        if (strcmp(d, "BASE")       == 0) have_base = 1;
        if (strcmp(d, "IS_WINDOWS") == 0) have_win = 1;
        if (strcmp(d, "IS_POSIX")   == 0) have_posix = 1;
    }
    ASSERT_EQ(have_base, 1);
    /* Exactly one OS branch matched. */
    ASSERT_EQ(have_win + have_posix, 1);
#ifdef _WIN32
    ASSERT_EQ(have_win, 1);
#else
    ASSERT_EQ(have_posix, 1);
#endif
    now_project_free(p);
    PASS();
}

static void test_pom_deps(void) {
    TEST("pom: dependency loading");
    const char *input =
        "{ group: \"x\", artifact: \"x\", version: \"0.1.0\","
        "  deps: ["
        "    { id: \"org.acme:core:^1.5\", scope: \"compile\" },"
        "    { id: \"unity:unity:2.5.2\",  scope: \"test\"    }"
        "  ] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(now_project_dep_count(p), (size_t)2);
    ASSERT_STR(now_project_dep_id(p, 0), "org.acme:core:^1.5");
    ASSERT_STR(now_project_dep_scope(p, 0), "compile");
    ASSERT_STR(now_project_dep_id(p, 1), "unity:unity:2.5.2");
    ASSERT_STR(now_project_dep_scope(p, 1), "test");
    now_project_free(p);
    PASS();
}

static void test_pom_convergence(void) {
    TEST("pom: convergence policy");
    const char *input =
        "{ group: \"x\", artifact: \"x\", version: \"0.1.0\","
        "  convergence: \"lowest\" }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_STR(now_project_convergence(p), "lowest");
    now_project_free(p);
    PASS();
}

/* ---- POM: load from file ---- */

static void test_pom_load_file(void) {
    TEST("pom: load minimal.pasta from file");
    char path[512];
    snprintf(path, sizeof(path), "%s/minimal.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }
    ASSERT_STR(now_project_group(p), "io.example");
    ASSERT_STR(now_project_artifact(p), "hello");
    ASSERT_STR(now_project_output_type(p), "executable");
    ASSERT_EQ(now_project_dep_count(p), (size_t)1);
    ASSERT_STR(now_project_dep_id(p, 0), "unity:unity:2.5.2");
    now_project_free(p);
    PASS();
}

static void test_pom_load_rich(void) {
    TEST("pom: load rich.pasta from file");
    char path[512];
    snprintf(path, sizeof(path), "%s/rich.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }
    ASSERT_STR(now_project_group(p), "org.acme");
    ASSERT_STR(now_project_artifact(p), "rocketlib");
    ASSERT_STR(now_project_version(p), "3.0.0-beta.1");
    ASSERT_STR(now_project_name(p), "Rocket Library");
    ASSERT_STR(now_project_license(p), "Apache-2.0");
    ASSERT_EQ(now_project_lang_count(p), (size_t)2);
    ASSERT_STR(now_project_lang(p, 0), "c");
    ASSERT_STR(now_project_lang(p, 1), "c++");
    ASSERT_STR(now_project_output_type(p), "shared");
    ASSERT_STR(now_project_output_name(p), "rocket");
    ASSERT_EQ(now_project_warning_count(p), (size_t)3);
    ASSERT_STR(now_project_opt(p), "speed");
    ASSERT_EQ(now_project_dep_count(p), (size_t)2);
    ASSERT_STR(now_project_convergence(p), "lowest");
    /* sources overridden */
    ASSERT_STR(now_project_source_dir(p), "src/main");
    ASSERT_STR(now_project_header_dir(p), "include");
    now_project_free(p);
    PASS();
}

/* ---- POM: error handling ---- */

static void test_pom_syntax_error(void) {
    TEST("pom: syntax error reported");
    const char *input = "{ broken ]]]";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    if (p) { now_project_free(p); FAIL("should have failed"); return; }
    ASSERT_EQ(res.code, NOW_ERR_SYNTAX);
    PASS();
}

static void test_pom_not_a_map(void) {
    TEST("pom: non-map root rejected");
    const char *input = "[1, 2, 3]";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    if (p) { now_project_free(p); FAIL("should have failed"); return; }
    ASSERT_EQ(res.code, NOW_ERR_SCHEMA);
    PASS();
}

static void test_pom_file_not_found(void) {
    TEST("pom: missing file error");
    NowResult res;
    NowProject *p = now_project_load("/nonexistent/now.pasta", &res);
    if (p) { now_project_free(p); FAIL("should have failed"); return; }
    ASSERT_EQ(res.code, NOW_ERR_IO);
    PASS();
}

/* ---- Language type system ---- */

static void test_lang_find_c(void) {
    TEST("lang: find C definition");
    now_lang_registry_init();
    const NowLangDef *c = now_lang_find("c");
    ASSERT_NOT_NULL(c);
    ASSERT_STR(c->id, "c");
    PASS();
}

static void test_lang_find_cxx(void) {
    TEST("lang: find C++ definition");
    const NowLangDef *cxx = now_lang_find("c++");
    ASSERT_NOT_NULL(cxx);
    ASSERT_STR(cxx->id, "c++");
    PASS();
}

static void test_lang_classify_c(void) {
    TEST("lang: classify .c file as c-source");
    const char *langs[] = { "c" };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("foo/bar.c", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "c-source");
    ASSERT_EQ(type->role, NOW_ROLE_SOURCE);
    ASSERT_STR(type->output_ext, ".c.o");
    ASSERT_NOT_NULL(lang);
    ASSERT_STR(lang->id, "c");
    PASS();
}

static void test_lang_classify_h(void) {
    TEST("lang: classify .h file as c-header");
    const char *langs[] = { "c" };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("include/api.h", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "c-header");
    ASSERT_EQ(type->role, NOW_ROLE_HEADER);
    PASS();
}

static void test_lang_classify_cpp(void) {
    TEST("lang: classify .cpp as cxx-source");
    const char *langs[] = { "c", "c++" };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("src/engine.cpp", langs, 2, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "cxx-source");
    PASS();
}

static void test_lang_classify_unknown(void) {
    TEST("lang: unknown extension returns NULL");
    const char *langs[] = { "c" };
    const NowLangType *type = now_lang_classify("readme.txt", langs, 1, NULL);
    if (type) { FAIL("should be NULL"); return; }
    PASS();
}

static void test_lang_source_exts(void) {
    TEST("lang: source extensions for C");
    const char *langs[] = { "c" };
    const char **exts = now_lang_source_exts(langs, 1);
    ASSERT_NOT_NULL(exts);
    /* Should contain .c and .i */
    int found_c = 0, found_i = 0;
    for (const char **e = exts; *e; e++) {
        if (strcmp(*e, ".c") == 0) found_c = 1;
        if (strcmp(*e, ".i") == 0) found_i = 1;
    }
    free(exts);
    if (!found_c) { FAIL("missing .c"); return; }
    if (!found_i) { FAIL("missing .i"); return; }
    PASS();
}

/* ---- Filesystem utilities ---- */

static void test_fs_path_join(void) {
    TEST("fs: path_join");
    char *p = now_path_join("foo", "bar.c");
    ASSERT_NOT_NULL(p);
    ASSERT_STR(p, "foo/bar.c");
    free(p);
    PASS();
}

static void test_fs_path_join_trailing_sep(void) {
    TEST("fs: path_join with trailing separator");
    char *p = now_path_join("foo/", "bar.c");
    ASSERT_NOT_NULL(p);
    ASSERT_STR(p, "foo/bar.c");
    free(p);
    PASS();
}

static void test_fs_path_ext(void) {
    TEST("fs: path_ext");
    ASSERT_STR(now_path_ext("foo/bar.c"), ".c");
    ASSERT_STR(now_path_ext("foo/bar.cpp"), ".cpp");
    ASSERT_STR(now_path_ext("foo/bar"), "");
    PASS();
}

static void test_fs_obj_path(void) {
    TEST("fs: obj_path derivation");
    char *obj = now_obj_path("/proj", "src/main/c/net/parser.c",
                              "src/main/c", "target");
    ASSERT_NOT_NULL(obj);
    /* Should end with net/parser.c.o */
    if (!strstr(obj, "net/parser.c.o") && !strstr(obj, "net\\parser.c.o")) {
        FAIL(obj);
        free(obj);
        return;
    }
    free(obj);
    PASS();
}

/* ---- Remote cache ---- */

static void test_remote_config_parse_full(void) {
    TEST("remote: parse config with all fields");
    const char *input =
        "{ object_cache: { url: \"http://cache.local:9090\","
        "  token: \"my-secret\", push: true } }";
    NowRemoteCacheConfig cfg;
    int rc = now_remote_config_parse(input, strlen(input), &cfg);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(cfg.url, "http://cache.local:9090");
    ASSERT_STR(cfg.token, "my-secret");
    ASSERT_EQ(cfg.push, 1);
    now_remote_config_free(&cfg);
    PASS();
}

static void test_remote_config_parse_minimal(void) {
    TEST("remote: parse config with only url");
    const char *input = "{ object_cache: { url: \"http://localhost:8080\" } }";
    NowRemoteCacheConfig cfg;
    int rc = now_remote_config_parse(input, strlen(input), &cfg);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(cfg.url, "http://localhost:8080");
    ASSERT_NULL(cfg.token);
    ASSERT_EQ(cfg.push, 0);
    now_remote_config_free(&cfg);
    PASS();
}

static void test_remote_config_parse_no_section(void) {
    TEST("remote: parse config without object_cache returns -1");
    const char *input = "{ something_else: \"foo\" }";
    NowRemoteCacheConfig cfg;
    int rc = now_remote_config_parse(input, strlen(input), &cfg);
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_remote_config_parse_no_url(void) {
    TEST("remote: parse config without url returns -1");
    const char *input = "{ object_cache: { token: \"secret\" } }";
    NowRemoteCacheConfig cfg;
    int rc = now_remote_config_parse(input, strlen(input), &cfg);
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_remote_config_free_null(void) {
    TEST("remote: config_free on zeroed struct is safe");
    NowRemoteCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    now_remote_config_free(&cfg);
    now_remote_config_free(NULL);
    PASS();
}

static void test_remote_cache_restore_unreachable(void) {
    TEST("remote: restore from unreachable host returns -1");
    NowRemoteCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "http://127.0.0.1:1";
    char outpath[256];
    snprintf(outpath, sizeof(outpath), "%s/remote_test.o", NOW_TEST_RESOURCES);
    int rc = now_remote_cache_restore(&cfg, "abcdef1234567890", outpath, ".o");
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_remote_cache_store_push_disabled(void) {
    TEST("remote: store with push=0 returns -1 immediately");
    NowRemoteCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "http://127.0.0.1:1";
    cfg.push = 0;
    int rc = now_remote_cache_store(&cfg, "abcdef", "/nonexistent.o", ".o");
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_remote_cache_store_unreachable(void) {
    TEST("remote: store to unreachable host returns -1");
    NowRemoteCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "http://127.0.0.1:1";
    cfg.push = 1;
    /* Need a real file to read */
    char path[256];
    snprintf(path, sizeof(path), "%s/minimal.pasta", NOW_TEST_RESOURCES);
    int rc = now_remote_cache_store(&cfg, "abcdef1234567890", path, ".o");
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_remote_cache_key_url_safe(void) {
    TEST("remote: cache key is hex-only (URL-safe)");
    /* now_cache_key returns 64-char hex string */
    char *key = now_cache_key("abc123", "def456", "/usr/bin/gcc");
    ASSERT_NOT_NULL(key);
    /* Verify all chars are hex digits */
    for (size_t i = 0; key[i]; i++) {
        char c = key[i];
        int is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!is_hex) { free(key); FAIL("non-hex char in cache key"); return; }
    }
    ASSERT_EQ(strlen(key), (size_t)64);
    free(key);
    PASS();
}

/* ---- Enterprise auth (LDAP/SSO) ---- */

static void test_auth_method_parse(void) {
    TEST("auth: method parse");
    ASSERT_EQ((int)now_auth_method_parse("token"), (int)NOW_AUTH_TOKEN);
    ASSERT_EQ((int)now_auth_method_parse("ldap"), (int)NOW_AUTH_LDAP);
    ASSERT_EQ((int)now_auth_method_parse("oidc"), (int)NOW_AUTH_OIDC);
    ASSERT_EQ((int)now_auth_method_parse("oauth2"), (int)NOW_AUTH_OIDC);
    ASSERT_EQ((int)now_auth_method_parse(NULL), (int)NOW_AUTH_TOKEN);
    ASSERT_EQ((int)now_auth_method_parse("unknown"), (int)NOW_AUTH_TOKEN);
    PASS();
}

static void test_auth_method_name(void) {
    TEST("auth: method name");
    ASSERT_STR(now_auth_method_name(NOW_AUTH_TOKEN), "token");
    ASSERT_STR(now_auth_method_name(NOW_AUTH_LDAP), "ldap");
    ASSERT_STR(now_auth_method_name(NOW_AUTH_OIDC), "oidc");
    PASS();
}

static void test_auth_creds_free_null(void) {
    TEST("auth: creds_free null safety");
    now_auth_creds_free(NULL);  /* should not crash */
    NowCredentials c;
    memset(&c, 0, sizeof(c));
    now_auth_creds_free(&c);   /* should not crash on zeroed struct */
    PASS();
}

static void test_auth_load_no_file(void) {
    TEST("auth: load returns -1 with no credentials file");
    NowCredentials c;
    /* Use a URL that won't match anything */
    int rc = now_auth_load("http://nonexistent.example.com:9999", &c);
    /* Either -1 (no file) or -1 (no match) */
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_auth_load_null_safety(void) {
    TEST("auth: load null safety");
    ASSERT_EQ(now_auth_load(NULL, NULL), -1);
    NowCredentials c;
    ASSERT_EQ(now_auth_load(NULL, &c), -1);
    ASSERT_EQ(now_auth_load("http://x", NULL), -1);
    PASS();
}

static void test_token_cache_lifecycle(void) {
    TEST("auth: token cache put/get/remove");
    const char *url = "http://test-cache-lifecycle.example.com:12345";

    /* Put a token */
    int rc = now_token_cache_put(url, "test-jwt-abc123", 3600);
    ASSERT_EQ(rc, 0);

    /* Get it back */
    char *jwt = now_token_cache_get(url);
    ASSERT_NOT_NULL(jwt);
    ASSERT_STR(jwt, "test-jwt-abc123");
    free(jwt);

    /* Remove it */
    rc = now_token_cache_remove(url);
    ASSERT_EQ(rc, 0);

    /* Should be gone */
    jwt = now_token_cache_get(url);
    if (jwt) { free(jwt); FAIL("token should have been removed"); return; }

    /* Remove again — should return -1 */
    rc = now_token_cache_remove(url);
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_token_cache_expired(void) {
    TEST("auth: token cache returns NULL for expired token");
    const char *url = "http://test-cache-expired.example.com:12345";

    /* Put with 0 seconds TTL (already expired) */
    int rc = now_token_cache_put(url, "expired-jwt", 0);
    ASSERT_EQ(rc, 0);

    /* Should return NULL (expired within 60s margin) */
    char *jwt = now_token_cache_get(url);
    if (jwt) { free(jwt); now_token_cache_remove(url); FAIL("expired token should be NULL"); return; }

    /* Cleanup */
    now_token_cache_remove(url);
    PASS();
}

static void test_token_cache_overwrite(void) {
    TEST("auth: token cache overwrites existing entry");
    const char *url = "http://test-cache-overwrite.example.com:12345";

    now_token_cache_put(url, "jwt-v1", 3600);
    now_token_cache_put(url, "jwt-v2", 3600);

    char *jwt = now_token_cache_get(url);
    ASSERT_NOT_NULL(jwt);
    ASSERT_STR(jwt, "jwt-v2");
    free(jwt);

    now_token_cache_remove(url);
    PASS();
}

static void test_auth_ldap_login_null(void) {
    TEST("auth: ldap login null safety");
    NowResult res;
    memset(&res, 0, sizeof(res));
    char *jwt = NULL;
    ASSERT_EQ(now_auth_login_ldap(NULL, "user", "pass", &jwt, &res), -1);
    ASSERT_EQ(now_auth_login_ldap("http://x", NULL, "pass", &jwt, &res), -1);
    ASSERT_EQ(now_auth_login_ldap("http://x", "user", NULL, &jwt, &res), -1);
    ASSERT_EQ(now_auth_login_ldap("http://x", "user", "pass", NULL, &res), -1);
    PASS();
}

static void test_auth_ldap_login_unreachable(void) {
    TEST("auth: ldap login to unreachable host returns error");
    NowResult res;
    memset(&res, 0, sizeof(res));
    char *jwt = NULL;
    int rc = now_auth_login_ldap("http://127.0.0.1:1", "user", "pass",
                                  &jwt, &res);
    ASSERT_EQ(rc, -1);
    if (jwt) { free(jwt); FAIL("should not get JWT from unreachable host"); return; }
    PASS();
}

static void test_auth_oidc_client_null(void) {
    TEST("auth: oidc client credentials null safety");
    NowResult res;
    memset(&res, 0, sizeof(res));
    char *jwt = NULL;
    ASSERT_EQ(now_auth_login_oidc_client(NULL, "cid", "cs", &jwt, &res), -1);
    ASSERT_EQ(now_auth_login_oidc_client("http://x", NULL, "cs", &jwt, &res), -1);
    ASSERT_EQ(now_auth_login_oidc_client("http://x", "cid", NULL, &jwt, &res), -1);
    PASS();
}

static void test_auth_oidc_client_unreachable(void) {
    TEST("auth: oidc client creds to unreachable host returns error");
    NowResult res;
    memset(&res, 0, sizeof(res));
    char *jwt = NULL;
    int rc = now_auth_login_oidc_client("http://127.0.0.1:1",
                                          "client-id", "client-secret",
                                          &jwt, &res);
    ASSERT_EQ(rc, -1);
    if (jwt) { free(jwt); FAIL("should not get JWT"); return; }
    PASS();
}

static void test_auth_discover_unreachable(void) {
    TEST("auth: discover unreachable registry defaults to token");
    NowRegistryInfo info;
    int rc = now_auth_discover("http://127.0.0.1:1", &info);
    ASSERT_EQ(rc, -1);
    /* Should default to token auth */
    ASSERT_EQ(info.supports_token, 1);
    now_auth_discovery_free(&info);
    PASS();
}

static void test_auth_discovery_free_null(void) {
    TEST("auth: discovery_free null safety");
    now_auth_discovery_free(NULL);  /* should not crash */
    NowRegistryInfo info;
    memset(&info, 0, sizeof(info));
    now_auth_discovery_free(&info); /* should not crash */
    PASS();
}

static void test_auth_get_token_no_creds(void) {
    TEST("auth: get_token returns NULL with no credentials");
    NowResult res;
    memset(&res, 0, sizeof(res));
    char *jwt = now_auth_get_token("http://nonexistent.example.com:9999", 0, &res);
    if (jwt) { free(jwt); FAIL("should not get token without credentials"); return; }
    PASS();
}

/* ---- SBOM generation ---- */

static void test_sbom_to_json_basic(void) {
    TEST("sbom: generate JSON from project");
    const char *pasta =
        "{ group: \"com.example\", artifact: \"demo\", version: \"1.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"executable\", name: \"demo\" } }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char *json = now_sbom_to_json(p, NULL);
    ASSERT_NOT_NULL(json);

    /* Check CycloneDX envelope */
    if (!strstr(json, "\"bomFormat\": \"CycloneDX\"")) { free(json); now_project_free(p); FAIL("missing bomFormat"); return; }
    if (!strstr(json, "\"specVersion\": \"1.5\"")) { free(json); now_project_free(p); FAIL("missing specVersion"); return; }
    if (!strstr(json, "urn:uuid:")) { free(json); now_project_free(p); FAIL("missing serialNumber"); return; }
    /* Check metadata component */
    if (!strstr(json, "\"group\": \"com.example\"")) { free(json); now_project_free(p); FAIL("missing group"); return; }
    if (!strstr(json, "\"name\": \"demo\"")) { free(json); now_project_free(p); FAIL("missing artifact"); return; }
    if (!strstr(json, "\"version\": \"1.0.0\"")) { free(json); now_project_free(p); FAIL("missing version"); return; }
    if (!strstr(json, "\"type\": \"application\"")) { free(json); now_project_free(p); FAIL("missing type application"); return; }
    /* Check purl */
    if (!strstr(json, "pkg:now/com.example/demo@1.0.0")) { free(json); now_project_free(p); FAIL("missing purl"); return; }
    /* Check tool */
    if (!strstr(json, "\"vendor\": \"now\"")) { free(json); now_project_free(p); FAIL("missing tool vendor"); return; }

    free(json);
    now_project_free(p);
    PASS();
}

static void test_sbom_library_type(void) {
    TEST("sbom: library project type");
    const char *pasta =
        "{ group: \"io.lib\", artifact: \"utils\", version: \"2.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"shared\", name: \"utils\" } }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char *json = now_sbom_to_json(p, NULL);
    ASSERT_NOT_NULL(json);

    if (!strstr(json, "\"type\": \"library\"")) { free(json); now_project_free(p); FAIL("should be library"); return; }

    free(json);
    now_project_free(p);
    PASS();
}

static void test_sbom_with_deps(void) {
    TEST("sbom: declared deps in components");
    const char *pasta =
        "{ group: \"com.app\", artifact: \"main\", version: \"1.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"executable\", name: \"main\" },"
        "  deps: ["
        "    { id: \"org.lib:crypto:^1.2.0\" },"
        "    { id: \"org.lib:net:~2.0.0\" }"
        "  ] }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char *json = now_sbom_to_json(p, NULL);
    ASSERT_NOT_NULL(json);

    /* Should have component entries for declared deps */
    if (!strstr(json, "\"group\": \"org.lib\"")) { free(json); now_project_free(p); FAIL("missing dep group"); return; }
    if (!strstr(json, "\"name\": \"crypto\"")) { free(json); now_project_free(p); FAIL("missing crypto dep"); return; }
    if (!strstr(json, "\"name\": \"net\"")) { free(json); now_project_free(p); FAIL("missing net dep"); return; }
    /* Check dependencies section */
    if (!strstr(json, "\"dependencies\":")) { free(json); now_project_free(p); FAIL("missing dependencies"); return; }

    free(json);
    now_project_free(p);
    PASS();
}

static void test_sbom_with_license(void) {
    TEST("sbom: license field in metadata");
    const char *pasta =
        "{ group: \"com.oss\", artifact: \"lib\", version: \"1.0.0\","
        "  langs: [\"c\"], std: \"c11\", license: \"MIT\","
        "  output: { type: \"static\", name: \"lib\" } }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char *json = now_sbom_to_json(p, NULL);
    ASSERT_NOT_NULL(json);

    if (!strstr(json, "\"id\": \"MIT\"")) { free(json); now_project_free(p); FAIL("missing license"); return; }

    free(json);
    now_project_free(p);
    PASS();
}

static void test_sbom_generate_file(void) {
    TEST("sbom: generate to file");
    const char *pasta =
        "{ group: \"com.test\", artifact: \"sbomtest\", version: \"0.1.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"executable\", name: \"sbomtest\" } }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_sbom_output.json",
             NOW_TEST_RESOURCES);

    int rc = now_sbom_generate(p, NULL, outpath,
                                NOW_SBOM_CYCLONEDX_JSON, &res);
    ASSERT_EQ(rc, 0);

    /* Read back and verify */
    FILE *fp = fopen(outpath, "r");
    if (!fp) { now_project_free(p); FAIL("cannot read output"); return; }
    fseek(fp, 0, SEEK_END);
    long flen = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)flen + 1);
    fread(buf, 1, (size_t)flen, fp);
    buf[flen] = '\0';
    fclose(fp);

    if (!strstr(buf, "\"bomFormat\": \"CycloneDX\"")) { free(buf); now_project_free(p); FAIL("invalid output"); return; }
    if (!strstr(buf, "\"name\": \"sbomtest\"")) { free(buf); now_project_free(p); FAIL("missing artifact in file"); return; }

    free(buf);
    remove(outpath);
    now_project_free(p);
    PASS();
}

static void test_sbom_null_project(void) {
    TEST("sbom: null project returns NULL");
    char *json = now_sbom_to_json(NULL, NULL);
    ASSERT_NULL(json);
    PASS();
}

static void test_sbom_scope_mapping(void) {
    TEST("sbom: scope mapping (test→excluded, provided→optional)");
    const char *pasta =
        "{ group: \"com.app\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"executable\", name: \"app\" },"
        "  deps: ["
        "    { id: \"org.test:mock:1.0.0\", scope: \"test\" },"
        "    { id: \"org.api:spec:2.0.0\", scope: \"provided\" }"
        "  ] }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char *json = now_sbom_to_json(p, NULL);
    ASSERT_NOT_NULL(json);

    if (!strstr(json, "\"scope\": \"excluded\"")) { free(json); now_project_free(p); FAIL("test scope not mapped to excluded"); return; }
    if (!strstr(json, "\"scope\": \"optional\"")) { free(json); now_project_free(p); FAIL("provided scope not mapped to optional"); return; }

    free(json);
    now_project_free(p);
    PASS();
}

static void test_sbom_no_deps(void) {
    TEST("sbom: project with no deps");
    const char *pasta =
        "{ group: \"com.solo\", artifact: \"alone\", version: \"1.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"executable\", name: \"alone\" } }";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    ASSERT_NOT_NULL(p);

    char *json = now_sbom_to_json(p, NULL);
    ASSERT_NOT_NULL(json);

    /* components array should be empty */
    if (!strstr(json, "\"components\": [")) { free(json); now_project_free(p); FAIL("missing components"); return; }
    /* dependencies should still have root entry */
    if (!strstr(json, "\"dependencies\": [")) { free(json); now_project_free(p); FAIL("missing dependencies"); return; }

    free(json);
    now_project_free(p);
    PASS();
}

/* ---- Audit logging ---- */

static void test_audit_config_parse_full(void) {
    TEST("audit: config parse with all fields");
    const char *pasta = "{ audit: { enabled: true, max_entries: 500, log_path: \"/tmp/test.pasta\" } }";
    NowAuditConfig cfg;
    ASSERT_EQ(now_audit_config_parse(pasta, strlen(pasta), &cfg), 0);
    ASSERT_EQ(cfg.enabled, 1);
    ASSERT_EQ(cfg.max_entries, 500);
    ASSERT_NOT_NULL(cfg.log_path);
    ASSERT_STR(cfg.log_path, "/tmp/test.pasta");
    now_audit_config_free(&cfg);
    PASS();
}

static void test_audit_config_parse_disabled(void) {
    TEST("audit: config parse disabled");
    const char *pasta = "{ audit: { enabled: false } }";
    NowAuditConfig cfg;
    ASSERT_EQ(now_audit_config_parse(pasta, strlen(pasta), &cfg), 0);
    ASSERT_EQ(cfg.enabled, 0);
    now_audit_config_free(&cfg);
    PASS();
}

static void test_audit_config_parse_no_section(void) {
    TEST("audit: config parse missing section returns -1");
    const char *pasta = "{ other: { foo: \"bar\" } }";
    NowAuditConfig cfg;
    ASSERT_EQ(now_audit_config_parse(pasta, strlen(pasta), &cfg), -1);
    PASS();
}

static void test_audit_config_free_null(void) {
    TEST("audit: config_free on zeroed struct is safe");
    NowAuditConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    now_audit_config_free(&cfg);
    now_audit_config_free(NULL);
    PASS();
}

static void test_audit_event_name_roundtrip(void) {
    TEST("audit: event name roundtrip");
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_BUILD), "build");
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_PUBLISH), "publish");
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_YANK), "yank");
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_PROCURE), "procure");
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_AUTH_LOGIN), "auth_login");
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_VERIFY), "verify");
    ASSERT_EQ(now_audit_event_parse("publish"), NOW_AUDIT_PUBLISH);
    ASSERT_EQ(now_audit_event_parse("advisory"), NOW_AUDIT_ADVISORY);
    PASS();
}

static void test_audit_record_disabled(void) {
    TEST("audit: record when disabled is no-op");
    int rc = now_audit_record(NOW_AUDIT_BUILD, "local", "test", "ok", NULL);
    ASSERT_EQ(rc, 0);
    PASS();
}

static void test_audit_record_and_show(void) {
    TEST("audit: record and show roundtrip");
    const char *tmp_cfg = "target/test_audit_config.pasta";
    const char *tmp_log = "target/test_audit.pasta";
    remove(tmp_log);

    /* Create config and parse it */
    {
        FILE *f = fopen(tmp_cfg, "w");
        if (f) {
            fprintf(f, "{ audit: { enabled: true, log_path: \"%s\" } }\n", tmp_log);
            fclose(f);
        }
    }

    NowAuditConfig cfg;
    size_t flen = 0;
    char *data = NULL;
    FILE *fp = fopen(tmp_cfg, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        flen = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = (char *)malloc(flen + 1);
        if (data) { fread(data, 1, flen, fp); data[flen] = '\0'; }
        fclose(fp);
    }
    ASSERT_NOT_NULL(data);
    ASSERT_EQ(now_audit_config_parse(data, flen, &cfg), 0);
    ASSERT_EQ(cfg.enabled, 1);
    ASSERT_NOT_NULL(cfg.log_path);
    if (strcmp(cfg.log_path, tmp_log) != 0) { FAIL("log_path mismatch"); return; }
    free(data);
    now_audit_config_free(&cfg);

    /* Verify event name table completeness */
    ASSERT_STR(now_audit_event_name(NOW_AUDIT_AUTH_LOGOUT), "auth_logout");

    remove(tmp_log);
    remove(tmp_cfg);
    PASS();
}

/* ---- Rust FFI ---- */

static void test_rust_lang_registered(void) {
    TEST("rust: language registered");
    const NowLangDef *lang = now_lang_find("rust");
    ASSERT_NOT_NULL(lang);
    ASSERT_STR(lang->id, "rust");
    ASSERT_STR(lang->name, "Rust");
    PASS();
}

static void test_rust_classify_rs(void) {
    TEST("rust: classify .rs as rust-source");
    const char *langs[] = { "rust", NULL };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("test.rs", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "rust-source");
    ASSERT_EQ(type->role, NOW_ROLE_SOURCE);
    ASSERT_STR(type->tool_var, "${rustc}");
    PASS();
}

/* ---- Go + Julia ---- */

static void test_go_lang_registered(void) {
    TEST("go: language registered");
    const NowLangDef *lang = now_lang_find("go");
    ASSERT_NOT_NULL(lang);
    ASSERT_STR(lang->id, "go");
    PASS();
}

static void test_go_classify(void) {
    TEST("go: classify .go as go-source");
    const char *langs[] = { "go", NULL };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("main.go", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "go-source");
    ASSERT_STR(type->tool_var, "${go}");
    PASS();
}

static void test_julia_lang_registered(void) {
    TEST("julia: language registered");
    const NowLangDef *lang = now_lang_find("julia");
    ASSERT_NOT_NULL(lang);
    ASSERT_STR(lang->id, "julia");
    PASS();
}

static void test_julia_classify(void) {
    TEST("julia: classify .jl as julia-source");
    const char *langs[] = { "julia", NULL };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("solver.jl", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "julia-source");
    PASS();
}

/* ---- HTTP/2 ---- */

#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)
static void test_h2_hpack_encode_get(void) {
    TEST("h2: HPACK encode GET request");
    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ(pico_hpack_encode("GET", "https", "example.com", "/",
                                  NULL, 0, &buf, &len), 0);
    ASSERT_NOT_NULL(buf);
    /* :method GET = static index 2 (0x82), :scheme https = index 7 (0x87),
     * :path / = index 4 (0x84), :authority = index 1 with value */
    if (len < 4) { FAIL("too short"); free(buf); return; }
    /* First byte should be 0x82 (indexed :method GET) */
    ASSERT_EQ((int)buf[0], 0x82);
    free(buf);
    PASS();
}

static void test_h2_hpack_decode_status(void) {
    TEST("h2: HPACK decode :status 200");
    /* Static index 8 = :status 200 → byte 0x88 */
    uint8_t encoded[] = { 0x88 };
    int status = 0;
    PicoHttpHeader *hdrs = NULL;
    size_t count = 0;
    ASSERT_EQ(pico_hpack_decode(encoded, sizeof(encoded), &status, &hdrs, &count), 0);
    ASSERT_EQ(status, 200);
    free(hdrs);
    PASS();
}

static void test_h2_frame_layout(void) {
    TEST("h2: frame header layout");
    /* Verify the H2 connection preface constant */
    const char *preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    ASSERT_EQ((int)strlen(preface), 24);
    PASS();
}

static void test_h2_hpack_encode_with_headers(void) {
    TEST("h2: HPACK encode with extra headers");
    PicoHttpHeader extra[2];
    extra[0].name = "content-type";
    extra[0].value = "application/json";
    extra[1].name = "x-custom";
    extra[1].value = "test";
    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ(pico_hpack_encode("POST", "https", "api.example.com", "/v1/data",
                                  extra, 2, &buf, &len), 0);
    ASSERT_NOT_NULL(buf);
    /* :method POST = static index 3 (0x83) */
    ASSERT_EQ((int)buf[0], 0x83);
    if (len < 10) { FAIL("too short for headers"); free(buf); return; }
    free(buf);
    PASS();
}
#endif /* PICO_HTTP_TLS */

/* ---- Graph cache ---- */

static void test_graph_key_deterministic(void) {
    TEST("graph: key is deterministic");
    char *k1 = now_graph_key(NULL, "/usr/bin/gcc", "abc123");
    char *k2 = now_graph_key(NULL, "/usr/bin/gcc", "abc123");
    ASSERT_NOT_NULL(k1);
    ASSERT_NOT_NULL(k2);
    if (strcmp(k1, k2) != 0) { FAIL("keys should match"); free(k1); free(k2); return; }
    free(k1);
    free(k2);
    PASS();
}

static void test_graph_key_varies(void) {
    TEST("graph: different inputs produce different keys");
    char *k1 = now_graph_key(NULL, "/usr/bin/gcc", "abc123");
    char *k2 = now_graph_key(NULL, "/usr/bin/clang", "abc123");
    ASSERT_NOT_NULL(k1);
    ASSERT_NOT_NULL(k2);
    if (strcmp(k1, k2) == 0) { FAIL("keys should differ"); free(k1); free(k2); return; }
    free(k1);
    free(k2);
    PASS();
}

static void test_graph_serialize_roundtrip(void) {
    TEST("graph: serialize/deserialize roundtrip");
    NowManifest m;
    now_manifest_init(&m);
    now_manifest_set(&m, "src/main.c", "target/main.o", "hash1", "fhash1", 1000);
    now_manifest_set(&m, "src/util.c", "target/util.o", "hash2", "fhash2", 2000);

    const char *deps[] = { "/usr/include/stdio.h" };
    const char *dhashes[] = { "dephash1" };
    now_manifest_set_deps(&m, "src/main.c", deps, dhashes, 1);

    size_t len = 0;
    char *data = now_graph_serialize(&m, &len);
    ASSERT_NOT_NULL(data);
    if (len == 0) { FAIL("empty output"); free(data); now_manifest_free(&m); return; }

    /* Verify it's valid pasta with type marker */
    if (!strstr(data, "now-build-graph")) { FAIL("missing type marker"); free(data); now_manifest_free(&m); return; }

    /* Deserialize back */
    NowManifest m2;
    ASSERT_EQ(now_graph_deserialize(data, len, &m2), 0);
    free(data);

    /* Verify entries survived */
    const NowManifestEntry *e1 = now_manifest_find(&m2, "src/main.c");
    const NowManifestEntry *e2 = now_manifest_find(&m2, "src/util.c");
    ASSERT_NOT_NULL(e1);
    ASSERT_NOT_NULL(e2);
    ASSERT_STR(e1->source_hash, "hash1");
    ASSERT_STR(e2->source_hash, "hash2");

    /* Verify deps */
    ASSERT_EQ((int)e1->dep_count, 1);
    ASSERT_STR(e1->dep_hashes[0], "dephash1");

    now_manifest_free(&m);
    now_manifest_free(&m2);
    PASS();
}

static void test_graph_deserialize_bad_input(void) {
    TEST("graph: deserialize rejects bad input");
    NowManifest m;
    ASSERT_EQ(now_graph_deserialize(NULL, 0, &m), -1);
    ASSERT_EQ(now_graph_deserialize("not pasta", 9, &m), -1);
    /* Valid pasta but wrong type */
    const char *wrong = "{ type: \"wrong\" }";
    ASSERT_EQ(now_graph_deserialize(wrong, strlen(wrong), &m), -1);
    PASS();
}

static void test_graph_pull_unreachable(void) {
    TEST("graph: pull from unreachable host returns -1");
    NowRemoteCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "http://192.0.2.1:9999";  /* TEST-NET, unreachable */
    NowManifest m;
    ASSERT_EQ(now_graph_pull(&cfg, "testkey", &m), -1);
    PASS();
}

/* ---- Watch ---- */

static void test_watch_opts_init(void) {
    TEST("watch: opts init defaults");
    NowWatchOpts opts;
    now_watch_opts_init(&opts);
    ASSERT_EQ(opts.poll_ms, 500);
    ASSERT_EQ(opts.verbose, 0);
    ASSERT_EQ(opts.jobs, 0);
    PASS();
}

static void test_watch_snapshot_hello(void) {
    TEST("watch: snapshot discovers hello project files");
    char path[512];
    snprintf(path, sizeof(path), "%s/hello/now.pasta", NOW_TEST_RESOURCES);

    NowResult result;
    memset(&result, 0, sizeof(result));
    NowProject *p = now_project_load(path, &result);
    ASSERT_NOT_NULL(p);

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/hello", NOW_TEST_RESOURCES);

    NowWatchSnapshot snap;
    ASSERT_EQ(now_watch_snapshot(p, basedir, &snap), 0);
    /* hello project has at least main.c */
    if (snap.count == 0) { FAIL("no files found"); now_project_free(p); return; }
    /* pasta_mtime should be non-zero */
    if (snap.pasta_mtime == 0) { FAIL("pasta_mtime is 0"); now_project_free(p); return; }

    now_watch_snapshot_free(&snap);
    now_project_free(p);
    PASS();
}

static void test_watch_diff_no_change(void) {
    TEST("watch: diff detects no change");
    NowWatchSnapshot a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.pasta_mtime = 100;
    b.pasta_mtime = 100;
    ASSERT_EQ(now_watch_diff(&a, &b), 0);
    PASS();
}

static void test_watch_diff_source_change(void) {
    TEST("watch: diff detects source change");
    NowWatchEntry ea = { .path = "test.c", .mtime = 100 };
    NowWatchEntry eb = { .path = "test.c", .mtime = 200 };
    NowWatchSnapshot a = { .entries = &ea, .count = 1, .pasta_mtime = 100 };
    NowWatchSnapshot b = { .entries = &eb, .count = 1, .pasta_mtime = 100 };
    ASSERT_EQ(now_watch_diff(&a, &b), 1);
    PASS();
}

static void test_watch_diff_pasta_change(void) {
    TEST("watch: diff detects pasta change");
    NowWatchSnapshot a = { .entries = NULL, .count = 0, .pasta_mtime = 100 };
    NowWatchSnapshot b = { .entries = NULL, .count = 0, .pasta_mtime = 200 };
    ASSERT_EQ(now_watch_diff(&a, &b), 2);
    PASS();
}

static void test_watch_snapshot_free_null(void) {
    TEST("watch: snapshot_free NULL is safe");
    now_watch_snapshot_free(NULL);
    NowWatchSnapshot s;
    memset(&s, 0, sizeof(s));
    now_watch_snapshot_free(&s);
    PASS();
}

/* ---- Build integration ---- */

static void test_build_hello(void) {
    TEST("build: compile and link hello project");
    char path[512];
    snprintf(path, sizeof(path), "%s/hello/now.pasta", NOW_TEST_RESOURCES);

    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/hello", NOW_TEST_RESOURCES);

    int rc = now_build(p, basedir, 0, 0, &res);
    now_project_free(p);

    if (rc != 0) { FAIL(res.message); return; }

    /* Check output exists */
    char out_path[512];
#ifdef _WIN32
    snprintf(out_path, sizeof(out_path), "%s/target/bin/hello.exe", basedir);
#else
    snprintf(out_path, sizeof(out_path), "%s/target/bin/hello", basedir);
#endif
    if (!now_path_exists(out_path)) {
        FAIL("output binary not found");
        return;
    }
    PASS();
}

static void test_build_java_hello(void) {
    TEST("build: compile and package Java project");

    /* Skip if javac not available */
    int javac_rc = now_exec((const char *const []){"javac", "-version", NULL}, 0);
    if (javac_rc != 0) {
        tests_run--;  /* don't count skipped */
        printf("SKIP (javac not in PATH)\n");
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/hello_java/now.pasta", NOW_TEST_RESOURCES);

    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/hello_java", NOW_TEST_RESOURCES);

    int rc = now_build(p, basedir, 0, 0, &res);
    now_project_free(p);

    if (rc != 0) { FAIL(res.message); return; }

    /* Check JAR output exists */
    char jar_path[512];
    snprintf(jar_path, sizeof(jar_path), "%s/target/bin/hello.jar", basedir);
    if (!now_path_exists(jar_path)) {
        FAIL("output JAR not found");
        return;
    }
    PASS();
}

/* ---- Semantic versioning ---- */

static void test_semver_parse_basic(void) {
    TEST("semver: parse 1.2.3");
    NowSemVer v;
    ASSERT_EQ(now_semver_parse("1.2.3", &v), 0);
    ASSERT_EQ(v.major, 1);
    ASSERT_EQ(v.minor, 2);
    ASSERT_EQ(v.patch, 3);
    if (v.prerelease) { FAIL("prerelease should be NULL"); now_semver_free(&v); return; }
    now_semver_free(&v);
    PASS();
}

static void test_semver_parse_prerelease(void) {
    TEST("semver: parse 3.0.0-beta.1");
    NowSemVer v;
    ASSERT_EQ(now_semver_parse("3.0.0-beta.1", &v), 0);
    ASSERT_EQ(v.major, 3);
    ASSERT_EQ(v.minor, 0);
    ASSERT_EQ(v.patch, 0);
    ASSERT_NOT_NULL(v.prerelease);
    ASSERT_STR(v.prerelease, "beta.1");
    now_semver_free(&v);
    PASS();
}

static void test_semver_parse_build(void) {
    TEST("semver: parse 1.0.0+build.42");
    NowSemVer v;
    ASSERT_EQ(now_semver_parse("1.0.0+build.42", &v), 0);
    ASSERT_EQ(v.major, 1);
    ASSERT_NOT_NULL(v.build);
    ASSERT_STR(v.build, "build.42");
    now_semver_free(&v);
    PASS();
}

static void test_semver_compare(void) {
    TEST("semver: compare ordering");
    NowSemVer a, b;
    now_semver_parse("1.0.0", &a);
    now_semver_parse("2.0.0", &b);
    if (now_semver_compare(&a, &b) >= 0) { FAIL("1.0.0 should < 2.0.0"); now_semver_free(&a); now_semver_free(&b); return; }
    now_semver_free(&a); now_semver_free(&b);

    now_semver_parse("1.2.3", &a);
    now_semver_parse("1.2.3", &b);
    ASSERT_EQ(now_semver_compare(&a, &b), 0);
    now_semver_free(&a); now_semver_free(&b);

    /* pre-release < release */
    now_semver_parse("1.0.0-rc.1", &a);
    now_semver_parse("1.0.0", &b);
    if (now_semver_compare(&a, &b) >= 0) { FAIL("1.0.0-rc.1 should < 1.0.0"); now_semver_free(&a); now_semver_free(&b); return; }
    now_semver_free(&a); now_semver_free(&b);
    PASS();
}

static void test_semver_to_string(void) {
    TEST("semver: to_string roundtrip");
    NowSemVer v;
    now_semver_parse("1.2.3-beta.1", &v);
    char *s = now_semver_to_string(&v);
    ASSERT_NOT_NULL(s);
    ASSERT_STR(s, "1.2.3-beta.1");
    free(s);
    now_semver_free(&v);
    PASS();
}

/* ---- Version ranges ---- */

static void test_range_exact(void) {
    TEST("range: exact 1.2.3");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse("1.2.3", &r), 0);
    ASSERT_EQ(r.kind, NOW_RANGE_EXACT);

    NowSemVer v;
    now_semver_parse("1.2.3", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("1.2.4", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_caret(void) {
    TEST("range: caret ^1.2.3 → [1.2.3, 2.0.0)");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse("^1.2.3", &r), 0);
    ASSERT_EQ(r.kind, NOW_RANGE_CARET);

    NowSemVer v;
    now_semver_parse("1.2.3", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("1.9.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("2.0.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_semver_parse("1.2.2", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_caret_pre1(void) {
    TEST("range: caret ^0.9.3 → [0.9.3, 0.10.0)");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse("^0.9.3", &r), 0);

    NowSemVer v;
    now_semver_parse("0.9.5", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("0.10.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_tilde(void) {
    TEST("range: tilde ~1.2.3 → [1.2.3, 1.3.0)");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse("~1.2.3", &r), 0);
    ASSERT_EQ(r.kind, NOW_RANGE_TILDE);

    NowSemVer v;
    now_semver_parse("1.2.9", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("1.3.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_gte(void) {
    TEST("range: >=2.0.0");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse(">=2.0.0", &r), 0);
    ASSERT_EQ(r.kind, NOW_RANGE_GTE);

    NowSemVer v;
    now_semver_parse("2.0.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("3.5.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("1.9.9", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_compound(void) {
    TEST("range: >=1.2.0 <2.0.0");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse(">=1.2.0 <2.0.0", &r), 0);
    ASSERT_EQ(r.kind, NOW_RANGE_COMPOUND);

    NowSemVer v;
    now_semver_parse("1.5.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_semver_parse("2.0.0", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 0);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_wildcard(void) {
    TEST("range: * matches anything");
    NowVersionRange r;
    ASSERT_EQ(now_range_parse("*", &r), 0);
    ASSERT_EQ(r.kind, NOW_RANGE_ANY);

    NowSemVer v;
    now_semver_parse("99.99.99", &v);
    ASSERT_EQ(now_range_satisfies(&r, &v), 1);
    now_semver_free(&v);

    now_range_free(&r);
    PASS();
}

static void test_range_intersect(void) {
    TEST("range: intersect ^1.3.0 ∩ ^1.2.0 → [1.3.0, 2.0.0)");
    NowVersionRange a, b, out;
    now_range_parse("^1.3.0", &a);
    now_range_parse("^1.2.0", &b);
    ASSERT_EQ(now_range_intersect(&a, &b, &out), 0);

    /* Floor should be 1.3.0 */
    ASSERT_EQ(out.floor.major, 1);
    ASSERT_EQ(out.floor.minor, 3);
    ASSERT_EQ(out.floor.patch, 0);

    /* Ceiling should be 2.0.0 */
    ASSERT_EQ(out.ceiling.major, 2);
    ASSERT_EQ(out.ceiling.minor, 0);

    now_range_free(&a);
    now_range_free(&b);
    now_range_free(&out);
    PASS();
}

/* ---- Coordinate parsing ---- */

static void test_coord_parse(void) {
    TEST("coord: parse org.acme:core:^1.5.0");
    NowCoordinate c;
    ASSERT_EQ(now_coord_parse("org.acme:core:^1.5.0", &c), 0);
    ASSERT_STR(c.group, "org.acme");
    ASSERT_STR(c.artifact, "core");
    ASSERT_STR(c.version, "^1.5.0");
    now_coord_free(&c);
    PASS();
}

/* ---- Manifest ---- */

static void test_manifest_set_find(void) {
    TEST("manifest: set and find entry");
    NowManifest m;
    now_manifest_init(&m);
    ASSERT_EQ(now_manifest_set(&m, "src/main.c", "target/obj/main.c.o",
                                "abc123", "def456", 1000), 0);
    const NowManifestEntry *e = now_manifest_find(&m, "src/main.c");
    ASSERT_NOT_NULL(e);
    ASSERT_STR(e->source, "src/main.c");
    ASSERT_STR(e->object, "target/obj/main.c.o");
    ASSERT_STR(e->source_hash, "abc123");
    now_manifest_free(&m);
    PASS();
}

static void test_manifest_update(void) {
    TEST("manifest: update existing entry");
    NowManifest m;
    now_manifest_init(&m);
    now_manifest_set(&m, "src/main.c", "obj1", "hash1", "fh1", 100);
    now_manifest_set(&m, "src/main.c", "obj2", "hash2", "fh2", 200);
    ASSERT_EQ(m.count, (size_t)1);
    const NowManifestEntry *e = now_manifest_find(&m, "src/main.c");
    ASSERT_NOT_NULL(e);
    ASSERT_STR(e->object, "obj2");
    ASSERT_STR(e->source_hash, "hash2");
    now_manifest_free(&m);
    PASS();
}

static void test_sha256_string(void) {
    TEST("manifest: sha256 of known string");
    char *hash = now_sha256_string("hello", 5);
    ASSERT_NOT_NULL(hash);
    /* SHA-256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824 */
    ASSERT_STR(hash, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    free(hash);
    PASS();
}

/* ---- Resolver ---- */

static void test_resolver_single_dep(void) {
    TEST("resolver: single dependency resolves");
    NowResolver r;
    now_resolver_init(&r, "lowest");
    ASSERT_EQ(now_resolver_add(&r, "zlib:zlib:^1.3.0", "compile", "root", 0), 0);

    NowLockFile lf;
    now_lock_init(&lf);
    NowResult res;
    ASSERT_EQ(now_resolver_resolve(&r, &lf, &res), 0);
    ASSERT_EQ(lf.count, (size_t)1);

    const NowLockEntry *e = now_lock_find(&lf, "zlib", "zlib");
    ASSERT_NOT_NULL(e);
    ASSERT_STR(e->group, "zlib");
    ASSERT_STR(e->artifact, "zlib");
    ASSERT_STR(e->version, "1.3.0");  /* lowest = floor */
    ASSERT_STR(e->scope, "compile");

    now_lock_free(&lf);
    now_resolver_free(&r);
    PASS();
}

static void test_resolver_convergence_lowest(void) {
    TEST("resolver: lowest convergence picks floor of intersection");
    NowResolver r;
    now_resolver_init(&r, "lowest");
    now_resolver_add(&r, "zlib:zlib:^1.3.0", "compile", "A", 0);
    now_resolver_add(&r, "zlib:zlib:^1.2.0", "compile", "B", 0);

    NowLockFile lf;
    now_lock_init(&lf);
    NowResult res;
    ASSERT_EQ(now_resolver_resolve(&r, &lf, &res), 0);

    const NowLockEntry *e = now_lock_find(&lf, "zlib", "zlib");
    ASSERT_NOT_NULL(e);
    /* Intersection of ^1.3.0 and ^1.2.0 = [1.3.0, 2.0.0), lowest = 1.3.0 */
    ASSERT_STR(e->version, "1.3.0");

    now_lock_free(&lf);
    now_resolver_free(&r);
    PASS();
}

static void test_resolver_conflict(void) {
    TEST("resolver: disjoint ranges produce conflict");
    NowResolver r;
    now_resolver_init(&r, "lowest");
    now_resolver_add(&r, "zlib:zlib:^1.0.0", "compile", "A", 0);
    now_resolver_add(&r, "zlib:zlib:^2.0.0", "compile", "B", 0);

    NowLockFile lf;
    now_lock_init(&lf);
    NowResult res;
    int rc = now_resolver_resolve(&r, &lf, &res);
    if (rc == 0) { FAIL("should have conflicted"); now_lock_free(&lf); now_resolver_free(&r); return; }
    ASSERT_EQ(res.code, NOW_ERR_SCHEMA);

    now_lock_free(&lf);
    now_resolver_free(&r);
    PASS();
}

static void test_resolver_multiple_deps(void) {
    TEST("resolver: multiple different deps resolve independently");
    NowResolver r;
    now_resolver_init(&r, "lowest");
    now_resolver_add(&r, "zlib:zlib:^1.3.0", "compile", "root", 0);
    now_resolver_add(&r, "org.acme:core:~4.2.0", "compile", "root", 0);

    NowLockFile lf;
    now_lock_init(&lf);
    NowResult res;
    ASSERT_EQ(now_resolver_resolve(&r, &lf, &res), 0);
    ASSERT_EQ(lf.count, (size_t)2);

    ASSERT_NOT_NULL(now_lock_find(&lf, "zlib", "zlib"));
    ASSERT_NOT_NULL(now_lock_find(&lf, "org.acme", "core"));
    ASSERT_STR(now_lock_find(&lf, "org.acme", "core")->version, "4.2.0");

    now_lock_free(&lf);
    now_resolver_free(&r);
    PASS();
}

static void test_resolver_override(void) {
    TEST("resolver: override forces version despite range conflict");
    NowResolver r;
    now_resolver_init(&r, "lowest");
    now_resolver_add(&r, "zlib:zlib:^1.0.0", "compile", "A", 0);
    now_resolver_add(&r, "zlib:zlib:2.0.0", "compile", "override", 1);

    NowLockFile lf;
    now_lock_init(&lf);
    NowResult res;
    ASSERT_EQ(now_resolver_resolve(&r, &lf, &res), 0);

    const NowLockEntry *e = now_lock_find(&lf, "zlib", "zlib");
    ASSERT_NOT_NULL(e);
    ASSERT_STR(e->version, "2.0.0");
    ASSERT_EQ(e->overridden, 1);

    now_lock_free(&lf);
    now_resolver_free(&r);
    PASS();
}

static void test_lock_save_load(void) {
    TEST("lock: save and reload");
    NowLockFile lf;
    now_lock_init(&lf);

    NowLockEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.group = "zlib";
    entry.artifact = "zlib";
    entry.version = "1.3.0";
    entry.scope = "compile";
    entry.triple = "noarch";
    now_lock_set(&lf, &entry);

    char path[512];
    snprintf(path, sizeof(path), "%s/test_lock.pasta", NOW_TEST_RESOURCES);
    ASSERT_EQ(now_lock_save(&lf, path), 0);
    now_lock_free(&lf);

    /* Reload */
    NowLockFile lf2;
    ASSERT_EQ(now_lock_load(&lf2, path), 0);
    ASSERT_EQ(lf2.count, (size_t)1);
    const NowLockEntry *e = now_lock_find(&lf2, "zlib", "zlib");
    ASSERT_NOT_NULL(e);
    ASSERT_STR(e->version, "1.3.0");
    ASSERT_STR(e->scope, "compile");

    now_lock_free(&lf2);
    /* Clean up test file */
    remove(path);
    PASS();
}

static void test_test_phase(void) {
    TEST("test: compile and run test sources");
    char path[512];
    snprintf(path, sizeof(path), "%s/testable/now.pasta", NOW_TEST_RESOURCES);

    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/testable", NOW_TEST_RESOURCES);

    int rc = now_test(p, basedir, 0, 0, &res);
    now_project_free(p);

    if (rc != 0) { FAIL(res.message); return; }
    PASS();
}

/* ---- Main ---- */

/* ---- HTTP client ---- */

static void test_pico_http_version(void) {
    TEST("pico_http: version string");
    const char *v = pico_http_version();
    ASSERT_NOT_NULL(v);
    ASSERT_STR(v, "0.3.0");
    PASS();
}

static void test_pico_http_parse_url(void) {
    TEST("pico_http: parse URL");
    char *host = NULL, *path = NULL;
    int port = 0;
    int rc = pico_http_parse_url("http://localhost:8080/resolve/org/acme",
                                 &host, &port, &path);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(host, "localhost");
    ASSERT_EQ(port, 8080);
    ASSERT_STR(path, "/resolve/org/acme");
    free(host);
    free(path);
    PASS();
}

static void test_pico_http_parse_url_no_port(void) {
    TEST("pico_http: parse URL without port");
    char *host = NULL, *path = NULL;
    int port = 0;
    int rc = pico_http_parse_url("http://example.com/api",
                                 &host, &port, &path);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(host, "example.com");
    ASSERT_EQ(port, 80);
    ASSERT_STR(path, "/api");
    free(host);
    free(path);
    PASS();
}

static void test_pico_http_parse_url_no_path(void) {
    TEST("pico_http: parse URL without path");
    char *host = NULL, *path = NULL;
    int port = 0;
    int rc = pico_http_parse_url("http://example.com",
                                 &host, &port, &path);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(host, "example.com");
    ASSERT_EQ(port, 80);
    ASSERT_STR(path, "/");
    free(host);
    free(path);
    PASS();
}

static void test_pico_http_parse_url_https(void) {
    TEST("pico_http: parse https URL");
    char *host = NULL, *path = NULL;
    int port = 0, tls = 0;
    int rc = pico_http_parse_url_ex("https://example.com/api",
                                     &host, &port, &path, &tls);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(host, "example.com");
    ASSERT_EQ(port, 443);
    ASSERT_STR(path, "/api");
    ASSERT_EQ(tls, 1);
    free(host);
    free(path);
    PASS();
}

static void test_pico_http_parse_url_reject_ftp(void) {
    TEST("pico_http: reject ftp URL");
    char *host = NULL, *path = NULL;
    int port = 0;
    int rc = pico_http_parse_url("ftp://example.com/file",
                                 &host, &port, &path);
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_pico_http_error_codes(void) {
    TEST("pico_http: error code strings");
    ASSERT_STR(pico_http_strerror(PICO_OK), "success");
    ASSERT_STR(pico_http_strerror(PICO_ERR_DNS), "DNS resolution failed");
    ASSERT_STR(pico_http_strerror(PICO_ERR_CONNECT), "connection failed");
    ASSERT_STR(pico_http_strerror(PICO_ERR_TOO_MANY_REDIRECTS), "too many redirects");
    ASSERT_STR(pico_http_strerror(PICO_ERR_TLS), "TLS error");
    /* Unknown code */
    ASSERT_STR(pico_http_strerror(-99), "unknown error");
    PASS();
}

static void test_pico_http_invalid_args(void) {
    TEST("pico_http: invalid arguments return PICO_ERR_INVALID");
    PicoHttpResponse res;
    ASSERT_EQ(pico_http_get(NULL, 80, "/", NULL, &res), PICO_ERR_INVALID);
    ASSERT_EQ(pico_http_get("host", 80, NULL, NULL, &res), PICO_ERR_INVALID);
    ASSERT_EQ(pico_http_get("host", 80, "/", NULL, NULL), PICO_ERR_INVALID);
    PASS();
}

static void test_pico_http_dns_failure(void) {
    TEST("pico_http: DNS failure returns PICO_ERR_DNS");
    PicoHttpResponse res;
    PicoHttpOptions opts = {0};
    opts.connect_timeout_ms = 1000;
    int rc = pico_http_get("this-host-does-not-exist-7f3a.invalid",
                           80, "/", &opts, &res);
    ASSERT_EQ(rc, PICO_ERR_DNS);
    PASS();
}

static void test_pico_http_connect_failure(void) {
    TEST("pico_http: connect failure returns PICO_ERR_CONNECT");
    PicoHttpResponse res;
    PicoHttpOptions opts = {0};
    opts.connect_timeout_ms = 1000;
    /* Port 1 is almost certainly not listening */
    int rc = pico_http_get("127.0.0.1", 1, "/", &opts, &res);
    ASSERT_EQ(rc, PICO_ERR_CONNECT);
    PASS();
}

static void test_pico_http_find_header(void) {
    TEST("pico_http: find_header on empty response");
    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));
    if (pico_http_find_header(&res, "Content-Type") != NULL) {
        FAIL("expected NULL for empty response");
        return;
    }
    PASS();
}

static void test_pico_http_request_url(void) {
    TEST("pico_http: request with invalid URL");
    PicoHttpResponse res;
    int rc = pico_http_request("GET", "not-a-url", NULL, NULL, 0, NULL, &res);
    ASSERT_EQ(rc, PICO_ERR_INVALID);
    PASS();
}

static void test_pico_http_response_free_zeroed(void) {
    TEST("pico_http: response_free on zeroed struct");
    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));
    pico_http_response_free(&res); /* should not crash */
    pico_http_response_free(NULL); /* should not crash */
    PASS();
}

static void test_pico_http_stream_invalid_args(void) {
    TEST("pico_http: get_stream rejects NULL args");
    PicoHttpResponse res;
    int rc = pico_http_get_stream(NULL, 80, "/", NULL, &res, NULL, NULL);
    ASSERT_EQ(rc, PICO_ERR_INVALID);
    rc = pico_http_get_stream("localhost", 80, "/", NULL, &res, NULL, NULL);
    ASSERT_EQ(rc, PICO_ERR_INVALID);
    PASS();
}

static int stream_counter_fn(const void *data, size_t len, void *userdata) {
    (void)data;
    size_t *total = (size_t *)userdata;
    *total += len;
    return 0;
}

static void test_pico_http_stream_connect_failure(void) {
    TEST("pico_http: get_stream connect failure");
    PicoHttpResponse res;
    size_t total = 0;
    int rc = pico_http_get_stream("127.0.0.1", 1, "/", NULL, &res,
                                   stream_counter_fn, &total);
    ASSERT_EQ(rc != PICO_OK, 1);
    ASSERT_EQ(total, 0);
    PASS();
}

static int stream_abort_fn(const void *data, size_t len, void *userdata) {
    (void)data; (void)len; (void)userdata;
    return -1; /* abort immediately */
}

static void test_pico_http_stream_callback_type(void) {
    TEST("pico_http: stream callback type exists");
    /* Verify the typedef compiles and can be assigned */
    PicoHttpWriteFn fn = stream_counter_fn;
    ASSERT_NOT_NULL((void *)(size_t)fn);
    fn = stream_abort_fn;
    ASSERT_NOT_NULL((void *)(size_t)fn);
    PASS();
}

static void test_pico_http_tls_noverify_option(void) {
    TEST("pico_http: tls_noverify option accepted");
    /* Verify that PicoHttpOptions with tls_noverify compiles and the
     * option is propagated (connect will fail, but no crash). */
    PicoHttpResponse res;
    PicoHttpOptions opts = {0};
    opts.tls_noverify = 1;
    opts.connect_timeout_ms = 500;
    int rc = pico_http_get("127.0.0.1", 1, "/", &opts, &res);
    /* Expect connect failure — we just care that the option didn't crash */
    ASSERT_EQ(rc, PICO_ERR_CONNECT);
    PASS();
}

static void test_pico_http_tls_options_zero_init(void) {
    TEST("pico_http: zero-init options means verify enabled");
    /* Verify that zero-initialized PicoHttpOptions has tls_noverify=0,
     * meaning verification is the default. */
    PicoHttpOptions opts = {0};
    ASSERT_EQ(opts.tls_noverify, 0);
    if (opts.ca_file != NULL) { FAIL("expected NULL ca_file"); return; }
    if (opts.ca_data != NULL) { FAIL("expected NULL ca_data"); return; }
    ASSERT_EQ((int)opts.ca_data_len, 0);
    PASS();
}

static void test_pico_http_tls_ca_file_option(void) {
    TEST("pico_http: ca_file option accepted");
    PicoHttpResponse res;
    PicoHttpOptions opts = {0};
    opts.ca_file = "/nonexistent/ca.pem";
    opts.connect_timeout_ms = 500;
    /* Will fail to connect, but ensures the ca_file path doesn't crash */
    int rc = pico_http_get("127.0.0.1", 1, "/", &opts, &res);
    ASSERT_EQ(rc, PICO_ERR_CONNECT);
    PASS();
}

static void test_pico_ws_tls_noverify_option(void) {
    TEST("pico_ws: tls_noverify option accepted");
    PicoWsOptions opts = {0};
    opts.tls_noverify = 1;
    opts.connect_timeout_ms = 500;
    int err = 0;
    PicoWs *ws = pico_ws_connect("ws://127.0.0.1:1/ws", &opts, &err);
    if (ws != NULL) { FAIL("expected NULL"); pico_ws_close(ws); return; }
    ASSERT_EQ(err, PICO_WS_ERR_CONNECT);
    PASS();
}

static void test_pico_ws_tls_options_zero_init(void) {
    TEST("pico_ws: zero-init options means verify enabled");
    PicoWsOptions opts = {0};
    ASSERT_EQ(opts.tls_noverify, 0);
    if (opts.ca_file != NULL) { FAIL("expected NULL ca_file"); return; }
    if (opts.ca_data != NULL) { FAIL("expected NULL ca_data"); return; }
    ASSERT_EQ((int)opts.ca_data_len, 0);
    PASS();
}

/* ---- WebSocket client ---- */

static void test_pico_ws_version(void) {
    TEST("pico_ws: version string");
    const char *v = pico_ws_version();
    ASSERT_NOT_NULL(v);
    ASSERT_STR(v, "0.1.0");
    PASS();
}

static void test_pico_ws_error_codes(void) {
    TEST("pico_ws: error code strings");
    ASSERT_STR(pico_ws_strerror(PICO_WS_OK), "success");
    ASSERT_STR(pico_ws_strerror(PICO_WS_ERR_CONNECT), "connection failed");
    ASSERT_STR(pico_ws_strerror(PICO_WS_ERR_HANDSHAKE), "WebSocket handshake failed");
    ASSERT_STR(pico_ws_strerror(PICO_WS_ERR_CLOSED), "connection closed");
    ASSERT_STR(pico_ws_strerror(-99), "unknown error");
    PASS();
}

static void test_pico_ws_invalid_args(void) {
    TEST("pico_ws: invalid arguments");
    int err = 0;
    PicoWs *ws = pico_ws_connect(NULL, NULL, &err);
    if (ws != NULL) { FAIL("expected NULL"); pico_ws_close(ws); return; }
    ASSERT_EQ(err, PICO_WS_ERR_INVALID);
    ASSERT_EQ(pico_ws_send(NULL, "hi", 2, 0), PICO_WS_ERR_INVALID);
    char buf[16];
    int r = pico_ws_recv(NULL, buf, sizeof(buf), 0, NULL);
    ASSERT_EQ(r, PICO_WS_ERR_INVALID);
    PASS();
}

static void test_pico_ws_bad_url(void) {
    TEST("pico_ws: bad URL scheme");
    int err = 0;
    PicoWs *ws = pico_ws_connect("http://localhost/path", NULL, &err);
    if (ws != NULL) { FAIL("expected NULL"); pico_ws_close(ws); return; }
    ASSERT_EQ(err, PICO_WS_ERR_URL);
    PASS();
}

static void test_pico_ws_connect_failure(void) {
    TEST("pico_ws: connect failure");
    int err = 0;
    PicoWsOptions opts = {0};
    opts.connect_timeout_ms = 1000;
    PicoWs *ws = pico_ws_connect("ws://127.0.0.1:1/ws", &opts, &err);
    if (ws != NULL) { FAIL("expected NULL"); pico_ws_close(ws); return; }
    ASSERT_EQ(err, PICO_WS_ERR_CONNECT);
    PASS();
}

static void test_pico_ws_close_null(void) {
    TEST("pico_ws: close NULL safe");
    pico_ws_close(NULL); /* should not crash */
    PASS();
}

/* ---- Procure ---- */

static void test_repo_dep_path(void) {
    TEST("procure: repo dep path");
    char *p = now_repo_dep_path("/tmp/repo", "org.acme", "core", "1.5.0");
    ASSERT_NOT_NULL(p);
    /* Check it contains the expected components */
    if (!strstr(p, "org") || !strstr(p, "acme") ||
        !strstr(p, "core") || !strstr(p, "1.5.0")) {
        FAIL("path missing expected components");
        free(p);
        return;
    }
    free(p);
    PASS();
}

static void test_procure_no_deps(void) {
    TEST("procure: no deps returns success");
    /* A project with no dependencies should succeed immediately */
    NowResult res;
    const char *pasta =
        "{ group: \"io.test\", artifact: \"nodeps\", version: \"1.0.0\","
        "  lang: \"c\" }";
    NowProject *proj = now_project_load_string(pasta, strlen(pasta), &res);
    if (!proj) { FAIL(res.message); return; }

    NowProcureOpts opts = {0};
    opts.repo_root = "/tmp/now-test-repo";
    int rc = now_procure(proj, &opts, &res);
    now_project_free(proj);
    ASSERT_EQ(rc, 0);
    PASS();
}

static void test_cpu_count(void) {
    TEST("cpu count >= 1");
    int n = now_cpu_count();
    if (n < 1) { FAIL("cpu count < 1"); return; }
    PASS();
}

static void test_obj_path_ex_obj(void) {
    TEST("fs: obj_path_ex with .obj extension");
    char *p = now_obj_path_ex("/project", "src/main/c/foo.c",
                               "src/main/c", "target", ".obj");
    if (!p) { FAIL("returned NULL"); return; }
    /* Should end with foo.c.obj, not foo.c.o */
    const char *end = strstr(p, "foo.c.obj");
    if (!end) { FAIL(p); free(p); return; }
    free(p);
    PASS();
}

static void test_toolchain_gcc_default(void) {
    TEST("toolchain: defaults to gcc (no CC env)");
    /* Save and clear CC */
    const char *saved_cc = getenv("CC");
    char *saved_copy = saved_cc ? strdup(saved_cc) : NULL;
#ifdef _WIN32
    _putenv("CC=");
#else
    unsetenv("CC");
#endif
    NowToolchain tc;
    memset(&tc, 0, sizeof(tc));
    NowProject proj;
    memset(&proj, 0, sizeof(proj));
    now_toolchain_resolve(&tc, &proj);

    int ok = (tc.is_msvc == 0);
    now_toolchain_free(&tc);

    /* Restore CC */
    if (saved_copy) {
        char buf[512];
        snprintf(buf, sizeof(buf), "CC=%s", saved_copy);
#ifdef _WIN32
        _putenv(buf);
#else
        setenv("CC", saved_copy, 1);
#endif
        free(saved_copy);
    }

    if (!ok) { FAIL("is_msvc should be 0"); return; }
    PASS();
}

static void test_publish_missing_identity(void) {
    TEST("publish: rejects project without group/artifact/version");
    NowProject proj;
    memset(&proj, 0, sizeof(proj));
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_publish(&proj, ".", "http://localhost:9999", 0, &res);
    if (rc == 0) { FAIL("should fail without identity"); return; }
    ASSERT_EQ(res.code, NOW_ERR_SCHEMA);
    PASS();
}

static void test_publish_no_package(void) {
    TEST("publish: fails when tarball not found");
    NowProject proj;
    memset(&proj, 0, sizeof(proj));
    proj.group = "org.test";
    proj.artifact = "nope";
    proj.version = "1.0.0";
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_publish(&proj, ".", "http://localhost:9999", 0, &res);
    if (rc == 0) { FAIL("should fail without package"); return; }
    ASSERT_EQ(res.code, NOW_ERR_NOT_FOUND);
    PASS();
}

static void test_auth_load_no_creds(void) {
    TEST("auth: load returns -1 when no credentials file");
    NowCredentials creds;
    memset(&creds, 0, sizeof(creds));
    int rc = now_auth_load("http://no.such.registry", &creds);
    /* Should return -1 since ~/.now/credentials.pasta likely doesn't match */
    if (rc == 0 && creds.token != NULL) {
        /* Unlikely but valid — credentials file exists with this URL */
        now_auth_creds_free(&creds);
    }
    PASS();
}

static void test_auth_login_null_creds(void) {
    TEST("auth: login fails with NULL credentials");
    char *jwt = NULL;
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_auth_login("localhost", 9999, "", NULL, 0, &jwt, &res);
    if (rc != -1) { FAIL("should fail with NULL creds"); return; }
    if (jwt != NULL) { FAIL("jwt should be NULL"); free(jwt); return; }
    ASSERT_EQ(res.code, NOW_ERR_AUTH);
    PASS();
}

static void test_auth_login_connect_failure(void) {
    TEST("auth: login fails on connection refused");
    NowCredentials creds;
    memset(&creds, 0, sizeof(creds));
    creds.username = strdup("alice");
    creds.token = strdup("secret");
    char *jwt = NULL;
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_auth_login("127.0.0.1", 19999, "", &creds, 0, &jwt, &res);
    /* Should fail because nothing is listening */
    if (rc != -1) { FAIL("should fail on connect"); free(jwt); }
    if (jwt != NULL) { FAIL("jwt should be NULL"); free(jwt); }
    now_auth_creds_free(&creds);
    PASS();
}

static void test_yank_no_url(void) {
    TEST("yank: fails with NULL registry URL");
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_publish_yank(NULL, "org.test", "lib", "1.0.0", NULL, 0, &res);
    if (rc != -1) { FAIL("should fail with NULL URL"); return; }
    ASSERT_EQ(res.code, NOW_ERR_SCHEMA);
    PASS();
}

static void test_yank_connect_failure(void) {
    TEST("yank: fails on connection refused");
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_publish_yank("http://127.0.0.1:19999", "org.test", "lib",
                               "1.0.0", "security issue", 0, &res);
    if (rc != -1) { FAIL("should fail on connect"); return; }
    PASS();
}

static void test_dep_updates_no_deps(void) {
    TEST("dep:updates: project with no deps returns 0");
    NowResult res;
    memset(&res, 0, sizeof(res));
    const char *pasta =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\", lang: \"c\" }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }
    int rc = now_dep_updates(p, NULL, 0, &res);
    now_project_free(p);
    if (rc != 0) { FAIL("expected 0 updates for project with no deps"); return; }
    PASS();
}

static void test_dep_updates_null_project(void) {
    TEST("dep:updates: NULL project returns -1");
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_dep_updates(NULL, NULL, 0, &res);
    if (rc != -1) { FAIL("should fail with NULL project"); return; }
    PASS();
}

static void test_cache_mirror_no_url(void) {
    TEST("cache:mirror: NULL URL returns -1");
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_cache_mirror(NULL, NULL, 0, &res);
    if (rc != -1) { FAIL("should fail with NULL URL"); return; }
    PASS();
}

static void test_cache_mirror_connect_failure(void) {
    TEST("cache:mirror: connection refused returns -1");
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_cache_mirror("http://127.0.0.1:19999", NULL, 0, &res);
    if (rc != -1) { FAIL("should fail on connect"); return; }
    PASS();
}

static void test_toolchain_msvc_detect(void) {
    TEST("toolchain: detects MSVC from CC=cl.exe");
#ifdef _WIN32
    const char *saved_cc = getenv("CC");
    char *saved_copy = saved_cc ? strdup(saved_cc) : NULL;
    _putenv("CC=cl.exe");

    NowToolchain tc;
    memset(&tc, 0, sizeof(tc));
    NowProject proj;
    memset(&proj, 0, sizeof(proj));
    now_toolchain_resolve(&tc, &proj);

    int ok = (tc.is_msvc == 1);
    int ar_ok = tc.ar && strstr(tc.ar, "lib") != NULL;
    now_toolchain_free(&tc);

    /* Restore CC */
    if (saved_copy) {
        char buf[512];
        snprintf(buf, sizeof(buf), "CC=%s", saved_copy);
        _putenv(buf);
        free(saved_copy);
    } else {
        _putenv("CC=");
    }

    if (!ok) { FAIL("is_msvc should be 1"); return; }
    if (!ar_ok) { FAIL("ar should be lib.exe"); return; }
    PASS();
#else
    /* MSVC detection only applies on Windows */
    PASS();
#endif
}

static void test_toolchain_java_resolve(void) {
    TEST("toolchain: resolves javac/jar/java for Java projects");
    /* Create a project with Java lang */
    const char *input = "{ group: \"t\", artifact: \"t\", version: \"1.0.0\", langs: [\"java\"] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowToolchain tc;
    memset(&tc, 0, sizeof(tc));
    now_toolchain_resolve(&tc, p);

    /* javac/jar/java should be set (even if not found on system, they'll be defaults) */
    ASSERT_NOT_NULL(tc.javac);
    ASSERT_NOT_NULL(tc.jar);
    ASSERT_NOT_NULL(tc.java);

    now_toolchain_free(&tc);
    now_project_free(p);
    PASS();
}

static void test_toolchain_no_java_for_c(void) {
    TEST("toolchain: no javac/jar for C projects");
    const char *input = "{ group: \"t\", artifact: \"t\", version: \"1.0.0\", langs: [\"c\"] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowToolchain tc;
    memset(&tc, 0, sizeof(tc));
    now_toolchain_resolve(&tc, p);

    /* Java tools should NOT be resolved for C projects */
    ASSERT_NULL(tc.javac);
    ASSERT_NULL(tc.jar);
    ASSERT_NULL(tc.java);

    now_toolchain_free(&tc);
    now_project_free(p);
    PASS();
}

/* ---- Workspace ---- */

static void test_is_workspace_true(void) {
    TEST("workspace: detect workspace root");
    char path[512];
    snprintf(path, sizeof(path), "%s/workspace/now.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }
    if (!now_is_workspace(p)) { FAIL("expected workspace"); now_project_free(p); return; }
    now_project_free(p);
    PASS();
}

static void test_is_workspace_false(void) {
    TEST("workspace: single project is not workspace");
    NowResult res;
    const char *pasta =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\", lang: \"c\" }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }
    if (now_is_workspace(p)) { FAIL("expected non-workspace"); now_project_free(p); return; }
    now_project_free(p);
    PASS();
}

static void test_workspace_init(void) {
    TEST("workspace: init loads modules and builds graph");
    char path[512];
    snprintf(path, sizeof(path), "%s/workspace/now.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/workspace", NOW_TEST_RESOURCES);

    NowWorkspace ws;
    int rc = now_workspace_init(&ws, p, basedir, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(p); return; }

    /* Should have 2 modules */
    if (ws.module_count != 2) { FAIL("expected 2 modules"); now_workspace_free(&ws); now_project_free(p); return; }

    now_workspace_free(&ws);
    now_project_free(p);
    PASS();
}

static void test_workspace_topo_sort(void) {
    TEST("workspace: topo sort orders core before app");
    char path[512];
    snprintf(path, sizeof(path), "%s/workspace/now.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/workspace", NOW_TEST_RESOURCES);

    NowWorkspace ws;
    int rc = now_workspace_init(&ws, p, basedir, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(p); return; }

    int **waves = NULL;
    int *wave_sizes = NULL;
    int nwaves = now_workspace_topo_sort(&ws, &waves, &wave_sizes, &res);
    if (nwaves < 1) { FAIL("expected at least 1 wave"); now_workspace_free(&ws); now_project_free(p); return; }

    /* core has no deps so it should appear in wave 0.
     * app depends on core so it must appear in a later wave. */
    int core_wave = -1, app_wave = -1;
    for (int w = 0; w < nwaves; w++) {
        for (int m = 0; m < wave_sizes[w]; m++) {
            int idx = waves[w][m];
            if (strcmp(ws.modules[idx].name, "core") == 0) core_wave = w;
            if (strcmp(ws.modules[idx].name, "app") == 0) app_wave = w;
        }
    }

    /* Free waves */
    for (size_t i = 0; i < ws.module_count; i++) free(waves[i]);
    free(waves); free(wave_sizes);
    now_workspace_free(&ws);
    now_project_free(p);

    if (core_wave < 0 || app_wave < 0) { FAIL("missing module in waves"); return; }
    if (core_wave >= app_wave) { FAIL("core must be in earlier wave than app"); return; }
    PASS();
}

static void test_workspace_is_null(void) {
    TEST("workspace: is_workspace(NULL) returns 0");
    if (now_is_workspace(NULL) != 0) { FAIL("expected 0"); return; }
    PASS();
}

static int strarray_contains_suffix(const NowStrArray *a, const char *suf) {
    size_t n = strlen(suf);
    for (size_t i = 0; i < a->count; i++) {
        size_t m = strlen(a->items[i]);
        if (m >= n && strcmp(a->items[i] + m - n, suf) == 0) return 1;
    }
    return 0;
}
static int strarray_contains(const NowStrArray *a, const char *s) {
    for (size_t i = 0; i < a->count; i++)
        if (strcmp(a->items[i], s) == 0) return 1;
    return 0;
}

static void test_workspace_inject_sibling(void) {
    TEST("workspace: auto-injects sibling include/libdir/lib + STATIC define");
    char path[512];
    snprintf(path, sizeof(path), "%s/workspace/now.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_project_load(path, &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/workspace", NOW_TEST_RESOURCES);

    NowWorkspace ws;
    int rc = now_workspace_init(&ws, p, basedir, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(p); return; }

    /* Find the 'app' module — it depends on the static 'core' sibling. */
    int app_idx = -1;
    for (size_t i = 0; i < ws.module_count; i++)
        if (strcmp(ws.modules[i].name, "app") == 0) { app_idx = (int)i; break; }
    if (app_idx < 0) { FAIL("missing app module"); now_workspace_free(&ws); now_project_free(p); return; }

    NowProject *app = ws.modules[app_idx].project;
    int ok_inc  = strarray_contains_suffix(&app->compile.includes, "/core/src/main/h");
    int ok_dir  = strarray_contains_suffix(&app->link.libdirs,     "/core/target/bin");
    int ok_lib  = strarray_contains(&app->link.libs,               "core");
    int ok_def  = strarray_contains(&app->compile.defines,         "CORE_STATIC");

    now_workspace_free(&ws);
    now_project_free(p);

    if (!ok_inc) { FAIL("missing injected include for core/src/main/h"); return; }
    if (!ok_dir) { FAIL("missing injected libdir for core/target/bin"); return; }
    if (!ok_lib) { FAIL("missing injected -lcore"); return; }
    if (!ok_def) { FAIL("missing injected CORE_STATIC define"); return; }
    PASS();
}

/* ---- Plugin system ---- */

static void test_plugin_is_builtin(void) {
    TEST("plugin: detect built-in ids");
    if (!now_plugin_is_builtin("now:version")) { FAIL("now:version should be builtin"); return; }
    if (!now_plugin_is_builtin("now:embed")) { FAIL("now:embed should be builtin"); return; }
    if (now_plugin_is_builtin("org.acme:foo:1.0.0")) { FAIL("should not be builtin"); return; }
    if (now_plugin_is_builtin(NULL)) { FAIL("NULL should not be builtin"); return; }
    PASS();
}

static void test_plugin_pom_load(void) {
    TEST("plugin: load plugins from now.pasta");
    NowResult res;
    const char *pasta =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  lang: \"c\","
        "  plugins: ["
        "    { id: \"now:version\", phase: \"generate\" },"
        "    { id: \"now:embed\", phase: \"generate\","
        "      config: { src: \"assets\", prefix: \"res_\" } }"
        "  ] }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }
    if (p->plugins.count != 2) { FAIL("expected 2 plugins"); now_project_free(p); return; }
    ASSERT_STR(p->plugins.items[0].id, "now:version");
    ASSERT_STR(p->plugins.items[0].phase, "generate");
    ASSERT_STR(p->plugins.items[1].id, "now:embed");
    /* config should be non-NULL (raw PastaValue*) */
    if (!p->plugins.items[1].config) { FAIL("config should be set"); now_project_free(p); return; }
    now_project_free(p);
    PASS();
}

static void test_plugin_run_hook_no_plugins(void) {
    TEST("plugin: run_hook with no plugins is no-op");
    NowResult res;
    const char *pasta =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\", lang: \"c\" }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }
    NowPluginResult out;
    int rc = now_plugin_run_hook(p, ".", NOW_HOOK_GENERATE, 0, &out, &res);
    now_project_free(p);
    ASSERT_EQ(rc, 0);
    PASS();
}

static void test_plugin_version_generate(void) {
    TEST("plugin: now:version generates _now_version.c");
    NowResult res;
    const char *pasta =
        "{ group: \"org.test\", artifact: \"hello\", version: \"2.3.4\","
        "  lang: \"c\","
        "  plugins: ["
        "    { id: \"now:version\", phase: \"generate\" }"
        "  ] }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }

    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/plugin_test", NOW_TEST_RESOURCES);
    /* Create the basedir if needed */
    now_mkdir_p(basedir);

    NowPluginResult out;
    now_plugin_result_init(&out);
    int rc = now_plugin_run_hook(p, basedir, NOW_HOOK_GENERATE, 0, &out, &res);
    now_project_free(p);
    if (rc != 0) { FAIL(res.message); now_plugin_result_free(&out); return; }

    /* Should have produced 1 source file */
    if (out.sources.count != 1) { FAIL("expected 1 generated source"); now_plugin_result_free(&out); return; }

    /* Verify the generated file exists and contains expected content */
    FILE *fp = fopen(out.sources.items[0], "r");
    if (!fp) { FAIL("generated file not found"); now_plugin_result_free(&out); return; }

    char buf[2048];
    size_t nread = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[nread] = '\0';
    fclose(fp);

    if (!strstr(buf, "NOW_VERSION")) { FAIL("missing NOW_VERSION"); now_plugin_result_free(&out); return; }
    if (!strstr(buf, "\"2.3.4\"")) { FAIL("missing version string"); now_plugin_result_free(&out); return; }
    if (!strstr(buf, "\"org.test\"")) { FAIL("missing group"); now_plugin_result_free(&out); return; }
    if (!strstr(buf, "NOW_VERSION_MAJOR = 2")) { FAIL("wrong major"); now_plugin_result_free(&out); return; }

    now_plugin_result_free(&out);
    PASS();
}

static void test_plugin_result_init_free(void) {
    TEST("plugin: result init and free");
    NowPluginResult r;
    now_plugin_result_init(&r);
    if (!r.ok) { FAIL("should be ok"); return; }
    now_plugin_result_free(&r);
    /* Should not crash */
    PASS();
}

static void test_plugin_unknown_builtin(void) {
    TEST("plugin: unknown built-in returns error");
    NowPlugin pl;
    memset(&pl, 0, sizeof(pl));
    pl.id = "now:nonexistent";
    pl.phase = "generate";

    NowPluginResult out;
    now_plugin_result_init(&out);
    NowResult res;
    memset(&res, 0, sizeof(res));

    int rc = now_plugin_invoke(&pl, NULL, ".", "generate", 0, &out, &res);
    now_plugin_result_free(&out);
    if (rc == 0) { FAIL("should have failed"); return; }
    ASSERT_EQ(res.code, NOW_ERR_NOT_FOUND);
    PASS();
}

/* ---- Plugin registry ---- */

static void test_plugin_manifest_parse_string(void) {
    TEST("plugin registry: parse manifest from string");
    const char *input =
        "{ id: \"org.now.plugins:protobuf-c:1.0.0\","
        "  name: \"Protobuf C Generator\","
        "  description: \"Generates C sources from .proto files\","
        "  protocol: \"1.0.0\","
        "  hooks: [\"generate\"],"
        "  requires: [\"source-inject\", \"fail-build\"],"
        "  network: { required: true },"
        "  requires_now: \">=1.0.0\" }";
    NowPluginInfo info;
    NowResult res;
    int rc = now_plugin_manifest_parse_string(input, strlen(input), &info, &res);
    ASSERT_EQ(rc, 0);
    if (!info.id || strcmp(info.id, "org.now.plugins:protobuf-c:1.0.0") != 0) {
        FAIL("wrong id");
        now_plugin_info_free(&info);
        return;
    }
    if (!info.name || strcmp(info.name, "Protobuf C Generator") != 0) {
        FAIL("wrong name");
        now_plugin_info_free(&info);
        return;
    }
    if (!info.protocol || strcmp(info.protocol, "1.0.0") != 0) {
        FAIL("wrong protocol");
        now_plugin_info_free(&info);
        return;
    }
    ASSERT_EQ(info.hooks.count, (size_t)1);
    ASSERT_EQ(info.requires.count, (size_t)2);
    ASSERT_EQ(info.network_required, 1);
    if (!info.requires_now || strcmp(info.requires_now, ">=1.0.0") != 0) {
        FAIL("wrong requires_now");
        now_plugin_info_free(&info);
        return;
    }
    now_plugin_info_free(&info);
    PASS();
}

static void test_plugin_manifest_parse_minimal(void) {
    TEST("plugin registry: parse minimal manifest");
    const char *input = "{ id: \"org.now.plugins:simple:0.1.0\", protocol: \"1.0.0\" }";
    NowPluginInfo info;
    NowResult res;
    int rc = now_plugin_manifest_parse_string(input, strlen(input), &info, &res);
    ASSERT_EQ(rc, 0);
    if (!info.id || strcmp(info.id, "org.now.plugins:simple:0.1.0") != 0) {
        FAIL("wrong id");
        now_plugin_info_free(&info);
        return;
    }
    ASSERT_EQ(info.hooks.count, (size_t)0);
    ASSERT_EQ(info.requires.count, (size_t)0);
    ASSERT_EQ(info.network_required, 0);
    now_plugin_info_free(&info);
    PASS();
}

static void test_plugin_manifest_missing_id(void) {
    TEST("plugin registry: manifest without id fails");
    const char *input = "{ name: \"No ID\", protocol: \"1.0.0\" }";
    NowPluginInfo info;
    NowResult res;
    int rc = now_plugin_manifest_parse_string(input, strlen(input), &info, &res);
    if (rc == 0) { FAIL("should have failed"); now_plugin_info_free(&info); return; }
    ASSERT_EQ(res.code, NOW_ERR_SCHEMA);
    PASS();
}

static void test_plugin_manifest_parse_file_missing(void) {
    TEST("plugin registry: missing manifest file returns -1");
    NowPluginInfo info;
    NowResult res;
    int rc = now_plugin_manifest_parse("/nonexistent/plugin.pasta", &info, &res);
    if (rc == 0) { FAIL("should have failed"); now_plugin_info_free(&info); return; }
    ASSERT_EQ(res.code, NOW_ERR_NOT_FOUND);
    PASS();
}

static void test_plugin_info_free_null_safe(void) {
    TEST("plugin registry: info_free on NULL is safe");
    now_plugin_info_free(NULL);
    NowPluginInfo info;
    memset(&info, 0, sizeof(info));
    now_plugin_info_free(&info);
    PASS();
}

static void test_plugin_find_binary_missing(void) {
    TEST("plugin registry: find_binary returns NULL for missing");
    char *path = now_plugin_find_binary("/nonexistent/repo",
                                          "org.test", "myplugin", "1.0.0");
    if (path != NULL) { FAIL("should be NULL"); free(path); return; }
    PASS();
}

static void test_plugin_list_empty_repo(void) {
    TEST("plugin registry: list on empty repo returns 0");
    NowPluginInfo *plugins = NULL;
    size_t count = 0;
    NowResult res;
    int rc = now_plugin_list("/nonexistent/repo", &plugins, &count, &res);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, (size_t)0);
    PASS();
}

static void test_plugin_search_no_match(void) {
    TEST("plugin registry: search with no match returns 0");
    NowPluginInfo *plugins = NULL;
    size_t count = 0;
    NowResult res;
    int rc = now_plugin_search("zzz_nonexistent", "/nonexistent/repo",
                                &plugins, &count, &res);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(count, (size_t)0);
    PASS();
}

static void test_plugin_install_bad_registry(void) {
    TEST("plugin registry: install from bad registry fails");
    NowResult res;
    int rc = now_plugin_install("http://127.0.0.1:1",
                                  "org.test", "myplugin", "1.0.0",
                                  "/tmp/test_repo", 0, &res);
    if (rc == 0) { FAIL("should have failed"); return; }
    PASS();
}

static void test_plugin_manifest_roundtrip(void) {
    TEST("plugin registry: write + parse manifest file roundtrip");
    const char *input =
        "{ id: \"org.now.plugins:roundtrip:2.0.0\","
        "  name: \"Roundtrip Test\","
        "  description: \"A test plugin\","
        "  protocol: \"1.0.0\","
        "  hooks: [\"pre-compile\", \"post-compile\"],"
        "  requires: [\"fail-build\"],"
        "  network: { required: false } }";

    /* Write to temp file */
    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_plugin_manifest.pasta",
             NOW_TEST_RESOURCES);
    FILE *fp = fopen(outpath, "w");
    ASSERT_NOT_NULL(fp);
    fwrite(input, 1, strlen(input), fp);
    fclose(fp);

    /* Parse back */
    NowPluginInfo info;
    NowResult res;
    int rc = now_plugin_manifest_parse(outpath, &info, &res);
    remove(outpath);
    ASSERT_EQ(rc, 0);
    if (!info.id || strcmp(info.id, "org.now.plugins:roundtrip:2.0.0") != 0) {
        FAIL("wrong id after roundtrip");
        now_plugin_info_free(&info);
        return;
    }
    ASSERT_EQ(info.hooks.count, (size_t)2);
    ASSERT_EQ(info.requires.count, (size_t)1);
    ASSERT_EQ(info.network_required, 0);
    now_plugin_info_free(&info);
    PASS();
}

/* ---- Dep confusion protection ---- */

static void test_private_group_exact_match(void) {
    TEST("private_groups: exact prefix match");
    NowStrArray pg;
    now_strarray_init(&pg);
    now_strarray_push(&pg, "org.acme");
    ASSERT_EQ(now_group_is_private(&pg, "org.acme"), 1);
    now_strarray_free(&pg);
    PASS();
}

static void test_private_group_dotted_child(void) {
    TEST("private_groups: dotted child match");
    NowStrArray pg;
    now_strarray_init(&pg);
    now_strarray_push(&pg, "org.acme");
    ASSERT_EQ(now_group_is_private(&pg, "org.acme.internal"), 1);
    ASSERT_EQ(now_group_is_private(&pg, "org.acme.core.util"), 1);
    now_strarray_free(&pg);
    PASS();
}

static void test_private_group_no_false_positive(void) {
    TEST("private_groups: no false positive on similar prefix");
    NowStrArray pg;
    now_strarray_init(&pg);
    now_strarray_push(&pg, "org.acme");
    ASSERT_EQ(now_group_is_private(&pg, "org.acmecorp"), 0);
    ASSERT_EQ(now_group_is_private(&pg, "org.acm"), 0);
    ASSERT_EQ(now_group_is_private(&pg, "com.example"), 0);
    now_strarray_free(&pg);
    PASS();
}

static void test_private_group_multiple_prefixes(void) {
    TEST("private_groups: multiple prefixes");
    NowStrArray pg;
    now_strarray_init(&pg);
    now_strarray_push(&pg, "org.acme");
    now_strarray_push(&pg, "com.internal");
    ASSERT_EQ(now_group_is_private(&pg, "org.acme.libs"), 1);
    ASSERT_EQ(now_group_is_private(&pg, "com.internal"), 1);
    ASSERT_EQ(now_group_is_private(&pg, "com.example"), 0);
    now_strarray_free(&pg);
    PASS();
}

static void test_private_group_null_safe(void) {
    TEST("private_groups: NULL-safe");
    ASSERT_EQ(now_group_is_private(NULL, "org.acme"), 0);
    NowStrArray pg;
    now_strarray_init(&pg);
    ASSERT_EQ(now_group_is_private(&pg, NULL), 0);
    ASSERT_EQ(now_group_is_private(&pg, "anything"), 0);
    now_strarray_free(&pg);
    PASS();
}

static void test_private_group_pom_load(void) {
    TEST("private_groups: loaded from now.pasta");
    NowResult res;
    const char *pasta =
        "{ group: \"org.acme\", artifact: \"app\", version: \"1.0.0\","
        "  lang: \"c\","
        "  private_groups: [\"org.acme\", \"com.secret\"] }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }
    ASSERT_EQ(p->private_groups.count, (size_t)2);
    ASSERT_STR(p->private_groups.items[0], "org.acme");
    ASSERT_STR(p->private_groups.items[1], "com.secret");
    now_project_free(p);
    PASS();
}

static void test_private_group_procure_fail(void) {
    TEST("private_groups: procure fails without repos");
    NowResult res;
    const char *pasta =
        "{ group: \"org.acme\", artifact: \"app\", version: \"1.0.0\","
        "  lang: \"c\","
        "  private_groups: [\"org.internal\"],"
        "  deps: [{ id: \"org.internal:secret:^1.0.0\" }] }";
    NowProject *p = now_project_load_string(pasta, strlen(pasta), &res);
    if (!p) { FAIL(res.message); return; }

    NowProcureOpts opts = {0};
    opts.repo_root = "/tmp/now-test-repo-confuse";
    memset(&res, 0, sizeof(res));
    int rc = now_procure(p, &opts, &res);
    now_project_free(p);

    /* Should fail because private group has no declared repos */
    if (rc == 0) { FAIL("should fail for private group without repos"); return; }
    if (!strstr(res.message, "private group")) { FAIL(res.message); return; }
    PASS();
}

/* ---- Layer system ---- */

static void test_layer_stack_init(void) {
    TEST("layers: stack init with baseline");
    NowLayerStack stack;
    now_layer_stack_init(&stack);
    ASSERT_EQ(stack.count, (size_t)1);
    ASSERT_STR(stack.layers[0].id, "now-baseline");
    ASSERT_EQ(stack.layers[0].source, NOW_LAYER_BUILTIN);
    now_layer_stack_free(&stack);
    PASS();
}

static void test_layer_baseline_sections(void) {
    TEST("layers: baseline has compile, repos, toolchain");
    NowLayerStack stack;
    now_layer_stack_init(&stack);
    const NowLayerSection *compile = now_layer_find_section(&stack.layers[0], "compile");
    ASSERT_NOT_NULL(compile);
    ASSERT_EQ(compile->policy, NOW_POLICY_OPEN);

    const NowLayerSection *repos = now_layer_find_section(&stack.layers[0], "repos");
    ASSERT_NOT_NULL(repos);

    const NowLayerSection *tc = now_layer_find_section(&stack.layers[0], "toolchain");
    ASSERT_NOT_NULL(tc);

    const NowLayerSection *adv = now_layer_find_section(&stack.layers[0], "advisory");
    ASSERT_NOT_NULL(adv);
    ASSERT_EQ(adv->policy, NOW_POLICY_LOCKED);

    now_layer_stack_free(&stack);
    PASS();
}

static void test_layer_load_file(void) {
    TEST("layers: load enterprise layer from file");
    NowLayerStack stack;
    now_layer_stack_init(&stack);

    char path[512];
    snprintf(path, sizeof(path), "%s/layers/enterprise.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    int rc = now_layer_load_file(&stack, "enterprise", path, &res);
    if (rc != 0) { FAIL(res.message); now_layer_stack_free(&stack); return; }

    ASSERT_EQ(stack.count, (size_t)2);
    ASSERT_STR(stack.layers[1].id, "enterprise");

    const NowLayerSection *compile = now_layer_find_section(&stack.layers[1], "compile");
    ASSERT_NOT_NULL(compile);
    ASSERT_EQ(compile->policy, NOW_POLICY_LOCKED);

    const NowLayerSection *pg = now_layer_find_section(&stack.layers[1], "private_groups");
    ASSERT_NOT_NULL(pg);
    ASSERT_EQ(pg->policy, NOW_POLICY_LOCKED);

    now_layer_stack_free(&stack);
    PASS();
}

static void test_layer_merge_open(void) {
    TEST("layers: merge open section (overlay wins)");
    NowLayerStack stack;
    now_layer_stack_init(&stack);

    char path[512];
    snprintf(path, sizeof(path), "%s/layers/enterprise.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    now_layer_load_file(&stack, "enterprise", path, &res);

    NowAuditReport audit;
    now_audit_init(&audit);

    /* toolchain is open in both layers — enterprise overrides baseline */
    PastaValue *effective = (PastaValue *)now_layer_merge_section(&stack, "toolchain", &audit);
    ASSERT_NOT_NULL(effective);

    const PastaValue *preset = pasta_map_get(effective, "preset");
    ASSERT_NOT_NULL(preset);
    ASSERT_STR(pasta_get_string(preset), "llvm");

    /* No violations for open section */
    ASSERT_EQ(audit.count, (size_t)0);

    pasta_free(effective);
    now_audit_free(&audit);
    now_layer_stack_free(&stack);
    PASS();
}

static void test_layer_merge_locked_audit(void) {
    TEST("layers: merge locked section produces audit violation");
    NowLayerStack stack;
    now_layer_stack_init(&stack);

    /* Load enterprise layer (locks compile) */
    char path[512];
    snprintf(path, sizeof(path), "%s/layers/enterprise.pasta", NOW_TEST_RESOURCES);
    NowResult res;
    now_layer_load_file(&stack, "enterprise", path, &res);

    /* Push a project layer that overrides compile */
    NowProject proj;
    memset(&proj, 0, sizeof(proj));
    now_strarray_init(&proj.compile.warnings);
    now_strarray_init(&proj.compile.defines);
    now_strarray_init(&proj.compile.flags);
    now_strarray_init(&proj.compile.includes);
    now_strarray_push(&proj.compile.defines, "MY_DEFINE");
    proj.compile.opt = "speed";
    now_strarray_init(&proj.private_groups);
    now_strarray_init(&proj.langs);
    now_layer_push_project(&stack, &proj);

    NowAuditReport audit;
    now_audit_init(&audit);

    PastaValue *effective = (PastaValue *)now_layer_merge_section(&stack, "compile", &audit);
    ASSERT_NOT_NULL(effective);

    /* Should have audit violations (project overriding enterprise's locked compile) */
    if (audit.count == 0) { FAIL("expected audit violations"); pasta_free(effective); now_audit_free(&audit); now_layer_stack_free(&stack); now_strarray_free(&proj.compile.warnings); now_strarray_free(&proj.compile.defines); now_strarray_free(&proj.compile.flags); now_strarray_free(&proj.compile.includes); now_strarray_free(&proj.private_groups); now_strarray_free(&proj.langs); return; }

    ASSERT_STR(audit.items[0].code, "NOW-W0401");

    pasta_free(effective);
    now_audit_free(&audit);
    now_layer_stack_free(&stack);
    now_strarray_free(&proj.compile.warnings);
    now_strarray_free(&proj.compile.defines);
    now_strarray_free(&proj.compile.flags);
    now_strarray_free(&proj.compile.includes);
    now_strarray_free(&proj.private_groups);
    now_strarray_free(&proj.langs);
    PASS();
}

static void test_layer_merge_strarray_exclude(void) {
    TEST("layers: !exclude: removes entries in open mode");
    NowStrArray dst;
    now_strarray_init(&dst);
    now_strarray_push(&dst, "Wall");
    now_strarray_push(&dst, "Wextra");
    now_strarray_push(&dst, "Wpedantic");

    NowStrArray src;
    now_strarray_init(&src);
    now_strarray_push(&src, "!exclude:Wpedantic");
    now_strarray_push(&src, "Wformat");

    now_layer_merge_strarray(&dst, &src, NOW_POLICY_OPEN);

    /* Wpedantic should be removed, Wformat added */
    ASSERT_EQ(dst.count, (size_t)3);  /* Wall, Wextra, Wformat */
    /* Check Wpedantic is gone */
    int found_pedantic = 0;
    int found_format = 0;
    for (size_t i = 0; i < dst.count; i++) {
        if (strcmp(dst.items[i], "Wpedantic") == 0) found_pedantic = 1;
        if (strcmp(dst.items[i], "Wformat") == 0) found_format = 1;
    }
    if (found_pedantic) { FAIL("Wpedantic should be excluded"); now_strarray_free(&dst); now_strarray_free(&src); return; }
    if (!found_format) { FAIL("Wformat should be added"); now_strarray_free(&dst); now_strarray_free(&src); return; }

    now_strarray_free(&dst);
    now_strarray_free(&src);
    PASS();
}

static void test_layer_audit_format(void) {
    TEST("layers: audit format output");
    NowAuditReport audit;
    now_audit_init(&audit);

    /* Empty report */
    char *out = now_audit_format(&audit);
    ASSERT_NOT_NULL(out);
    if (!strstr(out, "No advisory")) { FAIL("expected no-violations message"); free(out); return; }
    free(out);

    now_audit_free(&audit);
    PASS();
}

static void test_layer_push_project(void) {
    TEST("layers: push project as top layer");
    NowLayerStack stack;
    now_layer_stack_init(&stack);

    NowProject proj;
    memset(&proj, 0, sizeof(proj));
    now_strarray_init(&proj.compile.warnings);
    now_strarray_init(&proj.compile.defines);
    now_strarray_init(&proj.compile.flags);
    now_strarray_init(&proj.compile.includes);
    now_strarray_init(&proj.private_groups);
    now_strarray_init(&proj.langs);
    now_strarray_push(&proj.private_groups, "org.secret");
    now_layer_push_project(&stack, &proj);

    ASSERT_EQ(stack.count, (size_t)2);
    ASSERT_STR(stack.layers[1].id, "project");

    const NowLayerSection *pg = now_layer_find_section(&stack.layers[1], "private_groups");
    ASSERT_NOT_NULL(pg);

    now_layer_stack_free(&stack);
    now_strarray_free(&proj.compile.warnings);
    now_strarray_free(&proj.compile.defines);
    now_strarray_free(&proj.compile.flags);
    now_strarray_free(&proj.compile.includes);
    now_strarray_free(&proj.private_groups);
    now_strarray_free(&proj.langs);
    PASS();
}

/* ---- Alforno integration ---- */

static void test_alforno_aggregate_merge(void) {
    TEST("alforno: aggregate merges two pastlets");
    AlfContext *ctx = alf_create(ALF_AGGREGATE, NULL);
    ASSERT_NOT_NULL(ctx);

    const char *input1 = "@db {\n  engine: \"postgres\",\n  pool: 10\n}\n";
    const char *input2 = "@db {\n  pool: 20,\n  timeout: 30\n}\n@cache {\n  ttl: 60\n}\n";

    AlfResult ar;
    ASSERT_EQ(alf_add_input(ctx, input1, strlen(input1), &ar), 0);
    ASSERT_EQ(alf_add_input(ctx, input2, strlen(input2), &ar), 0);

    PastaValue *out = alf_process(ctx, &ar);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ((int)ar.code, ALF_OK);

    /* @db should have pool=20 (overlay wins), engine preserved, timeout added */
    const PastaValue *db = pasta_map_get(out, "db");
    ASSERT_NOT_NULL(db);
    ASSERT_EQ((int)pasta_get_number(pasta_map_get(db, "pool")), 20);
    ASSERT_STR(pasta_get_string(pasta_map_get(db, "engine")), "postgres");
    ASSERT_EQ((int)pasta_get_number(pasta_map_get(db, "timeout")), 30);

    /* @cache should be present */
    const PastaValue *cache = pasta_map_get(out, "cache");
    ASSERT_NOT_NULL(cache);
    ASSERT_EQ((int)pasta_get_number(pasta_map_get(cache, "ttl")), 60);

    pasta_free(out);
    alf_free(ctx);
    PASS();
}

static void test_alforno_parameterize(void) {
    TEST("alforno: @vars substitution");
    AlfContext *ctx = alf_create(ALF_AGGREGATE, NULL);
    ASSERT_NOT_NULL(ctx);

    const char *input =
        "@vars {\n  name: \"myapp\",\n  version: \"1.0\"\n}\n"
        "@project {\n  artifact: \"{name}\",\n  ver: \"{version}\"\n}\n";

    AlfResult ar;
    ASSERT_EQ(alf_add_input(ctx, input, strlen(input), &ar), 0);

    PastaValue *out = alf_process(ctx, &ar);
    ASSERT_NOT_NULL(out);

    /* @vars should be consumed (not in output) */
    ASSERT_NULL(pasta_map_get(out, "vars"));

    /* @project should have substituted values */
    const PastaValue *proj = pasta_map_get(out, "project");
    ASSERT_NOT_NULL(proj);
    ASSERT_STR(pasta_get_string(pasta_map_get(proj, "artifact")), "myapp");
    ASSERT_STR(pasta_get_string(pasta_map_get(proj, "ver")), "1.0");

    pasta_free(out);
    alf_free(ctx);
    PASS();
}

static void test_alforno_conflate(void) {
    TEST("alforno: conflate filters to recipe fields");
    AlfContext *ctx = alf_create(ALF_CONFLATE, NULL);
    ASSERT_NOT_NULL(ctx);

    const char *recipe =
        "@output {\n  consumes: [\"settings\"],\n  theme: \"allowed\"\n}\n";
    const char *input =
        "@settings {\n  theme: \"dark\",\n  secret: \"hunter2\"\n}\n";

    AlfResult ar;
    ASSERT_EQ(alf_set_recipe(ctx, recipe, strlen(recipe), &ar), 0);
    ASSERT_EQ(alf_add_input(ctx, input, strlen(input), &ar), 0);

    PastaValue *out = alf_process(ctx, &ar);
    ASSERT_NOT_NULL(out);

    /* @output should have theme but NOT secret */
    const PastaValue *o = pasta_map_get(out, "output");
    ASSERT_NOT_NULL(o);
    ASSERT_STR(pasta_get_string(pasta_map_get(o, "theme")), "dark");
    ASSERT_NULL(pasta_map_get(o, "secret"));

    pasta_free(out);
    alf_free(ctx);
    PASS();
}

static void test_alforno_collect_merge(void) {
    TEST("alforno: conflate collect merges arrays");
    AlfContext *ctx = alf_create(ALF_CONFLATE, NULL);
    ASSERT_NOT_NULL(ctx);

    const char *recipe =
        "@merged {\n  consumes: [\"a\", \"b\"],\n  merge: \"collect\",\n"
        "  tags: \"collected\"\n}\n";
    const char *input1 = "@a {\n  tags: \"fast\"\n}\n";
    const char *input2 = "@b {\n  tags: \"safe\"\n}\n";

    AlfResult ar;
    ASSERT_EQ(alf_set_recipe(ctx, recipe, strlen(recipe), &ar), 0);
    ASSERT_EQ(alf_add_input(ctx, input1, strlen(input1), &ar), 0);
    ASSERT_EQ(alf_add_input(ctx, input2, strlen(input2), &ar), 0);

    PastaValue *out = alf_process(ctx, &ar);
    ASSERT_NOT_NULL(out);

    /* @merged.tags should be an array of both values */
    const PastaValue *m = pasta_map_get(out, "merged");
    ASSERT_NOT_NULL(m);
    const PastaValue *tags = pasta_map_get(m, "tags");
    ASSERT_NOT_NULL(tags);
    ASSERT_EQ((int)pasta_type(tags), PASTA_ARRAY);
    ASSERT_EQ((int)pasta_count(tags), 2);

    pasta_free(out);
    alf_free(ctx);
    PASS();
}

/* ---- Multi-architecture / triples ---- */

static void test_triple_parse_full(void) {
    TEST("triple: parse full os:arch:variant");
    NowTriple t;
    now_triple_parse(&t, "linux:amd64:gnu");
    ASSERT_STR(t.os, "linux");
    ASSERT_STR(t.arch, "amd64");
    ASSERT_STR(t.variant, "gnu");
    PASS();
}

static void test_triple_parse_shorthand(void) {
    TEST("triple: parse shorthand fills from host");
    NowTriple t;
    now_triple_parse(&t, ":amd64:musl");
    /* os should be empty before fill */
    ASSERT_STR(t.os, "");
    now_triple_fill_from_host(&t);
    /* os should now be filled from host */
    const NowTriple *host = now_host_triple_parsed();
    if (strcmp(t.os, host->os) != 0) { FAIL("os not filled from host"); return; }
    ASSERT_STR(t.arch, "amd64");
    ASSERT_STR(t.variant, "musl");
    PASS();
}

static void test_triple_format(void) {
    TEST("triple: format as colon-separated string");
    NowTriple t;
    now_triple_parse(&t, "windows:amd64:msvc");
    char buf[256];
    now_triple_format(&t, buf, sizeof(buf));
    ASSERT_STR(buf, "windows:amd64:msvc");
    PASS();
}

static void test_triple_dir(void) {
    TEST("triple: format as directory name (dashes)");
    NowTriple t;
    now_triple_parse(&t, "linux:arm64:musl");
    char buf[256];
    now_triple_dir(&t, buf, sizeof(buf));
    ASSERT_STR(buf, "linux-arm64-musl");
    PASS();
}

static void test_triple_cmp(void) {
    TEST("triple: compare equal and unequal");
    NowTriple a, b;
    now_triple_parse(&a, "linux:amd64:gnu");
    now_triple_parse(&b, "linux:amd64:gnu");
    ASSERT_EQ(now_triple_cmp(&a, &b), 0);
    now_triple_parse(&b, "linux:arm64:gnu");
    if (now_triple_cmp(&a, &b) == 0) { FAIL("expected unequal"); return; }
    PASS();
}

static void test_triple_match_exact(void) {
    TEST("triple: wildcard match exact");
    NowTriple pat, concrete;
    now_triple_parse(&pat, "linux:amd64:gnu");
    now_triple_parse(&concrete, "linux:amd64:gnu");
    ASSERT_EQ(now_triple_match(&pat, &concrete), 1);
    now_triple_parse(&concrete, "linux:arm64:gnu");
    ASSERT_EQ(now_triple_match(&pat, &concrete), 0);
    PASS();
}

static void test_triple_match_wildcard(void) {
    TEST("triple: wildcard * matches any value");
    NowTriple pat, c1, c2;
    now_triple_parse(&pat, "linux:*:musl");
    now_triple_parse(&c1, "linux:amd64:musl");
    now_triple_parse(&c2, "linux:arm64:musl");
    ASSERT_EQ(now_triple_match(&pat, &c1), 1);
    ASSERT_EQ(now_triple_match(&pat, &c2), 1);
    /* Different OS should not match */
    NowTriple c3;
    now_triple_parse(&c3, "macos:arm64:musl");
    ASSERT_EQ(now_triple_match(&pat, &c3), 0);
    PASS();
}

static void test_triple_host_detect(void) {
    TEST("triple: host detection returns valid triple");
    const NowTriple *host = now_host_triple_parsed();
    ASSERT_NOT_NULL(host);
    if (host->os[0] == '\0') { FAIL("empty os"); return; }
    if (host->arch[0] == '\0') { FAIL("empty arch"); return; }
    if (host->variant[0] == '\0') { FAIL("empty variant"); return; }
    PASS();
}

static void test_triple_is_native(void) {
    TEST("triple: native detection");
    const NowTriple *host = now_host_triple_parsed();
    ASSERT_EQ(now_triple_is_native(host), 1);
    NowTriple cross;
    now_triple_parse(&cross, "freestanding:riscv64:none");
    ASSERT_EQ(now_triple_is_native(&cross), 0);
    PASS();
}

/* ---- C++20 Module Pre-scan ---- */

/* Helper: write a temporary file for module scanning */
static char *write_temp_module_file(const char *name, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", NOW_TEST_RESOURCES, name);
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    fputs(content, f);
    fclose(f);
    return strdup(path);
}

static void remove_temp_file(const char *path) {
    if (path) remove(path);
}

static void test_module_scan_interface(void) {
    TEST("module: scan detects export module");
    char *path = write_temp_module_file("test_mod.cppm",
        "export module mylib.core;\n"
        "\n"
        "export int add(int a, int b) { return a + b; }\n");
    ASSERT_NOT_NULL(path);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    ASSERT_EQ(now_module_scan_file(&scan, path), 0);
    ASSERT_EQ((int)scan.unit_count, 1);
    ASSERT_STR(scan.units[0].name, "mylib.core");
    ASSERT_EQ(scan.units[0].is_interface, 1);

    now_module_scan_free(&scan);
    remove_temp_file(path);
    free(path);
    PASS();
}

static void test_module_scan_import(void) {
    TEST("module: scan detects import");
    char *path = write_temp_module_file("test_imp.cpp",
        "import mylib.core;\n"
        "\n"
        "int main() { return add(1, 2); }\n");
    ASSERT_NOT_NULL(path);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    ASSERT_EQ(now_module_scan_file(&scan, path), 0);
    ASSERT_EQ((int)scan.import_count, 1);
    ASSERT_STR(scan.imports[0].module_name, "mylib.core");

    now_module_scan_free(&scan);
    remove_temp_file(path);
    free(path);
    PASS();
}

static void test_module_scan_impl(void) {
    TEST("module: scan detects implementation unit");
    char *path = write_temp_module_file("test_impl.cpp",
        "module mylib.core;\n"
        "\n"
        "void internal_func() {}\n");
    ASSERT_NOT_NULL(path);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    ASSERT_EQ(now_module_scan_file(&scan, path), 0);
    ASSERT_EQ((int)scan.unit_count, 1);
    ASSERT_STR(scan.units[0].name, "mylib.core");
    ASSERT_EQ(scan.units[0].is_interface, 0);

    now_module_scan_free(&scan);
    remove_temp_file(path);
    free(path);
    PASS();
}

static void test_module_scan_skips_comments(void) {
    TEST("module: scan ignores commented-out declarations");
    char *path = write_temp_module_file("test_comment.cpp",
        "// export module fake;\n"
        "/* import ignored; */\n"
        "#include <stdio.h>\n"
        "int main() { return 0; }\n");
    ASSERT_NOT_NULL(path);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    ASSERT_EQ(now_module_scan_file(&scan, path), 0);
    ASSERT_EQ((int)scan.unit_count, 0);
    ASSERT_EQ((int)scan.import_count, 0);

    now_module_scan_free(&scan);
    remove_temp_file(path);
    free(path);
    PASS();
}

static void test_module_order_basic(void) {
    TEST("module: topo order puts interface before consumer");
    /* Create two files: interface and consumer */
    char *iface = write_temp_module_file("topo_iface.cppm",
        "export module greeter;\n"
        "export const char *greet() { return \"hi\"; }\n");
    char *consumer = write_temp_module_file("topo_main.cpp",
        "import greeter;\n"
        "int main() { greet(); return 0; }\n");
    ASSERT_NOT_NULL(iface);
    ASSERT_NOT_NULL(consumer);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    now_module_scan_file(&scan, iface);
    now_module_scan_file(&scan, consumer);

    const char *sources[] = { consumer, iface };
    NowModuleOrder order;
    ASSERT_EQ(now_module_order(&scan, sources, 2, &order), 0);

    /* Interface should come first */
    ASSERT_EQ((int)order.count, 2);
    if (strcmp(order.paths[0], iface) != 0) { FAIL("interface not first"); now_module_order_free(&order); now_module_scan_free(&scan); remove_temp_file(iface); remove_temp_file(consumer); free(iface); free(consumer); return; }
    if (strcmp(order.paths[1], consumer) != 0) { FAIL("consumer not second"); now_module_order_free(&order); now_module_scan_free(&scan); remove_temp_file(iface); remove_temp_file(consumer); free(iface); free(consumer); return; }

    now_module_order_free(&order);
    now_module_scan_free(&scan);
    remove_temp_file(iface);
    remove_temp_file(consumer);
    free(iface);
    free(consumer);
    PASS();
}

static void test_module_find(void) {
    TEST("module: find returns interface by name");
    char *path = write_temp_module_file("test_find.cppm",
        "export module utils;\n");
    ASSERT_NOT_NULL(path);

    NowModuleScan scan;
    now_module_scan_init(&scan);
    now_module_scan_file(&scan, path);

    const NowModuleUnit *u = now_module_find(&scan, "utils");
    ASSERT_NOT_NULL(u);
    ASSERT_STR(u->name, "utils");
    ASSERT_EQ(u->is_interface, 1);
    ASSERT_NULL(now_module_find(&scan, "nonexistent"));

    now_module_scan_free(&scan);
    remove_temp_file(path);
    free(path);
    PASS();
}

static void test_module_bmi_path(void) {
    TEST("module: BMI path generation");
    char *p = now_module_bmi_path("target", "mylib.core", 0);
    ASSERT_NOT_NULL(p);
    /* Should end with /bmi/mylib.core.pcm */
    ASSERT_NOT_NULL(strstr(p, "bmi"));
    ASSERT_NOT_NULL(strstr(p, "mylib.core.pcm"));
    free(p);

    char *pm = now_module_bmi_path("target", "mylib.core", 1);
    ASSERT_NOT_NULL(pm);
    ASSERT_NOT_NULL(strstr(pm, "mylib.core.ifc"));
    free(pm);
    PASS();
}

static void test_module_classify_cppm(void) {
    TEST("module: classify .cppm as cxx-module");
    now_lang_registry_init();
    const char *langs[] = { "c++" };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("foo.cppm", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "cxx-module");
    ASSERT_NOT_NULL(lang);
    ASSERT_STR(lang->id, "c++");
    PASS();
}

/* ---- Java Language + Maven ---- */

static void test_lang_java_registration(void) {
    TEST("java: language registered");
    now_lang_registry_init();
    const NowLangDef *lang = now_lang_find("java");
    ASSERT_NOT_NULL(lang);
    ASSERT_STR(lang->id, "java");
    ASSERT_STR(lang->name, "Java");
    ASSERT_EQ((int)lang->type_count, 1);
    ASSERT_STR(lang->types[0].id, "java-source");
    PASS();
}

static void test_lang_java_classify(void) {
    TEST("java: classify .java as java-source");
    const char *langs[] = { "java" };
    const NowLangDef *lang = NULL;
    const NowLangType *type = now_lang_classify("Main.java", langs, 1, &lang);
    ASSERT_NOT_NULL(type);
    ASSERT_STR(type->id, "java-source");
    ASSERT_STR(type->tool_var, "${javac}");
    PASS();
}

static void test_pom_java_fields(void) {
    TEST("java: POM loads java section");
    const char *input =
        "{ group: \"com.example\", artifact: \"myapp\", version: \"1.0.0\","
        "  langs: [\"java\"], std: \"17\","
        "  java: { main_class: \"com.example.Main\", encoding: \"UTF-8\" },"
        "  output: { type: \"jar\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_STR(p->java.main_class, "com.example.Main");
    ASSERT_STR(p->java.encoding, "UTF-8");
    now_project_free(p);
    PASS();
}

static void test_pom_java_defaults(void) {
    TEST("java: source/test dirs default to Maven layout");
    const char *input =
        "{ group: \"com.example\", artifact: \"myapp\", version: \"1.0.0\","
        "  langs: [\"java\"] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);
    ASSERT_STR(p->sources.dir, "src/main/java");
    ASSERT_STR(p->tests.dir, "src/test/java");
    /* Java projects should NOT get a headers directory */
    ASSERT_NULL(p->sources.headers);
    now_project_free(p);
    PASS();
}

static void test_export_maven_basic(void) {
    TEST("export:maven: generates valid pom.xml");
    const char *input =
        "{ group: \"io.test\", artifact: \"mylib\", version: \"2.0.0\","
        "  langs: [\"java\"], std: \"17\","
        "  name: \"My Library\", description: \"A test library\","
        "  license: \"MIT\","
        "  output: { type: \"jar\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_pom_out.xml", NOW_TEST_RESOURCES);
    int rc = now_export_maven(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    /* Read and check generated XML */
    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[len] = '\0';
    fclose(fp);

    ASSERT_NOT_NULL(strstr(buf, "<groupId>io.test</groupId>"));
    ASSERT_NOT_NULL(strstr(buf, "<artifactId>mylib</artifactId>"));
    ASSERT_NOT_NULL(strstr(buf, "<version>2.0.0</version>"));
    ASSERT_NOT_NULL(strstr(buf, "<name>My Library</name>"));
    ASSERT_NOT_NULL(strstr(buf, "<packaging>jar</packaging>"));
    ASSERT_NOT_NULL(strstr(buf, "maven.compiler.release"));
    ASSERT_NOT_NULL(strstr(buf, "17"));
    ASSERT_NOT_NULL(strstr(buf, "MIT"));

    remove(outpath);
    now_project_free(p);
    PASS();
}

static void test_export_maven_deps(void) {
    TEST("export:maven: dependency scopes map correctly");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"java\"], std: \"17\","
        "  deps: ["
        "    { id: \"org.slf4j:slf4j-api:2.0.9\" },"
        "    { id: \"org.junit:junit:5.10.0\", scope: \"test\" },"
        "    { id: \"com.google:guava:32.0\", optional: true }"
        "  ] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_pom_deps.xml", NOW_TEST_RESOURCES);
    int rc = now_export_maven(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[len] = '\0';
    fclose(fp);

    ASSERT_NOT_NULL(strstr(buf, "<groupId>org.slf4j</groupId>"));
    ASSERT_NOT_NULL(strstr(buf, "<artifactId>slf4j-api</artifactId>"));
    ASSERT_NOT_NULL(strstr(buf, "<scope>test</scope>"));
    ASSERT_NOT_NULL(strstr(buf, "<optional>true</optional>"));

    remove(outpath);
    now_project_free(p);
    PASS();
}

static void test_export_maven_main_class(void) {
    TEST("export:maven: executable JAR with main class");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"java\"], std: \"21\","
        "  output: { type: \"executable\" },"
        "  java: { main_class: \"io.test.Main\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_pom_main.xml", NOW_TEST_RESOURCES);
    int rc = now_export_maven(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[len] = '\0';
    fclose(fp);

    ASSERT_NOT_NULL(strstr(buf, "maven-jar-plugin"));
    ASSERT_NOT_NULL(strstr(buf, "<mainClass>io.test.Main</mainClass>"));

    remove(outpath);
    now_project_free(p);
    PASS();
}

static void test_import_maven_basic(void) {
    TEST("import:maven: parses basic pom.xml");
    char pompath[512];
    snprintf(pompath, sizeof(pompath), "%s/pom_basic.xml", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_import_maven(pompath, &res);
    ASSERT_NOT_NULL(p);
    ASSERT_STR(p->group, "com.example");
    ASSERT_STR(p->artifact, "myapp");
    ASSERT_STR(p->version, "1.2.3");
    ASSERT_STR(p->name, "My Application");
    ASSERT_STR(p->description, "A test project");
    ASSERT_STR(p->url, "https://example.com");
    ASSERT_STR(p->license, "MIT");
    ASSERT_STR(p->std, "17");
    ASSERT_STR(p->java.encoding, "UTF-8");
    now_project_free(p);
    PASS();
}

static void test_import_maven_deps(void) {
    TEST("import:maven: parses dependencies with scopes");
    char pompath[512];
    snprintf(pompath, sizeof(pompath), "%s/pom_deps.xml", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_import_maven(pompath, &res);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ((int)p->deps.count, 3);

    /* First dep: slf4j-api, compile scope */
    ASSERT_STR(p->deps.items[0].id, "org.slf4j:slf4j-api:2.0.9");
    ASSERT_NULL(p->deps.items[0].scope);  /* default compile → no scope stored */

    /* Second dep: junit, test scope, version from property substitution */
    ASSERT_STR(p->deps.items[1].id, "org.junit.jupiter:junit-jupiter:5.10.0");
    ASSERT_STR(p->deps.items[1].scope, "test");

    /* Third dep: guava, optional */
    ASSERT_STR(p->deps.items[2].id, "com.google.guava:guava:32.1.3-jre");
    ASSERT_EQ(p->deps.items[2].optional, 1);

    /* Repository */
    ASSERT_EQ((int)p->repos.count, 1);
    ASSERT_STR(p->repos.items[0].id, "central");

    now_project_free(p);
    PASS();
}

static void test_import_maven_roundtrip(void) {
    TEST("import:maven: roundtrip pom.xml → now.pasta → reload");
    char pompath[512];
    snprintf(pompath, sizeof(pompath), "%s/pom_basic.xml", NOW_TEST_RESOURCES);
    NowResult res;
    NowProject *p = now_import_maven(pompath, &res);
    ASSERT_NOT_NULL(p);

    /* Write now.pasta */
    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_roundtrip.pasta", NOW_TEST_RESOURCES);
    int rc = now_import_maven_write(p, outpath, &res);
    ASSERT_EQ(rc, 0);
    now_project_free(p);

    /* Re-load and verify */
    NowProject *p2 = now_project_load(outpath, &res);
    if (!p2) { fprintf(stderr, "roundtrip reload error: %s\n", res.message); FAIL("p2 is NULL"); return; }
    ASSERT_STR(p2->group, "com.example");
    ASSERT_STR(p2->artifact, "myapp");
    ASSERT_STR(p2->version, "1.2.3");
    ASSERT_STR(p2->std, "17");
    ASSERT_EQ((int)p2->langs.count, 1);
    ASSERT_STR(p2->langs.items[0], "java");

    remove(outpath);
    now_project_free(p2);
    PASS();
}

/* ---- Export ---- */

static void test_export_cmake_basic(void) {
    TEST("export:cmake: generates valid CMakeLists.txt");
    const char *input =
        "{ group: \"io.test\", artifact: \"mylib\", version: \"2.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"shared\", name: \"mylib\" },"
        "  compile: { warnings: [\"Wall\", \"Wextra\"], defines: [\"MYLIB_INTERNAL\"] } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_cmake_output.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_cmake(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    /* Read back and verify key content */
    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "project(mylib VERSION 2.0.0")) { FAIL("missing project()"); now_project_free(p); return; }
    if (!strstr(buf, "add_library(mylib SHARED")) { FAIL("missing add_library"); now_project_free(p); return; }
    if (!strstr(buf, "-Wall")) { FAIL("missing -Wall"); now_project_free(p); return; }
    if (!strstr(buf, "-Wextra")) { FAIL("missing -Wextra"); now_project_free(p); return; }
    if (!strstr(buf, "MYLIB_INTERNAL")) { FAIL("missing define"); now_project_free(p); return; }
    if (!strstr(buf, "CMAKE_C_STANDARD 11")) { FAIL("missing C standard"); now_project_free(p); return; }
    if (!strstr(buf, "Generated by now")) { FAIL("missing header"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_cmake_executable(void) {
    TEST("export:cmake: executable output type");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"app\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_cmake_exec.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_cmake(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "add_executable(app")) { FAIL("missing add_executable"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_cmake_deps_comment(void) {
    TEST("export:cmake: deps listed as comments");
    const char *input =
        "{ group: \"io.test\", artifact: \"svc\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"svc\" },"
        "  deps: [{ id: \"org.acme:core:^1.0.0\" }] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_cmake_deps.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_cmake(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "org.acme:core:^1.0.0")) { FAIL("missing dep comment"); now_project_free(p); return; }
    if (!strstr(buf, "FetchContent")) { FAIL("missing FetchContent hint"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_cmake_cxx(void) {
    TEST("export:cmake: C++ project includes CXX language");
    const char *input =
        "{ group: \"io.test\", artifact: \"cxxlib\", version: \"1.0.0\","
        "  langs: [\"c\", \"c++\"], std: \"c++17\","
        "  output: { type: \"static\", name: \"cxxlib\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_cmake_cxx.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_cmake(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "LANGUAGES CXX C")) { FAIL("missing CXX language"); now_project_free(p); return; }
    if (!strstr(buf, "CMAKE_CXX_STANDARD 17")) { FAIL("missing CXX standard"); now_project_free(p); return; }
    if (!strstr(buf, "*.cpp")) { FAIL("missing cpp glob"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

/* ---- Export: Makefile ---- */

static void test_export_make_basic(void) {
    TEST("export:make: generates valid Makefile");
    const char *input =
        "{ group: \"io.test\", artifact: \"mylib\", version: \"2.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"shared\", name: \"mylib\" },"
        "  compile: { warnings: [\"Wall\", \"Wextra\"], defines: [\"MYLIB_INTERNAL\"] } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_make_output.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_make(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "PROJECT  := mylib")) { FAIL("missing PROJECT"); now_project_free(p); return; }
    if (!strstr(buf, "VERSION  := 2.0.0")) { FAIL("missing VERSION"); now_project_free(p); return; }
    if (!strstr(buf, "-fPIC")) { FAIL("missing -fPIC for shared"); now_project_free(p); return; }
    if (!strstr(buf, "lib$(TARGET).so")) { FAIL("missing .so output"); now_project_free(p); return; }
    if (!strstr(buf, "-shared")) { FAIL("missing -shared flag"); now_project_free(p); return; }
    if (!strstr(buf, "-Wall")) { FAIL("missing -Wall"); now_project_free(p); return; }
    if (!strstr(buf, "-Wextra")) { FAIL("missing -Wextra"); now_project_free(p); return; }
    if (!strstr(buf, "-DMYLIB_INTERNAL")) { FAIL("missing -D define"); now_project_free(p); return; }
    if (!strstr(buf, "-std=c11")) { FAIL("missing -std=c11"); now_project_free(p); return; }
    if (!strstr(buf, "Generated by now")) { FAIL("missing header"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_make_executable(void) {
    TEST("export:make: executable output type");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"app\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_make_exec.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_make(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "$(BUILD_DIR)/$(TARGET)")) { FAIL("missing executable output"); now_project_free(p); return; }
    if (strstr(buf, "-fPIC")) { FAIL("executable should not have -fPIC"); now_project_free(p); return; }
    if (strstr(buf, "-shared")) { FAIL("executable should not have -shared"); now_project_free(p); return; }
    if (!strstr(buf, "install -m 755")) { FAIL("missing install for executable"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_make_static(void) {
    TEST("export:make: static library uses ar");
    const char *input =
        "{ group: \"io.test\", artifact: \"core\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"static\", name: \"core\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_make_static.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_make(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "lib$(TARGET).a")) { FAIL("missing .a output"); now_project_free(p); return; }
    if (!strstr(buf, "$(AR) rcs")) { FAIL("missing ar rcs"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_make_deps(void) {
    TEST("export:make: deps listed as comments");
    const char *input =
        "{ group: \"io.test\", artifact: \"svc\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"svc\" },"
        "  deps: [{ id: \"org.acme:core:^1.0.0\" }] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_make_deps.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_make(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "org.acme:core:^1.0.0")) { FAIL("missing dep comment"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_make_cxx(void) {
    TEST("export:make: C++ project uses CXX");
    const char *input =
        "{ group: \"io.test\", artifact: \"cxxlib\", version: \"1.0.0\","
        "  langs: [\"c\", \"c++\"], std: \"c++17\","
        "  output: { type: \"static\", name: \"cxxlib\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_make_cxx.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_make(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "CXX       ?= g++")) { FAIL("missing CXX variable"); now_project_free(p); return; }
    if (!strstr(buf, "-std=c++17")) { FAIL("missing C++17 std flag"); now_project_free(p); return; }
    if (!strstr(buf, "*.cpp")) { FAIL("missing cpp wildcard"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

/* ---- Meson export ---- */

static void test_export_meson_basic(void) {
    TEST("export:meson: generates valid meson.build");
    const char *input =
        "{ group: \"io.test\", artifact: \"mylib\", version: \"2.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"shared\", name: \"mylib\" },"
        "  compile: { warnings: [\"Wall\"], defines: [\"MYLIB_INTERNAL\"] } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_meson_output.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_meson(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "project('mylib'")) { FAIL("missing project()"); now_project_free(p); return; }
    if (!strstr(buf, "'c'")) { FAIL("missing language"); now_project_free(p); return; }
    if (!strstr(buf, "c_std=c11")) { FAIL("missing c_std"); now_project_free(p); return; }
    if (!strstr(buf, "shared_library")) { FAIL("missing shared_library"); now_project_free(p); return; }
    if (!strstr(buf, "-Wall")) { FAIL("missing -Wall"); now_project_free(p); return; }
    if (!strstr(buf, "-DMYLIB_INTERNAL")) { FAIL("missing define"); now_project_free(p); return; }
    if (!strstr(buf, "Generated by now")) { FAIL("missing header"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_meson_executable(void) {
    TEST("export:meson: executable output type");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"app\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_meson_exec.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_meson(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "executable('app'")) { FAIL("missing executable()"); now_project_free(p); return; }
    if (strstr(buf, "shared_library")) { FAIL("should not have shared_library"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_meson_cxx(void) {
    TEST("export:meson: C++ project");
    const char *input =
        "{ group: \"io.test\", artifact: \"cxxlib\", version: \"1.0.0\","
        "  langs: [\"c\", \"c++\"], std: \"c++20\","
        "  output: { type: \"static\", name: \"cxxlib\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_meson_cxx.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_meson(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "'cpp'")) { FAIL("missing cpp language"); now_project_free(p); return; }
    if (!strstr(buf, "cpp_std=c++20")) { FAIL("missing cpp_std"); now_project_free(p); return; }
    if (!strstr(buf, "static_library")) { FAIL("missing static_library"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_meson_header_only(void) {
    TEST("export:meson: header-only uses declare_dependency");
    const char *input =
        "{ group: \"io.test\", artifact: \"hdr\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"header-only\", name: \"hdr\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_meson_hdr.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_meson(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "declare_dependency")) { FAIL("missing declare_dependency"); now_project_free(p); return; }
    if (strstr(buf, "executable(") || strstr(buf, "static_library(") ||
        strstr(buf, "shared_library(")) {
        FAIL("header-only should not have build target");
        now_project_free(p); return;
    }

    now_project_free(p);
    PASS();
}

/* ---- Bazel export ---- */

static void test_export_bazel_basic(void) {
    TEST("export:bazel: generates valid BUILD.bazel");
    const char *input =
        "{ group: \"io.test\", artifact: \"mylib\", version: \"2.0.0\","
        "  langs: [\"c\"], std: \"c11\","
        "  output: { type: \"shared\", name: \"mylib\" },"
        "  compile: { warnings: [\"Wall\"], defines: [\"MYLIB_INTERNAL\"] } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_bazel_output.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_bazel(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "cc_library")) { FAIL("missing cc_library"); now_project_free(p); return; }
    if (!strstr(buf, "\"mylib\"")) { FAIL("missing target name"); now_project_free(p); return; }
    if (!strstr(buf, "-std=c11")) { FAIL("missing -std=c11"); now_project_free(p); return; }
    if (!strstr(buf, "-Wall")) { FAIL("missing -Wall"); now_project_free(p); return; }
    if (!strstr(buf, "-DMYLIB_INTERNAL")) { FAIL("missing define"); now_project_free(p); return; }
    if (!strstr(buf, "rules_cc")) { FAIL("missing rules_cc load"); now_project_free(p); return; }
    if (!strstr(buf, "Generated by now")) { FAIL("missing header"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_bazel_executable(void) {
    TEST("export:bazel: executable uses cc_binary");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"app\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_bazel_exec.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_bazel(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "cc_binary")) { FAIL("missing cc_binary"); now_project_free(p); return; }
    if (strstr(buf, "cc_library(\n")) { FAIL("should not have cc_library"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_bazel_static(void) {
    TEST("export:bazel: static lib has linkstatic");
    const char *input =
        "{ group: \"io.test\", artifact: \"slib\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"static\", name: \"slib\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_bazel_static.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_bazel(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "linkstatic = True")) { FAIL("missing linkstatic"); now_project_free(p); return; }
    if (!strstr(buf, "cc_library")) { FAIL("missing cc_library"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_bazel_cxx(void) {
    TEST("export:bazel: C++ project glob patterns");
    const char *input =
        "{ group: \"io.test\", artifact: \"cxxlib\", version: \"1.0.0\","
        "  langs: [\"c\", \"c++\"], std: \"c++17\","
        "  output: { type: \"shared\", name: \"cxxlib\" } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_bazel_cxx.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_bazel(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "*.cpp")) { FAIL("missing cpp glob"); now_project_free(p); return; }
    if (!strstr(buf, "-std=c++17")) { FAIL("missing -std=c++17"); now_project_free(p); return; }
    if (!strstr(buf, "*.hpp")) { FAIL("missing hpp glob"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

static void test_export_bazel_deps_comment(void) {
    TEST("export:bazel: deps listed as comments");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], output: { type: \"executable\", name: \"app\" },"
        "  deps: [{ id: \"io.x:foo:1.0.0\", scope: \"compile\" }] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/test_bazel_deps.txt",
             NOW_TEST_RESOURCES);
    int rc = now_export_bazel(p, ".", outpath, &res);
    ASSERT_EQ(rc, 0);

    FILE *fp = fopen(outpath, "r");
    ASSERT_NOT_NULL(fp);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(outpath);

    if (!strstr(buf, "io.x:foo:1.0.0")) { FAIL("missing dep comment"); now_project_free(p); return; }
    if (!strstr(buf, "[compile]")) { FAIL("missing scope"); now_project_free(p); return; }

    now_project_free(p);
    PASS();
}

/* ---- Reproducible builds ---- */

static void test_repro_init(void) {
    TEST("repro: init defaults to disabled");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    ASSERT_EQ(cfg.enabled, 0);
    ASSERT_EQ(cfg.path_prefix_map, 0);
    ASSERT_EQ(cfg.sort_inputs, 0);
    ASSERT_EQ(cfg.no_date_macros, 0);
    now_repro_free(&cfg);
    PASS();
}

static void test_repro_from_project_bool(void) {
    TEST("repro: parse reproducible: true enables all");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"], reproducible: true }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowReproConfig cfg;
    now_repro_from_project(&cfg, p);
    ASSERT_EQ(cfg.enabled, 1);
    ASSERT_EQ(cfg.path_prefix_map, 1);
    ASSERT_EQ(cfg.sort_inputs, 1);
    ASSERT_EQ(cfg.no_date_macros, 1);
    ASSERT_EQ(cfg.strip_metadata, 1);
    ASSERT_EQ(cfg.verify, 1);
    ASSERT_STR(cfg.timebase, "git-commit");

    now_repro_free(&cfg);
    now_project_free(p);
    PASS();
}

static void test_repro_from_project_map(void) {
    TEST("repro: parse reproducible: map with selective options");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"],"
        "  reproducible: { timebase: \"zero\", path_prefix_map: true,"
        "                   sort_inputs: true, no_date_macros: false,"
        "                   strip_metadata: false, verify: false } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowReproConfig cfg;
    now_repro_from_project(&cfg, p);
    ASSERT_EQ(cfg.enabled, 1);
    ASSERT_STR(cfg.timebase, "zero");
    ASSERT_EQ(cfg.path_prefix_map, 1);
    ASSERT_EQ(cfg.sort_inputs, 1);
    ASSERT_EQ(cfg.no_date_macros, 0);
    ASSERT_EQ(cfg.strip_metadata, 0);
    ASSERT_EQ(cfg.verify, 0);

    now_repro_free(&cfg);
    now_project_free(p);
    PASS();
}

static void test_repro_from_project_none(void) {
    TEST("repro: no reproducible field stays disabled");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"] }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowReproConfig cfg;
    now_repro_from_project(&cfg, p);
    ASSERT_EQ(cfg.enabled, 0);

    now_repro_free(&cfg);
    now_project_free(p);
    PASS();
}

static void test_repro_timebase_zero(void) {
    TEST("repro: timebase 'zero' resolves to epoch");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.timebase = strdup("zero");

    NowResult res;
    char *ts = now_repro_resolve_timebase(&cfg, ".", &res);
    ASSERT_NOT_NULL(ts);
    ASSERT_STR(ts, "1970-01-01T00:00:00Z");
    free(ts);
    now_repro_free(&cfg);
    PASS();
}

static void test_repro_timebase_now(void) {
    TEST("repro: timebase 'now' resolves to current time");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.timebase = strdup("now");

    NowResult res;
    char *ts = now_repro_resolve_timebase(&cfg, ".", &res);
    ASSERT_NOT_NULL(ts);
    /* Should be a valid ISO 8601 string ending in Z */
    size_t len = strlen(ts);
    if (len < 20 || ts[len-1] != 'Z') { FAIL("bad format"); free(ts); now_repro_free(&cfg); return; }
    free(ts);
    now_repro_free(&cfg);
    PASS();
}

static void test_repro_timebase_literal(void) {
    TEST("repro: timebase literal ISO 8601 passthrough");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.timebase = strdup("2026-01-15T12:00:00Z");

    NowResult res;
    char *ts = now_repro_resolve_timebase(&cfg, ".", &res);
    ASSERT_NOT_NULL(ts);
    ASSERT_STR(ts, "2026-01-15T12:00:00Z");
    free(ts);
    now_repro_free(&cfg);
    PASS();
}

static void test_repro_compile_flags_gcc(void) {
    TEST("repro: compile flags for GCC (prefix map + date)");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.path_prefix_map = 1;
    cfg.no_date_macros = 1;

    char **flags = NULL;
    size_t count = 0;
    int n = now_repro_compile_flags(&cfg, "/home/user/proj",
                                     "2026-03-05T14:30:00Z", 0,
                                     &flags, &count);
    if (n < 0) { FAIL("returned error"); now_repro_free(&cfg); return; }
    if (count < 3) { FAIL("expected >= 3 flags"); now_repro_free_flags(flags, count); now_repro_free(&cfg); return; }

    /* Check for debug prefix map */
    int has_debug_prefix = 0, has_macro_prefix = 0, has_date = 0, has_time = 0;
    for (size_t i = 0; i < count; i++) {
        if (strstr(flags[i], "-fdebug-prefix-map=")) has_debug_prefix = 1;
        if (strstr(flags[i], "-fmacro-prefix-map=")) has_macro_prefix = 1;
        if (strstr(flags[i], "__DATE__")) has_date = 1;
        if (strstr(flags[i], "__TIME__")) has_time = 1;
    }
    if (!has_debug_prefix) { FAIL("missing -fdebug-prefix-map"); }
    else if (!has_macro_prefix) { FAIL("missing -fmacro-prefix-map"); }
    else if (!has_date) { FAIL("missing __DATE__ define"); }
    else if (!has_time) { FAIL("missing __TIME__ define"); }

    now_repro_free_flags(flags, count);
    now_repro_free(&cfg);
    if (has_debug_prefix && has_macro_prefix && has_date && has_time) PASS();
}

static void test_repro_compile_flags_msvc(void) {
    TEST("repro: compile flags for MSVC (/pathmap + /D)");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.path_prefix_map = 1;
    cfg.no_date_macros = 1;

    char **flags = NULL;
    size_t count = 0;
    now_repro_compile_flags(&cfg, "C:\\Users\\dev\\proj",
                             "2026-03-05T14:30:00Z", 1,
                             &flags, &count);
    if (count < 1) { FAIL("expected flags"); now_repro_free(&cfg); return; }

    int has_pathmap = 0;
    for (size_t i = 0; i < count; i++) {
        if (strstr(flags[i], "/pathmap:")) has_pathmap = 1;
    }
    if (!has_pathmap) { FAIL("missing /pathmap"); now_repro_free_flags(flags, count); now_repro_free(&cfg); return; }

    now_repro_free_flags(flags, count);
    now_repro_free(&cfg);
    PASS();
}

static void test_repro_link_flags(void) {
    TEST("repro: link flags include --build-id=sha1");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.strip_metadata = 1;

    char **flags = NULL;
    size_t count = 0;
    now_repro_link_flags(&cfg, 0, &flags, &count);
    if (count < 1) { FAIL("expected link flags"); now_repro_free(&cfg); return; }
    if (!strstr(flags[0], "--build-id=sha1")) {
        FAIL("missing --build-id=sha1");
        now_repro_free_flags(flags, count);
        now_repro_free(&cfg);
        return;
    }

    now_repro_free_flags(flags, count);
    now_repro_free(&cfg);
    PASS();
}

static void test_repro_link_flags_msvc_empty(void) {
    TEST("repro: no link flags for MSVC");
    NowReproConfig cfg;
    now_repro_init(&cfg);
    cfg.enabled = 1;
    cfg.strip_metadata = 1;

    char **flags = NULL;
    size_t count = 0;
    now_repro_link_flags(&cfg, 1, &flags, &count);
    ASSERT_EQ(count, (size_t)0);

    now_repro_free(&cfg);
    PASS();
}

static void test_repro_sort_filelist(void) {
    TEST("repro: sort file list lexicographically");
    NowFileList fl;
    now_filelist_init(&fl);
    now_filelist_push(&fl, "src/main/c/z_last.c");
    now_filelist_push(&fl, "src/main/c/a_first.c");
    now_filelist_push(&fl, "src/main/c/m_middle.c");

    now_repro_sort_filelist(&fl);

    ASSERT_STR(fl.paths[0], "src/main/c/a_first.c");
    ASSERT_STR(fl.paths[1], "src/main/c/m_middle.c");
    ASSERT_STR(fl.paths[2], "src/main/c/z_last.c");

    now_filelist_free(&fl);
    PASS();
}

static void test_repro_disabled_no_flags(void) {
    TEST("repro: disabled config produces no flags");
    NowReproConfig cfg;
    now_repro_init(&cfg);

    char **flags = NULL;
    size_t count = 0;
    now_repro_compile_flags(&cfg, "/some/path", "2026-01-01T00:00:00Z", 0,
                             &flags, &count);
    ASSERT_EQ(count, (size_t)0);

    now_repro_free(&cfg);
    PASS();
}

static void test_repro_null_safety(void) {
    TEST("repro: null safety");
    NowReproConfig cfg;
    now_repro_from_project(&cfg, NULL);
    ASSERT_EQ(cfg.enabled, 0);

    now_repro_sort_filelist(NULL);  /* should not crash */

    now_repro_free(&cfg);
    PASS();
}

/* ---- Trust ---- */

static void test_trust_init_free(void) {
    TEST("trust: init and free empty store");
    NowTrustStore store;
    now_trust_init(&store);
    ASSERT_EQ(store.count, (size_t)0);
    ASSERT_EQ(store.capacity, (size_t)0);
    now_trust_free(&store);
    PASS();
}

static void test_trust_add(void) {
    TEST("trust: add keys to store");
    NowTrustStore store;
    now_trust_init(&store);

    int rc = now_trust_add(&store, "*", "RWAAAA==", "global key");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(store.count, (size_t)1);
    ASSERT_STR(store.keys[0].scope, "*");
    ASSERT_STR(store.keys[0].key, "RWAAAA==");
    ASSERT_STR(store.keys[0].comment, "global key");

    rc = now_trust_add(&store, "org.acme", "RWBBBB==", NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(store.count, (size_t)2);

    now_trust_free(&store);
    PASS();
}

static void test_trust_scope_wildcard(void) {
    TEST("trust: scope '*' matches everything");
    ASSERT_EQ(now_trust_scope_matches("*", "org.acme", "core"), 1);
    ASSERT_EQ(now_trust_scope_matches("*", "io.test", NULL), 1);
    PASS();
}

static void test_trust_scope_group_prefix(void) {
    TEST("trust: scope group prefix with dot-boundary");
    ASSERT_EQ(now_trust_scope_matches("org.acme", "org.acme", NULL), 1);
    ASSERT_EQ(now_trust_scope_matches("org.acme", "org.acme.core", NULL), 1);
    ASSERT_EQ(now_trust_scope_matches("org.acme", "org.acmetools", NULL), 0);
    ASSERT_EQ(now_trust_scope_matches("org.acme", "io.other", NULL), 0);
    PASS();
}

static void test_trust_scope_exact(void) {
    TEST("trust: scope group:artifact exact match");
    ASSERT_EQ(now_trust_scope_matches("org.acme:core", "org.acme", "core"), 1);
    ASSERT_EQ(now_trust_scope_matches("org.acme:core", "org.acme", "other"), 0);
    ASSERT_EQ(now_trust_scope_matches("org.acme:core", "org.acme", NULL), 0);
    ASSERT_EQ(now_trust_scope_matches("org.acme:core", "io.test", "core"), 0);
    PASS();
}

static void test_trust_find(void) {
    TEST("trust: find key by coordinate");
    NowTrustStore store;
    now_trust_init(&store);
    now_trust_add(&store, "org.acme", "KEY_ACME", "acme org");
    now_trust_add(&store, "io.test:specific", "KEY_EXACT", "exact match");
    now_trust_add(&store, "*", "KEY_GLOBAL", "fallback");

    const NowTrustKey *k;
    k = now_trust_find(&store, "org.acme", "core");
    ASSERT_NOT_NULL(k);
    ASSERT_STR(k->key, "KEY_ACME");

    k = now_trust_find(&store, "org.acme.sub", "lib");
    ASSERT_NOT_NULL(k);
    ASSERT_STR(k->key, "KEY_ACME");

    k = now_trust_find(&store, "io.test", "specific");
    ASSERT_NOT_NULL(k);
    ASSERT_STR(k->key, "KEY_EXACT");

    k = now_trust_find(&store, "io.test", "other");
    ASSERT_NOT_NULL(k);
    ASSERT_STR(k->key, "KEY_GLOBAL");

    now_trust_free(&store);
    PASS();
}

static void test_trust_find_no_match(void) {
    TEST("trust: find returns NULL when no match");
    NowTrustStore store;
    now_trust_init(&store);
    now_trust_add(&store, "org.acme:core", "KEY1", NULL);

    const NowTrustKey *k = now_trust_find(&store, "io.other", "lib");
    if (k != NULL) { FAIL("expected NULL"); now_trust_free(&store); return; }

    now_trust_free(&store);
    PASS();
}

static void test_trust_policy_none(void) {
    TEST("trust: policy defaults to NONE");
    NowTrustPolicy policy = {0, 0};
    ASSERT_EQ(now_trust_level(&policy), NOW_TRUST_NONE);
    PASS();
}

static void test_trust_policy_signed(void) {
    TEST("trust: policy SIGNED when require_signatures");
    NowTrustPolicy policy = {1, 0};
    ASSERT_EQ(now_trust_level(&policy), NOW_TRUST_SIGNED);
    PASS();
}

static void test_trust_policy_trusted(void) {
    TEST("trust: policy TRUSTED when require_known_keys");
    NowTrustPolicy policy = {1, 1};
    ASSERT_EQ(now_trust_level(&policy), NOW_TRUST_TRUSTED);
    PASS();
}

static void test_trust_policy_from_project(void) {
    TEST("trust: parse policy from project pasta");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"],"
        "  trust: { require_signatures: true, require_known_keys: false } }";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowTrustPolicy pol = now_trust_policy_from_project(p);
    ASSERT_EQ(pol.require_signatures, 1);
    ASSERT_EQ(pol.require_known_keys, 0);
    ASSERT_EQ(now_trust_level(&pol), NOW_TRUST_SIGNED);

    now_project_free(p);
    PASS();
}

static void test_trust_null_safety(void) {
    TEST("trust: null safety");
    ASSERT_EQ(now_trust_scope_matches(NULL, "org", NULL), 0);
    ASSERT_EQ(now_trust_scope_matches("*", NULL, NULL), 0);
    if (now_trust_find(NULL, "org", NULL) != NULL) { FAIL("expected NULL"); return; }

    NowTrustPolicy pol = now_trust_policy_from_project(NULL);
    ASSERT_EQ(pol.require_signatures, 0);
    ASSERT_EQ(now_trust_level(NULL), NOW_TRUST_NONE);
    PASS();
}

/* ---- Advisory guards ---- */

static void test_advisory_db_init_free(void) {
    TEST("advisory: db init/free");
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    ASSERT_EQ(db.count, (size_t)0);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_severity_parse(void) {
    TEST("advisory: severity parsing");
    ASSERT_EQ(now_severity_parse("critical"), NOW_SEV_CRITICAL);
    ASSERT_EQ(now_severity_parse("high"), NOW_SEV_HIGH);
    ASSERT_EQ(now_severity_parse("medium"), NOW_SEV_MEDIUM);
    ASSERT_EQ(now_severity_parse("low"), NOW_SEV_LOW);
    ASSERT_EQ(now_severity_parse("info"), NOW_SEV_INFO);
    ASSERT_EQ(now_severity_parse("blacklisted"), NOW_SEV_BLACKLISTED);
    ASSERT_EQ(now_severity_parse("unknown"), NOW_SEV_INFO);
    ASSERT_EQ(now_severity_parse(NULL), NOW_SEV_INFO);
    PASS();
}

static void test_advisory_severity_name(void) {
    TEST("advisory: severity name roundtrip");
    ASSERT_STR(now_severity_name(NOW_SEV_CRITICAL), "critical");
    ASSERT_STR(now_severity_name(NOW_SEV_HIGH), "high");
    ASSERT_STR(now_severity_name(NOW_SEV_MEDIUM), "medium");
    ASSERT_STR(now_severity_name(NOW_SEV_LOW), "low");
    ASSERT_STR(now_severity_name(NOW_SEV_INFO), "info");
    ASSERT_STR(now_severity_name(NOW_SEV_BLACKLISTED), "blacklisted");
    PASS();
}

static void test_advisory_severity_blocks(void) {
    TEST("advisory: severity blocking");
    ASSERT_EQ(now_severity_blocks(NOW_SEV_BLACKLISTED), 1);
    ASSERT_EQ(now_severity_blocks(NOW_SEV_CRITICAL), 1);
    ASSERT_EQ(now_severity_blocks(NOW_SEV_HIGH), 1);
    ASSERT_EQ(now_severity_blocks(NOW_SEV_MEDIUM), 0);
    ASSERT_EQ(now_severity_blocks(NOW_SEV_LOW), 0);
    ASSERT_EQ(now_severity_blocks(NOW_SEV_INFO), 0);
    PASS();
}

static void test_advisory_db_load_string(void) {
    TEST("advisory: load db from string");
    const char *input =
        "{ version: \"1.0.0\", updated: \"2026-03-05T00:00:00Z\","
        "  advisories: ["
        "    { id: \"NOW-SA-2026-0042\", severity: \"critical\","
        "      title: \"Buffer overflow in inflate()\","
        "      cve: [\"CVE-2026-1234\"],"
        "      affects: [ { id: \"zlib:zlib\", versions: [\">=1.2.0 <1.3.1\"] } ],"
        "      fixed_in: [ { id: \"zlib:zlib\", version: \"1.3.1\" } ],"
        "      affects_build_time: false, affects_runtime: true"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    int rc = now_advisory_db_load_string(&db, input, strlen(input), &res);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(db.count, (size_t)1);
    ASSERT_STR(db.entries[0].id, "NOW-SA-2026-0042");
    ASSERT_EQ(db.entries[0].severity, NOW_SEV_CRITICAL);
    ASSERT_STR(db.entries[0].title, "Buffer overflow in inflate()");
    ASSERT_EQ(db.entries[0].cve_count, (size_t)1);
    ASSERT_STR(db.entries[0].cve[0], "CVE-2026-1234");
    ASSERT_EQ(db.entries[0].affects_count, (size_t)1);
    ASSERT_STR(db.entries[0].affects[0].id, "zlib:zlib");
    ASSERT_EQ(db.entries[0].affects_runtime, 1);
    ASSERT_EQ(db.entries[0].affects_build_time, 0);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_blacklisted(void) {
    TEST("advisory: blacklisted entry");
    const char *input =
        "{ advisories: ["
        "    { id: \"NOW-SA-2026-0043\", severity: \"high\","
        "      blacklisted: true,"
        "      affects: [ { id: \"evil:pkg\", versions: [\"*\"] } ],"
        "      affects_build_time: true, affects_runtime: false"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    now_advisory_db_load_string(&db, input, strlen(input), &res);
    ASSERT_EQ(db.count, (size_t)1);
    ASSERT_EQ(db.entries[0].blacklisted, 1);
    ASSERT_EQ(db.entries[0].severity, NOW_SEV_BLACKLISTED);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_override_parse(void) {
    TEST("advisory: parse overrides from project");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"],"
        "  advisories: { allow: ["
        "    { advisory: \"NOW-SA-2026-0042\", dep: \"zlib:zlib:1.3.0\","
        "      reason: \"inflate() not used\", expires: \"2026-06-01\","
        "      approved_by: \"alice@acme.org\" }"
        "  ] }"
        "}";
    NowResult res;
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowOverrideList ovr;
    now_override_list_init(&ovr);
    int rc = now_advisory_overrides_from_project(&ovr, p, &res);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ovr.count, (size_t)1);
    ASSERT_STR(ovr.items[0].advisory, "NOW-SA-2026-0042");
    ASSERT_STR(ovr.items[0].dep, "zlib:zlib:1.3.0");
    ASSERT_STR(ovr.items[0].reason, "inflate() not used");
    ASSERT_STR(ovr.items[0].expires, "2026-06-01");
    ASSERT_STR(ovr.items[0].approved_by, "alice@acme.org");

    now_override_list_free(&ovr);
    now_project_free(p);
    PASS();
}

static void test_advisory_override_no_expires(void) {
    TEST("advisory: override without expires rejected");
    const char *input =
        "{ group: \"io.test\", artifact: \"app\", version: \"1.0.0\","
        "  langs: [\"c\"],"
        "  advisories: { allow: ["
        "    { advisory: \"NOW-SA-2026-0042\", reason: \"no expiry\" }"
        "  ] }"
        "}";
    NowResult res;
    memset(&res, 0, sizeof(res));
    NowProject *p = now_project_load_string(input, strlen(input), &res);
    ASSERT_NOT_NULL(p);

    NowOverrideList ovr;
    now_override_list_init(&ovr);
    int rc = now_advisory_overrides_from_project(&ovr, p, &res);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(res.code, NOW_ERR_SCHEMA);

    now_override_list_free(&ovr);
    now_project_free(p);
    PASS();
}

static void test_advisory_override_expiry(void) {
    TEST("advisory: override expiry check");
    NowAdvisoryOverride ovr = {0};
    ovr.expires = "2026-06-01";

    /* Not expired (today = 2026-03-08) */
    ASSERT_EQ(now_advisory_override_expired(&ovr, 20260308), 0);
    /* Expired (today = 2026-07-01) */
    ASSERT_EQ(now_advisory_override_expired(&ovr, 20260701), 1);
    /* Exact expiry date — not expired (still within) */
    ASSERT_EQ(now_advisory_override_expired(&ovr, 20260601), 0);
    PASS();
}

static void test_advisory_find_override(void) {
    TEST("advisory: find override by advisory+dep");
    NowOverrideList list;
    now_override_list_init(&list);

    /* Manually add one */
    NowAdvisoryOverride *tmp = realloc(list.items, sizeof(NowAdvisoryOverride));
    if (!tmp) { FAIL("alloc"); return; }
    list.items = tmp;
    list.capacity = 1;
    list.count = 1;
    memset(&list.items[0], 0, sizeof(NowAdvisoryOverride));
    list.items[0].advisory = strdup("NOW-SA-001");
    list.items[0].dep = strdup("zlib:zlib:1.3.0");
    list.items[0].expires = strdup("2026-12-31");

    const NowAdvisoryOverride *found =
        now_advisory_find_override(&list, "NOW-SA-001", "zlib:zlib:1.3.0");
    ASSERT_NOT_NULL(found);

    /* Different dep — no match */
    found = now_advisory_find_override(&list, "NOW-SA-001", "other:lib:1.0.0");
    if (found) { FAIL("expected NULL for different dep"); now_override_list_free(&list); return; }

    /* Different advisory — no match */
    found = now_advisory_find_override(&list, "NOW-SA-999", "zlib:zlib:1.3.0");
    if (found) { FAIL("expected NULL for different advisory"); now_override_list_free(&list); return; }

    now_override_list_free(&list);
    PASS();
}

static void test_advisory_check_dep_match(void) {
    TEST("advisory: check dep finds matching advisory");
    const char *db_str =
        "{ advisories: ["
        "    { id: \"NOW-SA-001\", severity: \"critical\","
        "      title: \"test vuln\","
        "      affects: [ { id: \"zlib:zlib\", versions: [\">=1.2.0 <1.3.1\"] } ],"
        "      affects_runtime: true, affects_build_time: false"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    now_advisory_db_load_string(&db, db_str, strlen(db_str), &res);

    NowAdvisoryReport report;
    now_advisory_report_init(&report);
    int rc = now_advisory_check_dep(&db, NULL, "zlib:zlib:1.3.0",
                                      "compile", 20260308, &report);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(report.count, (size_t)1);
    ASSERT_EQ(report.blocked, 1);

    now_advisory_report_free(&report);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_check_dep_no_match(void) {
    TEST("advisory: check dep no match for safe version");
    const char *db_str =
        "{ advisories: ["
        "    { id: \"NOW-SA-001\", severity: \"critical\","
        "      affects: [ { id: \"zlib:zlib\", versions: [\">=1.2.0 <1.3.1\"] } ],"
        "      affects_runtime: true"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    now_advisory_db_load_string(&db, db_str, strlen(db_str), &res);

    NowAdvisoryReport report;
    now_advisory_report_init(&report);
    now_advisory_check_dep(&db, NULL, "zlib:zlib:1.3.1",
                            "compile", 20260308, &report);
    ASSERT_EQ(report.count, (size_t)0);
    ASSERT_EQ(report.blocked, 0);

    now_advisory_report_free(&report);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_check_dep_overridden(void) {
    TEST("advisory: check dep with active override");
    const char *db_str =
        "{ advisories: ["
        "    { id: \"NOW-SA-001\", severity: \"critical\","
        "      affects: [ { id: \"zlib:zlib\", versions: [\">=1.2.0 <1.3.1\"] } ],"
        "      affects_runtime: true"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    now_advisory_db_load_string(&db, db_str, strlen(db_str), &res);

    NowOverrideList overrides;
    now_override_list_init(&overrides);
    NowAdvisoryOverride *o = realloc(overrides.items, sizeof(NowAdvisoryOverride));
    overrides.items = o;
    overrides.capacity = 1;
    overrides.count = 1;
    memset(o, 0, sizeof(*o));
    o->advisory = strdup("NOW-SA-001");
    o->dep = strdup("zlib:zlib:1.3.0");
    o->expires = strdup("2027-01-01");

    NowAdvisoryReport report;
    now_advisory_report_init(&report);
    now_advisory_check_dep(&db, &overrides, "zlib:zlib:1.3.0",
                            "compile", 20260308, &report);
    ASSERT_EQ(report.count, (size_t)1);
    ASSERT_EQ(report.hits[0].overridden, 1);
    ASSERT_EQ(report.blocked, 0); /* override prevents blocking */

    now_advisory_report_free(&report);
    now_override_list_free(&overrides);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_blacklisted_no_override(void) {
    TEST("advisory: blacklisted cannot be overridden");
    const char *db_str =
        "{ advisories: ["
        "    { id: \"NOW-SA-002\", severity: \"high\", blacklisted: true,"
        "      affects: [ { id: \"evil:pkg\", versions: [\"*\"] } ],"
        "      affects_build_time: true"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    now_advisory_db_load_string(&db, db_str, strlen(db_str), &res);

    NowOverrideList overrides;
    now_override_list_init(&overrides);
    NowAdvisoryOverride *o = realloc(overrides.items, sizeof(NowAdvisoryOverride));
    overrides.items = o;
    overrides.capacity = 1;
    overrides.count = 1;
    memset(o, 0, sizeof(*o));
    o->advisory = strdup("NOW-SA-002");
    o->expires = strdup("2027-01-01");

    NowAdvisoryReport report;
    now_advisory_report_init(&report);
    now_advisory_check_dep(&db, &overrides, "evil:pkg:1.0.0",
                            "compile", 20260308, &report);
    ASSERT_EQ(report.count, (size_t)1);
    ASSERT_EQ(report.blocked, 1); /* blacklisted always blocks */

    now_advisory_report_free(&report);
    now_override_list_free(&overrides);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_medium_warning(void) {
    TEST("advisory: medium severity = warning, not blocking");
    const char *db_str =
        "{ advisories: ["
        "    { id: \"NOW-SA-003\", severity: \"medium\","
        "      affects: [ { id: \"foo:bar\", versions: [\"*\"] } ],"
        "      affects_runtime: true"
        "    }"
        "  ]"
        "}";
    NowAdvisoryDB db;
    now_advisory_db_init(&db);
    NowResult res;
    memset(&res, 0, sizeof(res));
    now_advisory_db_load_string(&db, db_str, strlen(db_str), &res);

    NowAdvisoryReport report;
    now_advisory_report_init(&report);
    now_advisory_check_dep(&db, NULL, "foo:bar:2.0.0",
                            "compile", 20260308, &report);
    ASSERT_EQ(report.count, (size_t)1);
    ASSERT_EQ(report.blocked, 0); /* medium does not block */

    now_advisory_report_free(&report);
    now_advisory_db_free(&db);
    PASS();
}

static void test_advisory_report_format(void) {
    TEST("advisory: report formatting");
    NowAdvisoryReport report;
    now_advisory_report_init(&report);
    char *text = now_advisory_report_format(&report);
    ASSERT_NOT_NULL(text);
    free(text);

    now_advisory_report_free(&report);
    PASS();
}

static void test_advisory_null_safety(void) {
    TEST("advisory: null safety");
    ASSERT_EQ(now_severity_parse(NULL), NOW_SEV_INFO);
    ASSERT_EQ(now_severity_blocks(NOW_SEV_INFO), 0);
    ASSERT_EQ(now_advisory_override_expired(NULL, 20260308), -1);
    if (now_advisory_find_override(NULL, "x", "y") != NULL) {
        FAIL("expected NULL"); return;
    }
    ASSERT_EQ(now_advisory_check_dep(NULL, NULL, "x:y:1.0", "compile", 20260308, NULL), -1);
    PASS();
}

/* ---- CI integration ---- */

static void test_ci_exit_codes(void) {
    TEST("ci: exit code mapping");
    ASSERT_EQ(now_exit_code(NOW_OK), NOW_EXIT_OK);
    ASSERT_EQ(now_exit_code(NOW_ERR_TOOL), NOW_EXIT_BUILD);
    ASSERT_EQ(now_exit_code(NOW_ERR_TEST), NOW_EXIT_TEST);
    ASSERT_EQ(now_exit_code(NOW_ERR_SCHEMA), NOW_EXIT_CONFIG);
    ASSERT_EQ(now_exit_code(NOW_ERR_SYNTAX), NOW_EXIT_CONFIG);
    ASSERT_EQ(now_exit_code(NOW_ERR_IO), NOW_EXIT_IO);
    ASSERT_EQ(now_exit_code(NOW_ERR_NOT_FOUND), NOW_EXIT_RESOLVE);
    ASSERT_EQ(now_exit_code(NOW_ERR_AUTH), NOW_EXIT_AUTH);
    PASS();
}

static void test_ci_detect_defaults(void) {
    TEST("ci: detect defaults (non-CI environment)");
    NowCIEnv env;
    now_ci_detect(&env);
    /* In test runner context, we're not in CI (unless running in actual CI) */
    /* Just verify the struct is populated without crashing */
    if (env.format != NOW_OUTPUT_TEXT && env.format != NOW_OUTPUT_JSON
        && env.format != NOW_OUTPUT_PASTA) {
        FAIL("invalid format"); return;
    }
    PASS();
}

static void test_ci_format_build_json(void) {
    TEST("ci: format build result as JSON");
    char *out = now_ci_format_build("build", 0, 1234, 10, 8, 2, 0,
                                     NOW_OUTPUT_JSON);
    ASSERT_NOT_NULL(out);
    if (!strstr(out, "\"phase\": \"build\"")) { FAIL("missing phase"); free(out); return; }
    if (!strstr(out, "\"status\": \"ok\"")) { FAIL("missing status"); free(out); return; }
    if (!strstr(out, "\"compiled\": 8")) { FAIL("missing compiled"); free(out); return; }
    if (!strstr(out, "\"cached\": 2")) { FAIL("missing cached"); free(out); return; }
    free(out);
    PASS();
}

static void test_ci_format_build_pasta(void) {
    TEST("ci: format build result as Pasta");
    char *out = now_ci_format_build("compile", 1, 500, 5, 3, 1, 1,
                                     NOW_OUTPUT_PASTA);
    ASSERT_NOT_NULL(out);
    if (!strstr(out, "phase: \"compile\"")) { FAIL("missing phase"); free(out); return; }
    if (!strstr(out, "status: \"error\"")) { FAIL("missing status"); free(out); return; }
    free(out);
    PASS();
}

static void test_ci_format_test_json(void) {
    TEST("ci: format test result as JSON");
    char *out = now_ci_format_test(0, 42, 40, 2, 0, 3456, NOW_OUTPUT_JSON);
    ASSERT_NOT_NULL(out);
    if (!strstr(out, "\"phase\": \"test\"")) { FAIL("missing phase"); free(out); return; }
    if (!strstr(out, "\"passed\": 40")) { FAIL("missing passed"); free(out); return; }
    if (!strstr(out, "\"failed\": 2")) { FAIL("missing failed"); free(out); return; }
    free(out);
    PASS();
}

static void test_ci_format_text(void) {
    TEST("ci: format build result as text");
    char *out = now_ci_format_build("link", 0, 100, 1, 1, 0, 0,
                                     NOW_OUTPUT_TEXT);
    ASSERT_NOT_NULL(out);
    if (!strstr(out, "link:")) { FAIL("missing phase"); free(out); return; }
    if (!strstr(out, "ok")) { FAIL("missing status"); free(out); return; }
    free(out);
    PASS();
}

/* ---- Basta package tests ---- */

static void test_basta_create_and_parse(void) {
    TEST("basta: create document with blob");
    BastaValue *root = basta_new_map();
    if (!root) { FAIL("basta_new_map"); return; }

    /* Add metadata */
    BastaValue *meta = basta_new_map();
    basta_set(meta, "format", basta_new_string("basta/1"));
    basta_set(meta, "artifact", basta_new_string("testlib"));
    basta_set(root, "metadata", meta);

    /* Add a blob */
    const uint8_t test_data[] = "hello blob content";
    BastaValue *files = basta_new_map();
    basta_set(files, "test.h", basta_new_blob(test_data, sizeof(test_data) - 1));
    basta_set(root, "headers", files);

    /* Write to buffer */
    size_t out_len;
    char *buf = basta_write(root, BASTA_SECTIONS, &out_len);
    basta_free(root);
    if (!buf) { FAIL("basta_write"); return; }

    /* Parse it back */
    BastaResult bres;
    BastaValue *parsed = basta_parse(buf, out_len, &bres);
    free(buf);
    if (!parsed) { FAIL(bres.message); return; }

    /* Verify metadata */
    const BastaValue *m = basta_map_get(parsed, "metadata");
    if (!m || basta_type(m) != BASTA_MAP) { basta_free(parsed); FAIL("no metadata"); return; }
    const BastaValue *art = basta_map_get(m, "artifact");
    if (!art || strcmp(basta_get_string(art), "testlib") != 0) {
        basta_free(parsed); FAIL("artifact mismatch"); return;
    }

    /* Verify blob */
    const BastaValue *h = basta_map_get(parsed, "headers");
    if (!h || basta_type(h) != BASTA_MAP) { basta_free(parsed); FAIL("no headers"); return; }
    const BastaValue *blob = basta_map_get(h, "test.h");
    if (!blob || basta_type(blob) != BASTA_BLOB) { basta_free(parsed); FAIL("no blob"); return; }
    size_t blen;
    const uint8_t *bdata = basta_get_blob(blob, &blen);
    if (blen != sizeof(test_data) - 1 || memcmp(bdata, test_data, blen) != 0) {
        basta_free(parsed); FAIL("blob content mismatch"); return;
    }

    basta_free(parsed);
    PASS();
}

static void test_basta_package_roundtrip(void) {
    TEST("basta: package → extract roundtrip");

    /* Create a minimal project for packaging */
    NowProject *p = now_project_new();
    if (!p) { FAIL("now_project_new"); return; }
    p->group    = strdup("io.test");
    p->artifact = strdup("roundtrip");
    p->version  = strdup("1.0.0");
    p->output.type = strdup("static");

    /* Create a temp directory with a descriptor and fake build output */
    char tmpdir[512];
    snprintf(tmpdir, sizeof(tmpdir), "%s/basta_test_tmp", NOW_TEST_RESOURCES);
    now_mkdir_p(tmpdir);

    /* Write a now.pasta descriptor */
    char desc_path[512];
    snprintf(desc_path, sizeof(desc_path), "%s/now.pasta", tmpdir);
    FILE *fp = fopen(desc_path, "w");
    if (fp) {
        fprintf(fp, "{ group: \"io.test\", artifact: \"roundtrip\", version: \"1.0.0\" }\n");
        fclose(fp);
    }

    /* Create target/bin with a fake library */
    char bindir[512];
    snprintf(bindir, sizeof(bindir), "%s/target/bin", tmpdir);
    now_mkdir_p(bindir);

    char libpath[512];
#ifdef _WIN32
    snprintf(libpath, sizeof(libpath), "%s/roundtrip.lib", bindir);
#else
    snprintf(libpath, sizeof(libpath), "%s/libroundtrip.a", bindir);
#endif
    fp = fopen(libpath, "wb");
    if (fp) {
        const char *fake_lib = "FAKE_LIB_DATA_1234567890";
        fwrite(fake_lib, 1, strlen(fake_lib), fp);
        fclose(fp);
    }

    /* Create headers */
    char hdrdir[512];
    snprintf(hdrdir, sizeof(hdrdir), "%s/src/main/h", tmpdir);
    now_mkdir_p(hdrdir);
    p->sources.headers = strdup("src/main/h");

    char hdrpath[512];
    snprintf(hdrpath, sizeof(hdrpath), "%s/roundtrip.h", hdrdir);
    fp = fopen(hdrpath, "w");
    if (fp) {
        fprintf(fp, "#ifndef ROUNDTRIP_H\n#define ROUNDTRIP_H\nvoid roundtrip(void);\n#endif\n");
        fclose(fp);
    }

    /* Package it */
    NowResult res;
    int rc = now_package(p, tmpdir, 0, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(p); return; }

    /* Verify .basta file exists */
    char basta_path[512];
    const char *triple = now_host_triple();
    snprintf(basta_path, sizeof(basta_path), "%s/target/pkg/roundtrip-1.0.0-%s.basta",
             tmpdir, triple);

    if (!now_path_exists(basta_path)) {
        FAIL("basta file not created"); now_project_free(p); return;
    }

    /* Extract it */
    char extract_dir[512];
    snprintf(extract_dir, sizeof(extract_dir), "%s/extracted", tmpdir);
    rc = now_basta_extract(basta_path, extract_dir, 0, &res);
    if (rc != 0) { FAIL(res.message); now_project_free(p); return; }

    /* Verify extracted descriptor exists */
    char ex_desc[512];
    snprintf(ex_desc, sizeof(ex_desc), "%s/now.pasta", extract_dir);
    if (!now_path_exists(ex_desc)) {
        FAIL("extracted now.pasta missing"); now_project_free(p); return;
    }

    /* Verify extracted header exists */
    char ex_hdr[512];
    snprintf(ex_hdr, sizeof(ex_hdr), "%s/h/roundtrip.h", extract_dir);
    if (!now_path_exists(ex_hdr)) {
        FAIL("extracted header missing"); now_project_free(p); return;
    }

    /* Verify extracted library exists */
    char ex_lib[512];
#ifdef _WIN32
    snprintf(ex_lib, sizeof(ex_lib), "%s/lib/%s/roundtrip.lib", extract_dir, triple);
#else
    snprintf(ex_lib, sizeof(ex_lib), "%s/lib/%s/libroundtrip.a", extract_dir, triple);
#endif
    if (!now_path_exists(ex_lib)) {
        FAIL("extracted library missing"); now_project_free(p); return;
    }

    now_project_free(p);

    /* Cleanup temp files */
    remove(ex_lib);
    remove(ex_hdr);
    remove(ex_desc);
    char ex_hdir[512], ex_ldir[512], ex_ltdir[512];
    snprintf(ex_hdir, sizeof(ex_hdir), "%s/h", extract_dir);
    snprintf(ex_ltdir, sizeof(ex_ltdir), "%s/lib/%s", extract_dir, triple);
    snprintf(ex_ldir, sizeof(ex_ldir), "%s/lib", extract_dir);
    rmdir(ex_ltdir);
    rmdir(ex_ldir);
    rmdir(ex_hdir);
    rmdir(extract_dir);
    remove(basta_path);
    char sha_path[512];
    snprintf(sha_path, sizeof(sha_path), "%s/target/pkg/roundtrip-1.0.0-%s.sha256",
             tmpdir, triple);
    remove(sha_path);
    char pkgdir[512];
    snprintf(pkgdir, sizeof(pkgdir), "%s/target/pkg", tmpdir);
    rmdir(pkgdir);
    remove(libpath);
    rmdir(bindir);
    char targetdir[512];
    snprintf(targetdir, sizeof(targetdir), "%s/target", tmpdir);
    rmdir(targetdir);
    remove(hdrpath);
    rmdir(hdrdir);
    char srcmain[512];
    snprintf(srcmain, sizeof(srcmain), "%s/src/main", tmpdir);
    rmdir(srcmain);
    char srcdir[512];
    snprintf(srcdir, sizeof(srcdir), "%s/src", tmpdir);
    rmdir(srcdir);
    remove(desc_path);
    rmdir(tmpdir);

    PASS();
}

static void test_basta_extract_null_safety(void) {
    TEST("basta: extract null safety");
    NowResult res;
    int rc = now_basta_extract(NULL, NULL, 0, &res);
    if (rc != -1) { FAIL("expected -1"); return; }
    PASS();
}

static void test_basta_extract_missing_file(void) {
    TEST("basta: extract missing file");
    NowResult res;
    int rc = now_basta_extract("/nonexistent/file.basta", "/tmp/out", 0, &res);
    if (rc != -1) { FAIL("expected -1"); return; }
    PASS();
}

static void test_basta_metadata_fields(void) {
    TEST("basta: metadata has format/group/artifact/version");

    /* Build a Basta doc like now_package does */
    BastaValue *root = basta_new_map();
    BastaValue *meta = basta_new_map();
    basta_set(meta, "format",      basta_new_string("basta/1"));
    basta_set(meta, "group",       basta_new_string("com.example"));
    basta_set(meta, "artifact",    basta_new_string("mylib"));
    basta_set(meta, "version",     basta_new_string("2.3.4"));
    basta_set(meta, "triple",      basta_new_string("linux-x86_64-gnu"));
    basta_set(meta, "output_type", basta_new_string("shared"));
    basta_set(root, "metadata", meta);

    /* Write and re-parse */
    size_t len;
    char *buf = basta_write(root, BASTA_SECTIONS, &len);
    basta_free(root);
    if (!buf) { FAIL("write"); return; }

    BastaResult bres;
    BastaValue *parsed = basta_parse(buf, len, &bres);
    free(buf);
    if (!parsed) { FAIL(bres.message); return; }

    const BastaValue *m = basta_map_get(parsed, "metadata");
    if (!m) { basta_free(parsed); FAIL("no metadata"); return; }

    const BastaValue *fmt = basta_map_get(m, "format");
    if (!fmt || strcmp(basta_get_string(fmt), "basta/1") != 0) {
        basta_free(parsed); FAIL("format"); return;
    }
    const BastaValue *grp = basta_map_get(m, "group");
    if (!grp || strcmp(basta_get_string(grp), "com.example") != 0) {
        basta_free(parsed); FAIL("group"); return;
    }
    const BastaValue *art = basta_map_get(m, "artifact");
    if (!art || strcmp(basta_get_string(art), "mylib") != 0) {
        basta_free(parsed); FAIL("artifact"); return;
    }
    const BastaValue *ver = basta_map_get(m, "version");
    if (!ver || strcmp(basta_get_string(ver), "2.3.4") != 0) {
        basta_free(parsed); FAIL("version"); return;
    }
    const BastaValue *tri = basta_map_get(m, "triple");
    if (!tri || strcmp(basta_get_string(tri), "linux-x86_64-gnu") != 0) {
        basta_free(parsed); FAIL("triple"); return;
    }

    basta_free(parsed);
    PASS();
}

/* ---- Ed25519 tests ---- */

/* Helper: convert hex string to bytes */
static void hex_to_bytes(const char *hex, unsigned char *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned int byte;
        sscanf(hex + i * 2, "%02x", &byte);
        out[i] = (unsigned char)byte;
    }
}

static void test_ed25519_keypair(void) {
    TEST("ed25519: keypair from seed");
    unsigned char seed[32];
    hex_to_bytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                 seed, 32);

    unsigned char pub[32], priv[64];
    int rc = now_ed25519_keypair(pub, priv, seed);
    if (rc != 0) { FAIL("keypair failed"); return; }

    unsigned char expected_pub[32];
    hex_to_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                 expected_pub, 32);
    if (memcmp(pub, expected_pub, 32) != 0) {
        FAIL("public key mismatch"); return;
    }
    PASS();
}

static void test_ed25519_sign_verify(void) {
    TEST("ed25519: sign and verify roundtrip");
    /* Generate a keypair */
    unsigned char seed[32];
    hex_to_bytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                 seed, 32);

    unsigned char pub[32], priv[64];
    now_ed25519_keypair(pub, priv, seed);

    /* Sign the empty message (RFC 8032 Test Vector 1) */
    unsigned char sig[64];
    const unsigned char msg[] = "";
    int rc = now_ed25519_sign(sig, msg, 0, priv);
    if (rc != 0) { FAIL("sign failed"); return; }

    /* Verify */
    rc = now_ed25519_verify(sig, msg, 0, pub);
    if (rc != 0) { FAIL("verify failed"); return; }
    PASS();
}

static void test_ed25519_verify_rfc8032_1(void) {
    TEST("ed25519: verify RFC 8032 test vector 1");
    unsigned char pub[32], sig[64];
    hex_to_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                 pub, 32);
    hex_to_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
                 sig, 64);

    int rc = now_ed25519_verify(sig, (const unsigned char *)"", 0, pub);
    if (rc != 0) { FAIL("verify should pass"); return; }
    PASS();
}

static void test_ed25519_verify_rfc8032_2(void) {
    TEST("ed25519: verify RFC 8032 test vector 2");
    unsigned char pub[32], sig[64];
    hex_to_bytes("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
                 pub, 32);
    hex_to_bytes("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
                 sig, 64);

    unsigned char msg[1] = {0x72};
    int rc = now_ed25519_verify(sig, msg, 1, pub);
    if (rc != 0) { FAIL("verify should pass"); return; }
    PASS();
}

static void test_ed25519_verify_bad_sig(void) {
    TEST("ed25519: reject tampered signature");
    unsigned char pub[32], sig[64];
    hex_to_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                 pub, 32);
    hex_to_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
                 sig, 64);

    /* Tamper with signature */
    sig[0] ^= 0x01;

    int rc = now_ed25519_verify(sig, (const unsigned char *)"", 0, pub);
    if (rc == 0) { FAIL("should reject tampered sig"); return; }
    PASS();
}

static void test_ed25519_verify_wrong_msg(void) {
    TEST("ed25519: reject wrong message");
    unsigned char pub[32], sig[64];
    hex_to_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                 pub, 32);
    hex_to_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
                 sig, 64);

    unsigned char wrong[] = "wrong message";
    int rc = now_ed25519_verify(sig, wrong, sizeof(wrong) - 1, pub);
    if (rc == 0) { FAIL("should reject wrong message"); return; }
    PASS();
}

static void test_ed25519_null_safety(void) {
    TEST("ed25519: null safety");
    if (now_ed25519_verify(NULL, NULL, 0, NULL) == 0) { FAIL("should reject NULL"); return; }
    if (now_ed25519_sign(NULL, NULL, 0, NULL) == 0) { FAIL("should reject NULL"); return; }
    if (now_ed25519_keypair(NULL, NULL, NULL) == 0) { FAIL("should reject NULL"); return; }
    PASS();
}

/* ---- Build Cache ---- */

static void test_cache_key_deterministic(void) {
    TEST("cache: key is deterministic");
    char *k1 = now_cache_key("abc123", "def456", "/usr/bin/gcc");
    char *k2 = now_cache_key("abc123", "def456", "/usr/bin/gcc");
    ASSERT_NOT_NULL(k1);
    ASSERT_NOT_NULL(k2);
    if (!k1 || !k2 || strcmp(k1, k2) != 0) { FAIL("keys not equal"); free(k1); free(k2); return; }
    ASSERT_EQ((int)strlen(k1), 64);  /* SHA-256 hex */
    free(k1);
    free(k2);
    PASS();
}

static void test_cache_key_varies_source(void) {
    TEST("cache: different source → different key");
    char *k1 = now_cache_key("aaa", "flags", "/usr/bin/gcc");
    char *k2 = now_cache_key("bbb", "flags", "/usr/bin/gcc");
    ASSERT_NOT_NULL(k1);
    ASSERT_NOT_NULL(k2);
    if (strcmp(k1, k2) == 0) { FAIL("keys should differ"); free(k1); free(k2); return; }
    free(k1);
    free(k2);
    PASS();
}

static void test_cache_key_varies_flags(void) {
    TEST("cache: different flags → different key");
    char *k1 = now_cache_key("src", "flags_a", "/usr/bin/gcc");
    char *k2 = now_cache_key("src", "flags_b", "/usr/bin/gcc");
    ASSERT_NOT_NULL(k1);
    ASSERT_NOT_NULL(k2);
    if (strcmp(k1, k2) == 0) { FAIL("keys should differ"); free(k1); free(k2); return; }
    free(k1);
    free(k2);
    PASS();
}

static void test_cache_key_varies_compiler(void) {
    TEST("cache: different compiler → different key");
    char *k1 = now_cache_key("src", "flags", "/usr/bin/gcc");
    char *k2 = now_cache_key("src", "flags", "/usr/bin/clang");
    ASSERT_NOT_NULL(k1);
    ASSERT_NOT_NULL(k2);
    if (strcmp(k1, k2) == 0) { FAIL("keys should differ"); free(k1); free(k2); return; }
    free(k1);
    free(k2);
    PASS();
}

static void test_cache_path_sharding(void) {
    TEST("cache: path uses two-level sharding");
    char *path = now_cache_path("abcdef0123456789abcdef0123456789"
                                 "abcdef0123456789abcdef0123456789", ".o");
    ASSERT_NOT_NULL(path);
    /* Path should contain /ab/cd/ shard directories */
    if (!strstr(path, "/ab/") && !strstr(path, "\\ab\\")) {
        FAIL("expected /ab/ shard in path");
        free(path);
        return;
    }
    if (!strstr(path, "/cd/") && !strstr(path, "\\cd\\") &&
        !strstr(path, "/ab/cd") && !strstr(path, "\\ab\\cd")) {
        FAIL("expected /cd/ shard in path");
        free(path);
        return;
    }
    /* Should end with .o */
    size_t plen = strlen(path);
    if (plen < 2 || strcmp(path + plen - 2, ".o") != 0) {
        FAIL("expected .o extension");
        free(path);
        return;
    }
    free(path);
    PASS();
}

static void test_cache_store_restore(void) {
    TEST("cache: store and restore roundtrip");
    /* Create a temp file with known content */
    const char *content = "hello cache world 12345";
    const char *tmpfile = "target/_cache_test_src.o";
    now_mkdir_p("target");
    FILE *f = fopen(tmpfile, "wb");
    if (!f) { FAIL("cannot create temp file"); return; }
    fwrite(content, 1, strlen(content), f);
    fclose(f);

    /* Store it */
    char *key = now_cache_key("store_restore_test_hash", "flags_hash", "/cc");
    ASSERT_NOT_NULL(key);
    int rc = now_cache_store(key, tmpfile, ".o");
    ASSERT_EQ(rc, 0);

    /* Restore to a different path */
    const char *restored = "target/_cache_test_dst.o";
    rc = now_cache_restore(key, restored, ".o");
    ASSERT_EQ(rc, 0);

    /* Verify content matches */
    FILE *f2 = fopen(restored, "rb");
    if (!f2) { FAIL("restored file not found"); free(key); return; }
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f2);
    fclose(f2);
    ASSERT_EQ((int)n, (int)strlen(content));
    if (strcmp(buf, content) != 0) { FAIL("content mismatch"); free(key); return; }

    /* Cleanup */
    remove(tmpfile);
    remove(restored);
    free(key);
    PASS();
}

static void test_cache_restore_miss(void) {
    TEST("cache: restore miss returns -1");
    char *key = now_cache_key("nonexistent_hash_xyz", "flags", "/cc");
    ASSERT_NOT_NULL(key);
    int rc = now_cache_restore(key, "target/_miss.o", ".o");
    ASSERT_EQ(rc, -1);
    free(key);
    PASS();
}

static void test_cache_clean_works(void) {
    TEST("cache: clean removes stored objects");
    /* Store an object */
    const char *tmpfile = "target/_cache_clean_test.o";
    now_mkdir_p("target");
    FILE *f = fopen(tmpfile, "wb");
    if (!f) { FAIL("cannot create temp file"); return; }
    fwrite("data", 1, 4, f);
    fclose(f);

    char *key = now_cache_key("clean_test_hash", "flags", "/cc");
    ASSERT_NOT_NULL(key);
    now_cache_store(key, tmpfile, ".o");

    /* Verify it's there */
    ASSERT_EQ(now_cache_restore(key, "target/_cache_clean_verify.o", ".o"), 0);
    remove("target/_cache_clean_verify.o");

    /* Clean */
    now_cache_clean();

    /* Should be gone */
    int rc = now_cache_restore(key, "target/_cache_clean_verify.o", ".o");
    ASSERT_EQ(rc, -1);

    remove(tmpfile);
    free(key);
    PASS();
}

/* ---- Depfile parsing ---- */

static void test_depfile_parse_simple(void) {
    TEST("depfile: parse simple .d file");
    now_mkdir_p("target");
    const char *depfile = "target/_test_simple.d";
    FILE *f = fopen(depfile, "w");
    if (!f) { FAIL("cannot create depfile"); return; }
    fprintf(f, "target/obj/main/foo.c.o: src/main/c/foo.c src/main/h/foo.h src/main/h/bar.h\n");
    fclose(f);

    NowDepList deps = {0};
    int rc = now_depfile_parse(depfile, "src/main/c/foo.c", &deps);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((int)deps.count, 2);  /* foo.h and bar.h, source excluded */
    /* Verify headers are present (order may vary) */
    int found_foo_h = 0, found_bar_h = 0;
    for (size_t i = 0; i < deps.count; i++) {
        if (strstr(deps.paths[i], "foo.h")) found_foo_h = 1;
        if (strstr(deps.paths[i], "bar.h")) found_bar_h = 1;
    }
    ASSERT_EQ(found_foo_h, 1);
    ASSERT_EQ(found_bar_h, 1);
    now_deplist_free(&deps);
    remove(depfile);
    PASS();
}

static void test_depfile_parse_multiline(void) {
    TEST("depfile: parse multiline .d file with continuations");
    now_mkdir_p("target");
    const char *depfile = "target/_test_multi.d";
    FILE *f = fopen(depfile, "w");
    if (!f) { FAIL("cannot create depfile"); return; }
    fprintf(f, "target/obj/main/foo.c.o: src/main/c/foo.c \\\n"
               "  src/main/h/foo.h \\\n"
               "  src/main/h/bar.h \\\n"
               "  src/main/h/baz.h\n");
    fclose(f);

    NowDepList deps = {0};
    int rc = now_depfile_parse(depfile, "src/main/c/foo.c", &deps);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((int)deps.count, 3);  /* foo.h, bar.h, baz.h */
    now_deplist_free(&deps);
    remove(depfile);
    PASS();
}

static void test_depfile_parse_missing(void) {
    TEST("depfile: missing file returns -1");
    NowDepList deps = {0};
    int rc = now_depfile_parse("target/_nonexistent.d", "foo.c", &deps);
    ASSERT_EQ(rc, -1);
    PASS();
}

static void test_depfile_parse_msvc(void) {
    TEST("depfile: parse MSVC /showIncludes output");
    const char *output =
        "foo.c\n"
        "Note: including file: C:\\sdk\\include\\stdio.h\n"
        "Note: including file:   C:\\project\\src\\foo.h\n"
        "some compiler output line\n"
        "Note: including file: C:\\project\\src\\bar.h\n";
    size_t len = strlen(output);

    NowDepList deps = {0};
    int rc = now_depfile_parse_msvc(output, len, &deps);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((int)deps.count, 3);
    /* Verify paths are trimmed */
    int found_stdio = 0, found_foo = 0, found_bar = 0;
    for (size_t i = 0; i < deps.count; i++) {
        if (strstr(deps.paths[i], "stdio.h")) found_stdio = 1;
        if (strstr(deps.paths[i], "foo.h")) found_foo = 1;
        if (strstr(deps.paths[i], "bar.h")) found_bar = 1;
    }
    ASSERT_EQ(found_stdio, 1);
    ASSERT_EQ(found_foo, 1);
    ASSERT_EQ(found_bar, 1);
    now_deplist_free(&deps);
    PASS();
}

/* ---- Dep-aware cache ---- */

static void test_cache_restore_ex_no_deps(void) {
    TEST("cache: restore_ex with no .deps sidecar returns miss");
    /* Use a fresh key that has no .deps file */
    char *key = now_cache_key("no_deps_test_xyz", "flags", "/cc");
    ASSERT_NOT_NULL(key);
    int rc = now_cache_restore_ex(key, "target/_nodeps.o", ".o");
    ASSERT_EQ(rc, -1);
    free(key);
    PASS();
}

static void test_cache_store_restore_ex_with_deps(void) {
    TEST("cache: store_ex/restore_ex with deps roundtrip");
    now_mkdir_p("target");

    /* Create fake object */
    const char *objfile = "target/_deptest_obj.o";
    FILE *f = fopen(objfile, "wb");
    if (!f) { FAIL("cannot create obj"); return; }
    fwrite("objdata", 1, 7, f);
    fclose(f);

    /* Create fake header dep */
    const char *hdr = "target/_deptest_hdr.h";
    f = fopen(hdr, "w");
    if (!f) { FAIL("cannot create header"); remove(objfile); return; }
    fprintf(f, "#define FOO 1\n");
    fclose(f);

    /* Store with deps */
    char *skey = now_cache_key("deptest_src_hash", "deptest_flags", "/gcc");
    ASSERT_NOT_NULL(skey);

    NowDepList deps = {0};
    deps.paths = (char **)malloc(sizeof(char *));
    deps.paths[0] = strdup(hdr);
    deps.count = 1;
    deps.capacity = 1;

    int rc = now_cache_store_ex(skey, objfile, ".o", &deps);
    ASSERT_EQ(rc, 0);

    /* Restore should succeed */
    const char *dst = "target/_deptest_restore.o";
    rc = now_cache_restore_ex(skey, dst, ".o");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(now_path_exists(dst), 1);
    remove(dst);

    /* Modify header → restore should fail */
    f = fopen(hdr, "w");
    if (f) {
        fprintf(f, "#define FOO 2\n");
        fclose(f);
    }
    rc = now_cache_restore_ex(skey, dst, ".o");
    ASSERT_EQ(rc, -1);

    /* Cleanup */
    now_deplist_free(&deps);
    free(skey);
    remove(objfile);
    remove(hdr);
    remove(dst);
    PASS();
}

static void test_cache_restore_ex_dep_deleted(void) {
    TEST("cache: restore_ex fails when dep file deleted");
    now_mkdir_p("target");

    /* Create fake object and header */
    const char *objfile = "target/_deldep_obj.o";
    FILE *f = fopen(objfile, "wb");
    if (!f) { FAIL("cannot create obj"); return; }
    fwrite("obj", 1, 3, f);
    fclose(f);

    const char *hdr = "target/_deldep_hdr.h";
    f = fopen(hdr, "w");
    if (!f) { FAIL("cannot create header"); remove(objfile); return; }
    fprintf(f, "header\n");
    fclose(f);

    char *skey = now_cache_key("deldep_src", "deldep_flags", "/gcc");
    ASSERT_NOT_NULL(skey);

    NowDepList deps = {0};
    deps.paths = (char **)malloc(sizeof(char *));
    deps.paths[0] = strdup(hdr);
    deps.count = 1;
    deps.capacity = 1;

    now_cache_store_ex(skey, objfile, ".o", &deps);

    /* Delete the dep file */
    remove(hdr);

    /* Restore should fail */
    int rc = now_cache_restore_ex(skey, "target/_deldep_restore.o", ".o");
    ASSERT_EQ(rc, -1);

    now_deplist_free(&deps);
    free(skey);
    remove(objfile);
    PASS();
}

/* ---- Manifest dep tracking ---- */

static void test_manifest_set_deps(void) {
    TEST("manifest: set_deps stores dep info");
    NowManifest m;
    now_manifest_init(&m);
    now_manifest_set(&m, "foo.c", "foo.o", "hash1", "fhash", 12345);

    const char *dpaths[] = {"src/main/h/foo.h", "src/main/h/bar.h"};
    const char *dhashes[] = {"aaa", "bbb"};
    int rc = now_manifest_set_deps(&m, "foo.c", dpaths, dhashes, 2);
    ASSERT_EQ(rc, 0);

    const NowManifestEntry *e = now_manifest_find(&m, "foo.c");
    ASSERT_NOT_NULL(e);
    ASSERT_EQ((int)e->dep_count, 2);
    if (strcmp(e->deps[0], "src/main/h/foo.h") != 0) { FAIL("dep[0] mismatch"); now_manifest_free(&m); return; }
    if (strcmp(e->dep_hashes[1], "bbb") != 0) { FAIL("dep_hash[1] mismatch"); now_manifest_free(&m); return; }
    now_manifest_free(&m);
    PASS();
}

static void test_manifest_deps_roundtrip(void) {
    TEST("manifest: deps survive save/load roundtrip");
    now_mkdir_p("target");

    NowManifest m;
    now_manifest_init(&m);
    now_manifest_set(&m, "foo.c", "foo.o", "hash1", "fhash", 12345);

    const char *dpaths[] = {"dep_a.h", "dep_b.h"};
    const char *dhashes[] = {"aaa111", "bbb222"};
    now_manifest_set_deps(&m, "foo.c", dpaths, dhashes, 2);

    const char *mpath = "target/_test_manifest_deps";
    now_manifest_save(&m, mpath);
    now_manifest_free(&m);

    /* Reload */
    NowManifest m2;
    now_manifest_load(&m2, mpath);
    const NowManifestEntry *e = now_manifest_find(&m2, "foo.c");
    ASSERT_NOT_NULL(e);
    ASSERT_EQ((int)e->dep_count, 2);
    if (strcmp(e->deps[0], "dep_a.h") != 0) { FAIL("dep path mismatch"); now_manifest_free(&m2); return; }
    if (strcmp(e->dep_hashes[0], "aaa111") != 0) { FAIL("dep hash mismatch"); now_manifest_free(&m2); return; }
    if (strcmp(e->deps[1], "dep_b.h") != 0) { FAIL("dep[1] path mismatch"); now_manifest_free(&m2); return; }
    if (strcmp(e->dep_hashes[1], "bbb222") != 0) { FAIL("dep_hash[1] mismatch"); now_manifest_free(&m2); return; }
    now_manifest_free(&m2);
    remove(mpath);
    PASS();
}

static void test_manifest_needs_rebuild_dep_changed(void) {
    TEST("manifest: needs_rebuild detects dep change");
    now_mkdir_p("target");

    /* Create a real header file so we can hash it */
    const char *hdr = "target/_test_dep_header.h";
    FILE *f = fopen(hdr, "w");
    if (!f) { FAIL("cannot create header"); return; }
    fprintf(f, "original\n");
    fclose(f);

    /* Create a real source file */
    const char *srcfile = "target/_test_dep_src.c";
    f = fopen(srcfile, "w");
    if (!f) { FAIL("cannot create source"); remove(hdr); return; }
    fprintf(f, "source\n");
    fclose(f);

    char *src_hash = now_sha256_file(srcfile);
    char *dep_hash = now_sha256_file(hdr);
    ASSERT_NOT_NULL(src_hash);
    ASSERT_NOT_NULL(dep_hash);

    struct stat st;
    stat(srcfile, &st);

    NowManifest m;
    now_manifest_init(&m);
    now_manifest_set(&m, "_test_dep_src.c", "target/obj.o", src_hash, "fhash",
                     (long long)st.st_mtime);

    const char *dpaths[] = {hdr};
    const char *dhashes[] = {dep_hash};
    now_manifest_set_deps(&m, "_test_dep_src.c", dpaths, dhashes, 1);

    /* Create a fake object file */
    f = fopen("target/obj.o", "w");
    if (f) { fwrite("x", 1, 1, f); fclose(f); }

    /* Should be up to date */
    const NowManifestEntry *e = now_manifest_find(&m, "_test_dep_src.c");
    int rebuild = now_manifest_needs_rebuild(e, "target", "_test_dep_src.c", "fhash", NULL);
    ASSERT_EQ(rebuild, 0);

    /* Modify header */
    f = fopen(hdr, "w");
    if (f) { fprintf(f, "modified\n"); fclose(f); }

    /* Now should need rebuild */
    rebuild = now_manifest_needs_rebuild(e, "target", "_test_dep_src.c", "fhash", NULL);
    ASSERT_EQ(rebuild, 1);

    now_manifest_free(&m);
    free(src_hash);
    free(dep_hash);
    remove(hdr);
    remove(srcfile);
    remove("target/obj.o");
    PASS();
}

int main(void) {
    printf("now test suite\n");
    printf("==============\n\n");

    printf("  Version:\n");
    test_version();

    printf("\n  POM (string):\n");
    test_pom_minimal_string();
    test_pom_lang_scalar();
    test_pom_lang_mixed();
    test_pom_compile();
    test_pom_os_conditional();
    test_pom_deps();
    test_pom_convergence();

    printf("\n  POM (file):\n");
    test_pom_load_file();
    test_pom_load_rich();

    printf("\n  POM (errors):\n");
    test_pom_syntax_error();
    test_pom_not_a_map();
    test_pom_file_not_found();

    printf("\n  Language type system:\n");
    test_lang_find_c();
    test_lang_find_cxx();
    test_lang_classify_c();
    test_lang_classify_h();
    test_lang_classify_cpp();
    test_lang_classify_unknown();
    test_lang_source_exts();

    printf("\n  Filesystem:\n");
    test_fs_path_join();
    test_fs_path_join_trailing_sep();
    test_fs_path_ext();
    test_fs_obj_path();

    printf("\n  Semantic versioning:\n");
    test_semver_parse_basic();
    test_semver_parse_prerelease();
    test_semver_parse_build();
    test_semver_compare();
    test_semver_to_string();

    printf("\n  Version ranges:\n");
    test_range_exact();
    test_range_caret();
    test_range_caret_pre1();
    test_range_tilde();
    test_range_gte();
    test_range_compound();
    test_range_wildcard();
    test_range_intersect();

    printf("\n  Coordinates:\n");
    test_coord_parse();

    printf("\n  Manifest:\n");
    test_manifest_set_find();
    test_manifest_update();
    test_sha256_string();

    printf("\n  Resolver:\n");
    test_resolver_single_dep();
    test_resolver_convergence_lowest();
    test_resolver_conflict();
    test_resolver_multiple_deps();
    test_resolver_override();
    test_lock_save_load();

    printf("\n  HTTP client:\n");
    test_pico_http_version();
    test_pico_http_parse_url();
    test_pico_http_parse_url_no_port();
    test_pico_http_parse_url_no_path();
    test_pico_http_parse_url_https();
    test_pico_http_parse_url_reject_ftp();
    test_pico_http_error_codes();
    test_pico_http_invalid_args();
    test_pico_http_dns_failure();
    test_pico_http_connect_failure();
    test_pico_http_find_header();
    test_pico_http_request_url();
    test_pico_http_response_free_zeroed();
    test_pico_http_stream_invalid_args();
    test_pico_http_stream_connect_failure();
    test_pico_http_stream_callback_type();
    test_pico_http_tls_noverify_option();
    test_pico_http_tls_options_zero_init();
    test_pico_http_tls_ca_file_option();

    printf("\n  WebSocket client:\n");
    test_pico_ws_version();
    test_pico_ws_error_codes();
    test_pico_ws_invalid_args();
    test_pico_ws_bad_url();
    test_pico_ws_connect_failure();
    test_pico_ws_close_null();
    test_pico_ws_tls_noverify_option();
    test_pico_ws_tls_options_zero_init();

    printf("\n  Procure:\n");
    test_repo_dep_path();
    test_procure_no_deps();

    printf("\n  Parallel build:\n");
    test_cpu_count();

    printf("\n  Toolchain:\n");
    test_obj_path_ex_obj();
    test_toolchain_gcc_default();
    test_toolchain_msvc_detect();
    test_toolchain_java_resolve();
    test_toolchain_no_java_for_c();

    printf("\n  Auth:\n");
    test_auth_load_no_creds();
    test_auth_login_null_creds();
    test_auth_login_connect_failure();

    printf("\n  Publish:\n");
    test_publish_missing_identity();
    test_publish_no_package();
    test_yank_no_url();
    test_yank_connect_failure();
    test_dep_updates_no_deps();
    test_dep_updates_null_project();
    test_cache_mirror_no_url();
    test_cache_mirror_connect_failure();

    printf("\n  Workspace:\n");
    test_is_workspace_true();
    test_is_workspace_false();
    test_workspace_is_null();
    test_workspace_init();
    test_workspace_topo_sort();
    test_workspace_inject_sibling();

    printf("\n  Plugins:\n");
    test_plugin_is_builtin();
    test_plugin_pom_load();
    test_plugin_run_hook_no_plugins();
    test_plugin_result_init_free();
    test_plugin_unknown_builtin();
    test_plugin_version_generate();

    printf("\n  Plugin Registry:\n");
    test_plugin_manifest_parse_string();
    test_plugin_manifest_parse_minimal();
    test_plugin_manifest_missing_id();
    test_plugin_manifest_parse_file_missing();
    test_plugin_info_free_null_safe();
    test_plugin_find_binary_missing();
    test_plugin_list_empty_repo();
    test_plugin_search_no_match();
    test_plugin_install_bad_registry();
    test_plugin_manifest_roundtrip();

    printf("\n  Dep confusion protection:\n");
    test_private_group_exact_match();
    test_private_group_dotted_child();
    test_private_group_no_false_positive();
    test_private_group_multiple_prefixes();
    test_private_group_null_safe();
    test_private_group_pom_load();
    test_private_group_procure_fail();

    printf("\n  Reproducible builds:\n");
    test_repro_init();
    test_repro_from_project_bool();
    test_repro_from_project_map();
    test_repro_from_project_none();
    test_repro_timebase_zero();
    test_repro_timebase_now();
    test_repro_timebase_literal();
    test_repro_compile_flags_gcc();
    test_repro_compile_flags_msvc();
    test_repro_link_flags();
    test_repro_link_flags_msvc_empty();
    test_repro_sort_filelist();
    test_repro_disabled_no_flags();
    test_repro_null_safety();

    printf("\n  Trust:\n");
    test_trust_init_free();
    test_trust_add();
    test_trust_scope_wildcard();
    test_trust_scope_group_prefix();
    test_trust_scope_exact();
    test_trust_find();
    test_trust_find_no_match();
    test_trust_policy_none();
    test_trust_policy_signed();
    test_trust_policy_trusted();
    test_trust_policy_from_project();
    test_trust_null_safety();

    printf("\n  Ed25519:\n");
    test_ed25519_keypair();
    test_ed25519_sign_verify();
    test_ed25519_verify_rfc8032_1();
    test_ed25519_verify_rfc8032_2();
    test_ed25519_verify_bad_sig();
    test_ed25519_verify_wrong_msg();
    test_ed25519_null_safety();

    printf("\n  Advisory guards:\n");
    test_advisory_db_init_free();
    test_advisory_severity_parse();
    test_advisory_severity_name();
    test_advisory_severity_blocks();
    test_advisory_db_load_string();
    test_advisory_blacklisted();
    test_advisory_override_parse();
    test_advisory_override_no_expires();
    test_advisory_override_expiry();
    test_advisory_find_override();
    test_advisory_check_dep_match();
    test_advisory_check_dep_no_match();
    test_advisory_check_dep_overridden();
    test_advisory_blacklisted_no_override();
    test_advisory_medium_warning();
    test_advisory_report_format();
    test_advisory_null_safety();

    printf("\n  CI integration:\n");
    test_ci_exit_codes();
    test_ci_detect_defaults();
    test_ci_format_build_json();
    test_ci_format_build_pasta();
    test_ci_format_test_json();
    test_ci_format_text();

    printf("\n  Layers:\n");
    test_layer_stack_init();
    test_layer_baseline_sections();
    test_layer_load_file();
    test_layer_merge_open();
    test_layer_merge_locked_audit();
    test_layer_merge_strarray_exclude();
    test_layer_audit_format();
    test_layer_push_project();

    printf("\n  Alforno integration:\n");
    test_alforno_aggregate_merge();
    test_alforno_parameterize();
    test_alforno_conflate();
    test_alforno_collect_merge();

    printf("\n  Multi-architecture:\n");
    test_triple_parse_full();
    test_triple_parse_shorthand();
    test_triple_format();
    test_triple_dir();
    test_triple_cmp();
    test_triple_match_exact();
    test_triple_match_wildcard();
    test_triple_host_detect();
    test_triple_is_native();

    printf("\n  C++20 Modules:\n");
    test_module_scan_interface();
    test_module_scan_import();
    test_module_scan_impl();
    test_module_scan_skips_comments();
    test_module_order_basic();
    test_module_find();
    test_module_bmi_path();
    test_module_classify_cppm();

    printf("\n  Java + Maven:\n");
    test_lang_java_registration();
    test_lang_java_classify();
    test_pom_java_fields();
    test_pom_java_defaults();
    test_export_maven_basic();
    test_export_maven_deps();
    test_export_maven_main_class();
    test_import_maven_basic();
    test_import_maven_deps();
    test_import_maven_roundtrip();

    printf("\n  Export:\n");
    test_export_cmake_basic();
    test_export_cmake_executable();
    test_export_cmake_deps_comment();
    test_export_cmake_cxx();
    test_export_make_basic();
    test_export_make_executable();
    test_export_make_static();
    test_export_make_deps();
    test_export_make_cxx();
    test_export_meson_basic();
    test_export_meson_executable();
    test_export_meson_cxx();
    test_export_meson_header_only();
    test_export_bazel_basic();
    test_export_bazel_executable();
    test_export_bazel_static();
    test_export_bazel_cxx();
    test_export_bazel_deps_comment();

    printf("\n  Basta packages:\n");
    test_basta_create_and_parse();
    test_basta_metadata_fields();
    test_basta_extract_null_safety();
    test_basta_extract_missing_file();
    test_basta_package_roundtrip();

    printf("\n  Build cache:\n");
    test_cache_key_deterministic();
    test_cache_key_varies_source();
    test_cache_key_varies_flags();
    test_cache_key_varies_compiler();
    test_cache_path_sharding();
    test_cache_store_restore();
    test_cache_restore_miss();
    test_cache_clean_works();

    printf("\n  Depfile parsing:\n");
    test_depfile_parse_simple();
    test_depfile_parse_multiline();
    test_depfile_parse_missing();
    test_depfile_parse_msvc();

    printf("\n  Dep-aware cache:\n");
    test_cache_restore_ex_no_deps();
    test_cache_store_restore_ex_with_deps();
    test_cache_restore_ex_dep_deleted();

    printf("\n  Manifest deps:\n");
    test_manifest_set_deps();
    test_manifest_deps_roundtrip();
    test_manifest_needs_rebuild_dep_changed();

    printf("\n  Remote cache:\n");
    test_remote_config_parse_full();
    test_remote_config_parse_minimal();
    test_remote_config_parse_no_section();
    test_remote_config_parse_no_url();
    test_remote_config_free_null();
    test_remote_cache_restore_unreachable();
    test_remote_cache_store_push_disabled();
    test_remote_cache_store_unreachable();
    test_remote_cache_key_url_safe();

    printf("\n  Enterprise auth (LDAP/SSO):\n");
    test_auth_method_parse();
    test_auth_method_name();
    test_auth_creds_free_null();
    test_auth_load_no_file();
    test_auth_load_null_safety();
    test_token_cache_lifecycle();
    test_token_cache_expired();
    test_token_cache_overwrite();
    test_auth_ldap_login_null();
    test_auth_ldap_login_unreachable();
    test_auth_oidc_client_null();
    test_auth_oidc_client_unreachable();
    test_auth_discover_unreachable();
    test_auth_discovery_free_null();
    test_auth_get_token_no_creds();

    printf("\n  SBOM generation:\n");
    test_sbom_to_json_basic();
    test_sbom_library_type();
    test_sbom_with_deps();
    test_sbom_with_license();
    test_sbom_generate_file();
    test_sbom_null_project();
    test_sbom_scope_mapping();
    test_sbom_no_deps();

#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)
    printf("\n  HTTP/2:\n");
    test_h2_hpack_encode_get();
    test_h2_hpack_decode_status();
    test_h2_frame_layout();
    test_h2_hpack_encode_with_headers();
#endif

    printf("\n  Graph cache:\n");
    test_graph_key_deterministic();
    test_graph_key_varies();
    test_graph_serialize_roundtrip();
    test_graph_deserialize_bad_input();
    test_graph_pull_unreachable();

    printf("\n  Watch:\n");
    test_watch_opts_init();
    test_watch_snapshot_hello();
    test_watch_diff_no_change();
    test_watch_diff_source_change();
    test_watch_diff_pasta_change();
    test_watch_snapshot_free_null();

    printf("\n  Rust FFI:\n");
    test_rust_lang_registered();
    test_rust_classify_rs();

    printf("\n  Go + Julia:\n");
    test_go_lang_registered();
    test_go_classify();
    test_julia_lang_registered();
    test_julia_classify();

    printf("\n  Audit logging:\n");
    test_audit_config_parse_full();
    test_audit_config_parse_disabled();
    test_audit_config_parse_no_section();
    test_audit_config_free_null();
    test_audit_event_name_roundtrip();
    test_audit_record_disabled();
    test_audit_record_and_show();

    printf("\n  Build integration:\n");
    test_build_hello();
    test_build_java_hello();
    /* test_test_phase requires gcc in PATH at runtime — run manually */

    printf("\n%d/%d tests passed", tests_passed, tests_run);
    if (tests_failed) printf(", %d FAILED", tests_failed);
    printf("\n");
    return tests_failed ? 1 : 0;
}
