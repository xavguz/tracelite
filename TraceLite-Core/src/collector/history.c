#include "history.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "span_storage.h"
#include "trace_printer.h"

typedef struct history_flags {
    uint64_t trace_id;
    unsigned int flags;
} history_flags_t;

typedef struct history_data {
    tl_span_storage_t storage;
    history_flags_t *flags;
    size_t flag_count;
    size_t flag_capacity;
} history_data_t;

static int parse_u64(const cJSON *item, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (!cJSON_IsString(item) || item->valuestring == NULL) return -1;
    errno = 0;
    parsed = strtoull(item->valuestring, &end, 10);
    if (errno != 0 || end == item->valuestring || *end != '\0') return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_i64(const cJSON *item, int64_t *value)
{
    char *end;
    long long parsed;

    if (!cJSON_IsString(item) || item->valuestring == NULL) return -1;
    errno = 0;
    parsed = strtoll(item->valuestring, &end, 10);
    if (errno != 0 || end == item->valuestring || *end != '\0') return -1;
    *value = (int64_t)parsed;
    return 0;
}

static int copy_json_text(const cJSON *item, char *output, size_t output_size,
                          int allow_empty)
{
    size_t length;

    if (!cJSON_IsString(item) || item->valuestring == NULL) return -1;
    length = strlen(item->valuestring);
    if (length >= output_size || (!allow_empty && length == 0)) return -1;
    memcpy(output, item->valuestring, length + 1u);
    return 0;
}

static int parse_span(const char *line, tl_span_t *span, unsigned int *flags)
{
    cJSON *json = cJSON_Parse(line);
    cJSON *status;
    cJSON *anomaly;
    int valid = 0;

    memset(span, 0, sizeof(*span));
    *flags = 0;
    if (json == NULL ||
        parse_u64(cJSON_GetObjectItemCaseSensitive(json, "trace_id"), &span->trace_id) ||
        parse_u64(cJSON_GetObjectItemCaseSensitive(json, "span_id"), &span->span_id) ||
        parse_u64(cJSON_GetObjectItemCaseSensitive(json, "parent_span_id"), &span->parent_span_id) ||
        parse_i64(cJSON_GetObjectItemCaseSensitive(json, "start_monotonic_ns"), &span->start_monotonic_ns) ||
        parse_i64(cJSON_GetObjectItemCaseSensitive(json, "duration_ns"), &span->duration_ns) ||
        copy_json_text(cJSON_GetObjectItemCaseSensitive(json, "service"), span->service_name, sizeof(span->service_name), 0) ||
        copy_json_text(cJSON_GetObjectItemCaseSensitive(json, "operation"), span->operation_name, sizeof(span->operation_name), 0) ||
        copy_json_text(cJSON_GetObjectItemCaseSensitive(json, "error_message"), span->error_message, sizeof(span->error_message), 1)) {
        cJSON_Delete(json);
        return -1;
    }
    status = cJSON_GetObjectItemCaseSensitive(json, "status");
    anomaly = cJSON_GetObjectItemCaseSensitive(json, "anomaly_flags");
    if (cJSON_IsNumber(status) && status->valuedouble >= TL_SPAN_STATUS_OK &&
        status->valuedouble <= TL_SPAN_STATUS_CANCELLED &&
        status->valuedouble == (double)(int)status->valuedouble) {
        span->status = (int32_t)status->valuedouble;
        valid = 1;
    }
    if (cJSON_IsNumber(anomaly) && anomaly->valuedouble >= 0 &&
        anomaly->valuedouble <= 3) *flags = (unsigned int)anomaly->valuedouble;
    cJSON_Delete(json);
    return valid ? 0 : -1;
}

static void history_destroy(history_data_t *data)
{
    tl_span_storage_destroy(&data->storage);
    free(data->flags);
}

static int add_flags(history_data_t *data, uint64_t trace_id, unsigned int flags)
{
    size_t index;
    history_flags_t *new_flags;

    for (index = 0; index < data->flag_count; index++) {
        if (data->flags[index].trace_id == trace_id) {
            data->flags[index].flags |= flags;
            return 0;
        }
    }
    if (data->flag_count == data->flag_capacity) {
        size_t capacity = data->flag_capacity == 0 ? 8u : data->flag_capacity * 2u;
        new_flags = realloc(data->flags, capacity * sizeof(*new_flags));
        if (new_flags == NULL) return -1;
        data->flags = new_flags;
        data->flag_capacity = capacity;
    }
    data->flags[data->flag_count].trace_id = trace_id;
    data->flags[data->flag_count++].flags = flags;
    return 0;
}

static unsigned int flags_for(const history_data_t *data, uint64_t trace_id)
{
    size_t index;
    for (index = 0; index < data->flag_count; index++)
        if (data->flags[index].trace_id == trace_id) return data->flags[index].flags;
    return 0;
}

static int history_load(const char *path, history_data_t *data, FILE *stream)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    size_t line_number = 0;

    memset(data, 0, sizeof(*data));
    tl_span_storage_init(&data->storage);
    file = fopen(path, "r");
    if (file == NULL) return -1;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        tl_span_t span;
        unsigned int flags;
        line_number++;
        if (length > 0 && line[length - 1] == '\n') line[length - 1] = '\0';
        if (parse_span(line, &span, &flags) != 0 ||
            tl_span_storage_add(&data->storage, &span) != 0 ||
            add_flags(data, span.trace_id, flags) != 0) {
            fprintf(stream, "[history] linea %zu omitida: registro invalido\n", line_number);
            continue;
        }
    }
    free(line);
    if (ferror(file)) { fclose(file); history_destroy(data); return -1; }
    fclose(file);
    return 0;
}

