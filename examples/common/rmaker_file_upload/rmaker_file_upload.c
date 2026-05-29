/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rmaker_file_upload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_rmaker_core.h"      /* esp_rmaker_cmd_response_publish() */
#include "esp_rmaker_cmd_resp.h"  /* cmd register/prepare, user roles */
#include "json_parser.h"

#define TAG "rmaker_file_upload"

/* RainMaker cloud-backend device file-upload protocol (cloud_backend MR !1969).
 *
 * Both the upload-request and the upload-confirm go on the wire with cmd id
 * FILE_UPLOAD_COMMAND (16). The cloud's dispatch switches on the presence of
 * a request_id: empty → treat as request, non-empty → treat as confirm and
 * run ConfirmFileUpload. The cloud's *reply* to the confirm uses cmd id 20
 * (FILE_UPLOAD_SUCCESSFUL_COMMAND), which is reply-only. */
#define CMD_FILE_UPLOAD_COMMAND             16
#define CMD_FILE_UPLOAD_SUCCESSFUL_COMMAND  20

#define URL_MAX_LEN          2048
#define FILE_ID_MAX_LEN      128
#define FILE_NAME_MAX        96
#define CONTENT_TYPE_MAX     32
#define REQ_BODY_MAX         (FILE_NAME_MAX + 32)
#define CONFIRM_BODY_MAX     (FILE_ID_MAX_LEN + CONTENT_TYPE_MAX + 64)
#define RESP_WAIT_MS         10000
#define PUT_TIMEOUT_MS       15000
#define WORKER_STACK         6144
#define UPLOAD_QUEUE_DEPTH   4
#define S3_PUT_CHUNK         (4 * 1024)

typedef struct {
    char    filename[FILE_NAME_MAX];
    char    content_type[CONTENT_TYPE_MAX];
    void   *data;
    size_t  len;
} upload_job_t;

static QueueHandle_t      s_upload_q;
static SemaphoreHandle_t  s_url_sem;       /* signalled when upload-URL reply arrives */
static SemaphoreHandle_t  s_confirm_sem;   /* signalled when confirm ack arrives      */

/* Reply data populated by the cmd-16 reply handler — accessed only from the
 * worker task between request and S3 PUT, so plain statics are fine. */
static char  s_url[URL_MAX_LEN];
static char  s_file_id[FILE_ID_MAX_LEN];
static char  s_req_id[REQ_ID_LEN];
static bool  s_url_valid;
static bool  s_confirm_ok;

/* Helper: log a parsed error_response (cloud returns this shape on failures). */
static void log_error_response(jparse_ctx_t *jctx, const char *what)
{
    char desc[96] = "";
    int status = 0;
    if (json_obj_get_object(jctx, "device_file_err") == OS_SUCCESS ||
        json_obj_get_object(jctx, "error_response") == OS_SUCCESS) {
        json_obj_get_string(jctx, "description", desc, sizeof(desc));
        json_obj_get_int(jctx, "status", &status);
        json_obj_leave_object(jctx);
        ESP_LOGE(TAG, "%s: cloud error status=%d desc=%s", what, status, desc);
    } else {
        ESP_LOGE(TAG, "%s: no recognisable fields in reply", what);
    }
}

/* CMD 16 reply handler: cloud returned FileUploadResponse { file_id, url, ... }. */
static esp_err_t file_upload_resp_handler(const void *in_data, size_t in_len, void **out_data,
                                          size_t *out_len, esp_rmaker_cmd_ctx_t *ctx, void *priv)
{
    if (out_data) { *out_data = NULL; }
    if (out_len)  { *out_len = 0; }

    s_url_valid = false;
    s_url[0] = '\0';
    s_file_id[0] = '\0';
    s_req_id[0] = '\0';

    if (!in_data || !in_len) {
        if (s_url_sem) xSemaphoreGive(s_url_sem);
        return ESP_OK;
    }

    if (ctx && ctx->req_id[0]) {
        strncpy(s_req_id, ctx->req_id, sizeof(s_req_id) - 1);
        s_req_id[sizeof(s_req_id) - 1] = '\0';
    }

    ESP_LOGI(TAG, "upload-req reply (%u): %.*s", (unsigned)in_len, (int)in_len, (const char *)in_data);

    jparse_ctx_t jctx;
    if (json_parse_start(&jctx, (const char *)in_data, in_len) == OS_SUCCESS) {
        if (json_obj_get_string(&jctx, "url", s_url, sizeof(s_url)) == OS_SUCCESS && s_url[0]) {
            json_obj_get_string(&jctx, "file_id", s_file_id, sizeof(s_file_id));
            s_url_valid = true;
        } else {
            log_error_response(&jctx, "upload request");
        }
        json_parse_end(&jctx);
    } else {
        ESP_LOGE(TAG, "upload-req reply: JSON parse failed");
    }

    if (s_url_sem) xSemaphoreGive(s_url_sem);
    return ESP_OK;
}

