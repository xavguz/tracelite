#include "business_protocol.h"
#include "ipc.h"
#include "service_config.h"
#include "tracelite/tracelite.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct inventory { tl_product_record_t items[TL_PRODUCT_LIST_MAX]; uint32_t count; } inventory_t;

static const char *inventory_path(void)
{ const char *path = getenv("TRACELITE_INVENTORY_FILE"); return path && path[0] ? path : TL_INVENTORY_FILE; }

static void seed_delay_generator(void)
{
    struct timespec now;
    unsigned int seed = (unsigned int)getpid();

    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        seed ^= (unsigned int)now.tv_sec;
        seed ^= (unsigned int)now.tv_nsec;
    }
    srand(seed);
}

static int configured_test_delay_ms(uint32_t *delay_ms)
{
    const char *text = getenv("TRACELITE_ORDER_DELAY_MS");
    char *end;
    unsigned long value;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > 60000u) {
        return -1;
    }

    *delay_ms = (uint32_t)value;
    return 1;
}

static uint32_t order_delay_ms(void)
{
    uint32_t delay_ms;
    int configured = configured_test_delay_ms(&delay_ms);

    if (configured > 0) {
        return delay_ms;
    }
    if (configured < 0) {
        fprintf(stderr,
                "[inventory] TRACELITE_ORDER_DELAY_MS invalida; usando aleatoria\n");
    }

    return TL_ORDER_DELAY_MIN_MS +
           (uint32_t)(rand() % (TL_ORDER_DELAY_MAX_MS -
                                TL_ORDER_DELAY_MIN_MS + 1u));
}

static int sleep_ms(uint32_t delay_ms)
{
    struct timespec remaining;

    remaining.tv_sec = (time_t)(delay_ms / 1000u);
    remaining.tv_nsec = (long)(delay_ms % 1000u) * 1000000L;
    while (nanosleep(&remaining, &remaining) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static int read_all_file(const char *path, char **content)
{
    FILE *file; long size; char *buffer;
    *content = NULL; file = fopen(path, "rb"); if (!file) return -1;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET)) { fclose(file); errno = EIO; return -1; }
    buffer = malloc((size_t)size + 1u); if (!buffer) { fclose(file); return -1; }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size) { fclose(file); free(buffer); errno = EIO; return -1; }
    if (fclose(file) != 0) { free(buffer); return -1; }
    buffer[size] = '\0'; *content = buffer; return 0;
}

static int inventory_load(const char *path, inventory_t *inventory)
{
    char *text; cJSON *root, *entry; int index;
    memset(inventory, 0, sizeof(*inventory));
    if (read_all_file(path, &text) < 0) return -1;
    root = cJSON_Parse(text); free(text);
    if (!root || !cJSON_IsArray(root)) { cJSON_Delete(root); errno = EPROTO; return -1; }
    cJSON_ArrayForEach(entry, root) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(entry, "id");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        cJSON *stock = cJSON_GetObjectItemCaseSensitive(entry, "stock");
        if (inventory->count >= TL_PRODUCT_LIST_MAX || !cJSON_IsString(id) || !id->valuestring || !cJSON_IsString(name) || !name->valuestring || !cJSON_IsNumber(stock) || stock->valuedouble < 0 || stock->valuedouble > UINT32_MAX || stock->valuedouble != (double)(uint32_t)stock->valuedouble) { cJSON_Delete(root); errno = EPROTO; return -1; }
        index = (int)inventory->count++;
        snprintf(inventory->items[index].id, sizeof(inventory->items[index].id), "%s", id->valuestring);
        snprintf(inventory->items[index].name, sizeof(inventory->items[index].name), "%s", name->valuestring);
        inventory->items[index].stock = (uint32_t)stock->valuedouble;
        if (!inventory->items[index].id[0] || !inventory->items[index].name[0]) { cJSON_Delete(root); errno = EPROTO; return -1; }
    }
    cJSON_Delete(root); if (!inventory->count) { errno = EPROTO; return -1; } return 0;
}

