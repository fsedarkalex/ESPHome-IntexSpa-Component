import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from .. import IntexSpa, CONF_INTEX_SPA_ID

CONF_FILTER_REMAINING_HM    = "filter_remaining_hm"
CONF_SANITIZER_REMAINING_HM = "sanitizer_remaining_hm"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_INTEX_SPA_ID): cv.use_id(IntexSpa),

        # Human-friendly "H:MM" formatted remaining time, e.g. "3:40".
        # Derived from the same sub-hour estimate as the numeric
        # *_remaining_precise sensors, just pre-formatted for display.
        cv.Optional(CONF_FILTER_REMAINING_HM): text_sensor.text_sensor_schema(
            icon="mdi:timer-outline",
        ),
        cv.Optional(CONF_SANITIZER_REMAINING_HM): text_sensor.text_sensor_schema(
            icon="mdi:timer-outline",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_INTEX_SPA_ID])

    if CONF_FILTER_REMAINING_HM in config:
        s = await text_sensor.new_text_sensor(config[CONF_FILTER_REMAINING_HM])
        cg.add(hub.set_filter_remaining_hm_text_sensor(s))

    if CONF_SANITIZER_REMAINING_HM in config:
        s = await text_sensor.new_text_sensor(config[CONF_SANITIZER_REMAINING_HM])
        cg.add(hub.set_sanitizer_remaining_hm_text_sensor(s))
