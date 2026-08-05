#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"

typedef void *httpd_handle_t;

typedef struct httpd_req {
    const char *query_string;
    const char *response_type;
    const char *response_status;
    const char *response_body;
    size_t response_length;
    const char *header_names[12];
    const char *header_values[12];
    size_t header_count;
} httpd_req_t;

typedef esp_err_t (*httpd_uri_func_t)(httpd_req_t *request);

typedef struct {
    const char *uri;
    int method;
    httpd_uri_func_t handler;
    void *user_ctx;
} httpd_uri_t;

typedef struct {
    int stack_size;
    int max_uri_handlers;
    bool lru_purge_enable;
    uint16_t server_port;
    uint16_t ctrl_port;
    size_t max_open_sockets;
    uint16_t send_wait_timeout;
} httpd_config_t;

#define HTTP_GET 0
#define HTTPD_RESP_USE_STRLEN ((long)-1)
#define HTTPD_400 "400 Bad Request"
#define HTTPD_409 "409 Conflict"
#define HTTPD_503 "503 Service Unavailable"
#define HTTPD_500_INTERNAL_SERVER_ERROR "500 Internal Server Error"
#define HTTPD_DEFAULT_CONFIG() \
    ((httpd_config_t){.stack_size = 4096, \
                      .max_uri_handlers = 8, \
                      .lru_purge_enable = false, \
                      .server_port = 80, \
                      .ctrl_port = 32768, \
                      .max_open_sockets = 7, \
                      .send_wait_timeout = 5})

esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t server);
esp_err_t httpd_register_uri_handler(httpd_handle_t server,
                                     const httpd_uri_t *uri);
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request,
                             const char *name,
                             const char *value);
esp_err_t httpd_resp_send(httpd_req_t *request,
                          const char *body,
                          long length);
esp_err_t httpd_resp_send_chunk(httpd_req_t *request,
                                const char *chunk,
                                ssize_t length);
esp_err_t httpd_resp_sendstr(httpd_req_t *request, const char *body);
esp_err_t httpd_resp_send_err(httpd_req_t *request,
                              const char *status,
                              const char *message);
size_t httpd_req_get_url_query_len(httpd_req_t *request);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *request,
                                      char *buffer,
                                      size_t buffer_length);
esp_err_t httpd_query_key_value(const char *query,
                                const char *key,
                                char *value,
                                size_t value_length);