static int inventory_save(const char *path, const inventory_t *inventory)
{
    cJSON *root = cJSON_CreateArray(); char *json, temporary[PATH_MAX]; int fd; FILE *file; uint32_t i;
    if (!root) return -1;
    for (i = 0; i < inventory->count; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry || !cJSON_AddStringToObject(entry, "id", inventory->items[i].id) || !cJSON_AddStringToObject(entry, "name", inventory->items[i].name) || !cJSON_AddNumberToObject(entry, "stock", inventory->items[i].stock) || !cJSON_AddItemToArray(root, entry)) { cJSON_Delete(entry); cJSON_Delete(root); errno = ENOMEM; return -1; }
    }
    json = cJSON_Print(root); cJSON_Delete(root); if (!json) return -1;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >= (int)sizeof(temporary)) { cJSON_free(json); errno = ENAMETOOLONG; return -1; }
    fd = mkstemp(temporary); if (fd < 0) { cJSON_free(json); return -1; }
    file = fdopen(fd, "w");
    if (file == NULL) {
        int saved_errno = errno;
        close(fd);
        unlink(temporary);
        cJSON_free(json);
        errno = saved_errno;
        return -1;
    }

    if (fputs(json, file) == EOF || fputc('\n', file) == EOF ||
        fflush(file) != 0 || fsync(fd) != 0) {
        int saved_errno = errno;
        fclose(file);
        unlink(temporary);
        cJSON_free(json);
        errno = saved_errno;
        return -1;
    }

    if (fclose(file) != 0) {
        int saved_errno = errno;
        unlink(temporary);
        cJSON_free(json);
        errno = saved_errno;
        return -1;
    }

    if (rename(temporary, path) != 0) {
        int saved_errno = errno;
        unlink(temporary);
        cJSON_free(json);
        errno = saved_errno;
        return -1;
    }

    cJSON_free(json); return 0;
}

static tl_product_record_t *find_item(inventory_t *inventory, const char *id)
{ uint32_t i; for (i = 0; i < inventory->count; i++) if (!strcmp(inventory->items[i].id, id)) return &inventory->items[i]; return NULL; }

static void process_request(const tl_inventory_request_t *request, tl_inventory_response_t *response)
{
    inventory_t inventory; tl_product_record_t *item; const char *path = inventory_path();
    memset(response, 0, sizeof(*response));
    if (request->operation < TL_INVENTORY_LIST_PRODUCTS || request->operation > TL_INVENTORY_RESERVE_STOCK) { response->status = TL_STOCK_STATUS_INVALID_REQUEST; snprintf(response->message, sizeof(response->message), "invalid operation"); return; }
    if (inventory_load(path, &inventory) < 0) { response->status = TL_STOCK_STATUS_INTERNAL_ERROR; snprintf(response->message, sizeof(response->message), "cannot read inventory: %s", strerror(errno)); return; }
    if (request->operation == TL_INVENTORY_LIST_PRODUCTS) { memcpy(response->products, inventory.items, sizeof(inventory.items)); response->product_count = inventory.count; response->status = TL_STOCK_STATUS_OK; snprintf(response->message, sizeof(response->message), "%u product(s)", inventory.count); return; }
    item = find_item(&inventory, request->product_id);
    if (!item) { response->status = TL_STOCK_STATUS_UNKNOWN_PRODUCT; snprintf(response->message, sizeof(response->message), "unknown product: %.80s", request->product_id); return; }
    if (request->operation == TL_INVENTORY_FIND_PRODUCT) { response->products[0] = *item; response->product_count = 1; response->status = TL_STOCK_STATUS_OK; snprintf(response->message, sizeof(response->message), "product found"); return; }
    if (!request->quantity) { response->status = TL_STOCK_STATUS_INVALID_REQUEST; snprintf(response->message, sizeof(response->message), "quantity must be positive"); return; }
    if (request->quantity > item->stock) { response->status = TL_STOCK_STATUS_OUT_OF_STOCK; response->products[0] = *item; response->product_count = 1; snprintf(response->message, sizeof(response->message), "insufficient stock"); return; }
    {
        uint32_t delay_ms = order_delay_ms();

        printf("[inventory] demora de %u ms para pedido\n", delay_ms);
        fflush(stdout);
        if (sleep_ms(delay_ms) < 0) {
            response->status = TL_STOCK_STATUS_INTERNAL_ERROR;
            snprintf(response->message, sizeof(response->message),
                     "cannot delay order: %s", strerror(errno));
            return;
        }
    }
    item->stock -= request->quantity;
    if (inventory_save(path, &inventory) < 0) { response->status = TL_STOCK_STATUS_INTERNAL_ERROR; snprintf(response->message, sizeof(response->message), "cannot update inventory: %s", strerror(errno)); return; }
    response->products[0] = *item; response->product_count = 1; response->status = TL_STOCK_STATUS_OK; snprintf(response->message, sizeof(response->message), "order completed");
}

