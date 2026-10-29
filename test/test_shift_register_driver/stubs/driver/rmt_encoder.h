#pragma once
#include "rmt_tx.h"
using rmt_encode_simple_cb_t = size_t (*)(const void *, size_t, size_t, size_t, rmt_symbol_word_t *, bool *, void *);
struct rmt_simple_encoder_config_t { rmt_encode_simple_cb_t callback; void *arg; size_t min_chunk_size; };
esp_err_t rmt_new_simple_encoder(const rmt_simple_encoder_config_t *, rmt_encoder_handle_t *);
esp_err_t rmt_del_encoder(rmt_encoder_handle_t);
