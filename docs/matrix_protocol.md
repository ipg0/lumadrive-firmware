# Distributed matrix protocol

The master owns the full protobuf `MatrixConfig`, persisted in NVS as one
versioned, checksummed record. Only the master evaluates sources and conditions.
The matrix schema, ESP-NOW envelope, and NVS record format are development v0.
Configuration revisions and master sessions still advance normally; they identify
runtime state, not protocol formats. An obsolete saved record is replaced by the
default matrix; corrupt records still report an error.
Slaves receive `AnimationDefinitions` for their own fixed drivers and GPIO inputs,
report edges, and execute timestamped commands. `matrix_service::init()` starts the worker and 10 ms
playback loop after connected drivers and ESP-NOW have initialized.

Players retain compact layer/mapping metadata, live effect instances, and encoded
definition bytes for recovery. Maximum-sized Nanopb keyframe arrays exist only in
temporary staging buffers. Large staging allocations check available storage
directly so an allocation failure can be reported without C++ exception handling.
The master also retains only populated matrix definitions between transfers.
Replacement players are prepared directly from the validated matrix, streaming
one layer at a time into the same protobuf wire format. They do not require a
second maximum-sized per-node definition buffer. The 16 KiB encoded limit does
not guarantee that every possible configuration fits available runtime heap.
Layout and matrix editor patches decode their repeated protobuf fields directly
into the replacement matrix, avoiding a second maximum-sized edit buffer. Nested
messages retain Nanopb bounds checking; malformed edits leave playback unchanged.

## Configuration

Use schema version 0, nonzero IDs unique within each definition category, and
valid effect/source/condition references. Limits are 32 entries, 32 definitions
per category, six physical drivers per node, 84 placed light sources across up
to 17 mesh nodes, and 16 KiB encoded bytes. The selected full-capacity target is
seven nodes with six two-LED modules each.
`MatrixConfig.layout` describes the one physical arrangement of the car, and
every animation renders on its complete canvas. Entries have no output targets.
See [Car layout](layout.md) for boundary indices, reversal, overlap, and examples.
Driver slot IDs are 1–6; historical enum names `WS2812`/`HC595` for slots 1/2 do
not constrain their types. Inventory reports the hardware kind independently.
A strip exposes source 0 with a configurable length; a 595 exposes each LED as a one-pixel
source. Layout/mapping identity includes `source_index`. Pin assignments remain
in the static `connected_lighting_drivers` registry.

Supported effects are static color, addressed RGBA keyframes, blink, breathe,
flowing gradient, rainbow, and fill; see [Animation effects](animations.md) for parameters.
The matrix may include up to 32 named modes with stable nonzero IDs. The active
mode is saved with the matrix; zero means no mode. A `MODE` condition matches
its selected mode, or every other mode when `inverted` is set. `ALWAYS` matches
every mode. `AND` and `OR` each reference two condition IDs and can be nested;
missing references and cycles are rejected. The app editor currently supports
one group with two mode or always conditions. An `ON_MODE_SWITCHED`
source triggers once when the active mode changes, at the new revision's shared
activation time. It does not trigger when a revision keeps the same mode or on
reboot. Switching replaces the runtime, stopping layers from the previous mode.
Duration is in milliseconds; zero runs indefinitely. Sources include default
state, on boot, mode changes, and local or slave GPIO edges. Set alpha explicitly:
protobuf's default zero is transparent.
GPIO inputs and their pull settings are set in each node's hardware configuration.
Discovery, flash, and connected-driver pins cannot be used as trigger inputs. GPIO sources
identify the owning node's ESP-NOW MAC; remote source and layout nodes must be paired.
Each node can have its own static wiring configuration. The master uses the
owning node's reported input capabilities; the slave validates its pins again
before accepting definitions. Empty matrices clear existing layers.
Input-only slaves receive definitions and snapshots even with no output layers.

When no saved record exists, the master creates and persists an ordinary matrix
with one default-state rainbow entry (eight-second cycle). All local outputs and
subsequently discovered slave outputs start at index zero; the canvas length is
the largest driver length. The firmware-owned `factory_default` flag allows this
initial inventory expansion and fixes rainbow phase to shared clock epoch zero.
Any user upload clears the flag. A saved empty matrix stays empty, and a corrupt
or unreadable saved record is reported rather than overwritten. Before receiving
master definitions, slaves run the same real default matrix locally; after adopting
a master configuration they do not reintroduce it during a disconnect.

