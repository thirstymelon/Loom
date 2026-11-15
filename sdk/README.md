# Loom Software Development Kit (SDK)

The Loom SDK provides the standard library, target support, and Board Support Packages (BSP) for Loom. The RP2040 BSP currently combines a small public GPIO/SPI/I2C/Clock surface with lower-level modules for other peripherals and advanced register access.

## Architecture: Two Dedicated Toolchain Streams

The current SDK contains two toolchain configurations:

1. **Native Desktop / Hosted Systems Stream (`sdk/std/`)**:
   - Designed for hosted operating system environments.
   - Links against standard host environments, providing general computational math (`math.loom`).
   - Native compiler builds are configured by the project toolchain.

2. **Embedded Bare-Metal Stream (`sdk/targets/rp2040/`)**:
   - Configured for the RP2040 microcontroller target.
   - Includes custom Cortex-M0+ vector table and boot stages (`boot.s`), hardware SRAM/Flash linker maps (`linker.ld`), and direct MMIO peripheral drivers (`bsp/`).
   - Includes board startup, linker, and BSP sources. Hardware behavior requires validation on RP2040 boards.

## SDK Directory Structure

```
sdk/
├── std/
│   └── math.loom          # Standard mathematical routines and operations
└── targets/
    └── rp2040/
        ├── boot.s         # Stage-2 bootloader and vector table for RP2040
        ├── linker.ld      # Bare-metal memory map and sections for RP2040
        └── bsp/
            ├── rp2040.loom # Master umbrella module exposing all peripherals
            ├── system.loom # Core reset controller, SIO chip revision, and platform queries
            ├── clock.loom  # Beginner-facing Pico reference-board clock setup
            ├── clock_internal.loom # Low-level oscillator, PLL, mux, and clock routines
            ├── gpio.loom   # Checked digital GPIO input/output/read/write/toggle calls
            ├── gpio_internal.loom # Register-level GPIO operations used by BSP modules
            ├── time.loom   # 64-bit 1 MHz hardware timer, microsecond delays, and 4 alarms
            ├── uart.loom   # Dual hardware UART0/1 initialization, FIFO, and string I/O
            ├── spi.loom    # Concise 8-bit SPI0/1 setup and byte/slice calls
            ├── spi_internal.loom # Register-level SPI implementation
            ├── i2c.loom    # Concise I2C0/1 setup, buffer calls, and 8-bit register helpers
            ├── i2c_internal.loom # Register-level I2C implementation
            ├── adc.loom    # 12-bit ADC channels and onboard temperature sensor
            ├── pwm.loom    # 8-slice / 16-channel pulse width modulation
            ├── dma.loom    # 12-channel DMA engine, DREQ pacing, and hardware memcpy
            ├── watchdog.loom # Hardware watchdog countdown, scratch registers, and BOOTSEL reboot
            ├── multicore.loom# Dual-core CPUID, 32 atomic spinlocks, FIFO, and Core 1 launch
            ├── pio.loom    # Dual PIO0/1 programmable I/O blocks and 8 state machines
            ├── rtc.loom    # Hardware Real-Time Clock calendar date and time registers
            └── interp.loom # Dual SIO hardware interpolators, lane blending, and clamping
```

## Module Resolution Order

When resolving `Import <Module>` or `use <Module>`, Loom searches in this sequence:
1. Current file's directory and immediate subdirectories
2. Explicit `-I <dir>` include directories and their subdirectories (`bsp/`, etc.)
3. Project root and project `src/` directory
4. Target BSP SDK directories (`$LOOM_SDK/targets/rp2040/bsp/`)
5. Standard library SDK directories (`$LOOM_SDK/std/`)

## Hiding HAL Sources in User Projects

Because the Loom SDK ships with pre-verified BSP modules for supported hardware targets, user projects do not need to copy `bsp/` or `rp2040/` files into their workspace:
- Write `Import RP2040` or import an individual BSP module such as `GPIO`, `SPI`, or `I2C` in your source.
- The umbrella imports the peripheral modules for convenience. Call each module's `use` and initialize the peripheral before accessing hardware.
- The compiler (`loomc`) and driver (`loom`) automatically discover the HAL from `/usr/local/share/loom/sdk` or `$LOOM_SDK_PATH`.
- If you wish to override a default driver, placing your custom implementation in `src/bsp/<module>.loom` will automatically take precedence.

## Peripheral handles and simple application calls

`I2C_Init(SDA, SCL)`, `SPI_Init(SCK, MOSI, MISO)`, and `UART_Init(TX, RX)` select the RP2040 controller from valid physical pins and return typed handles. I²C defaults to 100 kHz; SPI defaults to 1 MHz, 8-bit words, mode 0. Keep each returned handle and pass it to a device-library initializer. The device library retains that handle for its later calls, so applications do not pass a bus number or repeat configuration values.

```loom
Module Board_App
Import RP2040
Import I2C
Import SSD1306
Import SSD1306_Font
use RP2040
use I2C
use SSD1306
use SSD1306_Font

Block Main
    Declares
        OLED_I2C : I2C_Handle
    Implements
        RP2040_Init()
        OLED_I2C = I2C_Init(4, 5)
        SSD1306_Init(OLED_I2C)
        SSD1306_Clear()
        SSD1306_Text(12, 24, "Hello Loom", True)
        SSD1306_Present()
End Main
```

The same ownership pattern applies to an MPU6050: create `Sensor_I2C : I2C_Handle`, assign `Sensor_I2C = I2C_Init(SDA, SCL)`, then call `MPU6050_Init(Sensor_I2C)`. Every transfer uses the handle it was initialized with, so initializing another controller does not silently redirect the library.

For a contiguous sensor register read, `I2C_Read_Register_Block(Handle, Address, Start_Register, Bytes)` keeps the register pointer write and byte read in one repeated-start transaction. The MPU6050 driver uses this for its 14-byte sample burst.

Initialization and transfer failures are handled inside the peripheral/library call. Execution stops at the failure and leaves a readable `*_Last_Error_Message` plus numeric `*_Last_Error_Code` for debugger inspection. A returned data value, such as `I2C_Read_Register`, is sensor data; applications do not use it as a success flag. Loom allows calls with unused return values to appear as statements, so a Boolean-valued status helper does not have to be assigned or checked. The RP2040 diagnostic globals currently remain available to a debugger; the SDK does not yet print them to a console.

The lower-level `*_Internal` modules implement register access. Beginner signatures intentionally omit controller numbers, baud rates, and SPI modes; advanced configuration is not part of these public APIs. UART and GPIO also use internal diagnostics for operations; ADC, PWM, DMA, and advanced modules retain the APIs documented on their reference pages.

Clock setup assumes the Raspberry Pi Pico reference board's 12 MHz crystal. Select pins from the valid mux combinations, match the physical wiring, and use suitable external pull-ups for I²C. Hardware acceptance still needs board-level checks: verify UART baud, SPI mode/clock, and I²C START/repeated-START/STOP and NACK behavior with the intended devices. Repository BSP tests compile Loom examples only; they do not execute MMIO or verify electrical timing.
