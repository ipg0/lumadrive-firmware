# Distributed matrix protocol v1

The master owns the full protobuf `MatrixConfig`, persisted in NVS as one
versioned, checksummed record. Only the master evaluates sources and conditions.
Slaves receive `AnimationDefinitions` for their own fixed drivers and GPIO inputs,
report edges, and execute timestamped commands. `matrix_service::init()` starts the worker and 10 ms
playback loop after connected drivers and ESP-NOW have initialized.

## Configuration

Use schema version 1, nonzero IDs unique within each definition category, and
valid effect/source/condition references. Limits are 32 entries, 32 definitions
per category, 17 targets per entry, and 16 KiB encoded bytes. Targets are unique
node/driver pairs; one entry may drive both outputs of a node. Driver IDs are WS2812
`1` and HC595 `2`; pin assignments remain in `connected_lighting_drivers`.

Supported types are static color (RGBA 0–255, duration in milliseconds; zero
duration holds indefinitely), default state, local or slave GPIO edges, and always
conditions. Set alpha explicitly: protobuf's default zero is transparent.
GPIO inputs use external biasing, with internal pulls disabled. Role/discovery,
flash, and connected-driver pins cannot be used as trigger inputs. GPIO sources
identify the owning node's ESP-NOW MAC; remote sources and targets must be paired.
All nodes currently share the firmware wiring configuration. The owning slave
validates its input pins again before accepting definitions. Empty matrices clear existing layers.
Input-only slaves receive definitions and snapshots even with no output layers.

## BLE and app API

Existing service and mesh-status UUIDs are preserved. New encrypted characteristics:

| Characteristic | UUID | Operations |
| --- | --- | --- |
| Matrix upload | `294b8b2e-57d2-bf85-dc41-5c612c9d1539` | Write with response |
| Matrix status | `294b8b2f-57d2-bf85-dc41-5c612c9d1539` | Read, notify |

Send `MatrixUpload` envelopes: begin (transfer ID, total length, IEEE CRC32),
chunks (ID, byte offset, data), then commit (ID). Abort cancels a partial upload;
once commit is accepted, deployment continues even if BLE disconnects. Idle
partial uploads expire after five seconds. Disconnect invalidates queued writes
from that connection generation. Complete uploads are validated, prepared, and
persisted before replacing the running matrix; failures preserve its runtime.

Envelopes must fit ATT MTU minus three bytes, including protobuf framing. The app
client handles MTU 23 and above and uses received-byte status as backpressure.
Status notifications contain one byte (`1`), signalling a complete status read
through ATT long reads. Status includes the transfer ID, stage, revision, error,
per-node readiness, and `master_mac`. Use this ESP-NOW MAC for sources and local
targets; a Bluetooth address or platform device UUID is not interchangeable.

`MeshController` exposes `uploadMatrixConfig`, `getMatrixStatus`, and
`watchMatrixStatus`. Upload yields byte progress and finishes when the master's
revision activates; inspect node readiness for incomplete remote deployment.
The UI editor is deferred.

## ESP-NOW framing and reliability

New frames have an eight-byte header: little-endian magic `0x5854414d` (`MATX`),
version byte `1`, type byte, and little-endian uint16 body length. Complete frames
are at most 250 bytes. Types and bodies are:

| Type | Body, in order |
| --- | --- |
| 1 begin | transfer ID u32, kind u8 (1 definitions, 2 snapshot), length u32, CRC32 u32, revision u64, session u64 |
| 2 chunk | transfer ID u32, offset u32, up to 200 data bytes |
| 3 finish | transfer ID u32 |
| 4 ack | transfer ID u32, expected offset u32, phase u8 (0 begin, 1 chunk, 2 finish), result u8 (0 OK, 1 rejected) |
| 5 command | protobuf `AnimationCommand` |
| 6 command ACK | session u64, revision u64, sequence u32 |
| 7 snapshot request | empty |
| 8 hello | clock session u64, installed/staged revision u64, accepted command sequence u32, GPIO stream u64, GPIO loss flag u8 |
| 9 GPIO event | session u64, revision u64, GPIO stream u64, sequence u32, captured master time u64, pin u8, rising u8 (0 or 1) |
| 10 GPIO event ACK | session u64, revision u64, GPIO stream u64, sequence u32, result u8 (0 accepted/discarded, 1 rejected) |

