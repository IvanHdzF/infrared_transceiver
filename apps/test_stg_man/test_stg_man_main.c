// test_stg_man.c
#include "unity.h"

#include "stg_man/stg_man.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"

// Keep tests isolated: use a dedicated default file for unit tests
#define UT_BASE_PATH        "/spiflash"
#define UT_PARTITION_LABEL  "storage"
#define UT_DEFAULT_FILE     "ut_stg.dat"

static stg_man_cfg_t s_cfg = {
    .base_path = UT_BASE_PATH,
    .partition_label = UT_PARTITION_LABEL,
    .default_relpath = UT_DEFAULT_FILE,
    .max_open_files = 4,
    .alloc_unit = CONFIG_WL_SECTOR_SIZE,
    // In unit tests, OK to allow format on mount failure to recover a clean FS
    .format_if_mount_failed = true,
};

static void ut_path(char *out, size_t out_sz, const char *rel)
{
    // rel is expected to be safe for these tests
    snprintf(out, out_sz, "%s/%s", UT_BASE_PATH, rel ? rel : UT_DEFAULT_FILE);
}

static bool ut_file_exists(const char *rel)
{
    char path[128];
    ut_path(path, sizeof(path), rel);
    struct stat st;
    return (stat(path, &st) == 0);
}

static esp_err_t ut_write_raw_bytes(const char *rel, const void *data, size_t len)
{
    char path[128];
    ut_path(path, sizeof(path), rel);

    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return (w == len) ? ESP_OK : ESP_FAIL;
}

static void ut_storage_reset(void)
{
    static bool s_suite_inited = false;
    esp_err_t err;

    // One-time init + format for the whole test run
    if (!s_suite_inited) {
        err = stg_man_init(&s_cfg);
        TEST_ASSERT_EQUAL(ESP_OK, err);

        err = stg_man_format();
        TEST_ASSERT_EQUAL(ESP_OK, err);

        s_suite_inited = true;
    }

    // Per-test cleanup: delete ONLY the test file
    char path[128];
    ut_path(path, sizeof(path), NULL);
    (void)remove(path);

    // Recreate default file/header explicitly
    err = stg_man_check(NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    TEST_ASSERT_TRUE(ut_file_exists(NULL));
}
/* Setup / teardown */

void setUp(void)
{
    ut_storage_reset();
}
/* ---------------- Tests ---------------- */

TEST_CASE("stg_man init creates default file and header", "[stg_man]")
{
    // Make sure module can init from scratch
    esp_err_t err = stg_man_init(&s_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    TEST_ASSERT_TRUE_MESSAGE(ut_file_exists(NULL), "default file not created");

    // Optional: check() should pass (creates if missing, validates if exists)
    err = stg_man_check(NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);
}

TEST_CASE("stg_man write/read roundtrip (binary-safe)", "[stg_man]")
{
    ut_storage_reset();

    const uint8_t payload[] = { 0x00, 0x11, 0x22, 0xFF, 0x41, 0x00, 0x42, 0x7E };
    esp_err_t err = stg_man_write(NULL, payload, sizeof(payload));
    TEST_ASSERT_EQUAL(ESP_OK, err);

    uint8_t out[32] = {0};
    size_t out_len = 0;
    err = stg_man_read(NULL, out, sizeof(out), &out_len);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(sizeof(payload), out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, out, sizeof(payload));
}

TEST_CASE("stg_man read size query mode returns required length", "[stg_man]")
{
    ut_storage_reset();

    const char payload[] = "hello\0world"; // includes a NUL in the middle to prove binary-safe
    esp_err_t err = stg_man_write(NULL, payload, sizeof(payload));
    TEST_ASSERT_EQUAL(ESP_OK, err);

    size_t needed = 0;
    err = stg_man_read(NULL, NULL, 0, &needed);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(sizeof(payload), needed);

    // Now read with exact size
    uint8_t *buf = (uint8_t *)malloc(needed);
    TEST_ASSERT_NOT_NULL(buf);

    size_t got = 0;
    err = stg_man_read(NULL, buf, needed, &got);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(needed, got);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)payload, buf, needed);

    free(buf);
}

TEST_CASE("stg_man read returns ESP_ERR_NO_MEM when buffer too small", "[stg_man]")
{
    ut_storage_reset();

    uint8_t payload[64];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;

    esp_err_t err = stg_man_write(NULL, payload, sizeof(payload));
    TEST_ASSERT_EQUAL(ESP_OK, err);

    uint8_t small[8];
    size_t needed = 0;
    err = stg_man_read(NULL, small, sizeof(small), &needed);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, err);
    TEST_ASSERT_EQUAL(sizeof(payload), needed);
}

TEST_CASE("stg_man format removes payload (file recreated with empty header)", "[stg_man]")
{
    ut_storage_reset();

    const uint8_t payload[] = { 1, 2, 3, 4, 5 };
    esp_err_t err = stg_man_write(NULL, payload, sizeof(payload));
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Format FS
    err = stg_man_format();
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Default file should exist again (header recreated)
    TEST_ASSERT_TRUE(ut_file_exists(NULL));

    // Payload should now be empty
    size_t len = 1234;
    err = stg_man_read(NULL, NULL, 0, &len);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(0, len);
}

