// Exercise the actual worker state machine with deterministic radio, NVS,
// queue, and clock adapters. No production paths are replaced by a model.
#define main matrix_core_tests_main
#include "test_main.cpp"
#include <limits>
#undef main
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
// Existing ESP-NOW frame headers use the ESP-IDF flexible-array extension.
#include "../../src/matrix_runtime/matrix_service.cpp"
#pragma GCC diagnostic pop

uint64_t fake_now = 1000000;
int64_t fake_clock_offset = 0;
uint64_t fake_session = 10;
bool fake_synchronized = true, fake_mesh_master = true;
bool fail_commit = false;
std::vector<uint8_t> stored, staged;
struct transmission { uint8_t mac[6]; std::vector<uint8_t> bytes; };
std::deque<transmission> network;
int64_t esp_timer_get_time() { return fake_now; }
uint32_t esp_random() { static uint32_t id = 100; return ++id; }
esp_err_t esp_now_send(const uint8_t *mac, const uint8_t *bytes, size_t size) {
  assert(size <= 250); transmission t{}; memcpy(t.mac, mac, 6);
  t.bytes.assign(bytes, bytes + size); network.push_back(std::move(t)); return ESP_OK;
}
esp_err_t nvs_open(const char *, int mode, nvs_handle_t *h) {
  *h = 1; return mode == NVS_READONLY && stored.empty() ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *data, size_t size) {
  auto *p = static_cast<const uint8_t *>(data); staged.assign(p, p + size); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) { if (fail_commit) return ESP_FAIL; stored = staged; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *p, size_t *size) {
  if (p) { assert(*size >= stored.size()); memcpy(p, stored.data(), stored.size()); }
  *size = stored.size(); return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
global_config::global_config() : _master_node(true) {}
master_clock_manager::master_clock_manager()
    : _master_clock_offset_us(0), _last_sync_absolute_time_us(0), _session(10), _synchronized(true) {}
uint64_t master_clock_manager::session() { return fake_session; }
int64_t master_clock_manager::get_master_time() { return get_master_time_at(fake_now); }
int64_t master_clock_manager::get_master_time_at(int64_t timestamp) { return timestamp + fake_clock_offset; }
bool master_clock_manager::is_synchronized() { return fake_synchronized; }
mesh_interop::mesh_interop() : _discovery_mode(false), _state{}, _node_last_synced_at{}, _mesh_state_cb(nullptr) {}
bool mesh_interop::slave_node_exists(const uint8_t *mac) { return !memcmp(mac, slave_mac, 6); }
bool mesh_interop::master_node_matches(const uint8_t *mac) { return !memcmp(mac, master_mac, 6); }
mesh_interop::mesh_interop_state_result_t mesh_interop::get_state() {
  mesh_interop_state_result_t s{}; s.nodes_count = 1;
  memcpy(s.nodes[0].mac, fake_mesh_master ? slave_mac : master_mac, 6);
  s.nodes[0].last_synced_at = fake_now; return s;
}
namespace connected_lighting_drivers {
rgb_lighting_driver *find(uint32_t id) { return resolve(id); }
bool uses_gpio(uint32_t pin) { return pin == 5 || pin == 18 || pin == 19 || pin == 23; }
}
using matrix_service::service;
using matrix_service::packet;
namespace w = matrix::wire;
void configure_node(service &s, bool master) {
  s.master = master; s.session = 10;
  memcpy(s.mac, master ? master_mac : slave_mac, 6);
}
void upload(service &s, const matrix_config &c, uint32_t id) {
  auto bytes = encode(mesh_interop_MatrixConfig_fields, &c);
  mesh_interop_MatrixUpload u{};
  auto submit = [&] {
    auto encoded = encode(mesh_interop_MatrixUpload_fields, &u);
    packet p{}; p.connection = 1; p.size = encoded.size();
    p.generation = matrix_service::ble_generation.load();
    memcpy(p.data, encoded.data(), encoded.size()); s.handle_upload(p, fake_now);
  };
  u.which_operation = mesh_interop_MatrixUpload_begin_tag;
  u.operation.begin = {id, uint32_t(bytes.size()), crc32(bytes.data(), bytes.size())}; submit();
  for (size_t offset = 0; offset < bytes.size();) {
    u = {}; u.which_operation = mesh_interop_MatrixUpload_chunk_tag;
    auto &chunk = u.operation.chunk; chunk.transfer_id = id; chunk.offset = offset;
    chunk.data.size = std::min<size_t>(200, bytes.size() - offset);
    memcpy(chunk.data.bytes, bytes.data() + offset, chunk.data.size); submit(); offset += chunk.data.size;
  }
  u = {}; u.which_operation = mesh_interop_MatrixUpload_commit_tag; u.operation.commit = id; submit();
}
void deliver(service &master, service &slave, bool &drop_ack, w::type drop_type = w::ACK) {
  for (unsigned i = 0; !network.empty() && i < 128; ++i) {
    auto t = std::move(network.front()); network.pop_front();
    if (t.bytes[5] == drop_type && drop_ack) { drop_ack = false; continue; }
    bool to_slave = !memcmp(t.mac, slave_mac, 6);
    packet p{}; memcpy(p.mac, to_slave ? master_mac : slave_mac, 6);
    p.size = t.bytes.size(); memcpy(p.data, t.bytes.data(), p.size);
    (to_slave ? slave : master).handle_radio(p, fake_now);
  }
  assert(network.empty());
}
void cycles(service &master, service &slave, unsigned count, bool drop_one = false,
            w::type drop_type = w::ACK) {
  for (unsigned i = 0; i < count; ++i) {
    fake_now += 10000;
    master.master_step(fake_now, fake_now); master.transfer_step(fake_now);
    deliver(master, slave, drop_one, drop_type);
    master.playback_step(fake_now); slave.playback_step(fake_now);
  }
}
void test_deploy_recover_persist() {
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); c.effects[0].duration_ms = 0;
  c.sources[0].kind = mesh_interop_MatrixSource_Kind_DEFAULT_STATE;
  upload(master, c, 1); assert(master.deployment && master.revision == 1 && !stored.empty());
  cycles(master, slave, 250, true); // Lose the begin ACK, then retry it.
  assert(master.state.stage == mesh_interop_MatrixStatus_Stage_ACTIVE);
  assert(master.peers[0].ready && slave.local); red(true);
  auto d = definitions_for(c, slave_mac, 10, 1);
  auto before = slave.local->snapshot(d, 0, 0);
  assert(before.states_count == 1 && before.states[0].active);

  // Reboot the slave: HELLO reports an empty runtime and triggers definitions + snapshot recovery.
  slave.local.reset(); slave.pending_player.reset(); slave.pending_snapshot.reset();
  slave.completed_transfer = 0; slave.radio_rx.reset(); slave.local_sequence = 0;
  slave.gpio_stream = (uint64_t(esp_random()) << 32) | esp_random();
  std::vector<uint8_t> hello; w::put(hello, 10, 8); w::put(hello, 0, 8); w::put(hello, 0, 4);
  w::put(hello, slave.gpio_stream, 8); w::put(hello, 0, 1);
  auto raw = w::packet(w::HELLO, hello); packet p{};
  memcpy(p.mac, slave_mac, 6); p.size = raw.size(); memcpy(p.data, raw.data(), raw.size());
  master.handle_radio(p, fake_now); cycles(master, slave, 250);
  assert(master.peers[0].ready && slave.local);
  auto after = slave.local->snapshot(d, 0, 0);
  assert(after.states[0].started_at_us == before.states[0].started_at_us); red(true);

  const auto original_record = stored;
  auto invalid = c; invalid.entries[0].effect_id = 999;
  upload(master, invalid, 2);
  assert(master.state.stage == mesh_interop_MatrixStatus_Stage_FAILED);
  assert(master.revision == 1 && stored == original_record);
  fail_commit = true; upload(master, c, 3); fail_commit = false;
  assert(master.state.stage == mesh_interop_MatrixStatus_Stage_FAILED);
  assert(master.revision == 1 && stored == original_record); red(true);

  c.effects[0].red = 0; c.effects[0].blue = 255;
  upload(master, c, 4); assert(master.deployment);
  cycles(master, slave, 250);
  assert(slave.local->definitions().revision == 2);
  assert(output.shown.r == 0 && output.shown.b == 255);

  service restored; configure_node(restored, true); restored.session = 11; restored.load();
  assert(restored.deployment && restored.revision == 2);
  assert(restored.prepared_ledger->definitions().master_session == 11);
  stored.back() ^= 1;
  service corrupt; configure_node(corrupt, true); corrupt.load();
  assert(corrupt.state.stage == mesh_interop_MatrixStatus_Stage_FAILED && !corrupt.candidate);
}
void test_callback_queue_limits() {
  assert(matrix_service::init() == ESP_OK);
  const uint8_t bytes[] = {1};
  for (unsigned i = 0; i < 8; ++i) assert(matrix_service::upload(1, bytes, 1));
  assert(!matrix_service::upload(1, bytes, 1));
  auto raw = w::packet(w::COMMAND, {});
  for (unsigned i = 0; i < 16; ++i) assert(matrix_service::receive(master_mac, raw.data(), raw.size()));
  matrix_service::receive(master_mac, raw.data(), raw.size());
  assert(matrix_service::overflow.load());
  const auto generation = matrix_service::ble_generation.load();
  matrix_service::disconnect(1); assert(matrix_service::ble_generation.load() != generation);
  delete matrix_service::instance; matrix_service::instance = nullptr;
  delete matrix_service::ble_queue; delete matrix_service::radio_queue;
  delete matrix_service::priority_queue; delete matrix_service::gpio_queue;
}
void test_only_current_hello_format() {
  service master; configure_node(master, true);
  const auto index = master.find_peer(slave_mac); auto &peer = master.peers[index];
  peer.ready = true; peer.definitions = true; peer.seen = 123;
  std::vector<uint8_t> body; w::put(body, master.session, 8); w::put(body, 0, 8); w::put(body, 0, 4);
  const auto submit = [&] {
    auto raw = w::packet(w::HELLO, body); packet p{};
    memcpy(p.mac, slave_mac, 6); p.size = raw.size(); memcpy(p.data, raw.data(), raw.size());
    master.handle_radio(p, fake_now);
  };
  submit(); // Obsolete 20-byte HELLO cannot change readiness or liveness.
  assert(peer.ready && peer.definitions && peer.seen == 123 && !peer.gpio_stream);
  w::put(body, 456, 8); w::put(body, 0, 1); submit();
  assert(peer.ready && peer.seen == fake_now && peer.gpio_stream == 456);
}
void test_old_matrix_evaluates_until_shared_boundary() {
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto original = fixture(); upload(master, original, 20);
  cycles(master, slave, 200);
  matrix_service::gpio_event edge{fake_now, 12, true};
  assert(xQueueSend(matrix_service::gpio_queue, &edge, 0) == pdTRUE);
  cycles(master, slave, 21); red(false); // First finite animation has finished.
  auto replacement = original; replacement.sources[0].gpio = 13;
  replacement.effects[0].red = 0; replacement.effects[0].blue = 255;
  upload(master, replacement, 21);
  for (unsigned i = 0; !master.pending_snapshot && i < 100; ++i) cycles(master, slave, 1);
  assert(master.pending_snapshot && fake_now < master.activate_at);
  edge.at = fake_now;
  assert(xQueueSend(matrix_service::gpio_queue, &edge, 0) == pdTRUE);
  cycles(master, slave, 12);
  assert(slave.local->definitions().revision == 1); red(true);
  cycles(master, slave, 100);
  assert(slave.local->definitions().revision == 2); red(false);
  edge = {fake_now, 13, true};
  assert(xQueueSend(matrix_service::gpio_queue, &edge, 0) == pdTRUE);
  cycles(master, slave, 12);
  assert(output.shown.r == 0 && output.shown.b == 255);
}
void test_command_retry_gap_and_snapshot_overflow() {
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); c.effects[0].duration_ms = 5000;
  upload(master, c, 30); cycles(master, slave, 200);
  matrix_service::gpio_event edge{fake_now, 12, true};
  assert(xQueueSend(matrix_service::gpio_queue, &edge, 0) == pdTRUE);
  cycles(master, slave, 25, true, w::COMMAND_ACK);
  assert(master.peers[0].commands.empty());
  auto d = definitions_for(c, slave_mac, 10, 1);
  auto state = slave.local->snapshot(d, 0, 0);
  assert(state.states[0].started_at_us == edge.at + LEAD_US); // Retry did not restart.
  slave.local_sequence = 0; // Simulate a lost stream position.
  edge.at = fake_now;
  assert(xQueueSend(matrix_service::gpio_queue, &edge, 0) == pdTRUE);
  cycles(master, slave, 100);
  assert(master.peers[0].ready && slave.local_sequence == master.peers[0].sequence);
  state = slave.local->snapshot(d, 0, 0);
  assert(state.states[0].started_at_us == edge.at + LEAD_US);

  auto &peer = master.peers[0];
  peer.ready = false; peer.snapshotting = true;
  animation_command command{}; command.master_session = 10; command.revision = 1;
  command.entry_id = 4; command.effective_at_us = fake_now + LEAD_US;
  for (unsigned i = 0; i < 64; ++i) {
    auto queued = command; queued.sequence = ++peer.sequence; peer.commands.push_back(queued);
  }
  master.tx.peer_index = 0; master.tx.kind = 2;
  assert(service::emit(&master, command));
  assert(master.tx.peer_index == -1 && !peer.snapshotting && peer.commands.empty());
  cycles(master, slave, 100);
  assert(peer.ready && slave.local_sequence == peer.sequence);
  state = slave.local->snapshot(d, 0, 0);
  assert(state.states[0].started_at_us == command.effective_at_us);
}
void test_animated_deployment_and_reconnect() {
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = keyframe_fixture();
  c.sources[0].kind = mesh_interop_MatrixSource_Kind_DEFAULT_STATE;
  // Include a second independently addressed track to exercise chunking and persistence.
  auto &e = c.effects[0]; e.keyframes_count = 5;
  e.keyframes[3] = keyframe(0, 0, 255, 0, 255, mesh_interop_MatrixInterpolation_STEP, 1, 1, 2);
  e.keyframes[4] = keyframe(1000, 0, 0, 255, 255, mesh_interop_MatrixInterpolation_LINEAR, 1, 5, 1);
  upload(master, c, 70); cycles(master, slave, 200, true);
  assert(slave.local && master.peers[0].ready);
  assert(slave.local->definitions().layers[0].effect.keyframes_count == 5);
  const auto started = master.activate_at;
  fake_now = started + ((fake_now - started) / 1000000 + 1) * 1000000 + 250000;
  master.playback_step(fake_now); slave.playback_step(fake_now);
  assert(output.shown.r == 128 && output.shown.b == 128);
  slave.local.reset(); slave.pending_player.reset(); slave.pending_snapshot.reset();
  slave.completed_transfer = 0; slave.radio_rx.reset(); slave.local_sequence = 0;
  master.peers[0].ready = false; master.peers[0].definitions = false;
  cycles(master, slave, 200);
  assert(master.peers[0].ready && slave.local);
  auto d = definitions_for(c, slave_mac, 10, 1);
  auto snapshot = slave.local->snapshot(d, 0, 0);
  assert(snapshot.states[0].started_at_us == started);
  fake_now += 250000; slave.playback_step(fake_now); assert(output.shown.b == 255);
  service restored; configure_node(restored, true); restored.session = 11; restored.load();
  assert(restored.candidate && restored.candidate->effects[0].keyframes_count == 5);
  assert(restored.candidate->effects[0].keyframes[3].position == 1);
  const auto saved = stored;
  auto invalid = c; invalid.effects[0].keyframes[0].position = std::numeric_limits<float>::quiet_NaN();
  upload(master, invalid, 71);
  assert(master.state.stage == mesh_interop_MatrixStatus_Stage_FAILED);
  assert(master.revision == 1 && master.local && stored == saved);
}
void slave_tick(service &slave) {
  fake_mesh_master = false; slave.slave_step(fake_now); fake_mesh_master = true;
}
packet gpio_packet(const service &slave, uint32_t sequence, uint64_t at,
                   uint64_t revision = 1, uint32_t pin = 12, bool rising = true) {
  std::vector<uint8_t> b;
  w::put(b, slave.session, 8); w::put(b, revision, 8); w::put(b, slave.gpio_stream, 8);
  w::put(b, sequence, 4); w::put(b, at, 8); w::put(b, pin, 1); w::put(b, rising, 1);
  auto bytes = w::packet(w::GPIO_EVENT, b);
  packet p{}; memcpy(p.mac, slave.mac, 6); p.size = bytes.size();
  memcpy(p.data, bytes.data(), bytes.size()); return p;
}
void capture_slave_edge(service &slave, uint32_t pin, bool rising) {
  assert(slave.configured_pins[pin] && fake_gpio_handlers[pin]);
  fake_gpio_levels[pin] = rising;
  fake_gpio_handlers[pin](fake_gpio_args[pin]);
}
void test_remote_gpio_delivery() {
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); memcpy(c.sources[0].node_mac, slave_mac, 6);
  c.effects[0].duration_ms = 1000;
  c.layout_count = 2; c.layout[1] = c.layout[0];
  memcpy(c.layout[1].node_mac, master_mac, 6);
  upload(master, c, 40); cycles(master, slave, 200);
  assert(slave.local && slave.configured_pins[12] && !master.configured_pins[12]);
  const auto captured_at = fake_now;
  capture_slave_edge(slave, 12, true);
  fake_now += 20000; slave_tick(slave); // Worker latency is not part of the timestamp.
  assert(slave.gpio_events.size() == 1 && slave.gpio_events.front().edge.at == captured_at);
  bool drop = true; deliver(master, slave, drop, w::GPIO_EVENT_ACK);
  assert(!drop && master.peers[0].gpio_sequence == 1 && !slave.gpio_events.empty());
  cycles(master, slave, 11);
  auto d = definitions_for(c, slave_mac, 10, 1);
  auto state = slave.local->snapshot(d, 0, 0);
  assert(state.states[0].started_at_us == captured_at + LEAD_US); red(true);
  auto own = definitions_for(c, master_mac, 10, 1);
  auto own_state = master.local->snapshot(own, 0, 0);
  assert(own_state.states[0].started_at_us == state.states[0].started_at_us);
  const auto command_sequence = master.peers[0].sequence;
  slave_tick(slave); drop = false; deliver(master, slave, drop);
  assert(slave.gpio_events.empty() && master.peers[0].sequence == command_sequence);
  cycles(master, slave, 1);
  state = slave.local->snapshot(d, 0, 0);
  assert(state.states[0].started_at_us == captured_at + LEAD_US); // Retry did not retrigger.

  capture_slave_edge(slave, 12, false); slave_tick(slave); deliver(master, slave, drop);
  assert(master.peers[0].gpio_sequence == 2 && master.peers[0].sequence == command_sequence);
  fake_now += 10000; const auto retriggered_at = fake_now;
  capture_slave_edge(slave, 12, true); slave_tick(slave); deliver(master, slave, drop);
  cycles(master, slave, 12);
  state = slave.local->snapshot(d, 0, 0);
  assert(state.states[0].started_at_us == retriggered_at + LEAD_US);

  // A late finite effect is skipped using its original start, even on the master.
  auto late = gpio_packet(slave, 4, fake_now - 2000000);
  master.handle_radio(late, fake_now); deliver(master, slave, drop); cycles(master, slave, 1);
  state = slave.local->snapshot(d, 0, 0); assert(!state.states[0].active);
  assert(state.states[0].started_at_us == fake_now - 10000 - 2000000 + LEAD_US);

  // Events are authorized against the transport sender and the active input map.
  const auto before = master.peers[0].sequence;
  auto bad = gpio_packet(slave, 5, fake_now, 1, 13);
  master.handle_radio(bad, fake_now); assert(master.peers[0].sequence == before);
  bad = gpio_packet(slave, 6, fake_now, 99);
  master.handle_radio(bad, fake_now); assert(master.peers[0].sequence == before);
  bad = gpio_packet(slave, 7, fake_now + LEAD_US + 1);
  master.handle_radio(bad, fake_now); assert(master.peers[0].sequence == before);
  bad = gpio_packet(slave, 8, fake_now - TRANSFER_TIMEOUT_US - 1);
  master.handle_radio(bad, fake_now); assert(master.peers[0].sequence == before);
  bad = gpio_packet(slave, 9, fake_now); bad.data[w::HEADER_SIZE] ^= 1;
  master.handle_radio(bad, fake_now); assert(master.peers[0].gpio_sequence == 8);
  bad = gpio_packet(slave, 9, fake_now); bad.mac[0] = 6;
  master.handle_radio(bad, fake_now); assert(master.peer_count == 1);
  network.clear();
}
void test_remote_gpio_source_only_and_replacement() {
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); memcpy(c.sources[0].node_mac, slave_mac, 6);
  memcpy(c.layout[0].node_mac, master_mac, 6);
  upload(master, c, 50); cycles(master, slave, 200);
  assert(slave.local && !slave.local->definitions().layers_count && slave.configured_pins[12]);
  capture_slave_edge(slave, 12, true); slave_tick(slave);
  bool drop = false; deliver(master, slave, drop); cycles(master, slave, 12);
  auto own = definitions_for(c, master_mac, 10, 1);
  auto state = master.local->snapshot(own, 0, 0); assert(state.states[0].active);
  auto old = gpio_packet(slave, 2, fake_now);
  slave.gpio_sequence = 2;
  c.sources[0].gpio = 13; upload(master, c, 51); cycles(master, slave, 200);
  assert(slave.configured_pins[13] && !slave.configured_pins[12]);
  const auto before = master.peers[0].sequence;
  master.handle_radio(old, fake_now); assert(master.peers[0].sequence == before);
  network.clear();
  capture_slave_edge(slave, 13, true); slave_tick(slave); deliver(master, slave, drop);
  cycles(master, slave, 12);
  own = definitions_for(c, master_mac, 10, 2);
  state = master.local->snapshot(own, 0, 0); assert(state.states[0].active);

  auto invalid = c; invalid.sources[0].node_mac[0] = 6;
  upload(master, invalid, 52); assert(master.revision == 2 && !master.deployment);
  invalid = c; invalid.sources[0].gpio = 19;
  upload(master, invalid, 53); assert(master.revision == 2 && !master.deployment);

  // The owning slave validates its wiring again before acknowledging definitions.
  auto d = definitions_for(c, slave_mac, 10, 3); d.gpio_inputs[0] = 19;
  auto bytes = encode(mesh_interop_AnimationDefinitions_fields, &d);
  assert(slave.radio_rx.begin(987, bytes.size(), crc32(bytes.data(), bytes.size()), fake_now));
  assert(slave.radio_rx.append(987, 0, bytes.data(), bytes.size(), fake_now));
  slave.inbound_kind = 1; slave.inbound_revision = 3;
  assert(!slave.install_received() && slave.local->definitions().revision == 2);
  d.gpio_inputs[0] = 14; d.gpio_inputs[1] = 15; d.gpio_inputs_count = 2;
  fake_gpio_failure = 15;
  assert(!slave.prepare_gpio(d) && !slave.configured_pins[14] && slave.configured_pins[13]);
  fake_gpio_failure = -1;
}
void test_remote_gpio_time_queues_and_sessions() {
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); memcpy(c.sources[0].node_mac, slave_mac, 6);
  upload(master, c, 60); cycles(master, slave, 200);
  // The slave's local clock is three seconds behind the master clock.
  const auto master_now = fake_now;
  fake_now -= 3000000; fake_clock_offset = 3000000;
  capture_slave_edge(slave, 12, true); fake_now += 20000; slave_tick(slave);
  assert(slave.gpio_events.front().edge.at == master_now);
  fake_now = master_now + 20000; fake_clock_offset = 0;
  bool drop = false; deliver(master, slave, drop);
  assert(slave.gpio_events.empty());

  // A lost event packet is retransmitted with its original capture timestamp.
  const auto retried_at = fake_now;
  capture_slave_edge(slave, 12, true); slave_tick(slave); network.clear();
  fake_now += 100000; slave_tick(slave); deliver(master, slave, drop);
  assert(slave.gpio_events.empty());
  assert(master.peers[0].commands.back().effective_at_us == retried_at + LEAD_US);

  // Exhaust retransmissions; report loss and continue without restarting a stream.
  capture_slave_edge(slave, 12, true); slave_tick(slave); network.clear();
  for (unsigned i = 0; i < 4; ++i) {
    fake_now += 100000; slave_tick(slave); network.clear();
  }
  assert(slave.gpio_events.empty() && slave.gpio_degraded);
  slave.last_hello = 0; slave_tick(slave); deliver(master, slave, drop);
  master.master_step(fake_now, fake_now);
  assert(master.peers[0].gpio_degraded && master.state.nodes[1].state == matrix_service::NODE_DEGRADED);

  for (unsigned i = 0; i < 64; ++i) capture_slave_edge(slave, 12, true);
  capture_slave_edge(slave, 12, true); assert(matrix_service::gpio_overflow.load());
  slave.collect_gpio(fake_now);
  assert(slave.gpio_events.size() == 64 && !matrix_service::gpio_overflow.load());
  capture_slave_edge(slave, 12, true); slave.collect_gpio(fake_now);
  assert(slave.gpio_events.size() == 64 && slave.gpio_degraded);

  auto retired = gpio_packet(slave, 100, fake_now);
  slave.gpio_stream++; slave.gpio_sequence = 0; slave.gpio_events.clear();
  auto rebooted = gpio_packet(slave, 1, fake_now);
  master.handle_radio(rebooted, fake_now);
  assert(master.peers[0].gpio_sequence == 1 && master.peers[0].gpio_stream == slave.gpio_stream);
  master.handle_radio(retired, fake_now); assert(master.peers[0].gpio_sequence == 1);
  network.clear();
  capture_slave_edge(slave, 12, true); fake_synchronized = false; slave_tick(slave);
  assert(slave.gpio_events.empty()); fake_synchronized = true;
  capture_slave_edge(slave, 12, true); slave.collect_gpio(fake_now);
  assert(!slave.gpio_events.empty());
  fake_session = 11; slave_tick(slave); fake_session = 10;
  assert(slave.gpio_events.empty() && !slave.local && !slave.configured_pins[12]);
  network.clear();
}
void test_car_layout_distribution_and_recovery() {
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); c.effects[0].duration_ms = 5000; c.layout_count = 3;
  c.layout[0] = layout_row(slave_mac, mesh_interop_MatrixDriver_WS2812, 0, 1);
  c.layout[1] = layout_row(slave_mac, mesh_interop_MatrixDriver_HC595, 1, 0);
  c.layout[2] = layout_row(master_mac, mesh_interop_MatrixDriver_WS2812, 1, 2);
  upload(master, c, 80); cycles(master, slave, 200, true);
  assert(master.peers[0].ready && slave.local && master.local);
  assert(slave.local->definitions().canvas_size == 2 && master.local->definitions().canvas_size == 2);
  assert(slave.local->definitions().output_mappings_count == 2);
  matrix_service::gpio_event edge{fake_now, 12, true};
  assert(xQueueSend(matrix_service::gpio_queue, &edge, 0) == pdTRUE);
  cycles(master, slave, 12);
  assert(master.peers[0].sequence == 1); // One command per node, even with two outputs.
  assert(output.shown.r == 255 && output_secondary.shown.r == 255);
  slave.local.reset(); slave.pending_player.reset(); slave.pending_snapshot.reset();
  slave.completed_transfer = 0; slave.radio_rx.reset(); slave.local_sequence = 0;
  master.peers[0].ready = false; master.peers[0].definitions = false;
  cycles(master, slave, 150);
  assert(slave.local && slave.local->definitions().output_mappings[1].from == 1);
  const auto snapshot = slave.local->snapshot(slave.local->definitions(), 0, 0);
  assert(snapshot.states[0].started_at_us == edge.at + LEAD_US);
  c.layout[0].from = 2; c.layout[0].to = 1; upload(master, c, 81);
  for (unsigned i = 0; !slave.pending_snapshot && i < 100; ++i) cycles(master, slave, 1);
  assert(slave.pending_snapshot && fake_now < slave.pending_snapshot->activate_at_us);
  assert(slave.local->definitions().output_mappings[0].from == 0);
  assert(slave.pending_player->definitions().output_mappings[0].from == 2);
  cycles(master, slave, 150);
  assert(slave.local->definitions().output_mappings[0].from == 2);
  service restored; configure_node(restored, true); restored.session = 11; restored.load();
  assert(restored.candidate && restored.candidate->layout_count == 3 && restored.candidate->layout[0].from == 2);
  const auto saved = stored;
  auto invalid = c; invalid.layout[2].from = 0; // Local master span two, but hardware adapter has one pixel.
  upload(master, invalid, 82); assert(master.revision == 2 && stored == saved);
  invalid = c; invalid.layout[0].node_mac[0] = 6;
  upload(master, invalid, 83); assert(master.revision == 2 && stored == saved);
  invalid = c; invalid.layout_count = 0;
  upload(master, invalid, 84); assert(master.revision == 2 && stored == saved);
}
int main() {
  matrix_core_tests_main();
  matrix_service::gpio_queue = xQueueCreate(64, sizeof(matrix_service::gpio_event));
  test_deploy_recover_persist();
  test_old_matrix_evaluates_until_shared_boundary();
  test_command_retry_gap_and_snapshot_overflow();
  test_animated_deployment_and_reconnect();
  test_remote_gpio_delivery();
  test_remote_gpio_source_only_and_replacement();
  test_remote_gpio_time_queues_and_sessions();
  test_car_layout_distribution_and_recovery();
  test_only_current_hello_format();
  delete matrix_service::gpio_queue;
  test_callback_queue_limits();
  std::puts("Matrix worker: deployment, recovery, persistence, remote GPIO timing/retries/fencing, and queue limits passed.");
}