All fixed-width integers are little-endian. Transfers use stop-and-wait ACKs,
100 ms retry intervals, and three retries after the initial transmission.
Duplicate chunks do not reapply data. Unexpected offsets return the expected
offset; conflicting metadata, size violations, and invalid CRCs are rejected.
Only paired senders are accepted; slaves accept playback traffic only from their
configured master after clock synchronization. Radio callbacks enqueue bounded
copies; the worker processes command/ACK and GPIO event/ACK traffic before bulk chunks.
Legacy 20-byte HELLO bodies remain accepted, but remote GPIO requires updated firmware.

## Slave GPIO reporting

Definitions contain up to 32 distinct node-local `gpio_inputs`, with both edges
captured by ISR into a bounded 64-edge queue. The worker converts captured local
timestamps through the synchronized clock offset, filters pins against the active
revision, and queues up to 64 outbound events. Inputs switch at the same snapshot
activation epoch as their animation definitions; removed pins lose their handlers.
No trigger or condition evaluation runs on slaves. There is no software debounce.

Each slave uses a random GPIO stream ID and monotonically increasing sequence.
Stop-and-wait delivery retries every 100 ms, at most three times after the first
send. The master authenticates the source using the transport MAC, validates the
configured pin/revision/time, and acknowledges duplicates without retriggering.
Events more than five seconds old or 100 ms ahead of master time are rejected;
superseded revisions are acknowledged and discarded. Recent retired stream IDs
fence delayed traffic after slave reboots. Master-session changes flush input
queues and remove old input handlers until new definitions arrive.

Overflow, retry exhaustion, rejected events, and detected sequence gaps report
degraded delivery. A slave's HELLO carries its latched loss flag, so the master
exposes the affected node as `DEGRADED` even when no later edge arrives. Loss flags
reset with a new GPIO stream (slave reboot or master-session change). Playback
snapshots recover animation state; they cannot reconstruct lost physical edges.

## Activation, timing, and recovery

Revisions fence configuration changes; random master boot sessions fence clock
resets. Definition transfers prepare replacement players without touching active
outputs. Configuration activation reserves a shared future epoch, with extra
time for initial snapshots. The previous matrix continues evaluating until that
boundary. Available nodes proceed; failed/offline targets recover in the
background. A heartbeat or authenticated session HELLO within four seconds marks
a peer available.

Each local or remote GPIO-triggered start uses the captured master-clock timestamp plus 100 ms.
Players wait for future commands and catch up late commands using elapsed time
from their original timestamp. Expired finite effects are skipped; backward
clock corrections cannot underflow elapsed time. Priority increases toward the
top layer; equal priorities preserve registration order, with one clear/show
per participating driver each frame.

Per-node command streams are ordered and acknowledged. Sequence gaps, exhausted
retries, and queue pressure request recovery snapshots containing active layers
and pending transitions. Reconnecting slaves retain the original start times.
Master reboot restores the matrix under a new session; slaves clear old-session
state and await new definitions. Upgrade all nodes together: heartbeat payloads
now carry both master time and boot session.

## Validation

Run `bash test/test_animation_trigger_table/run.sh` and
`bash test/test_matrix_runtime/run.sh`, then `pio run -e esp32dev`.
The latter host suite exercises the actual service worker with fake radio, NVS,
queues, and clocks, including dropped ACKs, replacement boundaries, reboot
recovery, persistence failures, remote GPIO retries, clock offsets, source-only
nodes, revision/session fencing, and bounded queues. App tests are
`flutter test test/matrix_client_test.dart`.

For hardware acceptance, use a master and two slaves on the same firmware.
Upload a multi-chunk configuration, compare scheduled/observed playback times,
trigger an input on a slave, verify every target shares its start timestamp,
disconnect one slave, and verify recovery retains the original phase. Software
tests cannot establish physical start skew or GPIO electrical behavior.
