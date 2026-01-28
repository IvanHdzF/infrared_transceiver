#include "stg_man/stg_man.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define STG_MAN_MAX_PATH       128
#define STG_MAN_MAGIC          0x434F4F4CUL /* 'COOL' */
#define STG_MAN_VERSION        0x0001

// 8.3-safe temp file name to avoid EINVAL when LFN is off/limited
#define STG_MAN_TMP_83_NAME    "UTSTG.TMP"

static const char *TAG = "stg_man";

static wl_handle_t       s_wl_handle = WL_INVALID_HANDLE;
static stg_man_cfg_t     s_cfg       = {0};
static bool              s_inited    = false;
static SemaphoreHandle_t s_lock      = NULL;

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t payload_len;
    uint32_t header_crc;
}
stg_man_hdr_t;

static void stg_lock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void stg_unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

static stg_man_hdr_t stg_default_hdr(void)
{
    stg_man_hdr_t hdr = {
        .magic = STG_MAN_MAGIC,
        .version = STG_MAN_VERSION,
        .reserved = 0,
        .payload_len = 0,
        .header_crc = 0,
    };
    return hdr;
}

static bool stg_is_safe_relpath(const char *relpath)
{
    if ((relpath == NULL) || (relpath[0] == '\0')) {
        return false;
    }
    if (relpath[0] == '/') {
        return false;
    }
    if (strstr(relpath, "..") != NULL) {
        return false;
    }
    if (strchr(relpath, '\\') != NULL) {
        return false;
    }
    return true;
}

