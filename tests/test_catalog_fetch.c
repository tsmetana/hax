/* SPDX-License-Identifier: MIT */
/* Each fetch scenario runs in a child because catalog_prefetch is process-wide and runs once. */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "catalog.h"
#include "harness.h"
#include "model_meta.h"
#include "provider.h"

/* Parent-made temp root; children carve their own XDG_CACHE_HOME under it. */
static char *g_root;

/* ---------------- one-shot HTTP server ---------------- */

struct test_server {
    int listen_fd;
    const char *body;
    int delay_ms;
    _Atomic int served; /* Response fully written; the fetch worker has its bytes. */
};

/* The timeout turns a missing client into a failed scenario rather than a hung test. */
static void *serve_once(void *arg)
{
    struct test_server *server = arg;
    struct pollfd poll_fd = {.fd = server->listen_fd, .events = POLLIN};
    if (poll(&poll_fd, 1, 10000) <= 0)
        return NULL;
    int client_fd = accept(server->listen_fd, NULL, NULL);
    if (client_fd < 0)
        return NULL;
    char request[2048];
    (void)!read(client_fd, request, sizeof(request));
    if (server->delay_ms > 0) {
        struct timespec delay = {server->delay_ms / 1000, (server->delay_ms % 1000) * 1000000L};
        nanosleep(&delay, NULL);
    }
    dprintf(client_fd, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
            strlen(server->body), server->body);
    close(client_fd);
    atomic_store(&server->served, 1);
    return NULL;
}

static int server_listen(struct test_server *server)
{
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0)
        return -1;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server->listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listen_fd, 1) != 0)
        return -1;
    socklen_t address_length = sizeof(address);
    if (getsockname(server->listen_fd, (struct sockaddr *)&address, &address_length) != 0)
        return -1;
    return ntohs(address.sin_port);
}

/* ---------------- child-side helpers ---------------- */

/* Point the module at a private cache dir and the scenario's server.
 * catalog.refresh=1ms makes any existing snapshot count as stale, so the
 * fetch always spawns. */
static void child_env(const char *name, int port)
{
    char dir[512], url[64];
    snprintf(dir, sizeof(dir), "%s/%s", g_root, name);
    mkdir(dir, 0755);
    setenv("XDG_CACHE_HOME", dir, 1);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/api.json", port);
    setenv("HAX_CATALOG_URL", url, 1);
    setenv("HAX_CATALOG_REFRESH", "1ms", 1);
}

static void write_snapshot(const char *json)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/hax", getenv("XDG_CACHE_HOME"));
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", getenv("XDG_CACHE_HOME"));
    FILE *f = fopen(path, "w");
    if (!f)
        FAIL("fopen %s: %s", path, strerror(errno));
    fputs(json, f);
    fclose(f);
}

static void backdate_snapshot_days(long days)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/hax/catalog.json", getenv("XDG_CACHE_HOME"));
    struct timeval tv[2] = {{time(NULL) - days * 24 * 60 * 60, 0},
                            {time(NULL) - days * 24 * 60 * 60, 0}};
    EXPECT(utimes(path, tv) == 0);
}

/* Poll the asynchronous refresh for at most three seconds. */
static int wait_for_rate(const char *provider_id, const char *model, double expected_rate)
{
    for (int attempt = 0; attempt < 300; attempt++) {
        struct catalog_entry entry;
        if (catalog_lookup(NULL, provider_id, model, &entry) == 0 &&
            entry.cost_input == expected_rate)
            return 1;
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* ---------------- scenarios (each runs in its own child) ---------------- */

static void scenario_cold_start(void)
{
    /* The generation bump must invalidate the miss memoized while the first fetch runs. */
    struct test_server server = {.body = "{\"openai\": {\"models\": {"
                                         "\"m1\": {\"cost\": {\"input\": 7, \"output\": 1}}}}}"};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env("cold", port);
    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);

    catalog_prefetch();
    EXPECT(catalog_stale_days() == 0); /* no snapshot yet ⇒ nothing to be stale */
    EXPECT(wait_for_rate("openai", "m1", 7));

    pthread_join(server_thread, NULL);
    catalog_shutdown();
}

static void scenario_refresh_invalidates_memo(void)
{
    /* A stale snapshot answers (and is memoized) first; the refresh must
     * replace the file and the generation bump must invalidate the
     * memoized old value — the "estimates self-heal when a refresh lands
     * mid-session" contract. */
    struct test_server server = {.body = "{\"openai\": {\"models\": {"
                                         "\"m2\": {\"cost\": {\"input\": 9, \"output\": 1}}}}}"};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env("refresh", port);
    write_snapshot("{\"openai\": {\"models\": {"
                   "\"m2\": {\"cost\": {\"input\": 2, \"output\": 1}}}}}");

    struct catalog_entry entry;
    EXPECT(catalog_lookup(NULL, "openai", "m2", &entry) == 0);
    EXPECT(entry.cost_input == 2); /* old snapshot, now memoized */

    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);
    catalog_prefetch();
    EXPECT(catalog_stale_days() == 0); /* stale for the TTL, not for the alarm */
    EXPECT(wait_for_rate("openai", "m2", 9));

    pthread_join(server_thread, NULL);
    catalog_shutdown();
}