/* CMD 20 reply handler: cloud ack of the file-upload confirm. */
static esp_err_t file_confirm_resp_handler(const void *in_data, size_t in_len, void **out_data,
                                           size_t *out_len, esp_rmaker_cmd_ctx_t *ctx, void *priv)
{
    if (out_data) { *out_data = NULL; }
    if (out_len)  { *out_len = 0; }

    s_confirm_ok = false;

    if (in_data && in_len) {
        ESP_LOGI(TAG, "confirm reply (%u): %.*s", (unsigned)in_len, (int)in_len, (const char *)in_data);
        jparse_ctx_t jctx;
        if (json_parse_start(&jctx, (const char *)in_data, in_len) == OS_SUCCESS) {
            char desc[16] = "";
            bool has_err = (json_obj_get_object(&jctx, "device_file_err") == OS_SUCCESS) ||
                           (json_obj_get_object(&jctx, "error_response") == OS_SUCCESS);
            if (has_err) {
                json_obj_get_string(&jctx, "description", desc, sizeof(desc));
                ESP_LOGE(TAG, "confirm: cloud error: %s", desc);
                json_obj_leave_object(&jctx);
            } else {
                s_confirm_ok = true;
            }
            json_parse_end(&jctx);
        } else {
            s_confirm_ok = true;
        }
    } else {
        s_confirm_ok = true;
    }

    if (s_confirm_sem) xSemaphoreGive(s_confirm_sem);
    return ESP_OK;
}

/* Publish a device-initiated cmd-resp message with a JSON body and wait for
 * the cloud's reply on @p sem. */