static esp_err_t stg_make_abspath(char *out, size_t out_sz, const char *relpath)
{
    const char *rp = (relpath != NULL) ? relpath : s_cfg.default_relpath;
    int n = 0;

    if ((rp == NULL) || (s_cfg.base_path == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!stg_is_safe_relpath(rp)) {
        return ESP_ERR_INVALID_ARG;
    }

    n = snprintf(out, out_sz, "%s/%s", s_cfg.base_path, rp);
    if ((n < 0) || ((size_t)n >= out_sz)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t stg_make_tmp_abspath(char *out, size_t out_sz)
{
    int n = snprintf(out, out_sz, "%s/%s", s_cfg.base_path, STG_MAN_TMP_83_NAME);
    if ((n < 0) || ((size_t)n >= out_sz)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t stg_file_write_all(const char *path, const void *data, size_t len)
{
    esp_err_t err = ESP_OK;
    FILE *f = NULL;
    size_t w = 0;
    int rc = 0;

    f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen('%s','wb') failed: errno=%d (%s)", path, errno, strerror(errno));
        err = ESP_FAIL;
        goto out;
    }

    w = fwrite(data, 1, len, f);
    rc = fclose(f);
    f = NULL;

    if ((w != len) || (rc != 0)) {
        ESP_LOGE(TAG, "write/close failed: w=%u/%u rc=%d errno=%d (%s)",
                 (unsigned)w, (unsigned)len, rc, errno, strerror(errno));
        err = ESP_FAIL;
        goto out;
    }

out:
    if (f != NULL) {
        (void)fclose(f);
    }
    return err;
}

static esp_err_t stg_header_ensure_or_validate(const char *abspath, bool heal_short)
{
    esp_err_t err = ESP_OK;
    struct stat st;
    FILE *f = NULL;
    stg_man_hdr_t hdr;
    size_t r = 0;

    if (stat(abspath, &st) != 0) {
        stg_man_hdr_t nh = stg_default_hdr();
        err = stg_file_write_all(abspath, &nh, sizeof(nh));
        goto out;
    }

    f = fopen(abspath, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen('%s','rb') failed: errno=%d (%s)", abspath, errno, strerror(errno));
        err = ESP_FAIL;
        goto out;
    }

    r = fread(&hdr, 1, sizeof(hdr), f);
    if (fclose(f) != 0) {
        // if close fails, treat as I/O error
        err = ESP_FAIL;
        f = NULL;
        goto out;
    }
    f = NULL;

    if (r != sizeof(hdr)) {
        if (!heal_short) {
            err = ESP_FAIL;
            goto out;
        }
        ESP_LOGW(TAG, "short header in '%s' (r=%u), rewriting header", abspath, (unsigned)r);
        stg_man_hdr_t nh = stg_default_hdr();
        err = stg_file_write_all(abspath, &nh, sizeof(nh));
        goto out;
    }

    if (hdr.magic != STG_MAN_MAGIC) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (hdr.version != STG_MAN_VERSION) {
        ESP_LOGE(TAG, "version mismatch: file=%u expected=%u", hdr.version, STG_MAN_VERSION);
        err = ESP_ERR_INVALID_VERSION;
        goto out;
    }

out:
    if (f != NULL) {
        (void)fclose(f);
    }
    return err;
}

/* ========================= Public API ========================= */

esp_err_t stg_man_init(const stg_man_cfg_t *cfg)
{
    esp_err_t err = ESP_OK;

    if ((cfg == NULL) || (cfg->base_path == NULL) || (cfg->partition_label == NULL) || (cfg->default_relpath == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_inited) {
        return ESP_OK;
    }

    stg_lock();

    s_cfg = *cfg;

    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = (int)cfg->max_open_files,
        .format_if_mount_failed = cfg->format_if_mount_failed,
        .allocation_unit_size = (size_t)cfg->alloc_unit,
        .use_one_fat = false,
    };

    err = esp_vfs_fat_spiflash_mount_rw_wl(
              cfg->base_path,
              cfg->partition_label,
              &mount_config,
              &s_wl_handle
          );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        s_wl_handle = WL_INVALID_HANDLE;
        s_inited = false;
        goto out;
    }
    s_inited = true;
    ESP_LOGI(TAG, "storage ready: %s/%s", cfg->base_path, cfg->default_relpath);

out:
    stg_unlock();
    return err;
}

esp_err_t stg_man_deinit(void)
{
    esp_err_t err = ESP_OK;

    if (s_lock == NULL) {
        return ESP_OK;
    }

    stg_lock();

    if (!s_inited) {
        err = ESP_OK;
        goto out;
    }

    err = esp_vfs_fat_spiflash_unmount_rw_wl(s_cfg.base_path, s_wl_handle);
    s_wl_handle = WL_INVALID_HANDLE;
    s_inited = false;

out:
    stg_unlock();
    return err;
}

esp_err_t stg_man_format(void)
{
    esp_err_t err = ESP_OK;
    char path[STG_MAN_MAX_PATH];

    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    stg_lock();

    if (!s_inited) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    err = esp_vfs_fat_spiflash_format_rw_wl(s_cfg.base_path, s_cfg.partition_label);
    if (err != ESP_OK) {
        goto out;
    }

    // after format: recreate header (healing allowed)
    err = stg_make_abspath(path, sizeof(path), NULL);
    if (err != ESP_OK) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    err = stg_header_ensure_or_validate(path, true);

out:
    stg_unlock();
    return err;
}

esp_err_t stg_man_check(const char *relpath)
{
    esp_err_t err = ESP_OK;
    char path[STG_MAN_MAX_PATH];

    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    stg_lock();

    if (!s_inited) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    err = stg_make_abspath(path, sizeof(path), relpath);
    if (err != ESP_OK) {
        goto out;
    }

    // check: strict (do NOT auto-heal short header)
    err = stg_header_ensure_or_validate(path, false);

out:
    stg_unlock();
    return err;
}

esp_err_t stg_man_write(const char *relpath, const void *data, size_t len)
{
    esp_err_t err = ESP_OK;
    char path[STG_MAN_MAX_PATH];
    char tmp[STG_MAN_MAX_PATH];
    FILE *f = NULL;
    stg_man_hdr_t hdr;
    size_t w1 = 0;
    size_t w2 = 0;
    int rc = 0;

    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((data == NULL) && (len != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    stg_lock();

    if (!s_inited) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    err = stg_make_abspath(path, sizeof(path), relpath);
    if (err != ESP_OK) {
        goto out;
    }

    // writing: allow healing short header
    err = stg_header_ensure_or_validate(path, true);
    if (err != ESP_OK) {
        goto out;
    }

    err = stg_make_tmp_abspath(tmp, sizeof(tmp));
    if (err != ESP_OK) {
        goto out;
    }

    hdr = stg_default_hdr();
    hdr.payload_len = (uint32_t)len;

    f = fopen(tmp, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen('%s','wb') failed: errno=%d (%s)", tmp, errno, strerror(errno));
        err = ESP_FAIL;
        goto out;
    }

    w1 = fwrite(&hdr, 1, sizeof(hdr), f);
    if (len > 0U) {
        w2 = fwrite(data, 1, len, f);
    } else {
        w2 = 0;
    }
    rc = fclose(f);
    f = NULL;

    if ((w1 != sizeof(hdr)) || ((len > 0U) && (w2 != len)) || (rc != 0)) {
        (void)remove(tmp);
        err = ESP_FAIL;
        goto out;
    }

    (void)remove(path);
    if (rename(tmp, path) != 0) {
        (void)remove(tmp);
        err = ESP_FAIL;
        goto out;
    }

out:
    if (f != NULL) {
        (void)fclose(f);
    }
    stg_unlock();
    return err;
}

esp_err_t stg_man_read(const char *relpath, void *out_buf, size_t out_buf_size, size_t *out_len)
{
    esp_err_t err = ESP_OK;
    char path[STG_MAN_MAX_PATH];
    FILE *f = NULL;
    stg_man_hdr_t hdr;
    size_t r1 = 0;
    size_t r2 = 0;

    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    stg_lock();

    if (!s_inited) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    err = stg_make_abspath(path, sizeof(path), relpath);
    if (err != ESP_OK) {
        goto out;
    }

    f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen('%s','rb') failed: errno=%d (%s)", path, errno, strerror(errno));
        err = ESP_FAIL;
        goto out;
    }

    r1 = fread(&hdr, 1, sizeof(hdr), f);
    if (r1 != sizeof(hdr)) {
        err = ESP_FAIL;
        goto out;
    }

    if (hdr.magic != STG_MAN_MAGIC) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (hdr.version != STG_MAN_VERSION) {
        err = ESP_ERR_INVALID_VERSION;
        goto out;
    }

    // Size query mode
    if (out_buf == NULL) {
        if (out_len != NULL) {
            *out_len = hdr.payload_len;
        }
        err = ESP_OK;
        goto out;
    }

    if (out_buf_size < hdr.payload_len) {
        if (out_len != NULL) {
            *out_len = hdr.payload_len;
        }
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    r2 = 0;
    if (hdr.payload_len > 0U) {
        r2 = fread(out_buf, 1, hdr.payload_len, f);
    }

    if (r2 != hdr.payload_len) {
        err = ESP_FAIL;
        goto out;
    }

    if (out_len != NULL) {
        *out_len = hdr.payload_len;
    }

out:
    if (f != NULL) {
        (void)fclose(f);
    }
    stg_unlock();
    return err;
}
