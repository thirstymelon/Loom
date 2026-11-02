# Loom Software Development Kit (SDK)

The Loom SDK provides the standard library, platform targets, Board Support Packages (BSP), and hardware abstraction layers (HAL) for the Loom programming language.

## SDK Directory Structure

```
sdk/
├── std/
│   ├── math.loom          # Standard mathematical routines and operations
│   └── io.loom            # Standard I/O utilities
└── targets/
    └── rp2040/
        ├── boot.s         # Stage-2 bootloader and vector table for RP2040
        ├── linker.ld      # Bare-metal memory map and sections for RP2040
        └── bsp/
            ├── system.loom # Core clock configuration and peripheral reset control
            ├── gpio.loom   # GPIO pin configuration, pullups, and I/O
            ├── time.loom   # Microsecond/millisecond hardware timer and delay
            ├── uart.loom   # Hardware UART initialization, FIFO, and string I/O
            ├── adc.loom    # 12-bit ADC channels and onboard temperature sensor
            └── pwm.loom    # High-resolution pulse width modulation channels
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
- Simply write `Import System`, `Import GPIO`, etc., in your `src/main.loom`.
- The compiler (`loomc`) and driver (`loom`) automatically discover the HAL from `/usr/local/share/loom/sdk` or `$LOOM_SDK_PATH`.
- If you wish to override a default driver, placing your custom implementation in `src/bsp/<module>.loom` will automatically take precedence.

