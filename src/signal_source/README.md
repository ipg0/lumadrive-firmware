# Signal sources

Implement `signal_source::source` to provide a sampled value using the shared
master-clock timestamp. Sources can initialize and release resources through
`init()` and `deinit()`; `sample()` returns false when no value is available.
The source registry owns initialized instances and retains both configuration
revisions during an activation. Add source construction to `source_factory.hpp`
and the protocol-to-source mapping when introducing a new configurable type.

`virtual_source` implements the configured waveforms. `remote_sensor_source`
adapts incoming node sensor values to the same sampling interface. Hardware
drivers that produce asynchronous samples can call `signal_source::submit_sample`
with the sensor ID, value, and local capture timestamp; the registered transport
sink queues and batches samples without exposing the matrix API to the driver.
For a polling driver implementing `source`, `publish_sample` samples it and
submits the result through that same sink; the caller owns its sampling schedule.
The current sink is installed during matrix service initialization. Samples
are accepted only after initialization and may be dropped when the queue is full.

The matrix maps the resulting values to animation positions and broadcasts the
same timestamped batches for virtual generators and master-local or remote
hardware inputs. No physical sensor driver or provisioning UI is included yet.
