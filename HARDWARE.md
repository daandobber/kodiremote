# Tanmatsu Hardware Notes

Short hardware reference for MatrixMatsu development. Source pages:

- https://docs.tanmatsu.cloud/hardware/
- https://docs.tanmatsu.cloud/hardware/specifications/
- https://docs.tanmatsu.cloud/hardware/connectors/
- https://docs.tanmatsu.cloud/hardware/i2c/

The official specifications page notes that the hardware information is still
being actively worked on, so treat this file as a practical development summary
rather than the final electrical reference.

## Device

- Dimensions with case: 12 cm wide, 13.5 cm long, 1.8 cm high.
- Weight with case and battery: about 215 g.
- Main application processor: Espressif ESP32-P4NRW32.
- CPU: dual-core 32-bit RISC-V, up to 400 MHz.
- Main RAM: 32 MB.
- Main flash: 16 MB.
- Build target used by this repo: `esp32p4`.

## Display

- Panel: SWI LH397K-IC01.
- Size: 3.97 inch diagonal, 51.84 x 86.40 mm.
- Resolution: 480 x 800.
- Current software color mode: RGB565 / 16-bit / 65,536 colors.
- Controller: ST7701S.
- Interface: MIPI DSI, 2 data lanes.
- Brightness: 330 cd/m2.

MatrixMatsu currently renders through PAX buffers. For image work, prefer
decoding down to a bounded preview size and using RGB565 where possible.

## Input

- Front keyboard: 69-key alphanumeric keyboard with 6 colored function keys.
- Keyboard has white LED backlight.
- Keyboard events are exposed by the BSP as scancode, navigation, and keyboard
  events.
- Keyboard text events include ASCII and UTF-8 text with modifier handling.
- Right side buttons, top to bottom:
  - Power button.
  - `+` button, mapped by BSP to `BSP_INPUT_NAVIGATION_KEY_VOLUME_UP`.
  - `-` button, mapped by BSP to `BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN`.
- Holding `-` while powering on enters ESP32-P4 USB download mode.

MatrixMatsu uses the side volume buttons for audio volume.

## Radio And Networking

- WiFi/BLE/802.15.4 module: ESP32-C6-WROOM-1-N8.
- ESP32-C6 CPU: single-core 32-bit RISC-V, up to 160 MHz.
- ESP32-C6 RAM: 512 KB.
- ESP32-C6 flash: 8 MB.
- WiFi: 2.4 GHz WiFi 6.
- BLE: 5.3.
- Mesh radio: IEEE 802.15.4-2015, with Thread 1.3 and Zigbee 3.0 support.
- The ESP32-C6 normally runs ESP-Hosted-MCU so the ESP32-P4 app can use WiFi
  and BLE over SDIO.

## Audio

- Audio codec: Everest Semiconductor ES8156.
- Codec features used by app work: stereo DAC and hardware volume control over
  I2C.
- Speaker amplifier: FM8002A mono amplifier.
- Built-in speaker: 8 ohm, connected through the amplifier.
- Speaker amplifier can be switched by the coprocessor.
- Headphone jack: 3.5 mm.

Useful app-development detail: the ES8156 is on the internal I2C bus at address
`0x08`.

## Storage And External Interfaces

- USB-C device port:
  - Charging.
  - ESP32-P4 USB serial/JTAG by default.
  - ESP32-C6 USB serial/JTAG.
  - BadgeLink app/file management when the launcher exposes it.
- USB-A host port:
  - USB 2.0 host.
  - 5 V output, up to 1 A with protection.
- MicroSD slot:
  - SDIO 2 and SDIO 3 compatible cards.
  - Supports 3.3 V and 1.8 V voltage levels.
- Qwiic / Stemma QT connector:
  - 4-pin SH connector.
  - I2C or I3C accessories.
- Camera connector:
  - Raspberry Pi Zero / Raspberry Pi 5 compatible 22-pin MIPI CSI connector.
  - Software support is limited to supported camera sensors.
- CATT connector:
  - Combined PMOD / SAO compatible 2.54 mm pinsocket.
  - Can also expose JTAG for ESP32-P4 debugging.
- Battery connector:
  - PH-2.0 2-pin connector.
  - Protected single-cell LiPo/Li-Ion battery required.

## Battery And Power

- Battery chemistry: single-cell protected Lithium Polymer.
- Capacity: 2500 mAh.
- Nominal voltage: 3.7 V.
- Maximum cell voltage: 4.2 V.
- Minimum cell voltage listed by docs: 2.5 V, with 3.0 V recommended.
- PMIC: Texas Instruments BQ25895RTW.
- 3.3 V rail: Texas Instruments TPS63020DSJR buck/boost converter.
- Display and keyboard backlights use AP3032KTR drivers controlled by the
  CH32V203 coprocessor.

## Coprocessor And Internal I2C

- Management coprocessor: WCH CH32V203C8T6.
- CPU: single-core 32-bit RISC-V, up to 144 MHz.
- RAM: 20 KB.
- Flash: 64 KB.
- Responsibilities include:
  - keyboard scanning
  - power management
  - RTC and alarm wakeup
  - display and keyboard backlight PWM
  - USB-A power switching
  - ESP32-C6 power and boot-mode control
  - audio amplifier switching
  - headphone detection
  - 6 addressable LEDs

Internal I2C bus:

| Device | Address | Purpose |
| --- | ---: | --- |
| CH32V203 coprocessor | `0x5F` | Power, keyboard, RTC, LEDs, backlights |
| ES8156 audio codec | `0x08` | Stereo audio output |
| BMI270 IMU | `0x68` | Accelerometer and gyroscope |
| Internal add-on EEPROM | `0x50` | Optional add-on board identification |
| SCD4x CO2 sensor | `0x62` | Optional sensor header module |

Internal I2C pins on ESP32-P4:

- SDA: GPIO 9
- SCL: GPIO 10

Qwiic connector pins:

- SDA: GPIO 33
- SCL: GPIO 32

CATT startup I2C pins:

- SDA: GPIO 12
- SCL: GPIO 13

## LEDs

- Six user-controllable SK6805-EC20 addressable RGB LEDs around the screen.
- Color byte order is GRB.
- There is also one red LED on the back controlled by the power management chip.
- Addressable LED order:
  - LED0: Power
  - LED1: Radio
  - LED2: Messages
  - LED3: Power button LED
  - LED4: A
  - LED5: B

