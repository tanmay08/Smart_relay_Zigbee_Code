# ESP32-C6 Zigbee 4-Channel Relay

ESP-IDF C firmware for a mains-powered ESP32-C6 Zigbee router with four
independent Home Assistant On/Off endpoints. Each Zigbee endpoint controls one
relay output. Four maintained wall-switch inputs control the matching relays
using their actual ON/OFF level; switch changes are debounced rather than
treated as toggles. Each physical input is also published as a standard
Zigbee Binary Input endpoint, intended to appear as a Home Assistant ZHA
binary sensor.

GPIO assignments and electrical polarity are centralized in
[`main/hardware_config.h`](main/hardware_config.h). Defaults match the current
board mapping:

| Channel | Zigbee endpoint | Relay GPIO | Switch GPIO |
| --- | ---: | ---: | ---: |
| 1 | 10 | 17 | 21 |
| 2 | 11 | 19 | 22 |
| 3 | 12 | 20 | 23 |
| 4 | 13 | 18 | 16 |

Relay endpoints are 10–13. Switch-state binary sensor endpoints are 20–23 and
publish their `PresentValue` from the corresponding maintained switch input.

Relays are active-high. Switch GPIOs use internal pull-ups and a closed wall
switch pulls the input to GND (ON). Confirm these electrical levels match the
relay driver circuitry and switch wiring on the final PCB before connecting
mains voltage.

## Build and flash

The checked-in project configuration targets ESP32-C6 and selects the Zigbee
Coordinator/Router stack variant. Build and flash with ESP-IDF:

```text
idf.py build
idf.py -p PORT flash monitor
```

### Zigbee OTA updates

The Zigbee OTA Upgrade client is attached to relay endpoint 10. It checks for
updates hourly. Images must use manufacturer code `0x131B`, image type `0x0001`,
and a file version greater than `HA_ESP_OTA_FILE_VERSION` in
[`main/esp_zb_light.h`](main/esp_zb_light.h). The packaged Zigbee image contains
the ESP-IDF application binary as its OTA upgrade-image subelement.

Version 2 is the first OTA candidate for devices running version 1. Build the
firmware and package it with the same version:

```text
idf.py build
python tools\create_ota_image.py build\Zigbee_Smart_Switch_Relay.bin --version 2 --output build\relay-v2.ota
```

To serve the image from Home Assistant, copy `relay-v2.ota` into
`/config/ota/` and add this legacy ZHA configuration to `configuration.yaml`
(merge it with any existing `zha:` section rather than creating a duplicate):

```yaml
zha:
  zigpy_config:
    ota:
      extra_providers:
        - type: advanced
          path: /config/ota
          warning: "I understand I can *destroy* my devices by enabling OTA updates from files. Some OTA updates can be mistakenly applied to the wrong device, breaking it. I am consciously using this at my own risk."
```

Restart Home Assistant so ZHA loads the local image provider. Open the relay's
ZHA device page and use its firmware update action to check for and install the
available image. Keep the relay powered and within reliable Zigbee range until
it reboots. The bootloader validates the new application and rolls back
automatically if the new firmware does not complete its first startup.

For subsequent releases, increase `HA_ESP_OTA_FILE_VERSION`, rebuild, and pass
that same version to the image packer. Keep both values identical; every OTA
version must be greater than the version currently running on the device.

The partition table has two 1.375 MiB application slots. Install this OTA-ready
partition table and firmware once over USB/serial after `idf.py erase-flash`;
this initial erase removes saved Zigbee network credentials, so pair the relay
again. Subsequent firmware updates can use Zigbee OTA without erasing flash.

For boards connected through their built-in USB Serial/JTAG connector, select
the corresponding COM port and use UART/serial flashing. The application console
is routed to USB Serial/JTAG. OpenOCD JTAG flashing requires a separate,
properly connected JTAG probe; it is not the same as selecting the board's USB
serial port.

Erase Zigbee network state if the device should join as a new device:

```text
idf.py -p PORT erase-flash
```

After startup, the device uses standard Zigbee network steering. Add it to
Home Assistant through ZHA; each relay is exposed as an independent On/Off
endpoint. Zigbee On/Off commands drive the matching relay. Physical switch
changes update the matching local On/Off attribute and report it to the
coordinator. LQI and signal-strength information are network diagnostics
maintained by ZHA/the coordinator rather than extra relay firmware entities;
view them in the device's ZHA diagnostics when available.

After flashing firmware that adds the switch endpoints, reconfigure or
re-interview the device in ZHA so endpoints 20–23 are discovered. If ZHA keeps
the old endpoint list, remove the device from ZHA and pair it again.
