# RP2040 IMU Display

A Loom bare-metal firmware example that reads an MPU6050 and continuously shows its three accelerometer and three gyroscope axes on a 128x64 SSD1306 I2C OLED.

## Wiring

Both modules use 3.3 V logic and share ground. The firmware uses two RP2040 I2C controllers:

| Device | RP2040 pin | Connect to |
|---|---:|---|
| SSD1306 OLED | GP4 | SDA |
| SSD1306 OLED | GP5 | SCL |
| MPU6050 | GP2 | SDA |
| MPU6050 | GP3 | SCL |
| Both | 3V3 | VCC (if the module supports 3.3 V) |
| Both | GND | GND |

Use I2C pull-ups appropriate for your modules if they are not already fitted. Confirm the pinout and voltage requirements of your specific boards before wiring. The drivers use their default addresses: `0x3C` for the OLED and `0x68` for the MPU6050.

## Display

The screen is organized into two compact sections. Accelerometer values are shown in milli-g (`mg`); gyro values are shown in degrees per second (`dps`) with two decimal places. The sensor is calibrated at startup with 100 samples, so keep it still and level during startup. Calibration removes gyro zero-rate bias and the accelerometer's level-position bias while retaining gravity on Z.

## Build and flash

Select the RP2040 toolchain, then build from this directory:

```sh
loom build --release --toolchain rp2040
```

The build produces a UF2 file under `target/`. Hold the Pico BOOTSEL button while connecting it over USB, then copy the UF2 to the mounted `RPI-RP2` drive. The command selects the RP2040 toolchain just for this build, leaving your default toolchain unchanged.

The `src/ssd1306`, `src/mpu6050`, and `src/bsp` entries link to the ready-to-use libraries and RP2040 BSP in the parent Loom repository, so this project builds against those sources without maintaining duplicate copies.
