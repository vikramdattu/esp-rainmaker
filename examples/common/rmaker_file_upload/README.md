# rmaker_file_upload

Helper component for RainMaker devices that need to upload files (snapshots, logs, captures, …) to the cloud's S3 bucket.

## What it does

Implements the device-side half of the RainMaker file-upload protocol
(cloud_backend MR !1969):

1. Device asks the cloud for a pre-signed S3 URL via `esp-command-response`
   command id 16 (`FILE_UPLOAD_COMMAND`) with body `{"file_name": "<name>"}`.
2. Cloud replies with a `file_id` and the pre-signed `url`.
3. Device PUTs the bytes to the URL.
4. Device publishes the upload-confirm — same command id 16, with the
   request_id from step 1 as the correlator and `{"file_id": "<id>",
   "file_type": "<mime>"}` as the body. The cloud writes the `file_details`
   row keyed on the node (`entity_type=node`, `entity_id=<node>`); the file
   then shows up under `GET /v1/user/file?entity_id=<node>&entity_type=node`.

Uploads are asynchronous. The caller hands a heap-allocated buffer to the
component; an internal worker task drains the queue and `free()`s the buffer
when done.

## Usage

```c
#include "rmaker_file_upload.h"

void app_main(void) {
    /* ... esp_rmaker_node_init() / esp_rmaker_start() ... */
    rmaker_file_upload_init();
}

void on_capture_done(uint8_t *jpeg_heap, size_t jpeg_len) {
    /* Ownership of jpeg_heap transfers to the component on ESP_OK. */
    esp_err_t err = rmaker_file_upload_submit("snapshot.jpg",
                                              jpeg_heap, jpeg_len,
                                              "image/jpeg");
    if (err != ESP_OK) {
        free(jpeg_heap);  // caller still owns on error
    }
}
```

## Dependencies

- `espressif/esp_rainmaker` (≥1.0) for the cmd-resp publish API
- `espressif/rmaker_common` (≥1.4.0) for `esp_rmaker_cmd_resp_prepare_response_payload`
- `espressif/json_parser` (~1.0.3) for parsing the cloud's reply

## Notes

- The cloud's S3 bucket has a 7-day lifecycle expiration on uploaded files.
- Listing files for a node uses the access token (not the id token) and the
  query `?entity_id=<node>&entity_type=node`. To get a presigned download
  URL for a specific file, query with `?entity_id=<node>&entity_type=node&file_id=<id>`.
