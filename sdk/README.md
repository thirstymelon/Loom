# Loom Software Development Kit (SDK)

The Loom SDK provides the standard library, platform targets, Board Support Packages (BSP), and hardware abstraction layers (HAL) for the Loom programming language.

## Architecture: Two Dedicated Toolchain Streams

The Loom ecosystem cleanly separates into two specialized streams:

1. **Native Desktop / Hosted Systems Stream (`sdk/std/`)**:
   - Designed for operating system environments (macOS Apple Silicon/Intel, Linux, Windows).
   - Links against standard host environments, providing general computational math (`math.loom`).
   - Packaged and distributed via `Loom-1.1.0-macos-arm64-native.dmg`.

2. **Embedded Bare-Metal Stream (`sdk/targets/rp2040/`)**:
   - Designed for freestanding microcontrollers without an operating system (`no_std`).
   - Includes custom Cortex-M0+ vector table and boot stages (`boot.s`), hardware SRAM/Flash linker maps (`linker.ld`), and direct MMIO peripheral drivers (`bsp/`).
   - Provides deterministic hardware timing, zero-allocation runtime, and direct UF2 generation for drag-and-drop flashing.
   - Packaged and distributed via `Loom-1.1.0-macos-arm64-rp2040.dmg`.

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
            ├── clock.loom  # Full clock management: XOSC, ROSC, PLL_SYS/USB, glitchless muxes, FC0, and Resus
            ├── gpio.loom   # GPIO pin configuration, pullups, drive strength, and interrupts
            ├── time.loom   # 64-bit 1 MHz hardware timer, microsecond delays, and 4 alarms
            ├── uart.loom   # Dual hardware UART0/1 initialization, FIFO, and string I/O
            ├── spi.loom    # Dual SPI0/1 controller, Motorola modes 0-3, and slice transfers
            ├── i2c.loom    # Dual I2C0/1 controller, Standard/Fast mode, and register I/O
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
- Simply write `Import RP2040` (or individual modules like `Import GPIO`, `Import UART`) in your `src/main.loom`.
- `Import RP2040` transitively exports all peripheral drivers, allowing full access to all RP2040 hardware with a single import statement.
- The compiler (`loomc`) and driver (`loom`) automatically discover the HAL from `/usr/local/share/loom/sdk` or `$LOOM_SDK_PATH`.
- If you wish to override a default driver, placing your custom implementation in `src/bsp/<module>.loom` will automatically take precedence.

