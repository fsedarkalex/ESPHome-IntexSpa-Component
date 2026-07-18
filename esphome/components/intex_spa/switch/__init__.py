import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG
from .. import intex_spa_ns, IntexSpa, CONF_INTEX_SPA_ID

SpaSwitch = intex_spa_ns.class_("SpaSwitch", switch.Switch)

CONF_POWER      = "power"
CONF_BUBBLE     = "bubble"
CONF_WATER_JET  = "water_jet"
CONF_FAHRENHEIT = "fahrenheit_mode"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_INTEX_SPA_ID): cv.use_id(IntexSpa),

        cv.Optional(CONF_POWER): switch.switch_schema(
            SpaSwitch,
            icon="mdi:power",
        ),
        cv.Optional(CONF_BUBBLE): switch.switch_schema(
            SpaSwitch,
            icon="mdi:chart-bubble",
        ),
        cv.Optional(CONF_WATER_JET): switch.switch_schema(
            SpaSwitch,
            icon="mdi:waves",
        ),
        # Fahrenheit mode: configuration entity
        cv.Optional(CONF_FAHRENHEIT): switch.switch_schema(
            SpaSwitch,
            icon="mdi:temperature-fahrenheit",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
    }
)

_MAP = {
    CONF_POWER:      "set_power_switch",
    CONF_BUBBLE:     "set_bubble_switch",
    CONF_WATER_JET:  "set_water_jet_switch",
    CONF_FAHRENHEIT: "set_fahrenheit_switch",
}


async def to_code(config):
    hub = await cg.get_variable(config[CONF_INTEX_SPA_ID])
    for key, setter in _MAP.items():
        if key in config:
            sw = await switch.new_switch(config[key])
            await cg.register_parented(sw, hub)
            cg.add(getattr(hub, setter)(sw))
