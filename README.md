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

Relay outputs are active-low: Zigbee/Home Assistant ON drives the relay GPIO
low, and OFF drives it high. Switch GPIOs use internal pull-ups and a closed
wall switch pulls the input to GND (ON). Confirm these electrical levels match
the relay driver circuitry and switch wiring on the final PCB before connecting
mains voltage.
Switch inputs are sampled every 10 ms and use a 30 ms debounce interval to
reject contact bounce while keeping button response prompt. Physical changes
update the relay locally and queue Zigbee state reports immediately; reports
retry if the Zigbee stack is temporarily busy.

Relay states are saved to NVS whenever a Zigbee command or physical switch
changes a relay. At startup, relay GPIOs are initialized to their inactive
level first; saved states are restored only after NVS, Zigbee, and GPIO
initialization succeeds. A maintained physical switch is sampled for its
binary sensor, but does not override the saved relay state until that switch
position changes. Brownout detection is enabled to reset the ESP32 if its
supply voltage falls below the configured threshold; it is not used to write
flash at power loss, because voltage may disappear before such a write can
complete. For predictable relay-off behavior while the ESP32 is unpowered or
resetting, the relay driver circuit must also provide an external inactive-level
pull resistor.

## Build and flash

The checked-in project configuration targets ESP32-C6 and selects the Zigbee
Coordinator/Router stack variant. Build and flash with ESP-IDF:

```text
idf.py build
idf.py -p PORT erase-flash
idf.py -p PORT flash monitor
```

Erasing flash clears Zigbee network credentials and all other device data.
After flashing, remove the old device entry from Home Assistant if it remains,
then pair the relay again. Use this full erase-and-flash sequence when a clean
Zigbee rejoin is needed; normal firmware updates do not require erasing flash.

### Zigbee OTA updates

This device receives firmware through the Zigbee OTA Upgrade cluster; it does
not download the image over Wi-Fi. The OTA client is on relay endpoint 10 and
queries for available images hourly. Home Assistant must use ZHA, and the
device must be powered, joined to the same Zigbee network as the coordinator,
and in reliable radio range for the update.

#### 1. Confirm the device is ready for OTA

OTA requires the two application slots in this project's partition table and
firmware that includes the Zigbee OTA client. If the device already runs
OTA-capable firmware with this partition table, skip this step. If it runs
older firmware or uses a different partition table, install this project over
USB/serial once:

```text
idf.py build
idf.py -p PORT erase-flash
idf.py -p PORT flash monitor
```

Replace `PORT` with the device's serial port (for example, `COM5`). Erasing
flash deletes Zigbee pairing and saved settings, so pair the device with ZHA
again afterward. **Do not erase flash for normal OTA releases.** OTA updates
replace only the application; they do not update the bootloader or partition
table.

#### 2. Build and package the release

The OTA file version must be greater than the version currently running on the
device. It must also match in both places:

1. Set `HA_ESP_OTA_FILE_VERSION` in
   [`main/esp_zb_light.h`](main/esp_zb_light.h) to the new release number.
2. Build the firmware and pass that exact same number to the image packer.

The checked-in source currently uses version `3`. For devices already running
version 2, build and package version 3 as follows:

```text
idf.py build
python tools\create_ota_image.py build\Zigbee_Smart_Switch_Relay.bin --version 3 --output build\relay-v3.ota
```

For the next release after version 3, change the macro to `4`, rebuild, and
package with `--version 4`. A version 3 image is also newer than version 1,
provided that device already has OTA-capable firmware and the OTA-ready
partition table. For each release, use the same number in the firmware and
image. Do not reuse a version or package with a number different from the
firmware: the device ignores images that are not newer or whose metadata does
not match.

The packer wraps the ESP-IDF application binary in the Zigbee OTA image format.
The image uses manufacturer code `0x131B` and image type `0x0001`. Keep the
generated `.ota` extension and do not modify the image contents.

#### 3. Make the image available to ZHA

On the Home Assistant host, create `/config/ota/` if it does not exist and copy
the generated image there. For the example above, the final path is
`/config/ota/relay-v3.ota`.

Configure ZHA's local OTA file provider in `configuration.yaml`. If the file
already has a `zha:` section, merge the `zigpy_config` contents into it; do not
add a second top-level `zha:` key:

```yaml
zha:
  zigpy_config:
    ota:
      extra_providers:
        - type: advanced
          path: /config/ota
          warning: "I understand I can *destroy* my devices by enabling OTA updates from files. Some OTA updates can be mistakenly applied to the wrong device, breaking it. I am consciously using this at my own risk."
```

This is the legacy, YAML-based ZHA provider configuration. Restart Home
Assistant after changing it so ZHA discovers the provider and image.

#### 4. Start and monitor the update

1. In Home Assistant, open the relay in **Settings → Devices & services →
   ZHA → Devices**.
2. On the relay's device page, find **Firmware** in the **Configuration**
   section. If ZHA has found the image, Home Assistant will show an available
   version and an update action there and/or in its update notification. Open
   the update entry, confirm the offered version matches your release, and
   choose **Update**.
3. If the Firmware row says **Unknown** and there is no update action, no OTA
   transfer is in progress and no usable update is currently being offered.
   The hourly query can take up to an hour; after configuring the provider,
   restart Home Assistant and allow time for the next query.
4. Keep the relay powered and within reliable Zigbee range. Do not unplug it
   or interrupt the coordinator while the image is transferring or the relay
   is rebooting.
5. Wait for the relay to restart and rejoin. The bootloader validates the new
   application; if the new firmware fails its first startup, rollback restores
   the previously working application.

If Firmware remains **Unknown** or no update is offered after the next query,
check that the `.ota` file is under `/config/ota/`, the provider configuration
is valid and Home Assistant was restarted, and the image version is newer than
the running firmware and matches `HA_ESP_OTA_FILE_VERSION`. Also confirm the
device has OTA-capable firmware and the OTA-ready partition table. If an update
fails, keep the device powered, check the device/coordinator logs, and retry
after correcting the image or radio-range issue. A normal failed OTA does not
require erasing flash or re-pairing.

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
