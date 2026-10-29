#include "matrix_runtime/matrix_core.hpp"
#include "matrix_runtime/matrix_wire.hpp"
#include "animation_trigger_table/animation_effect/impl/keyframe_animation_effect.hpp"
#include "animation_trigger_table/animation_effect/impl/periodic_animation_effect.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>

using namespace matrix;
static_assert(sizeof(player) < 8192, "Players must not retain maximum-sized protobuf effect arrays");
const uint8_t master_mac[6] = {2, 1, 2, 3, 4, 5};
const uint8_t slave_mac[6] = {4, 1, 2, 3, 4, 5};
mesh_interop_MatrixLayout layout_row(const uint8_t *mac, mesh_interop_MatrixDriver driver,
                                    uint32_t from, uint32_t to) {
  mesh_interop_MatrixLayout row{}; memcpy(row.node_mac, mac, 6);
  row.driver = driver; row.from = from; row.to = to; return row;
}
matrix_config fixture() {
  matrix_config c{}; c.schema_version = SCHEMA_VERSION;
  c.effects_count = 1; auto &effect = c.effects[0];
  effect.id = 1; effect.kind = mesh_interop_MatrixEffect_Kind_STATIC_COLOR;
  effect.red = 255; effect.alpha = 255; effect.duration_ms = 100;
  c.sources_count = 1; auto &s = c.sources[0];
  s.id = 2; s.kind = mesh_interop_MatrixSource_Kind_GPIO_EDGE; s.gpio = 12; s.rising = true;
  memcpy(s.node_mac, master_mac, 6);
  c.conditions_count = 1; c.conditions[0] = {3, mesh_interop_MatrixCondition_Kind_ALWAYS};
  c.entries_count = 1; auto &e = c.entries[0];
  e.id = 4; e.effect_id = 1; e.source_id = 2; e.condition_id = 3; e.priority = 10;
  c.layout_count = 1; memcpy(c.layout[0].node_mac, slave_mac, 6);
  c.layout[0].driver = mesh_interop_MatrixDriver_WS2812; c.layout[0].to = 1;
  return c;
}
class fake_driver : public rgb_lighting_driver {
public:
  lighting_driver_kind get_kind() const override { return lighting_driver_kind::LED_STRIP; }
  color pixel{}, shown{}; unsigned clears = 0, shows = 0;
  esp_err_t init() override { return ESP_OK; }
  esp_err_t deinit() override { return ESP_OK; }
  size_t get_pixel_count() const override { return 1; }
  esp_err_t set_pixel(uint16_t, alpha_color c) override { return fill(c); }
  esp_err_t fill(alpha_color c) override { pixel = c.blend_with(pixel); return ESP_OK; }
  esp_err_t clear() override { ++clears; pixel = {}; return ESP_OK; }
  esp_err_t show() override { ++shows; shown = pixel; return ESP_OK; }
  esp_err_t set_global_brightness(uint8_t) override { return ESP_OK; }
  bool _is_ready() const override { return true; }
} output, output_secondary;
rgb_lighting_driver *resolve(uint32_t id) {
  if (id == 1) return &output;
  if (id == 2) return &output_secondary;
  return nullptr;
}
void red(bool expected) { assert(output.shown.r == (expected ? 255 : 0)); }
animation_command start(uint64_t at = 100000) {
  animation_command c{}; c.master_session = 10; c.revision = 20; c.sequence = 1;
  c.entry_id = 4; c.effective_at_us = at; return c;
}
void test_transfer() {
  const auto text = reinterpret_cast<const uint8_t *>("123456789");
  assert(crc32(text, 9) == 0xcbf43926);
  std::vector<uint8_t> bytes(MAX_BYTES);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = i;
  receiver r;
  assert(!r.begin(0, 10, 0, 0)); assert(!r.begin(1, MAX_BYTES + 1, 0, 0));
  assert(r.begin(1, bytes.size(), crc32(bytes.data(), bytes.size()), 1));
  assert(!r.begin(1, bytes.size() - 1, 0, 2));
  assert(!r.append(1, 200, bytes.data() + 200, 200, 3));
  for (size_t offset = 0; offset < bytes.size();) {
    auto size = std::min<size_t>(200, bytes.size() - offset);
    assert(r.append(1, offset, bytes.data() + offset, size, 10));
    assert(r.append(1, offset, bytes.data() + offset, size, 11)); // lost ACK retry
    offset += size;
  }
  assert(r.complete(1));
  uint8_t conflict = 123;
  assert(!r.append(1, 0, &conflict, 1, 12));
  assert(r.complete(1));
  r.expire(TRANSFER_TIMEOUT_US + 11); assert(r.id() == 0);
  assert(r.begin(2, 9, 0, 20)); assert(r.append(2, 0, text, 9, 21));
  assert(!r.complete(2)); // bad CRC
  assert(!r.append(2, SIZE_MAX, text, 9, 22));
  for (size_t size = 1; size <= 200; ++size) {
    auto packet = wire::packet(wire::CHUNK, std::vector<uint8_t>(size + 8));
    assert(wire::valid(packet.data(), packet.size())); assert(packet.size() <= 250);
    packet[6]++; assert(!wire::valid(packet.data(), packet.size()));
  }
  assert(wire::packet(wire::CHUNK, std::vector<uint8_t>(243)).empty());
}
void test_schema_validation() {
  auto c = fixture(); assert(!validate(c, master_mac));
  auto bytes = encode(mesh_interop_MatrixConfig_fields, &c); matrix_config decoded{};
  assert(decode(mesh_interop_MatrixConfig_fields, &decoded, bytes.data(), bytes.size()));
  assert(!validate(decoded, master_mac));
  c.effects_count = 2; c.effects[1] = c.effects[0]; assert(validate(c, master_mac));
  c = fixture(); c.entries[0].effect_id = 99; assert(validate(c, master_mac));
  c = fixture(); memcpy(c.sources[0].node_mac, slave_mac, 6); assert(!validate(c, master_mac));
  c.sources[0].node_mac[0] |= 1; assert(validate(c, master_mac));
  memset(c.sources[0].node_mac, 0, 6); assert(validate(c, master_mac));
  c = fixture(); c.sources[0].gpio = 40; assert(validate(c, master_mac));
  c = fixture(); c.layout[0].driver = mesh_interop_MatrixDriver_DRIVER_UNSPECIFIED;
  assert(validate(c, master_mac));
  c = fixture(); c.effects[0].alpha = 256; assert(validate(c, master_mac));
  c = fixture(); c.schema_version = 1; assert(validate(c, master_mac));
}
void test_scheduled_player() {
  auto config = fixture(); auto d = definitions_for(config, slave_mac, 10, 20);
  assert(d.layers_count == 1); assert(definitions_for(config, master_mac, 10, 20).layers_count == 0);
  player p(resolve); assert(p.prepare(d)); assert(p.enqueue(start()));
  assert(p.tick(99999) == ESP_OK); red(false);
  p.tick(100000); red(true);
  p.tick(180000); red(true);
  p.tick(120000); red(true); // backward clock correction must not finish the effect
  p.tick(99999); red(true); // or blank a layer that has already begun
  p.tick(200000); red(false);
  assert(p.enqueue(start())); p.tick(300000); red(false); // late, already expired
  auto bad = start(); bad.revision++; assert(!p.enqueue(bad));
  bad = start(); bad.master_session++; assert(!p.enqueue(bad));
  bad = start(); bad.entry_id++; assert(!p.enqueue(bad));
  for (unsigned i = 0; i < 64; ++i) assert(p.enqueue(start(1000000 + i)));
  assert(!p.enqueue(start(1000100)));
}
void test_master_evaluation_and_recovery() {
  auto config = fixture(); evaluator e;
  e.configure(&config, 10, 20);
  std::vector<animation_command> commands;
  auto sink = [](void *ptr, const animation_command &c) {
    static_cast<std::vector<animation_command> *>(ptr)->push_back(c); return true;
  };
  e.tick(5, nullptr, 0, 5, sink, &commands); assert(commands.empty());
  trigger_gpio_edge_t edge{}; memcpy(edge.from_node_mac, master_mac, 6);
  edge.gpio_num = static_cast<gpio_num_t>(12); edge.rising = true;
  e.tick(30, &edge, 1, 10, sink, &commands);
  assert(commands.size() == 1 && commands[0].effective_at_us == 100010);
  e.tick(50, &edge, 1, 40, sink, &commands);
  assert(commands.size() == 2 && commands[1].effective_at_us == 100040);
  auto d = definitions_for(config, slave_mac, 10, 20);
  player ledger; assert(ledger.prepare(d));
  assert(ledger.enqueue(commands[0])); ledger.tick(110000, false);
  assert(ledger.enqueue(start(160000)));
  auto snapshot = ledger.snapshot(d, 7, 0);
  player slave(resolve); assert(slave.prepare(d)); assert(slave.restore(snapshot));
  slave.tick(150000); red(true); // original start retained after reconnect
  slave.tick(200020); red(true); // pending retrigger preserved
  slave.tick(260000); red(false);
  snapshot.revision++; assert(!slave.restore(snapshot));
}
void test_layering() {
  auto config = fixture(); auto d = definitions_for(config, slave_mac, 10, 20);
  d.layers[0].effect.duration_ms = 0;
  d.layers[1] = d.layers[0]; d.layers[1].entry_id = 5;
  d.layers[1].effect.red = 0; d.layers[1].effect.blue = 255; d.layers[1].effect.alpha = 128;
  d.layers[1].priority = 20; d.layers[1].order = 1; d.layers_count = 2;
  player p(resolve); assert(p.prepare(d));
  auto c = start(); assert(p.enqueue(c)); c.entry_id = 5; assert(p.enqueue(c));
  p.tick(100000); assert(output.shown.r == 127 && output.shown.b == 128);
  p.tick(110000); assert(output.shown.r == 127 && output.shown.b == 128);
  c.stop = true; c.effective_at_us = 120000; assert(p.enqueue(c));
  p.tick(120000); red(true);
}
void test_one_entry_two_drivers() {
  auto config = fixture();
  config.layout_count = 2; config.layout[1] = config.layout[0];
  assert(validate(config, master_mac)); // Exact duplicate is invalid.
  config.layout[1].driver = mesh_interop_MatrixDriver_HC595;
  assert(!validate(config, master_mac));
  auto d = definitions_for(config, slave_mac, 10, 20);
  assert(d.layers_count == 1 && d.output_mappings_count == 2);
  player p(resolve); assert(p.prepare(d)); assert(p.enqueue(start()));
  const auto first = output.shows, second = output_secondary.shows;
  p.tick(100000);
  assert(output.shows == first + 1 && output_secondary.shows == second + 1);
  assert(output.shown.r == 255 && output_secondary.shown.r == 255);
}
void test_node_input_definitions() {
  auto c = fixture(); memcpy(c.sources[0].node_mac, slave_mac, 6);
  c.sources[1] = c.sources[0]; c.sources[1].id = 8; c.sources[1].rising = false; c.sources_count = 2;
  memcpy(c.layout[0].node_mac, master_mac, 6);
  assert(!validate(c, master_mac));
  auto d = definitions_for(c, slave_mac, 10, 20);
  assert(d.layers_count == 0 && d.gpio_inputs_count == 1 && d.gpio_inputs[0] == 12);
  auto bytes = encode(mesh_interop_AnimationDefinitions_fields, &d);
  animation_definitions decoded{};
  assert(decode(mesh_interop_AnimationDefinitions_fields, &decoded, bytes.data(), bytes.size()));
  assert(decoded.gpio_inputs_count == 1 && decoded.gpio_inputs[0] == 12);
  assert(definitions_for(c, master_mac, 10, 20).gpio_inputs_count == 0);
  player p; assert(p.prepare(decoded));
  decoded.gpio_inputs_count = 2; decoded.gpio_inputs[1] = 12; assert(!p.prepare(decoded));
  decoded.gpio_inputs_count = 1; decoded.gpio_inputs[0] = 40; assert(!p.prepare(decoded));
}
mesh_interop_MatrixKeyframe keyframe(uint32_t at, uint8_t red, uint8_t green, uint8_t blue,
    uint8_t alpha, mesh_interop_MatrixInterpolation interpolation, uint32_t track = 0,
    float position = 0, float width = 1) {
  mesh_interop_MatrixKeyframe frame{};
  frame.time_ms = at; frame.red = red; frame.green = green; frame.blue = blue; frame.alpha = alpha;
  frame.interpolation = interpolation; frame.track_id = track; frame.position = position;
  frame.has_width = true; frame.width = width;
  return frame;
}
matrix_config keyframe_fixture() {
  auto c = fixture(); auto &e = c.effects[0];
  e.kind = mesh_interop_MatrixEffect_Kind_KEYFRAMES; e.duration_ms = 0;
  e.keyframes_count = 3; e.loop = true;
  e.keyframes[0] = keyframe(0, 255, 0, 0, 255, mesh_interop_MatrixInterpolation_LINEAR);
  e.keyframes[1] = keyframe(500, 0, 0, 255, 255, mesh_interop_MatrixInterpolation_SMOOTH);
  e.keyframes[2] = keyframe(1000, 255, 0, 0, 255, mesh_interop_MatrixInterpolation_STEP);
  return c;
}
void test_effect_validation_and_roundtrip() {
  auto c = keyframe_fixture(); assert(!validate(c, master_mac));
  const auto bytes = encode(mesh_interop_MatrixConfig_fields, &c);
  matrix_config decoded{};
  assert(decode(mesh_interop_MatrixConfig_fields, &decoded, bytes.data(), bytes.size()));
  assert(decoded.effects[0].keyframes[1].interpolation == mesh_interop_MatrixInterpolation_SMOOTH);
  assert(decoded.effects[0].keyframes[1].blue == 255 && decoded.effects[0].loop);
  c.effects[0].keyframes[3] = keyframe(0, 0, 255, 0, 255, mesh_interop_MatrixInterpolation_LINEAR, 1, 3, 2);
  c.effects[0].keyframes[4] = keyframe(1000, 255, 0, 255, 255, mesh_interop_MatrixInterpolation_LINEAR, 1, 9.5, 0);
  c.effects[0].keyframes_count = 5; assert(!validate(c, master_mac));
  const auto spatial_bytes = encode(mesh_interop_MatrixConfig_fields, &c);
  assert(decode(mesh_interop_MatrixConfig_fields, &decoded, spatial_bytes.data(), spatial_bytes.size()));
  assert(decoded.effects[0].keyframes[4].track_id == 1 && decoded.effects[0].keyframes[4].position == 9.5);
  assert(decoded.effects[0].keyframes[4].has_width && decoded.effects[0].keyframes[4].width == 0);
  c.effects[0].keyframes[3].wrap = true; // Track settings must agree at each frame.
  assert(validate(c, master_mac));
  c = keyframe_fixture();
  c.effects[0].keyframes[1].time_ms = 0; assert(validate(c, master_mac));
  c = keyframe_fixture(); c.effects[0].keyframes[0].time_ms = 1; assert(validate(c, master_mac));
  c = keyframe_fixture(); c.effects[0].keyframes_count = 1; assert(validate(c, master_mac));
  c = keyframe_fixture(); c.effects[0].keyframes_count = 17; assert(validate(c, master_mac));
  c = keyframe_fixture(); c.effects[0].keyframes[0].interpolation = static_cast<mesh_interop_MatrixInterpolation>(99);
  assert(validate(c, master_mac));
  auto d = definitions_for(c, slave_mac, 10, 20); player invalid;
  assert(!invalid.prepare(d)); // Slave validates definitions independently.
  c = fixture(); c.effects[0].kind = mesh_interop_MatrixEffect_Kind_BLINK;
  assert(!validate(c, master_mac)); c.effects[0].period_ms = 1; assert(validate(c, master_mac));
  c.effects[0].period_ms = 2; c.effects[0].duty_percent = 101; assert(validate(c, master_mac));
  c.effects[0].duty_percent = 100; assert(!validate(c, master_mac));
  c.effects[0].kind = mesh_interop_MatrixEffect_Kind_FLOWING_GRADIENT; assert(validate(c, master_mac));
  c.effects[0].has_secondary_color = true; c.effects[0].secondary_color = {0, 0, 255, 128};
  assert(!validate(c, master_mac));
  c = fixture(); c.effects[0].kind = static_cast<mesh_interop_MatrixEffect_Kind>(99);
  assert(validate(c, master_mac));
  c = keyframe_fixture(); c.effects[0].kind = mesh_interop_MatrixEffect_Kind_STATIC_COLOR;
  assert(validate(c, master_mac));
}
void test_animated_player_phase_and_snapshots() {
  auto config = keyframe_fixture(); auto d = definitions_for(config, slave_mac, 10, 20);
  player original(resolve), recovered(resolve); assert(original.prepare(d) && recovered.prepare(d));
  assert(original.enqueue(start())); original.tick(350000);
  assert(output.shown.r == 128 && output.shown.b == 128);
  auto state = original.snapshot(d, 1, 0); assert(recovered.restore(state));
  recovered.tick(350000); assert(output.shown.r == 128 && output.shown.b == 128);
  recovered.tick(225000); assert(output.shown.r == 128 && output.shown.b == 128); // Clamp backwards.
  recovered.tick(600000); assert(output.shown.b == 255);
  assert(recovered.enqueue(start(700000))); recovered.tick(700000); red(true); // Retrigger resets phase.
  const auto long_elapsed_ms = uint64_t(UINT32_MAX) + 455; // 4,294,967,750 -> phase 750 ms.
  original.tick(100000 + long_elapsed_ms * 1000);
  assert(output.shown.r == 128 && output.shown.b == 128);

  for (const auto kind : {mesh_interop_MatrixEffect_Kind_BLINK, mesh_interop_MatrixEffect_Kind_BREATHE,
                         mesh_interop_MatrixEffect_Kind_FLOWING_GRADIENT}) {
    config = fixture(); auto &e = config.effects[0]; e.kind = kind; e.duration_ms = 0;
    e.period_ms = 1000; e.has_secondary_color = true; e.secondary_color = {0, 0, 255, 255};
    d = definitions_for(config, slave_mac, 10, 20);
    assert(original.prepare(d) && recovered.prepare(d)); assert(original.enqueue(start()));
    original.tick(350000); const auto expected = output.shown;
    state = original.snapshot(d, 1, 0); assert(recovered.restore(state)); recovered.tick(350000);
    assert(output.shown.r == expected.r && output.shown.b == expected.b);
    if (kind == mesh_interop_MatrixEffect_Kind_BREATHE) assert(expected.r == 128);
    if (kind == mesh_interop_MatrixEffect_Kind_FLOWING_GRADIENT) assert(expected.r == 128 && expected.b == 128);
    auto stop = start(400000); stop.stop = true; assert(recovered.enqueue(stop));
    recovered.tick(400000); red(false); assert(output.shown.b == 0);
    e.duration_ms = 200; d = definitions_for(config, slave_mac, 10, 20);
    assert(recovered.prepare(d) && recovered.enqueue(start())); recovered.tick(500000);
    red(false); assert(output.shown.b == 0); // Finite animations already expired on arrival.
  }
}
void test_compact_player_definition_ownership() {
  player recovered(resolve);
  animation_snapshot snapshot{};
  {
    player original(resolve);
    {
      auto config = make_heap_object<matrix_config>(keyframe_fixture());
      auto definitions = make_definitions(*config, slave_mac, 10, 20);
      assert(definitions && original.prepare(*definitions));
      assert(original.definition_bytes().size() < sizeof(animation_definitions) / 10);
    }
    assert(original.enqueue(start())); original.tick(350000);
    snapshot = original.snapshot(original.definitions(), 3, 0);
    assert(recovered.prepare(original));
    assert(recovered.definition_bytes() == original.definition_bytes());
    assert(recovered.restore(snapshot));
  }
  // The clone owns both its effect instances and its encoded definition bytes.
  recovered.tick(350000); assert(output.shown.r == 128 && output.shown.b == 128);
  recovered.tick(600000); assert(output.shown.b == 255);
}
void test_compact_config_and_direct_preparation() {
  static_assert(sizeof(runtime_config) < 512);
  for (const auto keyframes : {false, true}) {
    auto c = make_heap_object<matrix_config>(keyframes ? keyframe_fixture() : fixture());
    auto compact = make_heap_object<runtime_config>(*c);
    assert(compact->effects.size() == 1 && compact->entries.size() == 1);
    assert(compact->encoded() == encode(mesh_interop_MatrixConfig_fields, c.get()));
    for (const auto *mac : std::array<const uint8_t *, 3>{nullptr, master_mac, slave_mac}) {
      auto expected = make_definitions(*c, mac, 10, 20);
      auto actual = make_definitions(*compact, mac, 10, 20);
      assert(encode(mesh_interop_AnimationDefinitions_fields, expected.get()) ==
          encode(mesh_interop_AnimationDefinitions_fields, actual.get()));
      player direct(mac ? resolve : nullptr);
      assert(direct.prepare(*c, mac, 10, 20));
      assert(direct.definition_bytes() == encode(mesh_interop_AnimationDefinitions_fields, expected.get()));
    }
    auto copy = compact->to_wire();
    assert(copy && encode(mesh_interop_MatrixConfig_fields, copy.get()) == compact->encoded());
    c->effects[0].red = 17;
    assert(compact->effects[0].red != 17); // Owned independently of staging storage.
  }
  auto empty = make_heap_object<matrix_config>(); empty->schema_version = SCHEMA_VERSION;
  runtime_config compact(*empty);
  assert(compact.encoded() == encode(mesh_interop_MatrixConfig_fields, empty.get()));
}
class motion_test_driver : public rgb_lighting_driver {
public:
  std::array<color, 4> pixels{}, shown{};
  esp_err_t init() override { return ESP_OK; }
  esp_err_t deinit() override { return ESP_OK; }
  size_t get_pixel_count() const override { return pixels.size(); }
  esp_err_t set_pixel(uint16_t i, alpha_color c) override { pixels.at(i) = c.blend_with(pixels.at(i)); return ESP_OK; }
  esp_err_t fill(alpha_color c) override { for (size_t i = 0; i < pixels.size(); ++i) set_pixel(i, c); return ESP_OK; }
  esp_err_t clear() override { pixels = {}; return ESP_OK; }
  esp_err_t show() override { shown = pixels; return ESP_OK; }
  esp_err_t set_global_brightness(uint8_t) override { return ESP_OK; }
  bool _is_ready() const override { return true; }
} motion_output;
rgb_lighting_driver *resolve_motion(uint32_t id) { return id == 1 ? &motion_output : nullptr; }
void test_motion_phase_recovery() {
  auto c = keyframe_fixture(); c.layout[0].to = 4;
  auto &e = c.effects[0]; e.keyframes_count = 2;
  e.keyframes[0] = keyframe(0, 255, 0, 0, 255, mesh_interop_MatrixInterpolation_LINEAR, 5, 0);
  e.keyframes[1] = keyframe(1000, 255, 0, 0, 255, mesh_interop_MatrixInterpolation_LINEAR, 5, 4);
  e.keyframes[0].wrap = e.keyframes[1].wrap = true;
  auto d = definitions_for(c, slave_mac, 10, 20);
  player original(resolve_motion), recovered(resolve_motion);
  assert(original.prepare(d) && recovered.prepare(d) && original.enqueue(start()));
  original.tick(225000); // 125 ms of playback, position 0.5.
  assert(motion_output.shown[0].r == 128 && motion_output.shown[1].r == 128);
  assert(motion_output.shown[2].r == 0 && motion_output.shown[3].r == 0);
  auto snapshot = original.snapshot(d, 1, 0); assert(recovered.restore(snapshot));
  recovered.tick(975000); // Late recovery at position 3.5, including seam coverage.
  assert(motion_output.shown[0].r == 128 && motion_output.shown[3].r == 128);
  recovered.tick(875000); // Backward correction preserves motion phase.
  assert(motion_output.shown[0].r == 128 && motion_output.shown[3].r == 128);
  recovered.tick(1100000); assert(motion_output.shown[0].r == 255 && motion_output.shown[3].r == 0);
  const auto elapsed = uint64_t(UINT32_MAX) + 830; // Phase 125 ms after 32-bit millisecond rollover.
  recovered.tick(100000 + elapsed * 1000);
  assert(motion_output.shown[0].r == 128 && motion_output.shown[1].r == 128);
}
void test_expanded_definition_limit() {
  auto c = keyframe_fixture(); auto &effect = c.effects[0]; effect.keyframes_count = 16;
  for (size_t i = 0; i < effect.keyframes_count; ++i) {
    effect.keyframes[i] = keyframe(i * 200000000, 255, 255, 255, 255,
        mesh_interop_MatrixInterpolation_SMOOTH, UINT32_MAX, i % 2 ? 1000000 : -1000000, 65536);
    effect.keyframes[i].wrap = true;
  }
  for (size_t i = 1; i < 32; ++i) { c.entries[i] = c.entries[0]; c.entries[i].id += i; }
  c.entries_count = 32;
  assert(!encode(mesh_interop_MatrixConfig_fields, &c).empty());
  const auto error = validate(c, master_mac);
  assert(error && !strcmp(error, "Node animation definitions exceed 16 KiB"));
}
void test_layout_validation() {
  auto c = fixture(); c.layout_count = 0; assert(validate(c, master_mac));
  c = fixture(); c.layout[0].from = 1; assert(validate(c, master_mac));
  c = fixture(); c.layout[0].to = 65537; assert(validate(c, master_mac));
  c = fixture(); c.layout[0].from = 12; c.layout[0].to = 10; assert(!validate(c, master_mac));
  auto d = definitions_for(c, slave_mac, 10, 20);
  assert(d.canvas_size == 12 && d.output_mappings[0].from == 12 && d.output_mappings[0].to == 10);
  player p(resolve); assert(!p.prepare(d)); // Physical length is one in this adapter, not two.
  d.output_mappings[0].to = 11; assert(p.prepare(d));
  d.canvas_size = 11; assert(!p.prepare(d));
  d.canvas_size = 65537; assert(!p.prepare(d));
  d.canvas_size = 12; d.output_mappings_count = 0; assert(!p.prepare(d));
  c = fixture(); memset(c.layout[0].node_mac, 0, 6); assert(validate(c, master_mac));
  c = fixture(); c.layout[0].node_mac[0] |= 1; assert(validate(c, master_mac));
  c = fixture(); c.layout[0].driver = static_cast<mesh_interop_MatrixDriver>(99); assert(validate(c, master_mac));
  c = fixture(); c.layout_count = 2; c.layout[1] = c.layout[0]; assert(validate(c, master_mac));
  c.layout[1].driver = mesh_interop_MatrixDriver_HC595; assert(!validate(c, master_mac)); // Overlap is valid.
  d = definitions_for(c, slave_mac, 10, 20);
  d.output_mappings[1].driver = d.output_mappings[0].driver; assert(!p.prepare(d));
  c.layout_count = 35; assert(validate(c, master_mac));
  c = fixture(); c.entries_count = 0; d = definitions_for(c, slave_mac, 10, 20);
  assert(p.prepare(d)); output.pixel = color(255, 0, 0);
  p.tick(0); red(false); // An empty animation matrix still clears the configured car outputs.
}
class canvas_test_driver : public rgb_lighting_driver {
public:
  explicit canvas_test_driver(size_t count) : pixels(count), shown(count) {}
  std::vector<color> pixels, shown;
  unsigned clears = 0, shows = 0;
  esp_err_t init() override { return ESP_OK; }
  esp_err_t deinit() override { return ESP_OK; }
  size_t get_pixel_count() const override { return pixels.size(); }
  esp_err_t set_pixel(uint16_t pixel, alpha_color c) override {
    pixels.at(pixel) = c.blend_with(pixels.at(pixel)); return ESP_OK;
  }
  esp_err_t fill(alpha_color c) override { for (auto &pixel : pixels) pixel = c.blend_with(pixel); return ESP_OK; }
  esp_err_t clear() override { std::fill(pixels.begin(), pixels.end(), color()); ++clears; return ESP_OK; }
  esp_err_t show() override { shown = pixels; ++shows; return ESP_OK; }
  esp_err_t set_global_brightness(uint8_t) override { return ESP_OK; }
  bool _is_ready() const override { return true; }
};
class discrete_test_driver : public canvas_test_driver {
public:
  discrete_test_driver() : canvas_test_driver(2) {}
  lighting_driver_kind get_kind() const override { return lighting_driver_kind::RGB_LED_MODULE; }
  size_t get_layout_source_count() const override { return 2; }
  size_t get_layout_pixel_count(uint32_t source) const override { return source < 2 ? 1 : 0; }
  size_t get_layout_pixel_offset(uint32_t source) const override { return source; }
};
std::array<discrete_test_driver, 6> six_outputs;
rgb_lighting_driver *resolve_six(uint32_t id) { return id >= 1 && id <= 6 ? &six_outputs[id - 1] : nullptr; }
void test_seven_node_six_driver_point_layout() {
  auto c = make_heap_object<matrix_config>(fixture()); c->layout_count = 0;
  c->effects[0].kind = mesh_interop_MatrixEffect_Kind_RAINBOW;
  c->effects[0].duration_ms = 0; c->effects[0].period_ms = 1000;
  std::array<std::array<uint8_t, 6>, 7> nodes{};
  for (size_t node = 0; node < nodes.size(); ++node) {
    nodes[node] = {uint8_t(2 + node * 2), 1, 2, 3, 4, 5};
    for (uint32_t driver = 1; driver <= 6; ++driver) for (uint32_t source = 0; source < 2; ++source) {
      const uint32_t position = node * 12 + (driver - 1) * 2 + source;
      auto &row = c->layout[c->layout_count++];
      row = layout_row(nodes[node].data(), static_cast<mesh_interop_MatrixDriver>(driver), position, position + 1);
      row.source_index = source;
    }
  }
  assert(c->layout_count == 84 && !validate(*c, master_mac));
  assert(!encode(mesh_interop_MatrixConfig_fields, c.get()).empty());
  canvas_test_driver reference(84);
  periodic_animation_options options; options.period_ms = 1000;
  options.primary = alpha_color(color(255, 255, 255));
  std::unique_ptr<animation_effect_instance_base> rainbow(
      animation_effect_descriptor<animation_effect_type::RAINBOW>(options).create_instance());
  for (const auto &node : nodes) {
    player local(resolve_six); assert(local.prepare(*c, node.data(), 10, 20));
    assert(local.definitions().output_mappings_count == 12 && local.enqueue(start()));
    for (uint64_t elapsed : {0, 250}) {
      const auto before = six_outputs[0].shows;
      local.tick(100000 + elapsed * 1000); reference.clear(); rainbow->seek(elapsed); rainbow->render(reference);
      assert(six_outputs[0].shows == before + 1); // One presentation per physical driver, not per LED.
      for (const auto &row : c->layout) {
        if (memcmp(row.node_mac, node.data(), 6)) continue;
        const auto actual = six_outputs[uint32_t(row.driver) - 1].shown[row.source_index];
        const auto expected = reference.pixels[row.from];
        assert(actual.r == expected.r && actual.g == expected.g && actual.b == expected.b);
      }
    }
    auto snapshot = local.snapshot(local.definitions(), 3, 0);
    player recovered(resolve_six); assert(recovered.prepare(local) && recovered.restore(snapshot));
    recovered.tick(350000);
  }
  auto invalid = c->layout[1]; c->layout[1].source_index = 0;
  assert(validate(*c, master_mac)); c->layout[1] = invalid;
  c->layout[0].to = c->layout[0].from + 2;
  player invalid_player(resolve_six); assert(!invalid_player.prepare(*c, nodes[0].data(), 10, 20));
  c->layout_count = 85; assert(validate(*c, master_mac));
}
canvas_test_driver canvas_strip_a(4), canvas_strip_b(4), canvas_block(2);
rgb_lighting_driver *resolve_canvas_a(uint32_t id) {
  return id == 1 ? &canvas_strip_a : id == 2 ? &canvas_block : nullptr;
}
rgb_lighting_driver *resolve_canvas_b(uint32_t id) { return id == 1 ? &canvas_strip_b : nullptr; }
void assert_canvas_projection(const canvas_test_driver &reference) {
  const auto same = [](color a, color b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
  for (size_t i = 0; i < 4; ++i) {
    assert(same(canvas_strip_a.shown[i], reference.pixels[i]));
    assert(same(canvas_strip_b.shown[i], reference.pixels[7 - i]));
  }
  assert(same(canvas_block.shown[0], reference.pixels[2]));
  assert(same(canvas_block.shown[1], reference.pixels[1]));
}
void test_shared_canvas_motion_and_gradient() {
  auto c = keyframe_fixture(); c.layout_count = 3;
  c.layout[0] = layout_row(slave_mac, mesh_interop_MatrixDriver_WS2812, 0, 4);
  c.layout[1] = layout_row(slave_mac, mesh_interop_MatrixDriver_HC595, 3, 1);
  c.layout[2] = layout_row(master_mac, mesh_interop_MatrixDriver_WS2812, 8, 4);
  auto &effect = c.effects[0]; effect.keyframes_count = 2;
  effect.keyframes[0] = keyframe(0, 255, 0, 0, 255, mesh_interop_MatrixInterpolation_LINEAR, 1, 0);
  effect.keyframes[1] = keyframe(2000, 255, 0, 0, 255, mesh_interop_MatrixInterpolation_LINEAR, 1, 8);
  effect.keyframes[0].wrap = effect.keyframes[1].wrap = true;
  assert(!validate(c, master_mac));
  auto a = make_definitions(c, slave_mac, 10, 20), b = make_definitions(c, master_mac, 10, 20);
  assert(a->canvas_size == 8 && b->canvas_size == 8 && a->output_mappings_count == 2);
  player node_a(resolve_canvas_a), node_b(resolve_canvas_b), recovered(resolve_canvas_a);
  assert(node_a.prepare(*a) && node_b.prepare(*b) && recovered.prepare(*a));
  assert(node_a.enqueue(start()) && node_b.enqueue(start()));
  canvas_test_driver reference(8);
  animation_keyframe frames[] = {
    {0, alpha_color(color(255, 0, 0)), animation_interpolation::LINEAR, 1, 0, 1, true},
    {2000, alpha_color(color(255, 0, 0)), animation_interpolation::LINEAR, 1, 8, 1, true}
  };
  animation_effect_descriptor<animation_effect_type::KEYFRAMES> descriptor(frames, 2, true);
  std::unique_ptr<animation_effect_instance_base> expected(descriptor.create_instance());
  for (uint64_t elapsed : {0, 500, 1000, 1750, 1875, 2000, 2500}) {
    reference.clear(); expected->seek(elapsed); expected->render(reference);
    node_a.tick(100000 + elapsed * 1000); node_b.tick(100000 + elapsed * 1000);
    assert_canvas_projection(reference);
  }
  const auto snapshot = node_a.snapshot(*a, 1, 0); assert(recovered.restore(snapshot));
  reference.clear(); expected->seek(3875); expected->render(reference);
  recovered.tick(3975000); node_b.tick(3975000); assert_canvas_projection(reference);
  assert(canvas_strip_a.shown[0].r == 128 && canvas_strip_b.shown[0].r == 128); // Global seam, not driver seam.
  effect = {}; effect.id = 1; effect.kind = mesh_interop_MatrixEffect_Kind_FLOWING_GRADIENT;
  effect.red = 255; effect.alpha = 255; effect.has_secondary_color = true; effect.secondary_color = {0, 0, 255, 255};
  a = make_definitions(c, slave_mac, 10, 20); b = make_definitions(c, master_mac, 10, 20);
  assert(node_a.prepare(*a) && node_b.prepare(*b) && node_a.enqueue(start()) && node_b.enqueue(start()));
  periodic_animation_options options; options.primary = alpha_color(color(255, 0, 0));
  options.secondary = alpha_color(color(0, 0, 255));
  animation_effect_descriptor<animation_effect_type::FLOWING_GRADIENT> gradient(options);
  expected.reset(gradient.create_instance());
  for (uint64_t elapsed : {0, 250, 500, 750, 1000}) {
    reference.clear(); expected->seek(elapsed); expected->render(reference);
    node_a.tick(100000 + elapsed * 1000); node_b.tick(100000 + elapsed * 1000);
    assert_canvas_projection(reference);
  }
  // Every layer reaches every configured output, even outputs overlapping others.
  c.effects_count = 2; c.effects[1].id = 8; c.effects[1].kind = mesh_interop_MatrixEffect_Kind_STATIC_COLOR;
  c.effects[1].blue = 255; c.effects[1].alpha = 128;
  c.entries_count = 2; c.entries[1] = c.entries[0]; c.entries[1].id = 5; c.entries[1].effect_id = 8; c.entries[1].priority = 20;
  a = make_definitions(c, slave_mac, 10, 20); b = make_definitions(c, master_mac, 10, 20);
  assert(a->layers_count == 2 && b->layers_count == 2);
  assert(node_a.prepare(*a) && node_b.prepare(*b));
  auto overlay = start(); overlay.entry_id = 5;
  assert(node_a.enqueue(overlay) && node_b.enqueue(overlay));
  node_a.tick(100000); node_b.tick(100000);
  for (auto *driver : {&canvas_strip_a, &canvas_strip_b, &canvas_block})
    for (auto pixel : driver->shown) assert(pixel.r == 0 && pixel.b == 128);
}
int main() {
  test_transfer(); test_schema_validation(); test_scheduled_player();
  test_master_evaluation_and_recovery(); test_layering();
  test_one_entry_two_drivers();
  test_node_input_definitions();
  test_effect_validation_and_roundtrip(); test_animated_player_phase_and_snapshots();
  test_compact_player_definition_ownership();
  test_compact_config_and_direct_preparation();
  test_motion_phase_recovery();
  test_expanded_definition_limit();
  test_layout_validation(); test_shared_canvas_motion_and_gradient();
  test_seven_node_six_driver_point_layout();
  std::puts("Matrix runtime: transfer, schema, scheduling, recovery, and layering passed.");
  return 0;
}
