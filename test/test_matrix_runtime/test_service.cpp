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
uint64_t fake_last_sync = 0;
uint64_t fake_startup_at = 0;
bool fake_radio_standby = false;
bool fake_discovery = false;
bool fake_peer_online = true;
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
  *h = 1; return mode == NVS_READONLY && stored.empty() ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
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
int64_t master_clock_manager::last_sync_at() { return fake_last_sync ? fake_last_sync : fake_now; }
mesh_interop::mesh_interop() : _discovery_mode(false), _state{}, _node_last_synced_at{}, _startup_at(fake_now), _mesh_state_cb(nullptr) {}
bool mesh_interop::startup_waiting_for_nodes(uint64_t now) { return !fake_peer_online && now < fake_startup_at + STARTUP_WAKE_US; }
esp_err_t mesh_interop::set_standby_radio(bool standby) { fake_radio_standby = standby; return ESP_OK; }
bool mesh_interop::slave_node_exists(const uint8_t *mac) { return !memcmp(mac, slave_mac, 6); }
bool mesh_interop::master_node_matches(const uint8_t *mac) { return !memcmp(mac, master_mac, 6); }
mesh_interop::mesh_interop_state_result_t mesh_interop::get_state() {
  mesh_interop_state_result_t s{}; s.is_discovery_mode = fake_discovery;
  if (fake_discovery) return s;
  s.nodes_count = 1;
  memcpy(s.nodes[0].mac, fake_mesh_master ? slave_mac : master_mac, 6);
  s.nodes[0].last_synced_at = fake_peer_online ? fake_now : 0; return s;
}
namespace connected_lighting_drivers {
esp_err_t fake_status = ESP_OK;
uint64_t fake_gpio_mask = (1ULL << 12) | (1ULL << 13) | (1ULL << 14) | (1ULL << 35);
uint64_t fake_inverted_mask = 0;
esp_err_t status() { return fake_status; }
driver_resolver configured_resolver = resolve;
rgb_lighting_driver *find(uint32_t id) { return configured_resolver(id); }
bool uses_gpio(uint32_t pin) { return pin == 5 || pin == 18 || pin == 19 || pin == 23; }
bool gpio_input(uint32_t pin, uint8_t &pull, bool *inverted) {
  if (pin >= 40 || !(fake_gpio_mask & (1ULL << pin))) return false;
  pull = pin == 12 ? hardware_config::PULL_UP : pin == 13 ? hardware_config::PULL_DOWN : hardware_config::PULL_NONE;
  if (inverted) *inverted = bool(fake_inverted_mask & (1ULL << pin));
  return true;
}
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
  assert(w::get(stored.data() + 4, 4) == 0);
  stored[4] = 1;
  service obsolete; configure_node(obsolete, true); obsolete.initialize_configuration();
  assert(obsolete.candidate && obsolete.candidate->factory_default && obsolete.revision == 1);
  assert(w::get(stored.data() + 4, 4) == 0);
  stored = original_record;
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
  matrix_service::instance.reset();
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
void test_slave_reboot_after_offline_timeout() {
  stored.clear(); staged.clear(); network.clear();
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); c.effects[0].duration_ms = 0;
  c.sources[0].kind = mesh_interop_MatrixSource_Kind_DEFAULT_STATE;
  upload(master, c, 11); cycles(master, slave, 250);
  fake_mesh_master = false; slave.slave_step(fake_now); fake_mesh_master = true;
  bool drop = false; deliver(master, slave, drop);
  assert(master.peers[0].ready && master.peers[0].definitions && master.peers[0].gpio_stream);
  const auto previous_stream = master.peers[0].gpio_stream;
  fake_peer_online = false;
  cycles(master, slave, 420);
  assert(!master.peers[0].ready && master.peers[0].definitions);

  service rebooted; configure_node(rebooted, false);
  fake_peer_online = true;
  fake_mesh_master = false; rebooted.slave_step(fake_now); fake_mesh_master = true;
  deliver(master, rebooted, drop);
  assert(master.peers[0].gpio_stream != previous_stream);
  assert(!master.peers[0].ready && !master.peers[0].definitions);
  cycles(master, rebooted, 250);
  assert(master.peers[0].ready && rebooted.local);
  auto state = rebooted.local->snapshot(rebooted.local->definitions(), 0, 0);
  assert(state.states[0].active && state.states[0].started_at_us == master.activate_at);