TEST_CASE("stg_man rejects unsafe relpath traversal/absolute", "[stg_man]")
{
    ut_storage_reset();

    const uint8_t payload[] = { 9, 9, 9 };

    // Absolute path should be rejected
    esp_err_t err = stg_man_write("/evil.txt", payload, sizeof(payload));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);

    // Traversal should be rejected
    err = stg_man_write("../evil.txt", payload, sizeof(payload));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);

    // Windows separator should be rejected (per your sanitizer)
    err = stg_man_write("dir\\evil.txt", payload, sizeof(payload));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("stg_man detects corruption (magic mismatch)", "[stg_man]")
{
    ut_storage_reset();

    // Corrupt the file by overwriting first bytes
    uint32_t bad_magic = 0xDEADBEEF;
    esp_err_t err = ut_write_raw_bytes(NULL, &bad_magic, sizeof(bad_magic));
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // check should fail now
    err = stg_man_check(NULL);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, err);

    // read should fail too
    uint8_t out[16];
    size_t out_len = 0;
    err = stg_man_read(NULL, out, sizeof(out), &out_len);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, err);
}

TEST_CASE("stg_man write supports zero-length payload", "[stg_man]")
{
    ut_storage_reset();

    esp_err_t err = stg_man_write(NULL, NULL, 0);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    size_t needed = 123;
    err = stg_man_read(NULL, NULL, 0, &needed);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(0, needed);

    uint8_t out[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    size_t got = 999;
    err = stg_man_read(NULL, out, sizeof(out), &got);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(0, got);
    // out buffer should be unchanged (no payload read)
    for (size_t i = 0; i < sizeof(out); i++) {
        TEST_ASSERT_EQUAL_UINT8(0xAA, out[i]);
    }
}

TEST_CASE("stg_man supports non-default relpath (separate file)", "[stg_man]")
{
    ut_storage_reset();

    const uint8_t a[] = {1,2,3};
    const uint8_t b[] = {9,8,7,6};

    esp_err_t err = stg_man_write(NULL, a, sizeof(a));
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = stg_man_write("alt.dat", b, sizeof(b));
    TEST_ASSERT_EQUAL(ESP_OK, err);

    uint8_t out[16] = {0};
    size_t out_len = 0;

    err = stg_man_read(NULL, out, sizeof(out), &out_len);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(sizeof(a), out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, out, sizeof(a));

    memset(out, 0, sizeof(out));
    out_len = 0;
    err = stg_man_read("alt.dat", out, sizeof(out), &out_len);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(sizeof(b), out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, out, sizeof(b));
}

TEST_CASE("stg_man write leaves no temp file behind", "[stg_man]")
{
    ut_storage_reset();

    const uint8_t payload[] = { 0xA5, 0x5A, 0x01, 0x02 };
    esp_err_t err = stg_man_write(NULL, payload, sizeof(payload));
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // UTSTG.TMP must not exist after successful commit
    TEST_ASSERT_FALSE_MESSAGE(ut_file_exists("UTSTG.TMP"), "temp file left behind");
}

TEST_CASE("stg_man deinit + init works", "[stg_man]")
{
    ut_storage_reset();

    esp_err_t err = stg_man_deinit();
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = stg_man_init(&s_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    TEST_ASSERT_TRUE(ut_file_exists(NULL));
}

TEST_CASE("stg_man short header is strict in check but healed on write", "[stg_man]")
{
    ut_storage_reset();

    // Create a short/truncated header file (4 bytes only)
    uint32_t junk = 0x12345678;
    esp_err_t err = ut_write_raw_bytes(NULL, &junk, sizeof(junk));
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // check should fail (strict)
    err = stg_man_check(NULL);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, err);

    // write should heal and succeed
    const uint8_t payload[] = { 0x10, 0x20 };
    err = stg_man_write(NULL, payload, sizeof(payload));
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // now check should pass
    err = stg_man_check(NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);
}

TEST_CASE("stg_man detects corruption (version mismatch)", "[stg_man]")
{
    ut_storage_reset();

    // Construct a header with correct magic but wrong version, payload_len=0
    // Header layout must match stg_man_hdr_t in stg_man.c
    struct __attribute__((packed)) {
        uint32_t magic;
        uint16_t version;
        uint16_t reserved;
        uint32_t payload_len;
        uint32_t header_crc;
    }
    hdr = {
        .magic = 0x434F4F4CUL,   // 'COOL'
        .version = 0x9999,       // wrong
        .reserved = 0,
        .payload_len = 0,
        .header_crc = 0,
    };

    esp_err_t err = ut_write_raw_bytes(NULL, &hdr, sizeof(hdr));
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = stg_man_check(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, err);

    uint8_t out[8];
    size_t out_len = 0;
    err = stg_man_read(NULL, out, sizeof(out), &out_len);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, err);
}

/* ---------------- App entry ---------------- */

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
