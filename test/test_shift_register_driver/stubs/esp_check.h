#pragma once
#define ESP_RETURN_ON_ERROR(expression, tag, ...) do { \
  (void)(tag); \
  const auto result = (expression); \
  if (result != ESP_OK) return result; \
} while (false)
