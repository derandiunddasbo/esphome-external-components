# fingerprint_grow_extended

An add-on component for ESPHome's
[`fingerprint_grow`](https://esphome.io/components/fingerprint_grow/)
component. It makes advanced features of Grow fingerprint sensors (R503
and compatible) usable that go beyond what `fingerprint_grow` provides:

| Function | Description |
|---|---|
| `get_slot_map` | Which sensor slots are occupied, and which is the next free one |
| `move_fingerprint` | Move a stored fingerprint from one slot to another |
| `read_template` | Read the template stored in a slot (Base64 backup, length, checksum) |
| `write_template` | Write a previously saved template back into a slot (restore) |

The results are returned as an
[action response](https://esphome.io/components/api/#api-respond-action),
so they can be used directly in Home Assistant automations and scripts.

## Using it together with fingerprint_grow

`fingerprint_grow_extended` can be used on its own or in parallel with
`fingerprint_grow`. Both components talk to the same sensor over the same
UART bus. Because ESPHome runs single-threaded, their communication on
the bus does not interfere with each other.

## Installation

### 1. Add the external component

```yaml
external_components:
  - source: github://derandiunddasbo/esphome-external-components
    components: [fingerprint_grow_extended]
```

### 2. Configure the UART bus and the component

```yaml
uart:
  tx_pin: GPIO17
  rx_pin: GPIO5
  baud_rate: 57600

# Optional: the standard component, if you use it
fingerprint_grow:
  password: !secret password_fingerprint_grow

fingerprint_grow_extended:
  id: fingerprint_grow_extended_id
  password: !secret password_fingerprint_grow
```

Adjust the pins to your wiring. The sensor's default baud rate is 57600.

### Configuration variables

- **id** (*Optional*, ID): ID of the component. Needed to call its
  methods from `api.respond` lambdas.
- **password** (*Required*, 32-bit hex value): The sensor password, the
  same value as for `fingerprint_grow`.
- **uart_id** (*Optional*, ID): ID of the UART bus. Not needed if only one
  UART bus is configured.

## Using the functions

The functions are called from Home Assistant through
[API actions](https://esphome.io/components/api/) that you define in your
ESPHome configuration. Each action calls the corresponding method of the
component inside an `api.respond` lambda.

**A complete, ready-to-use definition of all four actions is in
[`example.yaml`](example.yaml).** Copy the `api: actions:` section into
your configuration. The examples below show how to call them from Home
Assistant and which values you get back.

In Home Assistant the actions appear as `esphome.<node_name>_<action>`,
where `<node_name>` is the name of your ESPHome device (with underscores
instead of dashes).

### get_slot_map

Reads which slots of the sensor are occupied.

```yaml
action: esphome.<node_name>_get_slot_map
response_variable: slot_map
```

Response:

| Field | Description |
|---|---|
| `success` | `true` if the sensor was read successfully |
| `slots` | JSON array of occupied slots, e.g. `"[0,1,5,12]"` |
| `next_free_slot` | Lowest free slot number (`200` if all slots are occupied) |

### move_fingerprint

Moves a fingerprint from one slot to another. The source slot is cleared
afterwards, so the fingerprint exists only in the destination slot.

```yaml
action: esphome.<node_name>_move_fingerprint
data:
  source_slot: 5
  dest_slot: 20
response_variable: move_result
```

Response:

| Field | Description |
|---|---|
| `success` | `true` if the fingerprint was moved |
| `error_message` | Empty on success, otherwise the error text |

> **Note:** If `dest_slot` is already occupied, its content is
> overwritten without warning. Check with `get_slot_map` that the
> destination slot is free before moving.

If copying succeeds but clearing the source slot fails, the fingerprint
remains in both slots. This is reported in `error_message`.

### read_template

Reads the complete template stored in a slot, for example to back it up.

```yaml
action: esphome.<node_name>_read_template
data:
  slot: 5
response_variable: template_result
```

Response:

| Field | Description |
|---|---|
| `success` | `true` if the template was read |
| `checksum` | CRC16 checksum of the template data, e.g. `"0x4A3F"` |
| `length` | Length of the template in bytes |
| `data` | The template, Base64-encoded |

Store the `data` string wherever you want to keep your backup (for
example in a file or a helper).

> **Notes:**
> - The template data is treated as an opaque block of bytes. Templates
>   often end in a run of `0xFF` bytes (visible in Base64 as several `/`
>   characters in a row); this is normal. Pass the string back unchanged
>   when restoring.
> - The template of the same finger differs every time it is enrolled,
>   so the checksum does not identify a person. It identifies exactly one
>   stored data set, which is useful for verifying that a template was
>   transferred unchanged.

### write_template

Writes a template previously obtained with `read_template` into a slot
(restore).

```yaml
action: esphome.<node_name>_write_template
data:
  slot: 5
  template_data: "<Base64 string from read_template>"
response_variable: write_result
```

Response:

| Field | Description |
|---|---|
| `success` | `true` if the template was written |
| `error_message` | Empty on success, otherwise the error text |
| `length` | Number of bytes written |

> **Note:** An occupied slot is overwritten without warning.

Each of these functions handles one slot. To back up an entire sensor,
use `get_slot_map` to find the occupied slots and call `read_template`
for each of them.

### Using the functions in ESPHome automations

Within ESPHome itself, the functions are also available as automation
actions: `fingerprint_grow_extended.get_slot_map`,
`fingerprint_grow_extended.move_fingerprint`,
`fingerprint_grow_extended.read_template` and
`fingerprint_grow_extended.write_template`. They perform the operation
but do not return a result, so they are only suitable when you do not
need the response (for example `move_fingerprint` or `write_template` in an
`on_...` trigger). Use the `api.respond` approach above to get results.

## Notes and limitations

- Developed for the **R503** (200 slots). Sensors with a different
  capacity may behave differently.
- Avoid calling the actions from within a `fingerprint_grow` trigger
  while a fingerprint scan is in progress (for example `on_finger_scan_start`).
  Calls made at boot, after enrollment or deletion, or from Home
  Assistant are unproblematic.

## License

Following the licensing model of ESPHome itself, the C++ code (`.cpp`,
`.h`) is published under the **GPLv3** and the Python code and all other
parts (documentation, example configuration) under the **MIT** license.
See the `LICENSE` file in the repository root.