static esp_err_t send_cmd_and_wait(uint16_t cmd, const char *body,
                                   SemaphoreHandle_t sem, TickType_t wait_ticks)
{
    void *payload = NULL;
    size_t payload_len = 0;
    esp_err_t err = esp_rmaker_cmd_resp_prepare_response_payload(NULL, ESP_RMAKER_USER_ROLE_NODE,
                        cmd, body, strlen(body), &payload, &payload_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prepare cmd=%u failed: %s", cmd, esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(sem, 0);  /* drop any stale signal */
    err = esp_rmaker_cmd_response_publish(payload, payload_len); /* frees payload */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "publish cmd=%u failed: %s", cmd, esp_err_to_name(err));
        return err;
    }
    if (xSemaphoreTake(sem, wait_ticks) != pdTRUE) {
        ESP_LOGE(TAG, "cmd=%u: timed out waiting for cloud reply", cmd);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* PUT bytes to the pre-signed S3 URL. Content-Type is intentionally NOT set on
 * the HTTP request — the cloud signs the URL without that header, and adding
 * an unsigned header trips S3's SignatureDoesNotMatch. The file's content
 * type is stored on the cloud via the upload-confirm payload instead. */
static esp_err_t put_to_s3(const char *url, const uint8_t *data, size_t len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_PUT,
        .timeout_ms = PUT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Pre-signed S3 URLs are long (SigV4 query params push them past 1.5KB);
         * the default 512-byte internal buffer trips "Out of buffer" on open. */
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    /* Write in fixed-size chunks so the progress log gets multiple data points;
     * esp_http_client_write() blocks until the full requested size is drained,
     * so passing `len - sent` in one shot would leave us with only a 100% line. */
    size_t sent = 0;
    int next_pct = 10;
    while (sent < len) {
        size_t want = len - sent;
        if (want > S3_PUT_CHUNK) want = S3_PUT_CHUNK;
        int w = esp_http_client_write(client, (const char *)data + sent, want);
        if (w < 0) {
            ESP_LOGE(TAG, "http write error at %u/%u", (unsigned)sent, (unsigned)len);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        sent += w;
        int pct = (int)((sent * 100ULL) / len);
        if (pct >= next_pct) {
            ESP_LOGI(TAG, "S3 PUT progress: %d%% (%u/%u bytes)",
                     pct, (unsigned)sent, (unsigned)len);
            next_pct = pct + 10;
        }
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "S3 PUT HTTP %d (%u bytes)", status, (unsigned)len);
    esp_http_client_cleanup(client);
    return (status == 200 || status == 204) ? ESP_OK : ESP_FAIL;
}

static esp_err_t upload_one(const upload_job_t *job)
{
    /* Step 1: ask the cloud for a pre-signed URL. */
    char req[REQ_BODY_MAX];
    snprintf(req, sizeof(req), "{\"file_name\":\"%s\"}", job->filename);

    esp_err_t err = send_cmd_and_wait(CMD_FILE_UPLOAD_COMMAND, req, s_url_sem,
                                      pdMS_TO_TICKS(RESP_WAIT_MS));
    if (err != ESP_OK || !s_url_valid) {
        ESP_LOGE(TAG, "upload-request failed for %s", job->filename);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    ESP_LOGI(TAG, "got file_id=%s url-len=%u", s_file_id, (unsigned)strlen(s_url));

    /* Step 2: PUT the bytes. */
    err = put_to_s3(s_url, (const uint8_t *)job->data, job->len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "S3 PUT failed for %s", job->filename);
        return err;
    }

    /* Step 3: confirm. Carry the cmd-16 request_id as the correlator so the
     * cloud's dispatch runs ConfirmFileUpload and writes the file_details row. */
    char confirm[CONFIRM_BODY_MAX];
    snprintf(confirm, sizeof(confirm),
             "{\"file_id\":\"%s\",\"file_type\":\"%s\"}", s_file_id, job->content_type);
    void *payload = NULL;
    size_t payload_len = 0;
    err = esp_rmaker_cmd_resp_prepare_response_payload(s_req_id[0] ? s_req_id : NULL,
                ESP_RMAKER_USER_ROLE_NODE, CMD_FILE_UPLOAD_COMMAND,
                confirm, strlen(confirm), &payload, &payload_len);
    if (err == ESP_OK) {
        err = esp_rmaker_cmd_response_publish(payload, payload_len); /* frees payload */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "confirm publish failed: %s (file PUT succeeded though)",
                 esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "file upload committed (file_id=%s)", s_file_id);
    return ESP_OK;
}

static void upload_worker(void *arg)
{
    upload_job_t job;
    while (true) {
        if (xQueueReceive(s_upload_q, &job, portMAX_DELAY) == pdTRUE) {
            esp_err_t err = upload_one(&job);
            ESP_LOGI(TAG, "upload %s: %s", job.filename,
                     err == ESP_OK ? "OK" : esp_err_to_name(err));
            free(job.data);
        }
    }
}

esp_err_t rmaker_file_upload_init(void)
{
    if (s_upload_q) return ESP_OK; /* already initialized */

    s_url_sem     = xSemaphoreCreateBinary();
    s_confirm_sem = xSemaphoreCreateBinary();
    s_upload_q    = xQueueCreate(UPLOAD_QUEUE_DEPTH, sizeof(upload_job_t));
    if (!s_url_sem || !s_confirm_sem || !s_upload_q) return ESP_ERR_NO_MEM;

    /* Cloud replies to the upload-request with cmd 16 (carries url + file_id),
     * and to the upload-confirm with cmd 20 (ack). Register both handlers. */
    esp_err_t err = esp_rmaker_cmd_register(CMD_FILE_UPLOAD_COMMAND, ESP_RMAKER_USER_ROLE_NODE,
                                            file_upload_resp_handler, false, NULL);
    if (err == ESP_OK) {
        err = esp_rmaker_cmd_register(CMD_FILE_UPLOAD_SUCCESSFUL_COMMAND, ESP_RMAKER_USER_ROLE_NODE,
                                      file_confirm_resp_handler, false, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cmd register failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(upload_worker, "rm_upload", WORKER_STACK, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t rmaker_file_upload_submit(const char *filename,
                                    void *data, size_t len,
                                    const char *content_type)
{
    if (!s_upload_q) {
        ESP_LOGE(TAG, "rmaker_file_upload_init() not called");
        return ESP_ERR_INVALID_STATE;
    }
    if (!filename || !data || !len || !content_type) {
        return ESP_ERR_INVALID_ARG;
    }

    upload_job_t job = {0};
    strncpy(job.filename, filename, sizeof(job.filename) - 1);
    strncpy(job.content_type, content_type, sizeof(job.content_type) - 1);
    job.data = data;
    job.len  = len;

    if (xQueueSend(s_upload_q, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "upload queue full, dropping %s", filename);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
