"""
ESPHome component for Intex® PureSpa

Supported models:
  28458 / 28462 / 28457 (US) / 28461 (US)  - with water jet + sanitizer
  28442 / 28440

Based on the original Arduino firmware by Yogui79
(https://github.com/Yogui79/IntexPureSpa, MIT License).

Hardware wiring (LC12S <-> ESP32):
  LC12S GND  -> ESP32 GND
  LC12S CS   -> ESP32 GPIO18
  LC12S SET  -> ESP32 GPIO19
  LC12S TX   -> ESP32 GPIO16  (UART2 RX)
  LC12S RX   -> ESP32 GPIO17  (UART2 TX)
  LC12S VCC  -> ESP32 3.3 V

HA notifications (persistent_notification.create/dismiss) are handled
Requires 'homeassistant_services: true' in the api: YAML block.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, time
from esphome.const import CONF_ID, CONF_TIME_ID
from esphome.core import CORE

CODEOWNERS = ["@Yogui79"]
DEPENDENCIES = ["uart", "api"]
AUTO_LOAD = ["sensor", "binary_sensor", "switch", "select", "climate", "text_sensor"]

intex_spa_ns = cg.esphome_ns.namespace("intex_spa")
IntexSpa = intex_spa_ns.class_("IntexSpa", cg.Component, uart.UARTDevice)

CONF_INTEX_SPA_ID = "intex_spa_id"
CONF_NETWORK_ID   = "network_id"
CONF_CHANNEL      = "channel"
CONF_MODEL        = "model"
CONF_CS_PIN       = "cs_pin"
CONF_SET_PIN      = "set_pin"
CONF_ACTIVE_SCAN  = "active_scan"

MODEL_28458 = 28458
MODEL_28442 = 28442

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID():                             cv.declare_id(IntexSpa),
            cv.Required(CONF_NETWORK_ID):                cv.hex_uint16_t,
            cv.Required(CONF_CHANNEL):                   cv.hex_uint8_t,
            cv.Required(CONF_MODEL):                     cv.one_of(MODEL_28458, MODEL_28442, int=True),
            cv.Optional(CONF_CS_PIN,      default=18):   cv.uint8_t,
            cv.Optional(CONF_SET_PIN,     default=19):   cv.uint8_t,
            cv.Optional(CONF_ACTIVE_SCAN, default=True): cv.boolean,
            # Optional real-time clock (e.g. `time: - platform: homeassistant`
            # or `sntp`). If omitted here, any `time:` platform configured
            # elsewhere in the YAML is auto-detected and used automatically –
            # no explicit wiring needed unless you have multiple time sources
            # and want to pick a specific one via time_id.
            # Without any RTC at all, the filter/sanitizer sub-hour estimate
            # still works but resets its progress on every reboot instead of
            # surviving it via NVS.
            cv.Optional(CONF_TIME_ID):                   cv.use_id(time.RealTimeClock),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_network_id(config[CONF_NETWORK_ID]))
    cg.add(var.set_channel(config[CONF_CHANNEL]))
    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_cs_pin(config[CONF_CS_PIN]))
    cg.add(var.set_set_pin(config[CONF_SET_PIN]))
    cg.add(var.set_active_scan(config[CONF_ACTIVE_SCAN]))

    if CONF_TIME_ID in config:
        time_var = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_id(time_var))
    else:
        # No explicit time_id given – auto-detect any configured `time:`
        # platform (e.g. `time: - platform: homeassistant`) and wire it up
        # automatically, so the user doesn't need to reference it by hand.
        # Uses the first one found if multiple time platforms are configured;
        # set `time_id:` explicitly under intex_spa: to pick a specific one.
        time_confs = CORE.config.get("time", [])
        if time_confs:
            time_var = await cg.get_variable(time_confs[0][CONF_ID])
            cg.add(var.set_time_id(time_var))
