import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from .. import intex_spa_ns, IntexSpa, CONF_INTEX_SPA_ID

SpaTimerSelect = intex_spa_ns.class_("SpaTimerSelect", select.Select)

CONF_FILTER_TIMER    = "filter_timer"
CONF_SANITIZER_TIMER = "sanitizer_timer"

# Valid options per function (pump only accepts these values)
FILTER_OPTIONS    = ["Off", "2h", "4h", "6h"]
SANITIZER_OPTIONS = ["Off", "3h", "5h", "8h"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_INTEX_SPA_ID): cv.use_id(IntexSpa),

        cv.Optional(CONF_FILTER_TIMER): select.select_schema(
            SpaTimerSelect,
            icon="mdi:filter-settings",
        ),
        # sanitizer_timer only makes sense for model 28458
        cv.Optional(CONF_SANITIZER_TIMER): select.select_schema(
            SpaTimerSelect,
            icon="mdi:bacteria-outline",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_INTEX_SPA_ID])

    if CONF_FILTER_TIMER in config:
        sel = await select.new_select(config[CONF_FILTER_TIMER], options=FILTER_OPTIONS)
        await cg.register_parented(sel, hub)
        cg.add(hub.set_filter_select(sel))

    if CONF_SANITIZER_TIMER in config:
        sel = await select.new_select(config[CONF_SANITIZER_TIMER], options=SANITIZER_OPTIONS)
        await cg.register_parented(sel, hub)
        cg.add(hub.set_sanitizer_select(sel))
