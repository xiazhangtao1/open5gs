/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "proto/ue-events.h"
#include "sbi/openapi/external/cJSON.h"
#include <pthread.h>

typedef struct { const char *key, *value; } arg_t;
static const char *argument(void *request, const char *key)
{
    const arg_t *args = request;
    for (; args && args->key; args++) if (!strcmp(args->key, key)) return args->value;
    return NULL;
}
static cJSON *query(const arg_t *args, int expected)
{
    int status = 0;
    char *text = ogs_ue_events_query(argument, (void *)args, &status);
    ogs_assert(text && status == expected);
    cJSON *root = cJSON_Parse(text);
    free(text); ogs_assert(root);
    return root;
}
static const cJSON *field(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
static int count(const cJSON *o) { return cJSON_GetArraySize(field(o, "items")); }
static uint64_t record(uint64_t trace, const char *supi, const char *name, bool action)
{
    ogs_ue_event_t e = {.trace_id = trace, .action = action};
    snprintf(e.supi, sizeof(e.supi), "%s", supi ? supi : "");
    snprintf(e.name, sizeof(e.name), "%s", name);
    snprintf(e.protocol, sizeof(e.protocol), "TEST");
    snprintf(e.result, sizeof(e.result), "received");
    return ogs_ue_events_record(&e);
}
static void test_binding_and_release(void)
{
    ogs_assert(ogs_ue_events_init("AMF", 32, 16));
    uint64_t trace = ogs_ue_events_new_trace();
    ogs_assert(record(trace, NULL, "InitialUEMessage", false) == 1);
    ogs_assert(record(trace, NULL, "Registration", true) == 2);
    arg_t args[] = {{"supi", "imsi-001010000000001"}, {"detail", "1"}, {NULL, NULL}};
    cJSON *root = query(args, 200); ogs_assert(count(root) == 0); cJSON_Delete(root);
    ogs_ue_events_bind(trace, "imsi-001010000000001");
    root = query(args, 200); ogs_assert(count(root) == 3);
    const cJSON *first = cJSON_GetArrayItem(field(root, "items"), 0);
    ogs_assert(!strcmp(field(first, "name")->valuestring, "InitialUEMessage"));
    ogs_assert(!strcmp(field(first, "seq")->valuestring, "1"));
    cJSON_Delete(root);
    /* Duplicate binding must not emit repeated IdentityResolved events. */
    ogs_ue_events_bind(trace, "imsi-001010000000001");
    root = query(args, 200); ogs_assert(count(root) == 3); cJSON_Delete(root);
    /* A different lifetime cannot inherit the identity from a reused pool ID. */
    uint64_t replacement = ogs_ue_events_new_trace(); ogs_assert(replacement != trace);
    record(replacement, "imsi-001010000000002", "Registration", true);
    root = query(args, 200); ogs_assert(count(root) == 3); cJSON_Delete(root);
    arg_t cursor[] = {{"supi", args[0].value}, {"after", "2"}, {NULL, NULL}};
    root = query(cursor, 200); ogs_assert(count(root) == 1);
    ogs_assert(!strcmp(field(cJSON_GetArrayItem(field(root, "items"), 0), "name")->valuestring, "IdentityResolved"));
    cJSON_Delete(root);
}
static void test_capacity_and_cursors(void)
{
    /* Late identity resolution must not retain older entries ahead of newer ones. */
    ogs_assert(ogs_ue_events_init("AMF", 8, 2));
    uint64_t anonymous = ogs_ue_events_new_trace();
    record(anonymous, NULL, "old-access", true);
    record(ogs_ue_events_new_trace(), "same-user", "new-access", true);
    ogs_ue_events_bind(anonymous, "same-user");
    arg_t same[] = {{"supi", "same-user"}, {"detail", "1"}, {NULL, NULL}};
    cJSON *ordered = query(same, 200);
    ogs_assert(count(ordered) == 2);
    ogs_assert(!strcmp(field(cJSON_GetArrayItem(field(ordered, "items"), 0), "seq")->valuestring, "2"));
    cJSON_Delete(ordered);
    ogs_assert(ogs_ue_events_init("SMF", 8, 3));
    uint64_t a = ogs_ue_events_new_trace(), b = ogs_ue_events_new_trace();
    for (int i = 0; i < 10; i++) record(a, "a", "modify", true);
    arg_t a_args[] = {{"supi", "a"}, {NULL, NULL}};
    cJSON *root = query(a_args, 200); ogs_assert(count(root) == 3); cJSON_Delete(root);
    for (int i = 0; i < 10; i++) record(b, "b", "modify", true);
    root = query(a_args, 200); ogs_assert(count(root) == 3); cJSON_Delete(root);
    for (int i = 0; i < 30; i++) record(ogs_ue_events_new_trace(), "", "unknown", true);
    root = query(NULL, 200); ogs_assert(count(root) == 8); cJSON_Delete(root);
    arg_t args[] = {{"after", "0"}, {"limit", "2"}, {NULL, NULL}};
    root = query(args, 200); ogs_assert(count(root) == 2);
    ogs_assert(cJSON_IsTrue(field(root, "gap")) && cJSON_IsTrue(field(root, "has_more")));
    char next[32]; snprintf(next, sizeof(next), "%s", field(root, "next_seq")->valuestring); cJSON_Delete(root);
    args[0].value = next;
    root = query(args, 200); ogs_assert(count(root) == 2); cJSON_Delete(root);
    /* Churn index groups and slot reuse far beyond the number of retained UEs. */
    for (int i = 0; i < 10000; i++) {
        uint64_t t = ogs_ue_events_new_trace();
        record(t, NULL, "access", false);
        ogs_ue_events_bind(t, i % 2 ? "a" : "b");
    }
    root = query(NULL, 200); ogs_assert(count(root) <= 6); cJSON_Delete(root);
}
static void test_errors_restart_and_u64(void)
{
    arg_t bad[] = {{"limit", "1001"}, {NULL, NULL}};
    cJSON *root = query(bad, 400); cJSON_Delete(root);
    bad[0].key = "after"; bad[0].value = "-1"; root = query(bad, 400); cJSON_Delete(root);
    bad[0].key = "since_us"; bad[0].value = "18446744073709551615"; root = query(bad, 400); cJSON_Delete(root);
    ogs_assert(ogs_ue_events_init("AMF", 16, 8));
    root = query(NULL, 200); char boot[64]; snprintf(boot, sizeof(boot), "%s", field(root, "boot_id")->valuestring); cJSON_Delete(root);
    ogs_assert(ogs_ue_events_init("AMF", 16, 8));
    ogs_ue_event_t e = {.trace_id = ogs_ue_events_new_trace(), .action = true, .upf_seid = UINT64_MAX};
    memset(e.info, 'x', sizeof(e.info));
    ogs_ue_events_record(&e);
    arg_t args[] = {{"boot_id", boot}, {"after", "100"}, {NULL, NULL}};
    root = query(args, 200); ogs_assert(count(root) == 1 && cJSON_IsTrue(field(root, "reset")));
    const cJSON *event = cJSON_GetArrayItem(field(root, "items"), 0);
    ogs_assert(!strcmp(field(event, "upf_seid")->valuestring, "18446744073709551615"));
    ogs_assert(strlen(field(event, "info")->valuestring) == sizeof(e.info)-1); cJSON_Delete(root);
    args[0].key = "trace_boot_id"; args[1].key = "trace_id"; args[1].value = "1";
    root = query(args, 200); ogs_assert(count(root) == 0); cJSON_Delete(root);
    ogs_assert(!ogs_ue_events_init("AMF", 0, 1));
    ogs_assert(!record(1, "a", "ignored", true));
    root = query(NULL, 200); ogs_assert(count(root) == 0 && cJSON_IsFalse(field(root, "enabled"))); cJSON_Delete(root);
}
static void *writer(void *unused)
{
    uint64_t t = ogs_ue_events_new_trace();
    for (int i = 0; i < 1000; i++) record(t, "a", "concurrent", true);
    return NULL;
}
static void test_concurrency(void)
{
    pthread_t a, b;
    ogs_assert(ogs_ue_events_init("AMF", 128, 64));
    ogs_assert(!pthread_create(&a, NULL, writer, NULL));
    ogs_assert(!pthread_create(&b, NULL, writer, NULL));
    for (int i = 0; i < 100; i++) { cJSON *root = query(NULL, 200); ogs_assert(count(root) <= 64); cJSON_Delete(root); }
    pthread_join(a, NULL); pthread_join(b, NULL);
    cJSON *root = query(NULL, 200); ogs_assert(count(root) == 64); cJSON_Delete(root);
}
int main(void)
{
    ogs_core_initialize();
    test_binding_and_release(); test_capacity_and_cursors(); test_errors_restart_and_u64(); test_concurrency();
    ogs_ue_events_final(); ogs_core_terminate(); talloc_disable_null_tracking();
    puts("UE events: identity, retention, cursors, bounds, restart and concurrency passed");
    return 0;
}
