#!/usr/bin/env python3
"""Package an ESP-IDF application binary as a Zigbee OTA image."""

import argparse
import pathlib
import struct

OTA_FILE_IDENTIFIER = 0x0BEEF11E
OTA_HEADER_VERSION = 0x0100
OTA_HEADER_LENGTH = 56
OTA_MANUFACTURER_CODE = 0x131B
OTA_IMAGE_TYPE = 0x0001
OTA_STACK_VERSION = 0x0002
OTA_IMAGE_TAG_ID = 0x0000
OTA_HEADER = struct.Struct("<IHHHHHIH32sI")
OTA_SUBELEMENT_HEADER = struct.Struct("<HI")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=pathlib.Path, help="ESP-IDF application .bin")
    parser.add_argument("--version", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()

    firmware = args.firmware.read_bytes()
    if not firmware or firmware[0] != 0xE9:
        parser.error("firmware is not an ESP application image (expected 0xE9 magic)")
    if not 0 <= args.version <= 0xFFFFFFFF:
        parser.error("version must fit in an unsigned 32-bit integer")

    image_size = (
        OTA_HEADER_LENGTH + OTA_SUBELEMENT_HEADER.size + len(firmware)
    )
    header = OTA_HEADER.pack(
        OTA_FILE_IDENTIFIER,
        OTA_HEADER_VERSION,
        OTA_HEADER_LENGTH,
        0,
        OTA_MANUFACTURER_CODE,
        OTA_IMAGE_TYPE,
        args.version,
        OTA_STACK_VERSION,
        b"Zigbee Smart Switch Relay".ljust(32, b"\0"),
        image_size,
    )
    subelement_header = OTA_SUBELEMENT_HEADER.pack(OTA_IMAGE_TAG_ID, len(firmware))
    args.output.write_bytes(header + subelement_header + firmware)
    print(f"Created {args.output} ({image_size} bytes, version {args.version})")


if __name__ == "__main__":
    main()