static void run_bad_payload_scenario(const char *name, const char *bad_body)
{
    struct test_server server = {.body = bad_body};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env(name, port);
    write_snapshot("{\"openai\": {\"models\": {"
                   "\"m3\": {\"cost\": {\"input\": 2, \"output\": 1}}}}}");

    struct catalog_entry entry;
    EXPECT(catalog_lookup(NULL, "openai", "m3", &entry) == 0);
    EXPECT(entry.cost_input == 2);

    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);
    catalog_prefetch();
    /* catalog_shutdown joins validation after the server has delivered the full response. */
    for (int i = 0; i < 300 && !atomic_load(&server.served); i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    EXPECT(atomic_load(&server.served));
    pthread_join(server_thread, NULL);
    catalog_shutdown();

    /* Shutdown clears the memo, forcing this lookup to read the snapshot on disk. */
    EXPECT(catalog_lookup(NULL, "openai", "m3", &entry) == 0);
    EXPECT(entry.cost_input == 2);
}

static void scenario_garbage_keeps_snapshot(void)
{
    /* A 200 response that isn't JSON at all (an HTML error page behind a
     * broken proxy) must never replace a working snapshot. */
    run_bad_payload_scenario("garbage", "<html>bad gateway</html>");
}

static void scenario_json_error_keeps_snapshot(void)
{
    /* A JSON-shaped error payload parses fine but lacks the catalog shape
     * (no provider entry carrying a models object) — it must be rejected
     * too, or its fresh mtime would suppress a recovering re-fetch for a
     * whole refresh interval. */
    run_bad_payload_scenario("json-error", "{\"error\": \"rate limited\"}");
}

static void scenario_truncated_tail_keeps_snapshot(void)
{
    /* A body whose prefix validates but which is cut mid-member (a proxy
     * truncation with a happens-to-match Content-Length) must be rejected
     * whole — accepting it would silently drop every provider after the
     * cut until the next refresh. */
    run_bad_payload_scenario("truncated-tail",
                             "{\"openai\": {\"models\": {\"m3\": {}}}, \"anthropic\":");
}

static void scenario_invalid_member_keeps_snapshot(void)
{
    /* Brace-balanced garbage after a valid member: the structural scan
     * alone would wave it through, so every member slice must survive a
     * real parse before the snapshot is replaced. */
    run_bad_payload_scenario("invalid-member",
                             "{\"openai\": {\"models\": {\"m3\": {}}}, \"tail\": wat}");
}

static void scenario_trailing_garbage_keeps_snapshot(void)
{
    /* Bytes after the root object's closing brace (a concatenated or
     * corrupted response) mean the body isn't the artifact — reject. */
    run_bad_payload_scenario("trailing-garbage",
                             "{\"openai\": {\"models\": {\"m3\": {}}}} garbage");
}

static void scenario_drain_completes_fetch(void)
{
    /* The one-shot exit path drains the in-flight fetch (bounded) instead
     * of letting shutdown cancel it: with a server slower than the run, a
     * post-drain lookup must already see the fetched values — no polling,
     * and no cold cache left behind. */
    struct test_server server = {.body = "{\"openai\": {\"models\": {"
                                         "\"m5\": {\"cost\": {\"input\": 7, \"output\": 1}}}}}",
                                 .delay_ms = 400};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env("drain", port);
    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);

    catalog_prefetch();
    catalog_drain(5000);
    struct catalog_entry entry;
    EXPECT(catalog_lookup(NULL, "openai", "m5", &entry) == 0);
    EXPECT(entry.cost_input == 7);

    pthread_join(server_thread, NULL);
    catalog_shutdown();
}

static void scenario_stale_snapshot_warns(void)
{
    /* A snapshot that hasn't refreshed for over the alarm window (~30d)
     * makes prefetch record its age for catalog_stale_days — the frontend's
     * cue to warn that estimates may have drifted — while the refresh it
     * spawns still recovers as usual. */
    struct test_server server = {.body = "{\"openai\": {\"models\": {"
                                         "\"m4\": {\"cost\": {\"input\": 9, \"output\": 1}}}}}",
                                 .delay_ms =
                                     300}; /* the age is read while the fetch is in flight */
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env("stale", port);
    write_snapshot("{\"openai\": {\"models\": {"
                   "\"m4\": {\"cost\": {\"input\": 2, \"output\": 1}}}}}");
    backdate_snapshot_days(40);

    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);
    catalog_prefetch();
    long stale_days = catalog_stale_days();
    EXPECT(stale_days >= 39 && stale_days <= 41);
    catalog_prefetch();                /* one fetch per run */
    EXPECT(catalog_stale_days() == 0); /* and one report */
    EXPECT(wait_for_rate("openai", "m4", 9));

    pthread_join(server_thread, NULL);
    catalog_shutdown();
}