static const tl_span_t *root_span(const tl_trace_group_t *group)
{
    size_t index;
    for (index = 0; index < group->span_count; index++)
        if (group->spans[index].parent_span_id == TL_INVALID_SPAN_ID)
            return &group->spans[index];
    return group->span_count ? &group->spans[0] : NULL;
}

int tl_history_list(const char *path, FILE *stream)
{
    history_data_t data;
    size_t index;
    if (history_load(path, &data, stream) < 0) return -1;
    fprintf(stream, "========== TRACE HISTORY ==========\n");
    for (index = 0; index < data.storage.group_count; index++) {
        const tl_trace_group_t *group = &data.storage.groups[index];
        const tl_span_t *root = root_span(group);
        fprintf(stream, "Trace ID: 0x%016" PRIx64 " | %s | %.2f ms | %s | anomalies=%s\n",
                group->trace_id, root->operation_name,
                (double)root->duration_ns / 1000000.0,
                root->status == TL_SPAN_STATUS_ERROR ? "ERROR" : "SUCCESS",
                flags_for(&data, group->trace_id) ? "yes" : "no");
    }
    history_destroy(&data);
    return 0;
}

int tl_history_find(const char *path, const char *trace_id, FILE *stream)
{
    history_data_t data;
    uint64_t id;
    size_t index;
    char *end;
    if (trace_id == NULL) { errno = EINVAL; return -1; }
    errno = 0; id = strtoull(trace_id, &end, 0);
    if (errno || end == trace_id || *end) { errno = EINVAL; return -1; }
    if (history_load(path, &data, stream) < 0) return -1;
    for (index = 0; index < data.storage.group_count; index++)
        if (data.storage.groups[index].trace_id == id) {
            fprintf(stream, "Trace ID: 0x%016" PRIx64 "\n", id);
            tl_trace_printer_print_group(&data.storage.groups[index], stream);
            history_destroy(&data); return 0;
        }
    history_destroy(&data); errno = ENOENT; return -1;
}

int tl_history_anomalies(const char *path, FILE *stream)
{
    history_data_t data;
    size_t index;
    if (history_load(path, &data, stream) < 0) return -1;
    for (index = 0; index < data.storage.group_count; index++) {
        const tl_trace_group_t *group = &data.storage.groups[index];
        if (flags_for(&data, group->trace_id))
            fprintf(stream, "Trace ID: 0x%016" PRIx64 " | anomaly_flags=%u\n",
                    group->trace_id, flags_for(&data, group->trace_id));
    }
    history_destroy(&data);
    return 0;
}
