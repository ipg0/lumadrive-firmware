#pragma once
struct esp_app_desc_t { char version[32]; };
inline esp_app_desc_t fake_app_description{"test-version"};
inline const esp_app_desc_t *esp_app_get_description() { return &fake_app_description; }
