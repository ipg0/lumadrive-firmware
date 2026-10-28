#include "matrix_core.hpp"
#include "../animation_trigger_table/animation_effect/impl/static_color_animation_effect.hpp"
#include "../animation_trigger_table/animation_effect/impl/keyframe_animation_effect.hpp"
#include "../animation_trigger_table/animation_effect/impl/periodic_animation_effect.hpp"
#include "../lighting_driver/impl/linear_lighting_driver_view.hpp"
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <new>
#include <pb_decode.h>
#include <pb_encode.h>

namespace matrix {
uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = ~0u;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

std::vector<uint8_t> encode(const pb_msgdesc_t *fields, const void *message) {
  size_t size = 0;
  if (!pb_get_encoded_size(&size, fields, message) || size > MAX_BYTES) return {};
  std::vector<uint8_t> bytes(size);
  auto stream = pb_ostream_from_buffer(bytes.data(), size);
  if (!pb_encode(&stream, fields, message)) return {};
  return bytes;
}
bool decode(const pb_msgdesc_t *fields, void *message,
            const uint8_t *data, size_t length) {
  if (length > MAX_BYTES) return false;
  auto stream = pb_istream_from_buffer(data, length);
  return pb_decode(&stream, fields, message) && stream.bytes_left == 0;
}
bool receiver::begin(uint32_t id, size_t size, uint32_t checksum, uint64_t now) {
  if (!id || !size || size > MAX_BYTES) return false;
  if (_id == id) {
    if (_bytes.size() != size || _checksum != checksum) return false;
  } else {
    _bytes.assign(size, 0);
    _id = id; _checksum = checksum; _offset = 0;
  }
  _touched = now;
  return true;
}
bool receiver::append(uint32_t id, size_t offset, const uint8_t *data,
                      size_t size, uint64_t now) {
  if (!_id || id != _id || !size || offset > _bytes.size() ||
      size > _bytes.size() - offset || offset > _offset) return false;
  if (offset < _offset) {
    if (size > _offset - offset || memcmp(_bytes.data() + offset, data, size))
      return false;
  } else {
    memcpy(_bytes.data() + offset, data, size);
    _offset += size;
  }
  _touched = now;
  return true;
}
bool receiver::complete(uint32_t id) const {
  return _id && id == _id && _offset == _bytes.size() &&
         crc32(_bytes.data(), _bytes.size()) == _checksum;
}
void receiver::expire(uint64_t now) {
  if (_id && now - _touched >= TRANSFER_TIMEOUT_US) reset();
}
void receiver::reset() { _id = 0; _offset = 0; _bytes.clear(); }

template <class item_type> const item_type *lookup(const item_type *items, size_t count, uint32_t id) {
  for (size_t i = 0; i < count; ++i) if (items[i].id == id) return items + i;
  return nullptr;
}
template <class item_type> bool unique(const item_type *items, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (!items[i].id) return false;
    for (size_t j = 0; j < i; ++j) if (items[j].id == items[i].id) return false;
  }
  return true;
}
template <class enum_type> auto enum_value(const enum_type &value) {
  // Nanopb can decode unknown enum numbers. Read their representation without
  // loading an out-of-range unscoped C++ enum before validating it.
  std::underlying_type_t<enum_type> number;
  memcpy(&number, &value, sizeof(number));
  return number;
}
bool valid_driver(const mesh_interop_MatrixDriver &driver) {
  const auto value = enum_value(driver);
  return value == mesh_interop_MatrixDriver_WS2812 || value == mesh_interop_MatrixDriver_HC595;
}
std::array<animation_keyframe, MAX_ANIMATION_KEYFRAMES> effect_keyframes(const matrix_effect &e) {
  std::array<animation_keyframe, MAX_ANIMATION_KEYFRAMES> frames{};
  for (size_t i = 0; i < e.keyframes_count && i < frames.size(); ++i) {
    const auto &f = e.keyframes[i];
    frames[i] = {f.time_ms, alpha_color(color(f.red, f.green, f.blue), f.alpha),
                 static_cast<animation_interpolation>(enum_value(f.interpolation)), f.track_id,
                 f.position, f.has_width ? f.width : 1, f.wrap, f.all_pixels};
  }
  return frames;
}
const char *validate_effect(const matrix_effect &e) {
  if (e.red > 255 || e.green > 255 || e.blue > 255 || e.alpha > 255) return "Invalid effect color";
  if (e.keyframes_count > MAX_ANIMATION_KEYFRAMES) return "Too many keyframes";
  const auto kind = enum_value(e.kind);
  if (kind != mesh_interop_MatrixEffect_Kind_KEYFRAMES && e.keyframes_count)
    return "Keyframes require the keyframe effect type";
  switch (kind) {
  case mesh_interop_MatrixEffect_Kind_STATIC_COLOR: return nullptr;
  case mesh_interop_MatrixEffect_Kind_KEYFRAMES: {
    const auto frames = effect_keyframes(e);
    return valid_animation_keyframes(frames.data(), e.keyframes_count) ? nullptr :
        "Invalid keyframe times, interpolation, or pixel tracks";
  }
  case mesh_interop_MatrixEffect_Kind_BLINK:
    if (e.duty_percent > 100) return "Blink duty must be 1..100 percent (zero defaults to 50)";
    break;
  case mesh_interop_MatrixEffect_Kind_BREATHE: break;
  case mesh_interop_MatrixEffect_Kind_FLOWING_GRADIENT:
    if (!e.has_secondary_color) return "Flowing gradient requires a secondary color";
    break;
  default: return "Unsupported effect type";
  }
  return e.period_ms == 1 ? "Animation period must be at least 2 ms" : nullptr;
}
animation_effect_instance_base *create_effect(const matrix_effect &e) {
  const alpha_color primary(color(e.red, e.green, e.blue), e.alpha);
  if (e.kind == mesh_interop_MatrixEffect_Kind_STATIC_COLOR)
    return animation_effect_descriptor<animation_effect_type::STATIC_COLOR>(primary, e.duration_ms).create_instance();
  if (e.kind == mesh_interop_MatrixEffect_Kind_KEYFRAMES) {
    const auto frames = effect_keyframes(e);
    return animation_effect_descriptor<animation_effect_type::KEYFRAMES>(
        frames.data(), e.keyframes_count, e.loop, e.duration_ms).create_instance();
  }
  periodic_animation_options options;
  options.primary = primary; options.duration_ms = e.duration_ms;
  options.period_ms = e.period_ms ? e.period_ms : 1000;
  options.duty_percent = e.kind == mesh_interop_MatrixEffect_Kind_BLINK && e.duty_percent ? e.duty_percent : 50;
  const auto &s = e.secondary_color;
  options.secondary = alpha_color(color(s.red, s.green, s.blue), s.alpha);
  options.wavelength_pixels = e.wavelength_pixels; options.reverse = e.reverse;
  switch (e.kind) {
  case mesh_interop_MatrixEffect_Kind_BLINK:
    return animation_effect_descriptor<animation_effect_type::BLINK>(options).create_instance();
  case mesh_interop_MatrixEffect_Kind_BREATHE:
    return animation_effect_descriptor<animation_effect_type::BREATHE>(options).create_instance();
  case mesh_interop_MatrixEffect_Kind_FLOWING_GRADIENT:
    return animation_effect_descriptor<animation_effect_type::FLOWING_GRADIENT>(options).create_instance();
  default: return nullptr;
  }
}
const char *validate(const matrix_config &c, const uint8_t master_mac[6]) {
  if (c.schema_version != SCHEMA_VERSION) return "Unsupported schema version (expected 2)";
  if (c.entries_count > 32 || c.effects_count > 32 || c.sources_count > 32 ||
      c.conditions_count > 32 || c.layout_count > 34) return "Too many definitions";
  if (!unique(c.effects, c.effects_count) || !unique(c.sources, c.sources_count) ||
      !unique(c.conditions, c.conditions_count) || !unique(c.entries, c.entries_count))
    return "Definition IDs must be nonzero and unique";
  for (size_t i = 0; i < c.effects_count; ++i) {
    if (auto error = validate_effect(c.effects[i])) return error;
  }
  for (size_t i = 0; i < c.sources_count; ++i) {
    const auto &s = c.sources[i];
    if (s.kind != mesh_interop_MatrixSource_Kind_DEFAULT_STATE &&
        s.kind != mesh_interop_MatrixSource_Kind_GPIO_EDGE) return "Invalid source";
    if (s.kind == mesh_interop_MatrixSource_Kind_GPIO_EDGE &&
        ((s.node_mac[0] & 1) || s.gpio > 39 ||
         std::all_of(s.node_mac, s.node_mac + 6, [](uint8_t v) { return v == 0; })))
      return "Invalid GPIO source node or pin";
  }
  for (size_t i = 0; i < c.conditions_count; ++i)
    if (c.conditions[i].kind != mesh_interop_MatrixCondition_Kind_ALWAYS)
      return "Unsupported condition";
  for (size_t i = 0; i < c.layout_count; ++i) {
    const auto &m = c.layout[i];
    if (!valid_driver(m.driver)) return "Unknown layout driver ID";
    if ((m.node_mac[0] & 1) ||
        std::all_of(m.node_mac, m.node_mac + 6, [](uint8_t v) { return v == 0; })) return "Invalid layout MAC";
    if (m.from == m.to || std::max(m.from, m.to) > 65536) return "Invalid layout bounds";
    for (size_t j = 0; j < i; ++j)
      if (!memcmp(m.node_mac, c.layout[j].node_mac, 6) && m.driver == c.layout[j].driver)
        return "Duplicate layout node/driver";
  }
  size_t node_count = 0;
  for (size_t i = 0; i < c.layout_count; ++i) {
    bool first = true;
    for (size_t j = 0; j < i; ++j)
      if (!memcmp(c.layout[i].node_mac, c.layout[j].node_mac, 6)) first = false;
    if (first && ++node_count > 17) return "Too many layout nodes";
  }
  for (size_t i = 0; i < c.entries_count; ++i) {
    const auto &e = c.entries[i];
    if (!lookup(c.effects, c.effects_count, e.effect_id) ||
        !lookup(c.sources, c.sources_count, e.source_id) ||
        !lookup(c.conditions, c.conditions_count, e.condition_id) ||
        e.priority > 255) return "Invalid entry reference or priority";
  }
  if (c.entries_count && !c.layout_count) return "Animations require the car layout";
  // Layer messages embed their effect, so repeated references can expand beyond
  // the uploaded config's size. Reject that before persistence/activation.
  for (size_t i = 0; i < c.layout_count; ++i) {
    const auto *mac = c.layout[i].node_mac;
    if (!memcmp(mac, master_mac, 6)) continue;
    bool first = true;
    for (size_t j = 0; j < i; ++j)
      if (!memcmp(mac, c.layout[j].node_mac, 6)) first = false;
    if (!first) continue;
    const auto d = make_definitions(c, mac, UINT64_MAX, UINT64_MAX);
    if (!d) return "Unable to allocate animation definitions";
    size_t size = 0;
    if (!pb_get_encoded_size(&size, mesh_interop_AnimationDefinitions_fields, d.get()) || size > MAX_BYTES)
      return "Node animation definitions exceed 16 KiB";
  }
  return nullptr;
}
void fill_definitions(const matrix_config &c, const uint8_t *mac,
                      uint64_t session, uint64_t revision, animation_definitions &d) {
  d.schema_version = SCHEMA_VERSION; d.master_session = session; d.revision = revision;
  for (size_t i = 0; i < c.layout_count; ++i) {
    const auto &m = c.layout[i];
    d.canvas_size = std::max(d.canvas_size, std::max(m.from, m.to));
    if (mac && !memcmp(m.node_mac, mac, 6))
      d.output_mappings[d.output_mappings_count++] = {m.driver, m.from, m.to};
  }
  for (size_t i = 0; mac && i < c.sources_count; ++i) {
    const auto &s = c.sources[i];
    if (s.kind != mesh_interop_MatrixSource_Kind_GPIO_EDGE || memcmp(s.node_mac, mac, 6)) continue;
    if (std::find(d.gpio_inputs, d.gpio_inputs + d.gpio_inputs_count, s.gpio) ==
        d.gpio_inputs + d.gpio_inputs_count) d.gpio_inputs[d.gpio_inputs_count++] = s.gpio;
  }
  for (size_t i = 0; i < c.entries_count; ++i) {
    const auto &e = c.entries[i];
    if (mac && !d.output_mappings_count) continue;
    auto &layer = d.layers[d.layers_count++];
    layer.entry_id = e.id; layer.has_effect = true;
    layer.effect = *lookup(c.effects, c.effects_count, e.effect_id);
    layer.priority = e.priority; layer.order = i;
  }
}
animation_definitions definitions_for(const matrix_config &c, const uint8_t *mac,
                            uint64_t session, uint64_t revision) {
  animation_definitions d{};
  fill_definitions(c, mac, session, revision, d);
  return d;
}
std::unique_ptr<animation_definitions> make_definitions(const matrix_config &c,
    const uint8_t *mac, uint64_t session, uint64_t revision) {
  std::unique_ptr<animation_definitions> d(new (std::nothrow) animation_definitions{});
  if (!d) return nullptr;
  fill_definitions(c, mac, session, revision, *d);
  return d;
}

