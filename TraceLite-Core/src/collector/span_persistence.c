#include "span_persistence.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

#include "cJSON.h"

int tl_span_persistence_append(const char *path, const tl_span_t *span,
                               unsigned int anomaly_flags)
{
    cJSON *json;
    char trace_id[32];
    char span_id[32];
    char parent_span_id[32];
    char start_ns[32];
    char duration_ns[32];
    char *line;
    FILE *file;
    int saved_errno;

    if (path == NULL || path[0] == '\0' || span == NULL) {
        errno = EINVAL;
        return -1;
    }

    snprintf(trace_id, sizeof(trace_id), "%" PRIu64 "", span->trace_id);
    snprintf(span_id, sizeof(span_id), "%" PRIu64 "", span->span_id);
    snprintf(parent_span_id, sizeof(parent_span_id), "%" PRIu64 "",
             span->parent_span_id);
    snprintf(start_ns, sizeof(start_ns), "%" PRId64 "",
             span->start_monotonic_ns);
    snprintf(duration_ns, sizeof(duration_ns), "%" PRId64 "",
             span->duration_ns);

    json = cJSON_CreateObject();
    if (json == NULL ||
        !cJSON_AddStringToObject(json, "trace_id", trace_id) ||
        !cJSON_AddStringToObject(json, "span_id", span_id) ||
        !cJSON_AddStringToObject(json, "parent_span_id", parent_span_id) ||
        !cJSON_AddStringToObject(json, "service", span->service_name) ||
        !cJSON_AddStringToObject(json, "operation", span->operation_name) ||
        !cJSON_AddStringToObject(json, "start_monotonic_ns", start_ns) ||
        !cJSON_AddStringToObject(json, "duration_ns", duration_ns) ||
        !cJSON_AddNumberToObject(json, "status", span->status) ||
        !cJSON_AddNumberToObject(json, "anomaly_flags", anomaly_flags) ||
        !cJSON_AddStringToObject(json, "error_message", span->error_message)) {
        cJSON_Delete(json);
        errno = ENOMEM;
        return -1;
    }

    line = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (line == NULL) {
        errno = ENOMEM;
        return -1;
    }

    file = fopen(path, "a");
    if (file == NULL) {
        cJSON_free(line);
        return -1;
    }
    if (fputs(line, file) == EOF || fputc('\n', file) == EOF ||
        fflush(file) != 0 || fsync(fileno(file)) != 0) {
        saved_errno = errno;
        fclose(file);
        cJSON_free(line);
        errno = saved_errno;
        return -1;
    }
    if (fclose(file) != 0) {
        saved_errno = errno;
        cJSON_free(line);
        errno = saved_errno;
        return -1;
    }

    cJSON_free(line);
    return 0;
}
