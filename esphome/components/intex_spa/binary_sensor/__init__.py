import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC
from .. import IntexSpa, CONF_INTEX_SPA_ID

CONF_HEATER_ACTIVE   = "heater_active"
CONF_WATER_FILTER    = "water_filter"
CONF_BUBBLE          = "bubble"
CONF_WATER_JET       = "water_jet"
CONF_SANITIZER       = "sanitizer"
CONF_COMM_ERROR      = "communication_error"
CONF_SPA_ACTIVE      = "spa_active"
CONF_SCANNING        = "channel_scanning"
CONF_IS_SENDING      = "is_sending"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_INTEX_SPA_ID): cv.use_id(IntexSpa),

        # True when heating element is actively running (red flame on remote)
        cv.Optional(CONF_HEATER_ACTIVE): binary_sensor.binary_sensor_schema(
            device_class="heat",
            icon="mdi:fire",
        ),
        cv.Optional(CONF_WATER_FILTER): binary_sensor.binary_sensor_schema(
            icon="mdi:filter",
        ),
        cv.Optional(CONF_BUBBLE): binary_sensor.binary_sensor_schema(
            icon="mdi:chart-bubble",
        ),
        cv.Optional(CONF_WATER_JET): binary_sensor.binary_sensor_schema(
            icon="mdi:waves",
        ),
        cv.Optional(CONF_SANITIZER): binary_sensor.binary_sensor_schema(
            icon="mdi:bacteria-outline",
        ),
        # Diagnostic – becomes 'on' when pump unreachable for > 10 s
        cv.Optional(CONF_COMM_ERROR): binary_sensor.binary_sensor_schema(
            device_class="problem",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # True = pump online AND spa power on
        cv.Optional(CONF_SPA_ACTIVE): binary_sensor.binary_sensor_schema(
            icon="mdi:hot-tub",
        ),
        # Diagnostic – true while channel auto-scan is running
        cv.Optional(CONF_SCANNING): binary_sensor.binary_sensor_schema(
            icon="mdi:radar",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # Diagnostic – true while a command sequence is being sent to the pump
        cv.Optional(CONF_IS_SENDING): binary_sensor.binary_sensor_schema(
            icon="mdi:send-clock",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)

_MAP = {
    CONF_HEATER_ACTIVE:  "set_heater_active_binary_sensor",
    CONF_WATER_FILTER:   "set_water_filter_binary_sensor",
    CONF_BUBBLE:         "set_bubble_binary_sensor",
    CONF_WATER_JET:      "set_water_jet_binary_sensor",
    CONF_SANITIZER:      "set_sanitizer_binary_sensor",
    CONF_COMM_ERROR:     "set_comm_error_binary_sensor",
    CONF_SPA_ACTIVE:     "set_spa_active_binary_sensor",
    CONF_SCANNING:       "set_scanning_binary_sensor",
    CONF_IS_SENDING:     "set_is_sending_binary_sensor",
}


async def to_code(config):
    hub = await cg.get_variable(config[CONF_INTEX_SPA_ID])
    for key, setter in _MAP.items():
        if key in config:
            s = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(hub, setter)(s))
