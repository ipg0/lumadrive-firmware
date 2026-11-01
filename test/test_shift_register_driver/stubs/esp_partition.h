#pragma once
#include <cstddef>
#include <cstdint>
#include <esp_err.h>
constexpr esp_err_t ESP_ERR_NOT_FOUND = 0x105;
enum esp_partition_type_t { ESP_PARTITION_TYPE_DATA = 1 };
enum esp_partition_subtype_t { TEST_SUBTYPE = 0x40 };
struct esp_partition_t { size_t size; };
const esp_partition_t *esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char *);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