  // Even if the previous stream was never observed, rejection of a snapshot
  // must cause definitions to be resent instead of retrying snapshots forever.
  service unknown_reboot; configure_node(unknown_reboot, false);
  master.peers[0].ready = false; master.peers[0].definitions = true;
  master.peers[0].gpio_stream = 0; master.peers[0].last_recover = 0;
  fake_mesh_master = false; unknown_reboot.slave_step(fake_now); fake_mesh_master = true;
  deliver(master, unknown_reboot, drop);
  cycles(master, unknown_reboot, 350);
  assert(master.peers[0].ready && unknown_reboot.local);
  fake_peer_online = true; stored.clear(); staged.clear(); network.clear();
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
  auto received = std::make_unique<animation_definitions>();
  const auto &encoded = slave.local->definition_bytes();
  assert(decode(mesh_interop_AnimationDefinitions_fields, received.get(), encoded.data(), encoded.size()));
  assert(received->layers[0].effect.keyframes_count == 5);
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
void test_boot_palette_deployment_and_restore() {
  stored.clear(); staged.clear(); network.clear();
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto config = fixture(); config.sources[0].kind = mesh_interop_MatrixSource_Kind_ON_BOOT;
  auto &effect = config.effects[0]; effect.kind = mesh_interop_MatrixEffect_Kind_FLOWING_GRADIENT;
  effect.duration_ms = 5000; effect.period_ms = 1000; effect.gradient_colors_count = 3;
  effect.gradient_colors[0] = {255, 0, 0, 255};
  effect.gradient_colors[1] = {0, 255, 0, 128};
  effect.gradient_colors[2] = {0, 0, 255, 255};
  upload(master, config, 75); cycles(master, slave, 200);
  assert(slave.local && master.peers[0].ready);
  const auto started = master.activate_at;
  auto snapshot = slave.local->snapshot(slave.local->definitions(), 0, 0);
  assert(snapshot.states[0].active && snapshot.states[0].started_at_us == started);
  auto received = std::make_unique<animation_definitions>();
  const auto &bytes = slave.local->definition_bytes();
  assert(decode(mesh_interop_AnimationDefinitions_fields, received.get(), bytes.data(), bytes.size()));
  assert(received->layers[0].effect.gradient_colors_count == 3);
  assert(received->layers[0].effect.gradient_colors[1].green == 255);
  slave.local.reset(); slave.pending_player.reset(); slave.pending_snapshot.reset();
  slave.completed_transfer = 0; slave.radio_rx.reset(); slave.local_sequence = 0;
  master.peers[0].ready = false; master.peers[0].definitions = false;
  cycles(master, slave, 150);
  assert(master.peers[0].ready && slave.local);
  snapshot = slave.local->snapshot(slave.local->definitions(), 0, 0);
  assert(snapshot.states[0].active && snapshot.states[0].started_at_us == started);
  periodic_animation_options options; options.gradient_color_count = 3;
  options.gradient_colors[0] = alpha_color(color(255, 0, 0), 255);
  options.gradient_colors[1] = alpha_color(color(0, 255, 0), 128);
  options.gradient_colors[2] = alpha_color(color(0, 0, 255), 255);
  std::unique_ptr<animation_effect_instance_base> expected(
      animation_effect_descriptor<animation_effect_type::FLOWING_GRADIENT>(options).create_instance());
  fake_driver reference; expected->seek((fake_now - started) / 1000); expected->render(reference);
  assert(output.shown.r == reference.pixel.r && output.shown.g == reference.pixel.g && output.shown.b == reference.pixel.b);
  cycles(master, slave, 500); red(false);
  upload(master, config, 76); cycles(master, slave, 200);
  snapshot = slave.local->snapshot(slave.local->definitions(), 0, 0);
  assert(!snapshot.states[0].active); // A save must not replay a boot trigger.
  service rebooted; configure_node(rebooted, true); assert(rebooted.load());
  cycles(rebooted, slave, 200);
  snapshot = slave.local->snapshot(slave.local->definitions(), 0, 0);
  assert(snapshot.states[0].active && snapshot.states[0].started_at_us == rebooted.activate_at);
  assert(snapshot.states[0].started_at_us != started);
  stored.clear(); staged.clear(); network.clear();
}
void test_mode_switch_deployment() {
  stored.clear(); staged.clear(); network.clear();
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto config = fixture(); config.modes_count = 2;
  config.modes[0].id = 1; strcpy(config.modes[0].name, "Day");
  config.modes[1].id = 2; strcpy(config.modes[1].name, "Night");
  config.active_mode_id = 1;
  config.conditions[0].kind = mesh_interop_MatrixCondition_Kind_MODE; config.conditions[0].mode_id = 2;
  config.sources[0].kind = mesh_interop_MatrixSource_Kind_ON_MODE_SWITCHED;
  config.effects[0].duration_ms = 5000;
  upload(master, config, 601); cycles(master, slave, 200);
  assert(slave.local && !slave.local->snapshot(slave.local->definitions(), 0, 0).states[0].active);
  config.active_mode_id = 2;
  upload(master, config, 602); cycles(master, slave, 200);
  const auto snapshot = slave.local->snapshot(slave.local->definitions(), 0, 0);
  assert(snapshot.states[0].active && snapshot.states[0].started_at_us == master.activate_at);
  service restored; configure_node(restored, true); assert(restored.load());
  assert(restored.candidate && restored.candidate->active_mode_id == 2 && restored.candidate->modes_count == 2);
  stored.clear(); staged.clear(); network.clear();
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
  connected_lighting_drivers::fake_inverted_mask = 1ULL << 12;
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); memcpy(c.sources[0].node_mac, slave_mac, 6);
  c.effects[0].duration_ms = 1000;
  c.layout_count = 2; c.layout[1] = c.layout[0];
  memcpy(c.layout[1].node_mac, master_mac, 6);
  upload(master, c, 40); cycles(master, slave, 200);
  assert(slave.local && slave.configured_pins[12] && !master.configured_pins[12]);
  assert(fake_gpio_configs[12].mode == GPIO_MODE_INPUT && fake_gpio_configs[12].intr_type == GPIO_INTR_ANYEDGE);
  assert(fake_gpio_configs[12].pull_up_en == GPIO_PULLUP_ENABLE && fake_gpio_configs[12].pull_down_en == GPIO_PULLDOWN_DISABLE);
  const auto captured_at = fake_now;
  capture_slave_edge(slave, 12, false); // Physical falling is logical rising.
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