int player::find(uint32_t id) const {
  for (size_t i = 0; i < _definitions.layers_count; ++i)
    if (_definitions.layers[i].entry_id == id) return static_cast<int>(i);
  return -1;
}
bool player::prepare(const animation_definitions &d) {
  if (d.schema_version != SCHEMA_VERSION || !d.master_session || !d.revision || d.layers_count > 32 ||
      d.gpio_inputs_count > 32 || d.canvas_size > 65536 || d.output_mappings_count > 2 ||
      (!d.canvas_size && (d.output_mappings_count || d.layers_count)) ||
      (_resolver && d.layers_count && !d.output_mappings_count))
    return false;
  for (size_t i = 0; i < d.output_mappings_count; ++i) {
    const auto &m = d.output_mappings[i];
    if (!valid_driver(m.driver) || m.from == m.to || std::max(m.from, m.to) > d.canvas_size) return false;
    for (size_t j = 0; j < i; ++j) if (d.output_mappings[j].driver == m.driver) return false;
    if (_resolver) {
      auto *driver = _resolver(m.driver);
      if (!driver || !linear_lighting_driver_view(*driver, m.from, m.to, d.canvas_size).valid()) return false;
    }
  }
  for (size_t i = 0; i < d.gpio_inputs_count; ++i) {
    if (d.gpio_inputs[i] > 39) return false;
    for (size_t j = 0; j < i; ++j) if (d.gpio_inputs[i] == d.gpio_inputs[j]) return false;
  }
  for (size_t i = 0; i < d.layers_count; ++i) {
    const auto &l = d.layers[i];
    if (!l.entry_id || !l.has_effect || validate_effect(l.effect) || l.priority > 255) return false;
    for (size_t j = 0; j < i; ++j) if (d.layers[j].entry_id == l.entry_id) return false;
  }
  _definitions = d; _pending_count = 0;
  for (auto &l : _layers) l = layer{};
  for (size_t i = 0; i < d.layers_count; ++i) {
    _layers[i].effect.reset(create_effect(d.layers[i].effect));
    if (!_layers[i].effect) return false;
  }
  return true;
}
bool player::enqueue(const animation_command &c) {
  if (c.master_session != _definitions.master_session || c.revision != _definitions.revision ||
      find(c.entry_id) < 0 || _pending_count == _pending.size()) return false;
  size_t at = _pending_count++;
  while (at && _pending[at - 1].effective_at_us > c.effective_at_us) {
    _pending[at] = _pending[at - 1]; --at;
  }
  _pending[at] = c;
  return true;
}
bool player::restore(const animation_snapshot &s) {
  if (s.master_session != _definitions.master_session || s.revision != _definitions.revision ||
      s.states_count > 32 || s.pending_count > 64) return false;
  for (size_t i = 0; i < s.states_count; ++i) {
    if (find(s.states[i].entry_id) < 0) return false;
    for (size_t j = 0; j < i; ++j) if (s.states[j].entry_id == s.states[i].entry_id) return false;
  }
  for (size_t i = 0; i < s.pending_count; ++i)
    if (find(s.pending[i].entry_id) < 0 || s.pending[i].master_session != s.master_session ||
        s.pending[i].revision != s.revision) return false;
  _pending_count = 0;
  for (auto &l : _layers) { l.active = false; l.begun = false; l.elapsed = 0; }
  for (size_t i = 0; i < s.states_count; ++i) {
    auto &l = _layers[find(s.states[i].entry_id)];
    l.active = s.states[i].active; l.started = s.states[i].started_at_us;
    if (l.active) l.effect->start(0);
  }
  for (size_t i = 0; i < s.pending_count; ++i) enqueue(s.pending[i]);
  return true;
}
animation_snapshot player::snapshot(const animation_definitions &subset, uint32_t sequence, uint64_t activate) const {
  animation_snapshot s{};
  s.master_session = _definitions.master_session; s.revision = _definitions.revision;
  s.sequence = sequence; s.activate_at_us = activate;
  auto included = [&](uint32_t id) {
    for (size_t i = 0; i < subset.layers_count; ++i) if (subset.layers[i].entry_id == id) return true;
    return false;
  };
  for (size_t i = 0; i < _definitions.layers_count; ++i) {
    if (!included(_definitions.layers[i].entry_id)) continue;
    auto &state = s.states[s.states_count++];
    state.entry_id = _definitions.layers[i].entry_id;
    state.active = _layers[i].active; state.started_at_us = _layers[i].started;
  }
  for (size_t i = 0; i < _pending_count; ++i)
    if (included(_pending[i].entry_id)) s.pending[s.pending_count++] = _pending[i];
  return s;
}
esp_err_t player::blank() {
  if (!_resolver) return ESP_OK;
  esp_err_t result = ESP_OK;
  std::array<rgb_lighting_driver *, 32> done{};
  size_t count = 0;
  for (size_t i = 0; i < _definitions.output_mappings_count; ++i) {
    auto *d = _resolver(_definitions.output_mappings[i].driver);
    if (std::find(done.begin(), done.begin() + count, d) != done.begin() + count) continue;
    done[count++] = d;
    auto err = d->clear(); if (err == ESP_OK) err = d->show();
    if (result == ESP_OK) result = err;
  }
  return result;
}
esp_err_t player::tick(uint64_t now, bool render) {
  while (_pending_count && _pending[0].effective_at_us <= now) {
    const auto c = _pending[0];
    auto &l = _layers[find(c.entry_id)];
    l.active = !c.stop; l.begun = !c.stop; l.started = c.effective_at_us; l.elapsed = 0;
    if (l.active) l.effect->start(0);
    std::move(_pending.begin() + 1, _pending.begin() + _pending_count, _pending.begin());
    --_pending_count;
  }
  std::array<size_t, 32> order{};
  for (size_t i = 0; i < _definitions.layers_count; ++i) {
    order[i] = i; auto &l = _layers[i];
    if (!l.active) continue;
    if (now >= l.started) l.begun = true;
    if (!l.begun) continue;
    const auto elapsed = now < l.started ? 0 : (now - l.started) / 1000;
    l.elapsed = std::max(l.elapsed, elapsed);
    l.effect->seek(l.elapsed);
    if (l.effect->is_finished()) l.active = false;
  }
  if (!render || !_resolver) return ESP_OK;
  std::sort(order.begin(), order.begin() + _definitions.layers_count, [&](size_t a, size_t b) {
    auto &x = _definitions.layers[a]; auto &y = _definitions.layers[b];
    return x.priority == y.priority ? x.order < y.order : x.priority < y.priority;
  });
  std::array<rgb_lighting_driver *, 32> drivers{};
  size_t count = 0;
  for (size_t i = 0; i < _definitions.output_mappings_count; ++i) {
    auto *d = _resolver(_definitions.output_mappings[i].driver);
    if (std::find(drivers.begin(), drivers.begin() + count, d) == drivers.begin() + count) {
      drivers[count++] = d;
      auto err = d->clear(); if (err != ESP_OK) return err;
    }
  }
  for (size_t j = 0; j < _definitions.layers_count; ++j) {
    size_t i = order[j];
    if (_layers[i].active && _layers[i].begun) {
      for (size_t k = 0; k < _definitions.output_mappings_count; ++k) {
        const auto &mapping = _definitions.output_mappings[k];
        auto *driver = _resolver(mapping.driver);
        linear_lighting_driver_view view(*driver, mapping.from, mapping.to, _definitions.canvas_size);
        auto err = _layers[i].effect->render(view);
        if (err != ESP_OK) return err;
      }
    }
  }
  esp_err_t result = ESP_OK;
  for (size_t i = 0; i < count; ++i) {
    auto err = drivers[i]->show(); if (result == ESP_OK) result = err;
  }
  return result;
}
void evaluator::configure(const matrix_config *c, uint64_t session, uint64_t revision) {
  _config = c; _session = session; _revision = revision; _starts.fill(0); _active.fill(false);
}
void evaluator::tick(uint64_t now, const trigger_gpio_edge_t *edges, size_t count,
                     uint64_t edge_time, command_sink sink, void *arg) {
  if (!_config) return;
  for (size_t i = 0; i < _config->entries_count; ++i) {
    const auto &entry = _config->entries[i];
    const auto &source = *lookup(_config->sources, _config->sources_count, entry.source_id);
    const auto &effect = *lookup(_config->effects, _config->effects_count, entry.effect_id);
    if (_active[i] && effect.duration_ms && now >= _starts[i] &&
        (now - _starts[i]) / 1000 >= effect.duration_ms) _active[i] = false;
    bool fired = source.kind == mesh_interop_MatrixSource_Kind_DEFAULT_STATE && !_active[i];
    for (size_t j = 0; source.kind == mesh_interop_MatrixSource_Kind_GPIO_EDGE && j < count; ++j)
      if (edges[j].gpio_num == static_cast<gpio_num_t>(source.gpio) && edges[j].rising == source.rising &&
          !memcmp(edges[j].from_node_mac, source.node_mac, 6)) fired = true;
    if (!fired) continue;
    animation_command c{};
    c.master_session = _session; c.revision = _revision; c.entry_id = entry.id;
    c.effective_at_us = (source.kind == mesh_interop_MatrixSource_Kind_GPIO_EDGE ? edge_time : now) + LEAD_US;
    if (sink(arg, c)) { _active[i] = true; _starts[i] = c.effective_at_us; }
  }
}
} // namespace matrix