static void scenario_wait_catalog_starts_fetch(void)
{
    /* A picker or pre-request wait on a catalog-backed provider is itself the trigger: nothing
     * has called catalog_prefetch before it, and the fetched values are visible when it returns,
     * without polling. */
    struct test_server server = {.body = "{\"openai\": {\"models\": {"
                                         "\"m6\": {\"cost\": {\"input\": 7, \"output\": 1}}}}}"};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env("wait-starts", port);
    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);

    struct provider provider = {.catalog_id = "openai"};
    model_meta_wait_catalog(&provider, 5000, NULL, NULL);
    struct catalog_entry entry;
    EXPECT(catalog_lookup(NULL, "openai", "m6", &entry) == 0);
    EXPECT(entry.cost_input == 7);

    pthread_join(server_thread, NULL);
    catalog_shutdown();
}

static void scenario_no_identity_never_fetches(void)
{
    /* A provider without a catalog identity (a local server) must not cause any request to the
     * catalog host, however the metadata path is exercised. */
    struct test_server server = {.body = "{}"};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env("no-identity", port);

    struct provider local = {.catalog_id = NULL};
    model_meta_prefetch(&local);
    model_meta_wait_catalog(&local, 5000, NULL, NULL);
    model_meta_wait_ms(&local, 5000);
    /* No connection may arrive on the listener within a generous grace period. */
    struct pollfd poll_fd = {.fd = server.listen_fd, .events = POLLIN};
    EXPECT(poll(&poll_fd, 1, 300) == 0);
    close(server.listen_fd);
    catalog_shutdown();
}

static int always_cancel(void *user)
{
    (void)user;
    return 1;
}

static void scenario_wait_honors_cancellation(void)
{
    /* A picker's Esc must dismiss the wait at once while the fetch keeps running to completion,
     * so the cache still warms for later callers. */
    struct test_server server = {.body = "{\"openai\": {\"models\": {"
                                         "\"m7\": {\"cost\": {\"input\": 7, \"output\": 1}}}}}",
                                 .delay_ms = 1500};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env("wait-cancel", port);
    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);

    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    catalog_prefetch();
    catalog_wait(5000, always_cancel, NULL);
    clock_gettime(CLOCK_MONOTONIC, &after);
    long elapsed_ms =
        (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
    EXPECT(elapsed_ms < 1000);
    EXPECT(wait_for_rate("openai", "m7", 7)); /* the fetch itself was not cancelled */

    pthread_join(server_thread, NULL);
    catalog_shutdown();
}

static void scenario_refresh_clears_stale_warning(void)
{
    /* When the refresh lands before the frontend reads the age — a picker waited for it — the
     * stale snapshot is gone and warning about it would be false. */
    struct test_server server = {.body = "{\"openai\": {\"models\": {"
                                         "\"m8\": {\"cost\": {\"input\": 9, \"output\": 1}}}}}"};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env("stale-refreshed", port);
    write_snapshot("{\"openai\": {\"models\": {"
                   "\"m8\": {\"cost\": {\"input\": 2, \"output\": 1}}}}}");
    backdate_snapshot_days(40);

    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);
    catalog_prefetch();
    catalog_wait(5000, NULL, NULL);
    struct catalog_entry entry;
    EXPECT(catalog_lookup(NULL, "openai", "m8", &entry) == 0);
    EXPECT(entry.cost_input == 9);
    EXPECT(catalog_stale_days() == 0);

    pthread_join(server_thread, NULL);
    catalog_shutdown();
}

/* ---------------- parent orchestration ---------------- */

static void run_scenario(const char *name, void (*scenario)(void))
{
    /* The include cleaner knows no direct glibc provider for pid_t here; its
     * typedef hides behind the ignored bits/ headers. */
    // NOLINTNEXTLINE(misc-include-cleaner)
    pid_t pid = fork();
    if (pid == 0) {
        scenario();
        _exit(t_failures ? 1 : 0);
    }
    EXPECT(pid > 0);
    if (pid <= 0)
        return;
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        FAIL("scenario '%s' failed in child (status 0x%x)", name, status);
}

int main(void)
{
    g_root = t_tempdir();

    run_scenario("cold-start", scenario_cold_start);
    run_scenario("refresh-invalidates-memo", scenario_refresh_invalidates_memo);
    run_scenario("garbage-keeps-snapshot", scenario_garbage_keeps_snapshot);
    run_scenario("json-error-keeps-snapshot", scenario_json_error_keeps_snapshot);
    run_scenario("truncated-tail-keeps-snapshot", scenario_truncated_tail_keeps_snapshot);
    run_scenario("invalid-member-keeps-snapshot", scenario_invalid_member_keeps_snapshot);
    run_scenario("trailing-garbage-keeps-snapshot", scenario_trailing_garbage_keeps_snapshot);
    run_scenario("drain-completes-fetch", scenario_drain_completes_fetch);
    run_scenario("stale-snapshot-warns", scenario_stale_snapshot_warns);
    run_scenario("wait-catalog-starts-fetch", scenario_wait_catalog_starts_fetch);
    run_scenario("no-identity-never-fetches", scenario_no_identity_never_fetches);
    run_scenario("wait-honors-cancellation", scenario_wait_honors_cancellation);
    run_scenario("refresh-clears-stale-warning", scenario_refresh_clears_stale_warning);

    T_REPORT();
}