static const char *child_operation(uint32_t operation)
{ return operation == TL_INVENTORY_LIST_PRODUCTS ? "list-products" : operation == TL_INVENTORY_FIND_PRODUCT ? "find-product" : "reserve-stock"; }

static int create_listening_socket(const char *path)
{
    struct sockaddr_un address; struct stat info; size_t length = strlen(path); int fd;
    if (ipc_ensure_parent_dir(path) < 0) return -1;
    if (length >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
    if (lstat(path, &info) == 0) { if (!S_ISSOCK(info.st_mode) || unlink(path) < 0) return -1; } else if (errno != ENOENT) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) return -1;
    memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX; memcpy(address.sun_path, path, length + 1u);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 1) < 0) { int e = errno; close(fd); unlink(path); errno = e; return -1; } return fd;
}

static int handle_connection(int fd, int tracing)
{
    for (;;) {
        tl_inventory_request_t request; tl_inventory_response_t response; tl_span_t span; uint32_t id; int span_started = 0;
        if (business_recv_request(fd, &request, &id) < 0) return errno == ECONNRESET ? 0 : -1;
        request.product_id[TL_PRODUCT_ID_MAX - 1u] = '\0';
        if (tracing && tl_span_start(&span, child_operation(request.operation), tl_context_is_valid(&request.trace_context) ? &request.trace_context : NULL, NULL) == 0) span_started = 1;
        process_request(&request, &response);
        if (business_send_response(fd, id, &response) < 0) { if (span_started) (void)tl_span_finish(&span, TL_SPAN_STATUS_ERROR, "response send failed"); return -1; }
        if (span_started) (void)tl_span_finish(&span, response.status == TL_STOCK_STATUS_OK ? TL_SPAN_STATUS_OK : TL_SPAN_STATUS_ERROR, response.status == TL_STOCK_STATUS_OK ? NULL : response.message);
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : TL_INVENTORY_SOCKET_PATH; int listen_fd = -1, client_fd = -1, ready_fd = -1, tracing, result = EXIT_FAILURE;
    if (argc > 3) { fprintf(stderr, "uso: %s [socket_path] [ready_fd]\n", argv[0]); return EXIT_FAILURE; }
    if (argc == 3) { ready_fd = atoi(argv[2]); if (ready_fd <= STDERR_FILENO) { errno = EINVAL; perror("[inventory] ready_fd"); return EXIT_FAILURE; } }
    tracing = tl_init("inventory-service") == 0;
    seed_delay_generator();
    listen_fd = create_listening_socket(path); if (listen_fd < 0) { perror("[inventory] socket"); goto cleanup; }
    if (ready_fd >= 0 && write(ready_fd, "R", 1) != 1) { perror("[inventory] ready"); goto cleanup; } if (ready_fd >= 0) { close(ready_fd); ready_fd = -1; }
    printf("[inventory] escuchando en %s; archivo=%s\n", path, inventory_path()); fflush(stdout);
    client_fd = accept(listen_fd, NULL, NULL); if (client_fd < 0 || handle_connection(client_fd, tracing) < 0) { perror("[inventory] connection"); goto cleanup; }
    result = EXIT_SUCCESS;
cleanup:
    if (client_fd >= 0)
        close(client_fd);
    if (listen_fd >= 0) {
        close(listen_fd);
        unlink(path);
    }
    if (ready_fd >= 0)
        close(ready_fd);
    if (tracing)
        tl_shutdown();
    return result;
}
