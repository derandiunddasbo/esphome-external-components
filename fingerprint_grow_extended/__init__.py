# SPDX-License-Identifier: MIT
"""ESPHome codegen for the standalone fingerprint_grow_extended component.

This component is deliberately INDEPENDENT of the original
fingerprint_grow component. It does not modify its code and does not
inherit from it. Both merely share the same UART bus (the same physical
R503 sensor).

Result handling: get_slot_map() and read_template() return their results
as a C++ return value (struct), NOT via sensor entities. Home Assistant
entity states are limited to 255 characters, which is not enough for raw
template data (Base64) and sometimes not even for the slot occupancy list.
Instead, the methods are called directly from an `api.respond` lambda in
the YAML config (see example.yaml).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
import esphome.components.uart as uart
from esphome.const import CONF_ID, CONF_PASSWORD

fingerprint_grow_extended_ns = cg.esphome_ns.namespace("fingerprint_grow_extended")
FingerprintGrowExtendedComponent = fingerprint_grow_extended_ns.class_(
    "FingerprintGrowExtendedComponent", cg.Component, uart.UARTDevice
)
GetSlotMapAction = fingerprint_grow_extended_ns.class_("GetSlotMapAction", automation.Action)
MoveSlotAction = fingerprint_grow_extended_ns.class_("MoveSlotAction", automation.Action)
ReadTemplateAction = fingerprint_grow_extended_ns.class_("ReadTemplateAction", automation.Action)
WriteTemplateAction = fingerprint_grow_extended_ns.class_("WriteTemplateAction", automation.Action)

CONF_SOURCE_SLOT = "source_slot"
CONF_DEST_SLOT = "dest_slot"
CONF_SLOT = "slot"
CONF_TEMPLATE_DATA = "template_data"

DEPENDENCIES = ["uart"]

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FingerprintGrowExtendedComponent),
            cv.Required(CONF_PASSWORD): cv.hex_uint32_t,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_password(config[CONF_PASSWORD]))


FINGERPRINT_GROW_EXTENDED_ACTION_SCHEMA = cv.Schema(
    {cv.GenerateID(): cv.use_id(FingerprintGrowExtendedComponent)}
)


@automation.register_action(
    "fingerprint_grow_extended.get_slot_map",
    GetSlotMapAction,
    FINGERPRINT_GROW_EXTENDED_ACTION_SCHEMA,
    synchronous=True,
)
async def fingerprint_grow_extended_get_slot_map_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "fingerprint_grow_extended.move_slot",
    MoveSlotAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowExtendedComponent),
            cv.Required(CONF_SOURCE_SLOT): cv.templatable(cv.uint16_t),
            cv.Required(CONF_DEST_SLOT): cv.templatable(cv.uint16_t),
        }
    ),
    synchronous=True,
)
async def fingerprint_grow_extended_move_slot_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    tmpl = await cg.templatable(config[CONF_SOURCE_SLOT], args, cg.uint16)
    cg.add(var.set_source_slot(tmpl))
    tmpl = await cg.templatable(config[CONF_DEST_SLOT], args, cg.uint16)
    cg.add(var.set_dest_slot(tmpl))
    return var


@automation.register_action(
    "fingerprint_grow_extended.read_template",
    ReadTemplateAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowExtendedComponent),
            cv.Required(CONF_SLOT): cv.templatable(cv.uint16_t),
        }
    ),
    synchronous=True,
)
async def fingerprint_grow_extended_read_template_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    tmpl = await cg.templatable(config[CONF_SLOT], args, cg.uint16)
    cg.add(var.set_slot(tmpl))
    return var


@automation.register_action(
    "fingerprint_grow_extended.write_template",
    WriteTemplateAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowExtendedComponent),
            cv.Required(CONF_SLOT): cv.templatable(cv.uint16_t),
            cv.Required(CONF_TEMPLATE_DATA): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def fingerprint_grow_extended_write_template_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    tmpl = await cg.templatable(config[CONF_SLOT], args, cg.uint16)
    cg.add(var.set_slot(tmpl))
    tmpl = await cg.templatable(config[CONF_TEMPLATE_DATA], args, cg.std_string)
    cg.add(var.set_template_data(tmpl))
    return var
