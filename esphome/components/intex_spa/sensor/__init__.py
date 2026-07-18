import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)
from .. import intex_spa_ns, IntexSpa, CONF_INTEX_SPA_ID

CONF_ACTUAL_TEMPERATURE      = "actual_temperature"
CONF_ERROR_CODE              = "error_code"
CONF_FILTER_REMAINING        = "filter_remaining"
CONF_SANITIZER_REMAINING     = "sanitizer_remaining"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_INTEX_SPA_ID): cv.use_id(IntexSpa),

        cv.Optional(CONF_ACTUAL_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
        # error_code: diagnostic entity
        cv.Optional(CONF_ERROR_CODE): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:alert-circle-outline",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # Remaining timer hours reported by the pump
        cv.Optional(CONF_FILTER_REMAINING): sensor.sensor_schema(
            unit_of_measurement="h",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:timer-outline",
        ),
        cv.Optional(CONF_SANITIZER_REMAINING): sensor.sensor_schema(
            unit_of_measurement="h",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:timer-outline",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_INTEX_SPA_ID])

    if CONF_ACTUAL_TEMPERATURE in config:
        s = await sensor.new_sensor(config[CONF_ACTUAL_TEMPERATURE])
        cg.add(hub.set_actual_temperature_sensor(s))

    if CONF_ERROR_CODE in config:
        s = await sensor.new_sensor(config[CONF_ERROR_CODE])
        cg.add(hub.set_error_code_sensor(s))

    if CONF_FILTER_REMAINING in config:
        s = await sensor.new_sensor(config[CONF_FILTER_REMAINING])
        cg.add(hub.set_filter_remaining_sensor(s))

    if CONF_SANITIZER_REMAINING in config:
        s = await sensor.new_sensor(config[CONF_SANITIZER_REMAINING])
        cg.add(hub.set_sanitizer_remaining_sensor(s))