  capture_slave_edge(slave, 12, true); slave_tick(slave); deliver(master, slave, drop);
  assert(master.peers[0].gpio_sequence == 2 && master.peers[0].sequence == command_sequence);
  fake_now += 10000; const auto retriggered_at = fake_now;
  capture_slave_edge(slave, 12, false); slave_tick(slave); deliver(master, slave, drop);
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
  connected_lighting_drivers::fake_inverted_mask = 0;
}
void test_provisioned_gpio_inventory() {
  network.clear();
  service master, slave; configure_node(master, true); configure_node(slave, false);
  connected_lighting_drivers::fake_gpio_mask = (1ULL << 13) | (1ULL << 35);
  slave.send_inventory(master_mac);
  bool drop = false; deliver(master, slave, drop);
  assert(master.peer_count == 1 && master.peers[0].catalog_known);
  assert(master.peers[0].gpio_mask == ((1ULL << 13) | (1ULL << 35)));
  assert(!strcmp(master.peers[0].firmware_version, "test-version"));
  // A legacy inventory arriving after the extended one must not erase its version.
  slave.send_inventory(master_mac);
  const auto legacy = network.front().bytes;
  auto malformed = network.back().bytes; network.clear();
  packet received{}; memcpy(received.mac, slave_mac, 6);
  received.size = legacy.size(); memcpy(received.data, legacy.data(), legacy.size());
  master.handle_radio(received, fake_now);
  assert(!strcmp(master.peers[0].firmware_version, "test-version"));
  // Invalid version length cannot replace a previously accepted catalog.
  malformed[w::HEADER_SIZE + 17 + malformed[w::HEADER_SIZE + 8] * 14] = 32;
  received.size = malformed.size(); memcpy(received.data, malformed.data(), malformed.size());
  master.handle_radio(received, fake_now + 1);
  assert(!strcmp(master.peers[0].firmware_version, "test-version"));
  assert(!service::valid_input(12) && service::valid_input(13) && service::valid_input(35));
  connected_lighting_drivers::fake_gpio_mask = (1ULL << 12) | (1ULL << 13) | (1ULL << 14) | (1ULL << 35);
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
  assert(fake_gpio_configs[13].pull_up_en == GPIO_PULLUP_DISABLE && fake_gpio_configs[13].pull_down_en == GPIO_PULLDOWN_ENABLE);
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
void upload_editor(service &s, const pb_msgdesc_t *fields, const void *update, uint32_t id, bool matrix = false) {
  const auto bytes = encode(fields, update);
  const auto submit = [&](const mesh_interop_MatrixUpload &u) {
    const auto encoded = encode(mesh_interop_MatrixUpload_fields, &u);
    packet p{}; p.connection = 1; p.generation = matrix_service::ble_generation.load(); p.size = encoded.size();
    memcpy(p.data, encoded.data(), encoded.size()); s.handle_upload(p, fake_now);
  };
  mesh_interop_MatrixUpload u{};
  u.which_operation = matrix ? mesh_interop_MatrixUpload_matrix_begin_tag : mesh_interop_MatrixUpload_layout_begin_tag;
  const mesh_interop_TransferBegin begin{id, uint32_t(bytes.size()), crc32(bytes.data(), bytes.size())};
  if (matrix) u.operation.matrix_begin = begin; else u.operation.layout_begin = begin;
  submit(u);
  for (size_t offset = 0; offset < bytes.size();) {
    u = {}; u.which_operation = mesh_interop_MatrixUpload_chunk_tag;
    auto &chunk = u.operation.chunk; chunk.transfer_id = id; chunk.offset = offset;
    chunk.data.size = std::min<size_t>(200, bytes.size() - offset);
    memcpy(chunk.data.bytes, bytes.data() + offset, chunk.data.size); submit(u); offset += chunk.data.size;
  }
  u = {}; u.which_operation = mesh_interop_MatrixUpload_commit_tag; u.operation.commit = id; submit(u);
}
void upload_layout(service &s, const mesh_interop_LayoutUpdate &update, uint32_t id) {
  upload_editor(s, mesh_interop_LayoutUpdate_fields, &update, id);
}
void test_editor_patch_decoding() {
  auto config = make_heap_object<matrix_config>(fixture());
  auto update = make_heap_object<mesh_interop_LayoutUpdate>();
  update->expected_revision = 300; update->master_session = UINT64_MAX;
  update->layout_count = MAX_LAYOUT_SOURCES;
  for (size_t i = 0; i < update->layout_count; ++i) {
    update->layout[i] = layout_row(slave_mac, mesh_interop_MatrixDriver_DRIVER_6, i, i + 1);
    update->layout[i].source_index = i % 2;
    snprintf(update->layout[i].name, sizeof(update->layout[i].name), "Independent LED %u", unsigned(i));
  }
  auto bytes = encode(mesh_interop_LayoutUpdate_fields, update.get());
  uint64_t revision, session;
  assert(matrix_service::decode_editor_update(bytes, true, *config, revision, session));
  assert(revision == 300 && session == UINT64_MAX && config->layout_count == MAX_LAYOUT_SOURCES);
  assert(config->effects_count == 1 && config->effects[0].red == 255 && config->entries_count == 1);
  runtime_config compact_layout(fixture());
  assert(matrix_service::decode_compact_editor_update(bytes, true, compact_layout, revision, session));
  assert(compact_layout.reencode() && compact_layout.encoded() == encode(mesh_interop_MatrixConfig_fields, config.get()));
  for (size_t i = 0; i < config->layout_count; ++i)
    assert(encode(mesh_interop_MatrixLayout_fields, &config->layout[i]) == encode(mesh_interop_MatrixLayout_fields, &update->layout[i]));
  // Unknown protobuf fields remain forward-compatible; malformed ones fail.
  bytes.insert(bytes.end(), {0xa0, 0x06, 123});
  assert(matrix_service::decode_editor_update(bytes, true, *config, revision, session));
  bytes.insert(bytes.end(), {0x1a, 2, 0x30, 1}); // An 85th layout source.
  assert(!matrix_service::decode_editor_update(bytes, true, *config, revision, session));
  assert(!matrix_service::decode_editor_update({0x1a, 3, 0x30, 1}, true, *config, revision, session));
  assert(!matrix_service::decode_editor_update({0x1a, 1, 0}, true, *config, revision, session));
  assert(!matrix_service::decode_editor_update({0x0a, 0}, true, *config, revision, session));
  assert(!matrix_service::decode_editor_update({0}, true, *config, revision, session));
  // Nested strings keep Nanopb's length limit.
  bytes = {0x1a, 0x84, 1, 0x2a, 0x81, 1}; bytes.insert(bytes.end(), 129, 'x');
  assert(!matrix_service::decode_editor_update(bytes, true, *config, revision, session));
  *config = fixture();
  auto matrix = make_heap_object<mesh_interop_MatrixUpdate>();
  matrix->expected_revision = 400; matrix->master_session = UINT64_MAX - 1;
  matrix->modes_count = 2; matrix->modes[0].id = 1; strcpy(matrix->modes[0].name, "Day");
  matrix->modes[1].id = 2; strcpy(matrix->modes[1].name, "Night"); matrix->active_mode_id = 2;
  matrix->effects_count = matrix->sources_count = matrix->conditions_count = matrix->entries_count = 32;
  for (size_t i = 0; i < 32; ++i) {
    matrix->effects[i] = config->effects[0]; matrix->effects[i].id = i + 1;
    matrix->sources[i] = config->sources[0]; matrix->sources[i].id = i + 1;
    matrix->conditions[i] = config->conditions[0]; matrix->conditions[i].id = i + 1;
    matrix->entries[i] = config->entries[0]; matrix->entries[i].id = i + 1;
  }
  bytes = encode(mesh_interop_MatrixUpdate_fields, matrix.get());
  assert(matrix_service::decode_editor_update(bytes, false, *config, revision, session));
  assert(revision == 400 && session == UINT64_MAX - 1 && config->layout_count == 1);
  assert(config->effects_count == 32 && config->sources_count == 32 && config->conditions_count == 32 && config->entries_count == 32);
  assert(config->effects[31].id == 32 && config->sources[31].id == 32 && config->conditions[31].id == 32 && config->entries[31].id == 32);
  assert(config->modes_count == 2 && config->active_mode_id == 2 && !strcmp(config->modes[1].name, "Night"));
  runtime_config compact_matrix(fixture());
  assert(matrix_service::decode_compact_editor_update(bytes, false, compact_matrix, revision, session));
  assert(compact_matrix.reencode() && compact_matrix.encoded() == encode(mesh_interop_MatrixConfig_fields, config.get()));
  assert(compact_matrix.modes_count == 2 && compact_matrix.active_mode_id == 2);
  bytes.insert(bytes.end(), {0x1a, 2, 0x08, 33}); // A 33rd effect.
  assert(!matrix_service::decode_editor_update(bytes, false, *config, revision, session));
  assert(!matrix_service::decode_compact_editor_update(bytes, false, compact_matrix, revision, session));
  // Empty repeated fields replace the edited portion, preserving the other half.
  assert(matrix_service::decode_editor_update({0x08, 1, 0x10, 2}, false, *config, revision, session));
  assert(!config->effects_count && !config->sources_count && !config->conditions_count && !config->entries_count && config->layout_count == 1);
  assert(!config->modes_count && !config->active_mode_id);
  *config = fixture();
  assert(matrix_service::decode_editor_update({0x08, 1, 0x10, 2}, true, *config, revision, session));
  assert(!config->layout_count && config->effects_count == 1 && config->entries_count == 1);
  auto original = fixture(); original.modes_count = 1; original.modes[0].id = 1;
  strcpy(original.modes[0].name, "Night"); original.active_mode_id = 1;
  const auto full = encode(mesh_interop_MatrixConfig_fields, &original);
  runtime_config compact_full;
  assert(matrix_service::decode_compact_config(full, compact_full));
  assert(compact_full.reencode() && compact_full.encoded() == full);
  assert(!validate(compact_full, master_mac));
  auto truncated = full; truncated.pop_back();
  runtime_config bad;
  assert(!matrix_service::decode_compact_config(truncated, bad));
}
void layout_request(service &s, uint32_t request_id, uint32_t snapshot_id = 0, uint32_t offset = 0,
                    bool identify = false, uint32_t output_index = 0, bool matrix = false, bool active = false,
                    uint32_t count = 0, bool reversed = false, bool whole_node = false) {
  mesh_interop_MatrixUpload u{}; u.which_operation = mesh_interop_MatrixUpload_layout_request_tag;
  auto &q = u.operation.layout_request; q.request_id = request_id; q.snapshot_id = snapshot_id;
  q.offset = offset; q.operation = active ? mesh_interop_LayoutRequest_Operation_READ_ACTIVE_MATRIX : matrix ? mesh_interop_LayoutRequest_Operation_READ_MATRIX : identify ? mesh_interop_LayoutRequest_Operation_IDENTIFY : mesh_interop_LayoutRequest_Operation_READ;
  q.output_index = output_index;
  if (count || reversed || whole_node) { q.has_identify_options = true; q.identify_options = output_index | (count << 8) | (uint32_t(reversed) << 20) | (uint32_t(whole_node) << 21); }
  const auto encoded = encode(mesh_interop_MatrixUpload_fields, &u);
  packet p{}; p.connection = 1; p.generation = matrix_service::ble_generation.load(); p.size = encoded.size();
  memcpy(p.data, encoded.data(), encoded.size()); s.handle_upload(p, fake_now);
}
mesh_interop_LayoutResponse layout_response() {
  uint8_t bytes[512]; const auto size = matrix_service::read_layout_response(bytes, sizeof(bytes));
  mesh_interop_LayoutResponse response{};
  assert(size && decode(mesh_interop_LayoutResponse_fields, &response, bytes, size)); return response;
}
void test_layout_editor_service() {
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); c.effects[0].duration_ms = 0; c.sources[0].kind = mesh_interop_MatrixSource_Kind_DEFAULT_STATE;
  c.layout_count = 3;
  c.layout[0] = layout_row(master_mac, mesh_interop_MatrixDriver_WS2812, 0, 1);
  c.layout[1] = layout_row(slave_mac, mesh_interop_MatrixDriver_WS2812, 1, 2);
  c.layout[2] = layout_row(slave_mac, mesh_interop_MatrixDriver_HC595, 2, 1);
  for (auto &row : c.layout) snprintf(row.name, sizeof(row.name), "%0120d", 1);
  upload(master, c, 90); cycles(master, slave, 200);
  assert(master.peers[0].catalog_known && master.peers[0].outputs_count == 2);
  assert(master.peers[0].outputs[0].pixel_count == output.get_pixel_count());
  layout_request(master, 1); const auto first = layout_response();
  assert(first.request_id == 1 && !first.error[0] && !first.complete && first.data.size == 320);
  std::vector<uint8_t> bytes(first.data.bytes, first.data.bytes + first.data.size);
  // Snapshot pages retain the same layout/revision while a new config is committed.
  c.layout[0].name[0] = 'X'; upload(master, c, 91); cycles(master, slave, 200);
  uint32_t request = 2;
  while (bytes.size() < first.total_size) {
    layout_request(master, request++, first.snapshot_id, bytes.size()); const auto page = layout_response();
    assert(!page.error[0] && page.snapshot_id == first.snapshot_id && page.offset == bytes.size());
    bytes.insert(bytes.end(), page.data.bytes, page.data.bytes + page.data.size);
  }
  auto snapshot = std::make_unique<mesh_interop_LayoutSnapshot>();
  assert(decode(mesh_interop_LayoutSnapshot_fields, snapshot.get(), bytes.data(), bytes.size()));
  assert(snapshot->revision == 1 && snapshot->layout[0].name[0] != 'X' && snapshot->outputs_count == 4);
  assert(snapshot->outputs[2].known && snapshot->outputs[2].pixel_count == 1);
  auto update = std::make_unique<mesh_interop_LayoutUpdate>(); update->master_session = 10; update->expected_revision = 1;
  update->layout_count = c.layout_count; std::copy_n(c.layout, c.layout_count, update->layout);
  const auto saved = stored; upload_layout(master, *update, 92);
  assert(master.state.stage == mesh_interop_MatrixStatus_Stage_FAILED && master.revision == 2 && stored == saved);
  update->expected_revision = 2; update->layout[1].from = 4; update->layout[1].to = 5;
  snprintf(update->layout[1].name, sizeof(update->layout[1].name), "Rear indicator");
  upload_layout(master, *update, 93);
  assert(master.deployment && master.candidate->entries_count == c.entries_count && master.candidate->effects[0].red == 255);
  assert(master.candidate->sources[0].kind == c.sources[0].kind && master.candidate->layout[1].from == 4);
  cycles(master, slave, 200); assert(master.revision == 3 && slave.local->definitions().canvas_size == 5);
  auto persisted = std::make_unique<matrix_config>();
  assert(decode(mesh_interop_MatrixConfig_fields, persisted.get(), stored.data() + 24, stored.size() - 24));
  assert(persisted->entries_count == 1 && !strcmp(persisted->layout[1].name, "Rear indicator"));
  const auto valid_saved = stored;
  update->expected_revision = 3; update->master_session = 11;
  upload_layout(master, *update, 94);
  assert(master.state.stage == mesh_interop_MatrixStatus_Stage_FAILED && master.revision == 3 && stored == valid_saved);
  update->master_session = 10; update->layout[1].to = 6;
  upload_layout(master, *update, 95);
  assert(master.state.stage == mesh_interop_MatrixStatus_Stage_FAILED && master.revision == 3 && stored == valid_saved);
  update->layout[1].to = 5;
  fail_commit = true; upload_layout(master, *update, 96); fail_commit = false;
  assert(master.state.stage == mesh_interop_MatrixStatus_Stage_FAILED && master.revision == 3 && stored == valid_saved);
  assert(master.config->effects[0].red == 255 && master.config->layout[1].to == 5);
  layout_request(master, 20); auto response = layout_response(); const auto token = response.snapshot_id;
  layout_request(master, 21, token, 0, true, 0); response = layout_response();
  assert(response.request_id == 21 && response.complete && master.identify_driver == 1);
  master.playback_step(fake_now); assert(output.shown.g == 220 && output.shown.r == 0);
  const auto before_identify = stored;
  fake_now += 3000001; master.playback_step(fake_now);
  assert(!master.identify_driver && output.shown.r == 255 && stored == before_identify);
  // Unmapped output identification also clears itself at the deadline.
  layout_request(master, 22); response = layout_response();
  layout_request(master, 23, response.snapshot_id, 0, true, 1); master.playback_step(fake_now);
  assert(output_secondary.shown.g == 220); fake_now += 3000001; master.playback_step(fake_now);
  assert(output_secondary.shown.r == 0 && output_secondary.shown.g == 0 && output_secondary.shown.b == 0);
  // Slave identify ACK loss retries without restarting the three-second window.
  master.master_step(fake_now, fake_now); bool drop = false; deliver(master, slave, drop);
  layout_request(master, 24); response = layout_response();
  layout_request(master, 25, response.snapshot_id, 0, true, 3);
  master.master_step(fake_now, fake_now); drop = true; deliver(master, slave, drop, w::IDENTIFY_ACK);
  assert(!drop && slave.identify_driver == 2); const auto deadline = slave.identify_until;
  fake_now += 100000; master.master_step(fake_now, fake_now); deliver(master, slave, drop);
  response = layout_response(); assert(response.request_id == 25 && response.complete && !master.identifying.active);
  assert(slave.identify_until == deadline); slave.playback_step(fake_now); assert(output_secondary.shown.g == 220);
  fake_now = deadline + 1; slave.playback_step(fake_now); assert(output_secondary.shown.r == 255);
  // Whole-node identify uses one acknowledged request and flashes every driver.
  layout_request(master, 30); response = layout_response();
  layout_request(master, 31, response.snapshot_id, 0, true, 3, false, false, 0, false, true);
  master.master_step(fake_now, fake_now); deliver(master, slave, drop);
  assert(layout_response().complete && slave.identify_driver == matrix_service::IDENTIFY_ALL_DRIVERS);
  slave.playback_step(fake_now);
  assert(output.shown.g == 220 && output_secondary.shown.g == 220);
  fake_now += 250000; slave.playback_step(fake_now);
  assert(output.shown.g == 0 && output_secondary.shown.g == 0);
  fake_now += 3000000; slave.playback_step(fake_now);
  assert(!slave.identify_driver);
  // Tokens cannot cross a BLE disconnection, and expired snapshots are explicit errors.
  layout_request(master, 26); response = layout_response(); matrix_service::disconnect(1);
  layout_request(master, 27, response.snapshot_id, 320); response = layout_response(); assert(response.error[0]);
  layout_request(master, 28); response = layout_response(); fake_now += TRANSFER_TIMEOUT_US;
  layout_request(master, 29, response.snapshot_id, 320); assert(layout_response().error[0]);
  network.clear();
  // First-time setup supports saving a layout before any animation exists.
  service fresh; configure_node(fresh, true);
  update->expected_revision = 0; update->layout_count = 1;
  update->layout[0] = layout_row(master_mac, mesh_interop_MatrixDriver_WS2812, 0, 1);
  upload_layout(fresh, *update, 97);
  assert(fresh.revision == 1 && fresh.candidate && fresh.candidate->entries_count == 0);
  assert(fresh.candidate->schema_version == SCHEMA_VERSION && fresh.candidate->layout_count == 1);
  network.clear();
}
void test_matrix_editor_service() {
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); c.sources[0].kind = mesh_interop_MatrixSource_Kind_DEFAULT_STATE;
  c.effects[0].duration_ms = 0;
  snprintf(c.effects[0].name, sizeof(c.effects[0].name), "%0120d", 1);
  snprintf(c.entries[0].name, sizeof(c.entries[0].name), "%0120d", 2);
  snprintf(c.layout[0].name, sizeof(c.layout[0].name), "%0120d", 3);
  upload(master, c, 101); cycles(master, slave, 200);
  strcpy(fake_app_description.version, "node-build-2");
  slave.send_inventory(master_mac);
  bool drop = false; deliver(master, slave, drop);
  strcpy(fake_app_description.version, "controller-build-1");
  layout_request(master, 100, 0, 0, false, 0, true); const auto first = layout_response();
  assert(!first.error[0] && !first.complete);
  std::vector<uint8_t> bytes(first.data.bytes, first.data.bytes + first.data.size);
  layout_request(master, 101, first.snapshot_id, 320); assert(layout_response().error[0]); // Wrong snapshot kind.
  while (bytes.size() < first.total_size) {
    layout_request(master, 102, first.snapshot_id, bytes.size(), false, 0, true); const auto page = layout_response();
    assert(!page.error[0] && page.offset == bytes.size());
    bytes.insert(bytes.end(), page.data.bytes, page.data.bytes + page.data.size);
  }
  auto document = std::make_unique<mesh_interop_MatrixDocument>();
  assert(decode(mesh_interop_MatrixDocument_fields, document.get(), bytes.data(), bytes.size()));
  assert(document->revision == 1 && document->nodes_count == 2 && document->nodes[1].known);
  assert(!strcmp(document->master_firmware_version, "controller-build-1"));
  assert(!strcmp(document->nodes[0].firmware_version, "controller-build-1"));
  assert(!strcmp(document->nodes[1].firmware_version, "node-build-2"));
  strcpy(fake_app_description.version, "test-version");
  for (size_t i = 0; i < document->nodes_count; ++i) {
    const auto &node = document->nodes[i];
    assert(std::find(node.gpios, node.gpios + node.gpios_count, 12) != node.gpios + node.gpios_count);
    assert(std::find(node.gpios, node.gpios + node.gpios_count, 19) == node.gpios + node.gpios_count);
  }
  auto update = std::make_unique<mesh_interop_MatrixUpdate>(); update->master_session = 10; update->expected_revision = 2;
  update->effects_count = c.effects_count; std::copy_n(c.effects, c.effects_count, update->effects);
  update->sources_count = c.sources_count; std::copy_n(c.sources, c.sources_count, update->sources);
  update->conditions_count = c.conditions_count; std::copy_n(c.conditions, c.conditions_count, update->conditions);
  update->entries_count = c.entries_count; std::copy_n(c.entries, c.entries_count, update->entries);
  update->effects[0].kind = mesh_interop_MatrixEffect_Kind_BREATHE; update->effects[0].period_ms = 2000;
  const auto original = stored;
  upload_editor(master, mesh_interop_MatrixUpdate_fields, update.get(), 102, true);
  assert(master.revision == 1 && stored == original);
  update->expected_revision = 1; upload_editor(master, mesh_interop_MatrixUpdate_fields, update.get(), 103, true);
  assert(master.candidate && master.candidate->layout[0].to == c.layout[0].to && !strcmp(master.candidate->layout[0].name, c.layout[0].name));
  cycles(master, slave, 200); assert(master.revision == 2 && master.config->effects[0].period_ms == 2000);
  const auto accepted = stored; update->expected_revision = 2; update->effects[0].blue = 255;
  fail_commit = true; upload_editor(master, mesh_interop_MatrixUpdate_fields, update.get(), 104, true); fail_commit = false;
  assert(master.revision == 2 && stored == accepted && master.config->effects[0].blue == c.effects[0].blue);
  update->effects[0].blue = c.effects[0].blue;
  update->modes_count = 2; update->modes[0].id = 1; strcpy(update->modes[0].name, "Day");
  update->modes[1].id = 2; strcpy(update->modes[1].name, "Night");
  for (unsigned i = 0; i < 8; ++i) {
    update->expected_revision = master.revision; update->active_mode_id = 1 + i % 2;
    upload_editor(master, mesh_interop_MatrixUpdate_fields, update.get(), 105 + i, true);
    assert(master.deployment); cycles(master, slave, 200);
    assert(master.config->active_mode_id == update->active_mode_id && master.config->modes_count == 2);
  }
  network.clear();
}
std::unique_ptr<mesh_interop_MatrixDocument> read_document(service &node, bool active = false) {
  layout_request(node, 300, 0, 0, false, 0, true, active); const auto first = layout_response();
  assert(!first.error[0]);
  std::vector<uint8_t> bytes(first.data.bytes, first.data.bytes + first.data.size);
  while (bytes.size() < first.total_size) {
    layout_request(node, 301, first.snapshot_id, bytes.size(), false, 0, true, active); const auto page = layout_response();
    assert(!page.error[0]); bytes.insert(bytes.end(), page.data.bytes, page.data.bytes + page.data.size);
  }
  auto document = std::make_unique<mesh_interop_MatrixDocument>();
  assert(decode(mesh_interop_MatrixDocument_fields, document.get(), bytes.data(), bytes.size()));
  return document;
}
void test_preconfigured_rainbow_and_current_read() {
  stored.clear(); staged.clear(); network.clear();
  service master, slave; configure_node(master, true); configure_node(slave, false);
  fake_synchronized = false; slave.initialize_configuration();
  assert(!slave.local && !slave.config);
  slave.playback_step(0); assert(!slave.local);
  fake_synchronized = true;
  master.initialize_configuration(); assert(master.candidate && master.candidate->factory_default && !stored.empty());
  cycles(master, slave, 400);
  assert(master.config && master.config->factory_default && master.config->layout_count == 4);
  assert(!slave.config && slave.local->definitions().output_mappings_count == 2);
  for (size_t i = 0; i < master.config->layout_count; ++i) assert(master.config->layout[i].from == 0 && master.config->layout[i].to == 1);
  auto document = read_document(master);
  assert(document->revision == master.revision && document->active_revision == master.revision && document->persisted && !document->activation_pending);
  auto running = read_document(master, true);
  auto current = std::make_unique<matrix_config>();
  assert(decode(mesh_interop_MatrixConfig_fields, current.get(), running->config.bytes, running->config.size));
  assert(current->factory_default && current->effects[0].kind == mesh_interop_MatrixEffect_Kind_RAINBOW);
  for (size_t i = 0; i < current->layout_count; ++i) assert(current->layout[i].name[0] == '\0');
  auto snapshot = slave.local->snapshot(slave.local->definitions(), 0, 0);
  assert(snapshot.states_count == 1 && snapshot.states[0].started_at_us == 0);
  const auto old_revision = master.revision;
  auto replacement = master.config->to_wire();
  replacement->effects[0].kind = mesh_interop_MatrixEffect_Kind_STATIC_COLOR;
  replacement->effects[0].red = 0; replacement->effects[0].green = 0; replacement->effects[0].blue = 255;
  upload(master, *replacement, 200); assert(master.candidate && !master.candidate->factory_default);
  document = read_document(master); running = read_document(master, true);
  assert(document->revision == old_revision + 1 && document->activation_pending);
  assert(running->revision == old_revision && running->active_revision == old_revision);
  for (unsigned i = 0; !master.pending_snapshot && i < 100; ++i) cycles(master, slave, 1);
  assert(master.pending_snapshot && fake_now < master.activate_at);
  running = read_document(master, true);
  assert(decode(mesh_interop_MatrixConfig_fields, current.get(), running->config.bytes, running->config.size));
  assert(current->factory_default && current->effects[0].kind == mesh_interop_MatrixEffect_Kind_RAINBOW);
  cycles(master, slave, 200); running = read_document(master, true);
  assert(running->revision == master.revision && !running->activation_pending);
  assert(output.shown.b == 255 && output.shown.r == 0);
  service restored; configure_node(restored, true); restored.initialize_configuration();
  assert(restored.candidate && !restored.candidate->factory_default && restored.candidate->effects[0].kind == mesh_interop_MatrixEffect_Kind_STATIC_COLOR);
  auto empty = std::make_unique<matrix_config>(); empty->schema_version = SCHEMA_VERSION;
  upload(master, *empty, 201); cycles(master, slave, 600);
  assert(master.config->entries_count == 0 && !master.config->factory_default);
  assert(output.shown.r == 0 && output.shown.g == 0 && output.shown.b == 0);
  service empty_restored; configure_node(empty_restored, true); empty_restored.initialize_configuration();
  assert(empty_restored.candidate && empty_restored.candidate->entries_count == 0);
  network.clear(); stored.clear(); fail_commit = true;
  service volatile_default; configure_node(volatile_default, true); volatile_default.initialize_configuration();
  assert(volatile_default.candidate && !volatile_default.configuration_persisted && stored.empty());
  fail_commit = false; network.clear();
}
void test_boot_and_default_start_together() {
  stored.clear(); staged.clear(); network.clear();
  service master, slave; configure_node(master, true); configure_node(slave, false);
  slave.initialize_configuration();
  assert(!slave.local && !slave.pending_player);
  auto c = fixture();
  c.sources[0].kind = mesh_interop_MatrixSource_Kind_DEFAULT_STATE;
  c.effects[0].duration_ms = 0;
  c.effects_count = 2; c.effects[1] = c.effects[0]; c.effects[1].id = 5;
  c.effects[1].red = 0; c.effects[1].blue = 255; c.effects[1].duration_ms = 5000;
  c.sources_count = 2; c.sources[1] = c.sources[0]; c.sources[1].id = 6;
  c.sources[1].kind = mesh_interop_MatrixSource_Kind_ON_BOOT;
  c.entries_count = 2; c.entries[1] = c.entries[0]; c.entries[1].id = 7;
  c.entries[1].source_id = 6; c.entries[1].effect_id = 5; c.entries[1].priority = 20;
  c.layout_count = 2;
  c.layout[0] = layout_row(master_mac, mesh_interop_MatrixDriver_WS2812, 0, 1);
  c.layout[1] = layout_row(slave_mac, mesh_interop_MatrixDriver_WS2812, 1, 2);
  upload(master, c, 202);
  for (unsigned i = 0; i < 250 && (!master.local || !slave.local); ++i) {
    cycles(master, slave, 1);
    if (master.pending_snapshot && fake_now < master.activate_at) assert(!master.local);
    if (slave.pending_snapshot && fake_now < slave.pending_snapshot->activate_at_us) assert(!slave.local);
  }
  assert(master.local && slave.local);
  for (auto *node : {&master, &slave}) {
    auto snapshot = node->local->snapshot(node->local->definitions(), 0, 0);
    assert(snapshot.states_count == 2);
    assert(snapshot.states[0].active && snapshot.states[1].active);
    assert(snapshot.states[0].started_at_us == master.activate_at);
    assert(snapshot.states[1].started_at_us == master.activate_at);
  }
  assert(output.shown.b == 255 && output.shown.r == 0);
  cycles(master, slave, 520);
  assert(output.shown.r == 255 && output.shown.b == 0);
  stored.clear(); staged.clear(); network.clear();
}
void test_boot_waits_for_first_slave_heartbeat() {
  stored.clear(); staged.clear(); network.clear(); fake_peer_online = false;
  fake_startup_at = fake_now;
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture();
  c.sources[0].kind = mesh_interop_MatrixSource_Kind_ON_BOOT;
  c.effects[0].duration_ms = 500;
  c.layout_count = 2;
  c.layout[0] = layout_row(master_mac, mesh_interop_MatrixDriver_WS2812, 0, 1);
  c.layout[1] = layout_row(slave_mac, mesh_interop_MatrixDriver_WS2812, 1, 2);
  upload(master, c, 203);
  cycles(master, slave, 99);
  assert(master.deployment && !master.local && !slave.local);
  fake_peer_online = true;
  for (unsigned i = 0; i < 180 && (!master.local || !slave.local); ++i) cycles(master, slave, 1);
  assert(master.local && slave.local && master.peers[0].ready);
  const auto snapshot = slave.local->snapshot(slave.local->definitions(), 0, 0);
  assert(snapshot.states[0].active && snapshot.states[0].started_at_us == master.activate_at);

  // A missing slave delays startup only for the bounded grace period.
  fake_peer_online = false; network.clear(); fake_startup_at = fake_now;
  service offline_master, offline_slave; configure_node(offline_master, true); configure_node(offline_slave, false);
  upload(offline_master, c, 204);
  cycles(offline_master, offline_slave, 205);
  assert(!offline_master.deployment && offline_master.activate_at > 0);
  fake_peer_online = true; stored.clear(); staged.clear(); network.clear();
}

