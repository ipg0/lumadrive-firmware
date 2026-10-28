#include "matrix_runtime/matrix_core.hpp"
#include "matrix_runtime/matrix_wire.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>

using namespace matrix;
const uint8_t master_mac[6] = {2, 1, 2, 3, 4, 5};
const uint8_t slave_mac[6] = {4, 1, 2, 3, 4, 5};
matrix_config fixture() {
  matrix_config c{}; c.schema_version = 1;
  c.effects_count = 1; c.effects[0] = {1, mesh_interop_MatrixEffect_Kind_STATIC_COLOR, 255, 0, 0, 255, 100};
  c.sources_count = 1; auto &s = c.sources[0];
  s.id = 2; s.kind = mesh_interop_MatrixSource_Kind_GPIO_EDGE; s.gpio = 12; s.rising = true;
  memcpy(s.node_mac, master_mac, 6);
  c.conditions_count = 1; c.conditions[0] = {3, mesh_interop_MatrixCondition_Kind_ALWAYS};
  c.entries_count = 1; auto &e = c.entries[0];
  e.id = 4; e.effect_id = 1; e.source_id = 2; e.condition_id = 3; e.priority = 10;
  e.targets_count = 1; memcpy(e.targets[0].node_mac, slave_mac, 6);
  e.targets[0].driver = mesh_interop_MatrixDriver_WS2812;
  return c;
}
class fake_driver : public rgb_lighting_driver {
public:
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
  c = fixture(); c.entries[0].targets[0].driver = mesh_interop_MatrixDriver_DRIVER_UNSPECIFIED;
  assert(validate(c, master_mac));
  c = fixture(); c.effects[0].alpha = 256; assert(validate(c, master_mac));
  c = fixture(); c.schema_version = 2; assert(validate(c, master_mac));
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
  auto config = fixture(); auto &entry = config.entries[0];
  entry.targets_count = 2; entry.targets[1] = entry.targets[0];
  assert(validate(config, master_mac)); // Exact duplicate is invalid.
  entry.targets[1].driver = mesh_interop_MatrixDriver_HC595;
  assert(!validate(config, master_mac));
  auto d = definitions_for(config, slave_mac, 10, 20);
  assert(d.layers_count == 1 && d.layers[0].drivers_count == 2);
  player p(resolve); assert(p.prepare(d)); assert(p.enqueue(start()));
  const auto first = output.shows, second = output_secondary.shows;
  p.tick(100000);
  assert(output.shows == first + 1 && output_secondary.shows == second + 1);
  assert(output.shown.r == 255 && output_secondary.shown.r == 255);
}
void test_node_input_definitions() {
  auto c = fixture(); memcpy(c.sources[0].node_mac, slave_mac, 6);
  c.sources[1] = c.sources[0]; c.sources[1].id = 8; c.sources[1].rising = false; c.sources_count = 2;
  memcpy(c.entries[0].targets[0].node_mac, master_mac, 6);
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
int main() {
  test_transfer(); test_schema_validation(); test_scheduled_player();
  test_master_evaluation_and_recovery(); test_layering();
  test_one_entry_two_drivers();
  test_node_input_definitions();
  std::puts("Matrix runtime: transfer, schema, scheduling, recovery, and layering passed.");
  return 0;
}
