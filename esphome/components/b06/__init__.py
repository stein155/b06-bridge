import esphome.codegen as cg
from esphome.components import esp32_ble, esp32_ble_client, esp32_ble_tracker
import esphome.config_validation as cv
from esphome.const import CONF_ID

AUTO_LOAD = ["esp32_ble_client", "esp32_ble_tracker", "sensor", "switch", "text_sensor"]
DEPENDENCIES = ["esp32"]
CODEOWNERS = ["@stein155"]
# One block per display; each takes one BLE connection.
MULTI_CONF = True

CONF_B06_ID = "b06_id"
CONF_DEVICE_NAMES = "device_names"
CONF_MAX_CONNECTION_TIME = "max_connection_time"
UNLIMITED = "unlimited"

# Matched case-insensitively as substrings of the advertised name; the display
# advertises as B06_BLE.
DEFAULT_DEVICE_NAMES = ["B06"]

b06_ns = cg.esphome_ns.namespace("b06")
B06Component = b06_ns.class_("B06Component", esp32_ble_client.BLEClientBase)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(B06Component),
            cv.Optional(CONF_DEVICE_NAMES, default=DEFAULT_DEVICE_NAMES): cv.All(
                cv.ensure_list(cv.string_strict), cv.Length(min=1)
            ),
            # Disconnects after reading the display for this long and reconnects
            # once it has been away; the display stays on while connected.
            cv.Optional(CONF_MAX_CONNECTION_TIME, default=UNLIMITED): cv.Any(
                cv.one_of(UNLIMITED, lower=True),
                cv.All(cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(seconds=5))),
            ),
        }
    )
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    esp32_ble.consume_connection_slots(1, "b06"),
)

B06_CHILD_SCHEMA = cv.Schema({cv.GenerateID(CONF_B06_ID): cv.use_id(B06Component)})


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_client(var, config)
    cg.add(var.set_storage_key(str(config[CONF_ID])))
    cg.add(var.set_device_names(config[CONF_DEVICE_NAMES]))
    if config[CONF_MAX_CONNECTION_TIME] != UNLIMITED:
        cg.add(var.set_max_connection_time(config[CONF_MAX_CONNECTION_TIME]))