## BLE and app API

Existing service and mesh-status UUIDs are preserved. New encrypted characteristics:

| Characteristic | UUID | Operations |
| --- | --- | --- |
| Matrix upload | `294b8b2e-57d2-bf85-dc41-5c612c9d1539` | Write with response |
| Matrix status | `294b8b2f-57d2-bf85-dc41-5c612c9d1539` | Read, notify |
| Layout response | `294b8b30-57d2-bf85-dc41-5c612c9d1539` | Read |

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
Notification calls release the BLE state mutex first: NimBLE delivers NOTIFY_TX
callbacks synchronously through the GAP handler, which takes that same mutex.

`MeshController` exposes `uploadMatrixConfig`, `getMatrixStatus`, and
`watchMatrixStatus`. Upload yields byte progress and finishes when the master's
revision activates; inspect node readiness for incomplete remote deployment.
The app's Car layout screen uses `readLayout`, `identifyOutput`, and `uploadLayout`.
The Matrix & animations screen uses `readMatrix` and `updateMatrix`, and offers
**Read current state** through `readCurrentMatrix`.

`MatrixUpload.layout_request` sends a request ID and READ or IDENTIFY operation.
READ starts with snapshot ID and offset zero. Read the matching `LayoutResponse`
from the encrypted response characteristic, then request subsequent offsets with
the returned snapshot ID. Pages contain at most 320 bytes; ATT long reads handle
the response at MTU 23. A frozen `LayoutSnapshot` includes persisted placements,
names, master session/revision/MAC, and device-reported output inventory. Tokens
belong to one BLE connection generation and expire after five idle seconds.
IDENTIFY uses the snapshot token and output index and replies after the node
acknowledges starting a three-second temporary overlay. Optional `identify_options`
packs output index (bits 0–7), draft LED count (bits 8–19, zero means current
capacity), reversal (bit 20), and whole-node identification (bit 21) into a fixed32.
Whole-node requests require zero length and no reversal; bits 22–31 must be zero. Omitting
`output_index` with this option keeps even the largest request within a 20-byte
envelope for ATT MTU 23. A strip fills over two seconds and holds fully lit for
one second, with purple end markers and a cyan fill; an individual RGB module LED pulses cyan.
Whole-node identification flashes all local outputs together.

`MatrixUpload.layout_begin` uses the existing begin/chunk/commit framing, but its
payload is `LayoutUpdate`: expected revision, master session, and replacement
layout. Commit rejects stale revisions or sessions. It clones the current full
matrix, replaces only its layout, and follows normal validation, persistence,
and deployment. A layout may be saved before any animations exist.

READ_MATRIX on the same paged request/response transport returns `MatrixDocument`:
canonical `MatrixConfig` bytes, revision, session, master MAC, and input capabilities
for every paired node. The document is bounded to 32 KiB to accommodate a 16 KiB
configuration plus metadata. Layout inventory snapshots also allow up to 32 KiB.
Only their actual output list and encoded pages are retained between requests;
the maximum-sized protobuf staging object is released. Snapshot kinds cannot be mixed under the same token.
READ_MATRIX returns the saved desired revision, including a revision being
deployed. READ_ACTIVE_MATRIX returns the configuration currently playing on the
master, retaining the previous revision until the shared activation boundary.
Both documents include `active_revision`, `activation_pending`, and `persisted`
(whether the returned configuration was saved in flash). At startup, before any
revision is active, an active read returns an empty schema-0 matrix at revision
zero. `MatrixClient.readCurrentMatrix()` and `MeshController.readCurrentMatrix()`
expose this paged read without replacing an editor draft.
The GPIO mask appended to inventory frames reports pins accepted by each node's
firmware, excluding discovery, flash, and output wiring. Unknown nodes expose
no selectable pins; known offline capability data may be cached by the app.
The matrix document also reports the controller's ESP-IDF application version
and each node's most recently received version. An empty version means unknown;
offline values are cached only for the current controller session. Versions come
from `esp_app_get_description()->version`, which uses ESP-IDF's project version
(normally the Git description, including a dirty suffix). Distinct uncommitted
builds can therefore show the same version.

`matrix_begin` transfers `MatrixUpdate`, with expected revision/session and the
replacement effects, sources, conditions, and entries. Commit merges these fields
into the current matrix while retaining layout exactly. Name fields on effects and
entries persist with the configuration. Validation, atomic NVS persistence, shared
activation and reconnect recovery follow the existing deployment path.

