import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_LIGHT

from . import B06_CHILD_SCHEMA, CONF_B06_ID, b06_ns

DEPENDENCIES = ["b06"]

B06LightSwitch = b06_ns.class_("B06LightSwitch", switch.Switch)

CONFIG_SCHEMA = B06_CHILD_SCHEMA.extend(
    {
        cv.Optional(CONF_LIGHT): switch.switch_schema(B06LightSwitch, icon="mdi:lightbulb", block_inverted=True),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_B06_ID])
    if light_config := config.get(CONF_LIGHT):
        sw = await switch.new_switch(light_config)
        await cg.register_parented(sw, config[CONF_B06_ID])
        cg.add(hub.set_light_switch(sw))
