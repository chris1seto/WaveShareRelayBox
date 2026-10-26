#include "ota.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "ota";

#define CHUNK 4096

static atomic_bool s_busy;

static esp_err_t fail(char *msg, size_t len, const char *text, esp_err_t err)
{
    snprintf(msg, len, "%s", text);
    ESP_LOGE(TAG, "%s (%s)", text, esp_err_to_name(err));
    return err;
}

// Check the image header before erasing anything: right chip, looks like an app.
static esp_err_t check_header(const uint8_t *buf, size_t len, char *msg, size_t msg_len)
{
    const size_t need = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                        sizeof(esp_app_desc_t);
    if (len < need) {
        return fail(msg, msg_len, "Image too small", ESP_ERR_INVALID_SIZE);
    }
    const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
    if (hdr->magic != ESP_IMAGE_HEADER_MAGIC) {
        return fail(msg, msg_len, "Not an ESP firmware image (use the .bin from build/)", ESP_ERR_INVALID_ARG);
    }
    if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return fail(msg, msg_len, "Image was built for a different chip", ESP_ERR_INVALID_ARG);
    }
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) +
                                                          sizeof(esp_image_segment_header_t));
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return fail(msg, msg_len, "Image has no app descriptor", ESP_ERR_INVALID_ARG);
    }
    ESP_LOGI(TAG, "Incoming image: %.32s %.32s (built %.16s %.16s)",
             desc->project_name, desc->version, desc->date, desc->time);
    return ESP_OK;
}

esp_err_t ota_from_request(httpd_req_t *req, char *msg, size_t msg_len)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        return fail(msg, msg_len, "Another update is in progress", ESP_ERR_INVALID_STATE);
    }

    esp_err_t err = ESP_OK;
    esp_ota_handle_t handle = 0;
    bool begun = false;
    uint8_t *buf = malloc(CHUNK);
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    size_t total = req->content_len, received = 0;
    int last_pct = -1;

    if (!buf) {
        err = fail(msg, msg_len, "Out of memory", ESP_ERR_NO_MEM);
        goto out;
    }
    if (!target) {
        err = fail(msg, msg_len, "No OTA partition (flash the new partition table over USB once)",
                   ESP_ERR_NOT_FOUND);
        goto out;
    }
    if (total == 0 || total > target->size) {
        err = fail(msg, msg_len, total ? "Image larger than OTA slot" : "Empty upload",
                   ESP_ERR_INVALID_SIZE);
        goto out;
    }
    ESP_LOGI(TAG, "Receiving %u bytes into %s", (unsigned)total, target->label);

    while (received < total) {
        // Fill the first chunk completely so the header check sees the whole descriptor.
        size_t want = total - received < CHUNK ? total - received : CHUNK;
        size_t got = 0;
        while (got < want) {
            int r = httpd_req_recv(req, (char *)buf + got, want - got);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (r <= 0) {
                err = fail(msg, msg_len, "Upload interrupted", ESP_FAIL);
                goto out;
            }
            got += r;
        }

        if (!begun) {
            if ((err = check_header(buf, got, msg, msg_len)) != ESP_OK) {
                goto out;
            }
            err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
            if (err != ESP_OK) {
                fail(msg, msg_len, "esp_ota_begin failed", err);
                goto out;
            }
            begun = true;
        }
        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            fail(msg, msg_len, "Flash write failed", err);
            goto out;
        }
        received += got;
        int pct = (int)(received * 100 / total);
        if (pct / 10 != last_pct / 10) {
            ESP_LOGI(TAG, "%d%%", pct);
            last_pct = pct;
        }
    }

    err = esp_ota_end(handle);
    begun = false;
    if (err != ESP_OK) {
        fail(msg, msg_len, err == ESP_ERR_OTA_VALIDATE_FAILED ? "Image failed validation (corrupt?)"
                                                              : "esp_ota_end failed", err);
        goto out;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        fail(msg, msg_len, "Could not set boot partition", err);
        goto out;
    }
    snprintf(msg, msg_len, "Update written to %s, rebooting", target->label);
    ESP_LOGI(TAG, "%s", msg);

out:
    if (begun) {
        esp_ota_abort(handle);
    }
    free(buf);
    atomic_store(&s_busy, false);
    return err;
}

bool ota_pending_verify(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

void ota_mark_valid(void)
{
    if (ota_pending_verify()) {
        ESP_LOGI(TAG, "New firmware confirmed, rollback cancelled");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

static void reboot_cb(void *arg)
{
    esp_restart();
}

void ota_schedule_reboot(uint32_t delay_ms)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t args = { .callback = reboot_cb, .name = "reboot" };
        esp_timer_create(&args, &t);
    }
    esp_timer_stop(t);
    esp_timer_start_once(t, (uint64_t)delay_ms * 1000);
}