## ESP-NOW framing and reliability

New frames have an eight-byte header: little-endian magic `0x5854414d` (`MATX`),
version byte `0`, type byte, and little-endian uint16 body length. Complete frames
are at most 250 bytes. All participating nodes must run this protocol version;
earlier matrix frames are rejected. Existing pairing and clock frames retain
their formats. Types and bodies are:

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
| 11 inventory request | session u64 |
| 12 inventory | session u64, physical driver count u8 (0–6), repeated slot ID u32, initial pixel count u32, ready u8, kind u8 (1 strip, 2 RGB module), maximum source pixel count u32, GPIO capability mask u64; extended frame appends version length u8 and up to 31 ASCII bytes |
| 13 identify | session u64, request ID u32, driver slot ID u32 (`0xffffffff` for all), source index u32, draft pixel count u32, reversed u8 (0 or 1) |
| 14 identify ACK | session u64, request ID u32, result u8 (0 OK, 1 unavailable) |

All fixed-width integers are little-endian. Transfers use stop-and-wait ACKs,
100 ms retry intervals, and three retries after the initial transmission.
Duplicate chunks do not reapply data. Unexpected offsets return the expected
offset; conflicting metadata, size violations, and invalid CRCs are rejected.
Only paired senders are accepted; slaves accept playback traffic only from their
configured master after clock synchronization. Radio callbacks enqueue bounded
copies; the worker processes command/ACK and GPIO event/ACK traffic before bulk chunks.
HELLO bodies must contain all 29 bytes, including the GPIO stream and loss flag.
The master refreshes available nodes' inventory every five seconds, retrying
unanswered requests once per second. Offline inventory stays cached in RAM;
the app also persists its last actual device-reported lengths. Identify uses
100 ms retries with three retries after the first send. Duplicate IDs acknowledge
without extending the overlay. Only the configured, synchronized master may
request inventory or identification on a slave.

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
Non-default-state entries can set `opaque_background` to hide lower layers with
black while active. Otherwise transparent pixels reveal lower layers. The setting
travels with each node's animation definitions and survives snapshot recovery.

Per-node command streams are ordered and acknowledged. Sequence gaps, exhausted
retries, and queue pressure request recovery snapshots containing active layers
and pending transitions. Reconnecting slaves retain the original start times.
Master reboot restores the matrix under a new session; slaves clear old-session
state and await new definitions. `ON_BOOT` sources start once at the first matrix
activation after master reboot, at the shared activation timestamp. They do not
repeat after finite durations, configuration saves, or slave reconnects. Failed
command enqueue attempts retry until accepted. Use default-state sources for
animations that should restart after ending.
Only the current configuration and protocol
formats are supported. During development, update all nodes and the control app
together and discard incompatible saved configurations; no migration is provided.

## Validation

Run `bash test/test_animation_trigger_table/run.sh` and
`bash test/test_matrix_runtime/run.sh`, then `pio run -e esp32dev`.
The latter host suite exercises the actual service worker with fake radio, NVS,
queues, and clocks, including dropped ACKs, replacement boundaries, reboot
recovery, persistence failures, remote GPIO retries, clock offsets, source-only
nodes, revision/session fencing, and bounded queues. App tests are
`flutter test test/matrix_client_test.dart`.

The layout/identify update was checked on an ESP32 master with the static GPIO19
WS2812 output (initially 60 LEDs) and a one-LED TPIC6B595N on GPIO23/18/5.
The Android app read both outputs, switched between Simple and Advanced, and
saved a 120-LED reversed strip at revision 7. The original 0–60 strip mapping and
RGB LED at 70 were restored at revision 8. Serial logs showed `Activated revision 7`
and `Activated revision 8`; BLE stayed connected through both saves. The identify
request was acknowledged and shown in the app. Host suites cover marker motion,
remote preview metadata, buffer growth failures, and minimum-MTU framing; these
checks do not establish signal quality or seven-node physical performance.

For hardware acceptance, use a master and two slaves on the same firmware.
Upload a multi-chunk configuration, compare scheduled/observed playback times,
trigger an input on a slave, verify every target shares its start timestamp,
disconnect one slave, and verify recovery retains the original phase. Software
tests cannot establish physical start skew or GPIO electrical behavior.
