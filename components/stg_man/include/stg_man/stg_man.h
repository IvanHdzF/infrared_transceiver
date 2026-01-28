#ifndef STG_MAN_H
#define STG_MAN_H

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Storage manager configuration.
 *
 * Describes how the storage manager mounts and manages
 * a FAT filesystem backed by wear leveling.
 */
typedef struct
{
    /** Mount point registered in VFS (e.g. "/spiflash"). */
    const char *base_path;

    /** Partition label as defined in the partition table (e.g. "storage"). */
    const char *partition_label;

    /**
     * Default relative file path used when APIs receive relpath == NULL
     * (e.g. "stg.dat"). Stored under @ref base_path.
     */
    const char *default_relpath;

    /** Maximum number of files that can be open simultaneously. */
    size_t max_open_files;

    /**
     * Allocation unit size for FATFS.
     * Typically CONFIG_WL_SECTOR_SIZE.
     */
    size_t alloc_unit;

    /**
     * If true, the filesystem will be formatted automatically
     * if mounting fails.
     *
     * Recommended:
     * - true for development / tests
     * - false for production firmware
     */
    bool format_if_mount_failed;
} stg_man_cfg_t;

/**
 * @brief Initialize the storage manager.
 *
 * This function:
 * - Mounts the FAT filesystem with wear leveling.
 * - Registers the filesystem in VFS.
 * - Ensures the default file exists and contains a valid header.
 *
 * The filesystem is only formatted automatically if
 * @ref stg_man_cfg_t::format_if_mount_failed is set and mounting fails.
 *
 * @param cfg Pointer to storage manager configuration.
 *
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if configuration is invalid
 * - ESP_FAIL or other esp_err_t values on mount or filesystem errors
 *
 * @note
 * This function is intended to be called once at system startup
 * by the application layer.
 */
esp_err_t stg_man_init(const stg_man_cfg_t *cfg);

/**
 * @brief Deinitialize the storage manager.
 *
 * Unmounts the filesystem and releases internal resources.
 *
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_STATE if the storage manager was not initialized
 *
 * @note
 * Many applications never need to call this explicitly.
 */
esp_err_t stg_man_deinit(void);

/**
 * @brief Format the storage filesystem.
 *
 * Performs a full FAT filesystem format on the configured partition
 * and recreates the default file header.
 *
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_STATE if the storage manager is not initialized
 * - ESP_FAIL on format failure
 *
 * @warning
 * This operation irreversibly deletes all stored data.
 * Caller must ensure no concurrent filesystem access.
 */
esp_err_t stg_man_format(void);

/**
 * @brief Write a payload to storage.
 *
 * Writes the provided payload to the specified file using a
 * temporary-file + rename strategy to reduce corruption risk.
 *
 * @param relpath Relative file path under @ref base_path.
 *                If NULL, @ref stg_man_cfg_t::default_relpath is used.
 * @param data Pointer to payload buffer.
 * @param len  Payload size in bytes.
 *
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if parameters are invalid
 * - ESP_ERR_INVALID_STATE if storage manager is not initialized
 * - ESP_FAIL on I/O or filesystem errors
 *
 * @note
 * Payload is treated as raw binary data (no string formatting).
 */
esp_err_t stg_man_write(const char *relpath, const void *data, size_t len);

/**
 * @brief Read a payload from storage.
 *
 * Reads the payload stored in the specified file, excluding
 * the internal header.
 *
 * @param relpath Relative file path under @ref base_path.
 *                If NULL, @ref stg_man_cfg_t::default_relpath is used.
 * @param out_buf Destination buffer for payload data.
 *                If NULL, the function only reports the required size.
 * @param out_buf_size Size of @p out_buf in bytes.
 * @param out_len Optional pointer to receive payload size in bytes.
 *
 * @return
 * - ESP_OK on success
 * - ESP_ERR_NO_MEM if @p out_buf is too small
 * - ESP_ERR_INVALID_STATE if storage manager is not initialized
 * - ESP_FAIL on I/O or filesystem errors
 */
esp_err_t stg_man_read(const char *relpath,
                       void *out_buf,
                       size_t out_buf_size,
                       size_t *out_len);

/**
 * @brief Validate the storage file header.
 *
 * Verifies that the file exists and that its header contains
 * the expected magic and version.
 *
 * @param relpath Relative file path under @ref base_path.
 *                If NULL, @ref stg_man_cfg_t::default_relpath is used.
 *
 * @return
 * - ESP_OK if header is valid
 * - ESP_ERR_INVALID_STATE if storage manager is not initialized
 * - ESP_FAIL or ESP_ERR_INVALID_VERSION if corruption is detected
 */
esp_err_t stg_man_check(const char *relpath);

#ifdef __cplusplus
}
#endif

#endif // STG_MAN_H
