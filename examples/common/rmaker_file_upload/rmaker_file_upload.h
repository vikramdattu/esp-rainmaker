/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RainMaker device file upload (cloud_backend MR !1969 protocol).
 *
 * Wire flow per file:
 *   1. device  -> cloud   cmd 16, body { "file_name": "<name>" }
 *   2. cloud   -> device  reply (cmd 16) with file_id + pre-signed S3 URL
 *   3. device  -> S3      HTTP PUT bytes
 *   4. device  -> cloud   cmd 16, body { "file_id": "<id>" }, req_id = (1)'s req_id
 *      cloud writes file_details row (entity_type=node, entity_id=<node>)
 *
 * Submissions are async: the caller hands the buffer to the component and
 * returns immediately; an internal worker task drains the queue. The buffer
 * must be heap-allocated; the component free()s it after the upload completes
 * (success or failure). */

/* Initialize once after RainMaker is started — registers the cmd-resp reply
 * handlers and spawns the worker task. */
esp_err_t rmaker_file_upload_init(void);

/* Submit a file for asynchronous upload.
 *
 * @param filename     null-terminated name shown on the cloud / in the user's
 *                     file listing. Copied into the queue entry.
 * @param data         heap-allocated buffer to upload. Ownership transfers to
 *                     the component on ESP_OK return. Caller must NOT free.
 *                     On error return, ownership stays with the caller.
 * @param len          number of bytes in @p data.
 * @param content_type MIME type stored alongside the file (e.g. "image/jpeg",
 *                     "application/json"). Copied into the queue entry. */
esp_err_t rmaker_file_upload_submit(const char *filename,
                                    void *data, size_t len,
                                    const char *content_type);

#ifdef __cplusplus
}
#endif