void test_standby_blanks_and_recovers() {
  stored.clear(); staged.clear(); network.clear();
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); c.sources[0].kind = mesh_interop_MatrixSource_Kind_DEFAULT_STATE;
  c.effects[0].duration_ms = 0;
  upload(master, c, 205); cycles(master, slave, 250);
  assert(slave.local);
  fake_last_sync = fake_now;
  fake_now += 4990000; fake_mesh_master = false; slave.slave_step(fake_now);
  assert(!slave.standby && !fake_radio_standby);
  fake_now += 10000; slave.slave_step(fake_now);
  assert(slave.standby && fake_radio_standby && !slave.local);
  assert(output.shown.r == 0 && output.shown.g == 0 && output.shown.b == 0);
  slave.playback_step(fake_now);
  assert(output.shown.r == 0 && output.shown.g == 0 && output.shown.b == 0);
  fake_last_sync = ++fake_now; slave.slave_step(fake_now);
  assert(!slave.standby && !fake_radio_standby && !slave.local);
  assert(output.shown.r == 0 && output.shown.g == 0 && output.shown.b == 0);
  fake_mesh_master = true; fake_last_sync = 0;
  bool drop = false; deliver(master, slave, drop); cycles(master, slave, 250);
  assert(slave.local && master.peers[0].ready);
  stored.clear(); staged.clear(); network.clear();
}

