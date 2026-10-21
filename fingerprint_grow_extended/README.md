# fingerprint_grow_extended — Standalone add-on component

An ESPHome add-on for the R503 / Grow fingerprint sensor family that adds
slot occupancy, slot moves and template backup/restore.

## Why this approach?

Earlier attempts to override or extend the original `fingerprint_grow`
component via `external_components` (inheritance, re-implementing the
internal API) repeatedly broke on internal API changes between ESPHome
versions (trigger mechanism, `Action` base class, internal method names,
etc.).

**Therefore: this solution does not touch the original component at all.**

Instead, `fingerprint_grow_extended` is a fully standalone, minimal
component that:
- does **not** inherit from `FingerprintGrowComponent`,
- does **not** use its internal state or methods,
- merely shares the same **UART bus** (the same physical R503 sensor),
- implements commands the original ESPHome component does not know
  (`READ_INDEX_TABLE`, template transfer via `UpChar`/`DownChar`).

The only dependency is `uart::UARTDevice` — a very stable ESPHome base
class that has been unchanged for years.

## Result delivery: action response instead of sensor entities

All four actions of this component (`get_slot_map`, `move_fingerprint`,
`read_template`, `write_template`) deliver their result **uniformly** as
an **action response** (`api.respond`, see the
[ESPHome documentation](https://esphome.io/components/api/#api-respond-action)) —
**not** via sensor/text sensor entities.

**Reason:** Home Assistant entity states are limited to 255 characters.
The slot occupancy list can reach this limit with many occupied slots,
and Base64-encoded raw template data practically always exceeds it by a
wide margin. Going through a sensor entity was needlessly error-prone for
this data — returning the response directly is the way ESPHome provides
for this.

**Practical consequence:** No helper sensors are needed. Instead, call
the action from Home Assistant with `response_variable` and receive the
data directly in the automation/script context — no need to read a
sensor state.

## Why UART sharing works

ESPHome is single-threaded/cooperative. Each component gets processing
time in turn (`loop()`/`update()`). This component sends its command and
waits synchronously (blocking, using `delay()`) for the response — without
yielding control to other components in between. As long as the actions
are not triggered in the middle of a running UART transaction of
`fingerprint_grow`, there is no collision.

**Practical consequence:** Actions of this component should not be
triggered during a running fingerprint scan (e.g. not in the
`on_finger_scan_start` trigger). At boot, after enrollment/deletion, or
on a manual API call it is unproblematic.

## Files

```
components/fingerprint_grow_extended/
├── README.md                           ← this documentation
├── example.yaml                        ← working configuration example
├── __init__.py                         ← Codegen
├── fingerprint_grow_extended.h         ← Header
└── fingerprint_grow_extended.cpp       ← Implementation
```

## Usage as an external component

The component is loaded directly from the Git repository
(see the [ESPHome documentation](https://esphome.io/components/external_components/)).
Nothing needs to be copied locally; your existing `fingerprint_grow:`
section stays unchanged.

```yaml
external_components:
  - source: github://<user>/<repository>@<tag-or-branch>
    components: [fingerprint_grow_extended]
    refresh: never   # when ref is a tag: never update automatically

fingerprint_grow_extended:
  id: fingerprint_grow_extended_id   # required for api.respond lambdas
  password: !secret password_fingerprint_grow
```

Equivalent long form:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/<user>/<repository>
      ref: v1.0.0
    components: [fingerprint_grow_extended]
```

For local development/testing, `source: {type: local, path: components}`
still works.

**Recommendation:** Use release tags (`v1.0.0`) as `ref` and set
`refresh: never`. This way the firmware build does not change unnoticed
when the default branch is updated.

A complete example including the `api.respond` actions is in
[`example.yaml`](example.yaml).

No `uart_id:` needed — since only one UART bus exists in the config,
ESPHome automatically binds the component to the same bus as
`fingerprint_grow`.

## Querying slot occupancy (get_slot_map)

The R503 internally keeps a **32-byte bitmap** (256 bits) per index page.
Each bit represents one slot: `1` = occupied, `0` = free.
Bit 0 of byte 0 = slot 0, bit 7 of byte 24 = slot 199.

`get_slot_map()`:
1. Verifies the password (defensively, in case the sensor restarted in
   the meantime or the order of `setup()` calls is unclear — costs only
   a few milliseconds)
2. Sends `READ_INDEX_TABLE` with page `0x00`
3. Parses the bitmap
4. Returns a struct:
   - **success**: `true`/`false`
   - **slots_json**: `"[0,1,5,12]"` — JSON array of occupied slots
   - **next_free_slot**: `5` — lowest free slot number (200 = full)

**Call from Home Assistant:**

```yaml
action: esphome.<node_name>_get_slot_map
response_variable: slot_map_result
```

Afterwards available: `slot_map_result.slots`,
`slot_map_result.next_free_slot`, `slot_map_result.success`.

Note: the C++ struct field is called `slots_json`; the `api.respond`
lambda in [`example.yaml`](example.yaml) publishes it under the key
`slots`. The key names in the response are defined by your lambda, so
you can rename them freely (just keep your HA automations in sync).

## Moving a fingerprint between slots (move_fingerprint)

**Protocol flow** (three individual R503 commands, each a simple
command/response packet without bulk data transfer):

1. **LoadChar** (0x07) — load the template from `source_slot` into the
   internal character buffer 1
2. **StoreChar** (0x06) — write the buffer contents to `dest_slot`
3. **DeleteChar** (0x0C) — delete `source_slot` so the fingerprint does
   not exist twice

If step 3 fails (e.g. flash error), the fingerprint remains in **both**
slots — this is logged as a warning AND returned in the `error_message`
field of the response. Steps 1 and 2 are critical; if either of them
fails, the operation is aborted immediately and nothing is changed.

Like all other actions of this component, `move_fingerprint` returns an
action response with `success` (bool) and `error_message` (empty on
success, otherwise the error text).

**Call from Home Assistant:**

```yaml
action: esphome.<node_name>_move_fingerprint
data:
  source_slot: 5
  dest_slot: 20
response_variable: move_result
```

Afterwards available: `move_result.success`, `move_result.error_message`.

**Important:** If `dest_slot` is already occupied, it is silently
overwritten during the `StoreChar` step — the R503 does not check this
itself. Before moving, check via `get_slot_map` whether the destination
slot is really free.

## Reading a template: checksum, length, backup data (read_template)

In addition to the plain occupied/free information, the complete stored
template can be read out per slot — the basis for:
- a **checksum** to recognize which concrete data set is stored in a
  slot (see the limitation below),
- a **Base64-encoded raw copy** of the template that can be saved
  externally and later restored via `write_template` (basis for
  backup/restore).

**Protocol flow:**

1. **ReadSysParam** (0x0F) — automatically on the first call: reads the
   data packet size configured on the sensor (32/64/128/256 bytes) and
   remembers it for all following multi-packet transfers
2. **LoadChar** (0x07) — load the template from the slot into buffer 1
3. **UpChar** (0x08) — read the buffer contents as a multi-packet data
   stream (DATA packets, last packet marked as END_DATA)
4. Compute CRC16 (CCITT/XModem) over the received raw data
5. Return the result as a struct: **success**, **checksum_hex**,
   **length**, **data_base64**

**Important limitation regarding the checksum:** An R503 template is
**not deterministic** per physical finger — when the same finger is
captured again, slightly different placement and feature extraction
produce a different template and thus a different checksum. The
checksum therefore does not identify "this person", only "exactly this
stored data set" — useful, for example, to verify transfer integrity
after `move_fingerprint` or to note in your own HA mapping table when a
slot was last written.

**Observation — trailing `0xFF` bytes in the template:** Templates often
end with a series of `0xFF` bytes (visible in Base64 as several `/`
characters in a row). This is presumably padding to a fixed internal
buffer size of the sensor, common in flash-based systems (0xFF =
erased/default state). This implementation consistently treats the
template as an **opaque byte blob**: nothing is interpreted, trimmed or
otherwise modified — `read_template` returns exactly the received bytes
(including padding), and `write_template` sends back exactly what is
passed in. As long as the Base64 string is passed unchanged from
`read_template` to `write_template` (not manually shortened or edited),
the padding is reproduced correctly.

**Call from Home Assistant:**

```yaml
action: esphome.<node_name>_read_template
data:
  slot: 5
response_variable: template_result
```

Afterwards available: `template_result.success`,
`template_result.checksum`, `template_result.length`,
`template_result.data`.

Note: the C++ struct fields are called `checksum_hex` and `data_base64`;
the `api.respond` lambda in [`example.yaml`](example.yaml) publishes them
under the keys `checksum` and `data`.

## Writing a template (restore) — write_template

Counterpart to `read_template` — writes previously saved Base64 data
back into a slot:

1. Announce **DownChar** (0x09)
2. Send the template data in packets of the detected size
   (last packet marked as END_DATA)
3. **StoreChar** (0x06) — store the buffer contents in the destination
   slot

Like all other actions of this component, `write_template` returns an
action response with `success` (bool), `error_message` (empty on
success, otherwise the error text) and `length` (number of bytes
written).

**Call from Home Assistant:**

```yaml
action: esphome.<node_name>_write_template
data:
  slot: 5
  template_data: "<Base64 string previously obtained via read_template>"
response_variable: write_result
```

Afterwards available: `write_result.success`, `write_result.error_message`,
`write_result.length`.

**Note on using this as backup/restore:** These actions back up/restore
**a single slot** each. For a complete library backup, an HA automation
would have to iterate over all occupied slots (from `get_slot_map`),
call `read_template` and store the resulting Base64 strings externally
(e.g. in a file) — this is possible with the building blocks provided
here, but not yet a finished "one-click backup" solution.

## Notes

- Developed for the **R503** (200 slots, `READ_INDEX_TABLE = 0x1F`
  according to the datasheet). Other Grow sensors with a different
  capacity may require adjusting the `MAX_SLOTS` constant.
- The password is re-verified on every call that needs it (defensive,
  minimal additional latency).
- The timeout for individual packets is set to 3 seconds — if you have
  problems with slow UART responses, adjust it in
  `fingerprint_grow_extended.cpp` (`timeout_ms` in `read_raw_packet_`).

## License

Following the licensing model of ESPHome itself, the C++ code (`.cpp`,
`.h`) is published under the **GPLv3** and the Python code and all other
parts (documentation, example configuration) under the **MIT** license.
See the `LICENSE` file in the repository root.