void test_discovery_wakes_node_and_requires_fresh_sync() {
  stored.clear(); staged.clear(); network.clear();
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); c.sources[0].kind = mesh_interop_MatrixSource_Kind_DEFAULT_STATE;
  c.effects[0].duration_ms = 0;
  upload(master, c, 206); cycles(master, slave, 250);
  fake_last_sync = fake_now;
  fake_now += 5000000; fake_mesh_master = false; slave.slave_step(fake_now);
  assert(slave.standby && fake_radio_standby);
  fake_discovery = true; slave.slave_step(fake_now);
  assert(slave.discovering && !slave.standby && !fake_radio_standby && !slave.local);
  fake_now += 10000000; slave.slave_step(fake_now);
  assert(slave.discovering && !slave.standby && !fake_radio_standby);
  fake_discovery = false; slave.slave_step(fake_now);
  assert(slave.waiting_for_discovery_sync && !slave.standby && !slave.local);
  fake_now += 4990000; slave.slave_step(fake_now);
  assert(slave.waiting_for_discovery_sync && !slave.standby);
  fake_last_sync = ++fake_now; slave.slave_step(fake_now);
  assert(!slave.waiting_for_discovery_sync && !slave.standby && !slave.local);
  fake_mesh_master = true; fake_last_sync = 0;
  bool drop = false; deliver(master, slave, drop); cycles(master, slave, 250);
  assert(slave.local && master.peers[0].ready);
  stored.clear(); staged.clear(); network.clear();
}
void test_six_driver_inventory_and_independent_remote_leds() {
  stored.clear(); staged.clear(); network.clear();
  connected_lighting_drivers::configured_resolver = resolve_six;
  service master, slave; configure_node(master, true); configure_node(slave, false);
  master.initialize_configuration(); slave.initialize_configuration(); cycles(master, slave, 400);
  assert(master.config && master.config->layout_count == 24);
  assert(master.peers[0].outputs_count == 12 && slave.local->definitions().output_mappings_count == 12);
  layout_request(master, 901);
  assert(master.snapshot_outputs.size() == 24);
  for (const auto &output : master.snapshot_outputs)
    assert(output.pixel_count == 1 && output.driver_kind == mesh_interop_MatrixDriverKind_RGB_LED_MODULE);
  auto replacement = master.config->to_wire();
  for (size_t i = 0; i < replacement->layout_count; ++i) {
    auto &row = replacement->layout[i];
    if (!memcmp(row.node_mac, slave_mac, 6) && row.driver == mesh_interop_MatrixDriver_DRIVER_6) {
      row.from = row.source_index ? 70 : 10; row.to = row.from + 1;
    }
  }
  upload(master, *replacement, 902); cycles(master, slave, 200);
  size_t points = 0;
  for (const auto &mapping : slave.local->definitions().output_mappings)
    if (mapping.driver == mesh_interop_MatrixDriver_DRIVER_6) {
      assert(mapping.from == (mapping.source_index ? 70u : 10u)); ++points;
    }
  assert(points == 2);
  layout_request(master, 903); const auto token = layout_response().snapshot_id;
  size_t index = 0;
  for (; index < master.snapshot_outputs.size(); ++index) {
    const auto &output = master.snapshot_outputs[index];
    if (!memcmp(output.node_mac, slave_mac, 6) && output.driver == mesh_interop_MatrixDriver_DRIVER_6 && output.source_index == 1) break;
  }
  assert(index < master.snapshot_outputs.size());
  layout_request(master, 904, token, 0, true, index); cycles(master, slave, 10);
  assert(slave.identify_driver == 6 && slave.identify_source == 1 && layout_response().complete);
  assert(six_outputs[5].shown[1].g == 220 && six_outputs[5].shown[1].r == 0);
  network.clear(); slave.send_inventory(master_mac);
  assert(network.size() == 2 && network.front().bytes.size() == w::HEADER_SIZE + 101);
  assert(network.back().bytes.size() == w::HEADER_SIZE + 102 + strlen(fake_app_description.version));
  auto frame = network.front().bytes; network.clear();
  packet packet{}; memcpy(packet.mac, slave_mac, 6); packet.size = frame.size();
  // Duplicate physical slot IDs cannot partially replace the existing catalog.
  frame[w::HEADER_SIZE + 9 + 14] = frame[w::HEADER_SIZE + 9];
  memcpy(packet.data, frame.data(), frame.size());
  const auto accepted_at = master.peers[0].catalog_at;
  master.handle_radio(packet, fake_now + 1); assert(master.peers[0].catalog_at == accepted_at);
  connected_lighting_drivers::configured_resolver = resolve; network.clear();
}
class growable_test_driver : public canvas_test_driver {
public:
  growable_test_driver() : canvas_test_driver(4) {}
  bool fail_growth = false;
  lighting_driver_kind get_kind() const override { return lighting_driver_kind::LED_STRIP; }
  size_t get_layout_max_pixel_count(uint32_t source) const override { return source ? 0 : 16; }
  esp_err_t reserve_layout_pixels(uint32_t source, size_t count) override {
    if (source || !count || count > 16) return ESP_ERR_INVALID_ARG;
    if (count > pixels.size()) {
      if (fail_growth) return ESP_ERR_NO_MEM;
      pixels.resize(count); shown.resize(count);
    }
    return ESP_OK;
  }
};
growable_test_driver growing_output;
rgb_lighting_driver *resolve_growing(uint32_t id) { return id == 1 ? &growing_output : nullptr; }
void test_configurable_strip_and_identify_preview() {
  stored.clear(); staged.clear(); network.clear();
  connected_lighting_drivers::configured_resolver = resolve_growing;
  service master, slave; configure_node(master, true); configure_node(slave, false);
  auto c = fixture(); c.effects[0].duration_ms = 0; c.sources[0].kind = mesh_interop_MatrixSource_Kind_DEFAULT_STATE;
  c.layout[0].to = 12; c.layout[0].car_position = mesh_interop_CarPosition_FRONT_LEFT_DOOR;
  upload(master, c, 1001); cycles(master, slave, 250);
  assert(master.revision == 1 && slave.local && growing_output.get_pixel_count() == 12);
  assert(slave.local->definitions().output_mappings[0].to == 12);
  layout_request(master, 1002); const auto token = layout_response().snapshot_id;
  size_t index = 0;
  for (; index < master.snapshot_outputs.size(); ++index) if (!memcmp(master.snapshot_outputs[index].node_mac, slave_mac, 6)) break;
  assert(index < master.snapshot_outputs.size() && master.snapshot_outputs[index].max_pixel_count == 16);
  layout_request(master, 1003, token, 0, true, index, false, false, 8, true); cycles(master, slave, 10);
  assert(slave.identify_length == 8 && slave.identify_reversed);
  const auto start = slave.identify_until - 3000000;
  fake_now = start; slave.playback_step(fake_now);
  assert(growing_output.shown[7].g == 220 && growing_output.shown[0].r == 222 && growing_output.shown[11].r == 0);
  fake_now = start + 1000000; slave.playback_step(fake_now);
  for (size_t i = 4; i < 7; ++i) assert(growing_output.shown[i].g == 101);
  assert(growing_output.shown[3].g == 0 && growing_output.shown[0].r == 222);
  fake_now = start + 1999999; slave.playback_step(fake_now);
  assert(growing_output.shown[1].g == 101 && growing_output.shown[0].r == 222);
  for (uint64_t elapsed : {2000000, 2500000, 2999999}) {
    fake_now = start + elapsed; slave.playback_step(fake_now);
    assert(growing_output.shown[0].r == 222 && growing_output.shown[7].g == 220);
    for (size_t i = 1; i < 7; ++i) assert(growing_output.shown[i].g == 101);
    for (size_t i = 8; i < 12; ++i) assert(growing_output.shown[i].r == 0 && growing_output.shown[i].g == 0 && growing_output.shown[i].b == 0);
  }
  fake_now = start + 3000001; slave.playback_step(fake_now);
  assert(!slave.identify_driver && growing_output.shown[11].r == 255); // Normal full-length playback resumes.
  assert(slave.begin_identify(1, fake_now, 0, 6, false));
  const auto forward_start = fake_now;
  fake_now += 1000000; slave.playback_step(fake_now);
  assert(growing_output.shown[0].g == 220);
  for (size_t i = 1; i < 3; ++i) assert(growing_output.shown[i].g == 101);
  assert(growing_output.shown[3].g == 0 && growing_output.shown[5].r == 222);
  fake_now = forward_start + 2500000; slave.playback_step(fake_now);
  assert(growing_output.shown[0].g == 220 && growing_output.shown[5].r == 222 && growing_output.shown[6].r == 0);
  fake_now = forward_start + 3000001; slave.playback_step(fake_now);
  assert(!slave.identify_driver && growing_output.shown[11].r == 255);
  const auto before = stored;
  growing_output.fail_growth = true; c.layout[0].to = 16;
  // The owning slave rejects failed buffer growth; the current player remains valid.
  auto replacement = definitions_for(c, slave_mac, 10, 2);
  player rejected(resolve_growing); assert(!rejected.prepare(replacement));
  assert(growing_output.get_pixel_count() == 12 && stored == before);
  growing_output.fail_growth = false;
  growing_output.pixels.resize(4); growing_output.shown.resize(4);
  service restored; configure_node(restored, true); restored.load();
  assert(restored.candidate && restored.candidate->layout[0].to == 12);
  assert(restored.candidate->layout[0].car_position == mesh_interop_CarPosition_FRONT_LEFT_DOOR);
  // A local persisted strip also reserves its configured length during restore.
  c.layout[0] = layout_row(master_mac, mesh_interop_MatrixDriver_WS2812, 0, 12);
  assert(restored.prepare_runtime(c, 2) && growing_output.get_pixel_count() == 12);
  connected_lighting_drivers::configured_resolver = resolve; network.clear(); stored.clear();
}
class layout_capacity_driver : public fake_driver {
public:
  size_t get_layout_max_pixel_count(uint32_t source) const override { return source ? 0 : 2048; }
} layout_capacity_output;
rgb_lighting_driver *resolve_layout_capacity(uint32_t id) {
  return id == 1 ? &layout_capacity_output : nullptr;
}
void test_local_layout_validation_errors() {
  service master; configure_node(master, true);
  auto c = fixture();
  c.layout[0] = layout_row(master_mac, mesh_interop_MatrixDriver_WS2812, 0, 1);
  assert(!master.platform_validate(c, true));
  c.layout[0].driver = mesh_interop_MatrixDriver_DRIVER_3;
  assert(!strcmp(master.platform_validate(c, true), "Layout references a missing local driver"));
  c.layout[0].driver = mesh_interop_MatrixDriver_WS2812;
  c.layout[0].source_index = 1;
  assert(!strcmp(master.platform_validate(c, true), "Layout references a missing local driver source"));
  c.layout[0].source_index = 0;
  c.layout[0].to = 2;
  assert(!strcmp(master.platform_validate(c, true), "Layout span exceeds local driver capacity"));
  c.layout[0].from = 2; c.layout[0].to = 1;
  assert(!master.platform_validate(c, true));
  connected_lighting_drivers::configured_resolver = resolve_layout_capacity;
  c.layout_count = 2;
  c.layout[0] = layout_row(master_mac, mesh_interop_MatrixDriver_WS2812, 0, 24);
  c.layout[1] = layout_row(slave_mac, mesh_interop_MatrixDriver_WS2812, 24, 84);
  assert(!master.platform_validate(c, true));
  connected_lighting_drivers::configured_resolver = resolve;
}
void test_missing_provisioning_reports_error() {
  connected_lighting_drivers::fake_status = ESP_ERR_INVALID_STATE;
  service master, slave; configure_node(master, true); configure_node(slave, false);
  master.initialize_configuration(); slave.initialize_configuration();
  assert(!master.candidate && !master.local && !slave.local);
  assert(strstr(master.state.error, "configuration") && strstr(slave.state.error, "configuration"));
  connected_lighting_drivers::fake_status = ESP_OK;
}
int main() {
  matrix_core_tests_main();
  matrix_service::gpio_queue = xQueueCreate(64, sizeof(matrix_service::gpio_event));
  test_deploy_recover_persist();
  test_old_matrix_evaluates_until_shared_boundary();
  test_command_retry_gap_and_snapshot_overflow();
  test_animated_deployment_and_reconnect();
  test_boot_palette_deployment_and_restore();
  test_mode_switch_deployment();
  test_remote_gpio_delivery();
  test_provisioned_gpio_inventory();
  test_remote_gpio_source_only_and_replacement();
  test_remote_gpio_time_queues_and_sessions();
  test_car_layout_distribution_and_recovery();
  test_editor_patch_decoding();
  test_layout_editor_service();
  test_matrix_editor_service();
  test_preconfigured_rainbow_and_current_read();
  test_boot_and_default_start_together();
  test_boot_waits_for_first_slave_heartbeat();
  test_standby_blanks_and_recovers();
  test_discovery_wakes_node_and_requires_fresh_sync();
  test_six_driver_inventory_and_independent_remote_leds();
  test_configurable_strip_and_identify_preview();
  test_local_layout_validation_errors();
  test_missing_provisioning_reports_error();
  test_only_current_hello_format();
  test_slave_reboot_after_offline_timeout();
  delete matrix_service::gpio_queue;
  test_callback_queue_limits();
  std::puts("Matrix worker: deployment, recovery, persistence, remote GPIO timing/retries/fencing, and queue limits passed.");
}
