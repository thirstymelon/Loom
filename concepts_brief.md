# Loom-OS: Formally Verified, Zero-Heap Hardware-Synthesized Microkernel on RP2040

* **Author:** Lokesh
* **Date:** 2026-11-25

## Overview

Existing embedded operating systems for microcontrollers (FreeRTOS, Zephyr, Embassy, RT-Thread) are ported from general-purpose desktop operating system paradigms: they treat the MCU as a weak generic CPU with a flat memory map. On the RP2040, this leads to CPU starvation, frame tearing, silent stack overflows, and catastrophic system freezes when an unprivileged task crashes.

**Loom-OS** is an ultra-minimal, hardware-synthesized microkernel authored in Loom. It delegates responsibilities across the RP2040's physical silicon:
1. **PIO State Machines**: Act as dedicated hardware display coprocessors (synthesizing cycle-exact DVI/HDMI or high-speed parallel video scanlines directly from SRAM with 0% CPU overhead).
2. **Core 0**: Acts as the Interactive Host & Shell Manager (handling USB CDC interactive console, preemptive multitasking, and MPU process isolation).
3. **Core 1**: Acts as the Dedicated Real-Time Compute Engine (rendering 2D/3D graphical widgets or running physics/signal processing via SIO hardware interpolators).
4. **DMA Crossbar**: Streams data between memory banks and display hardware with zero CPU intervention.
5. **Loom Compiler Guarantees**: Enforces compile-time DMA buffer locks (`error[E3005]`), static stack depth limits (`error[E4001]`), and deadlock-free spinlock acquisition DAGs (`error[E2005]`).

---

## Technical Architecture & Silicon Partitioning

```
+-------------------------------------------------------------------------+
|                              Loom-OS Kernel                             |
+-------------------------------------------------------------------------+
| Core 0: Interactive Host OS          | Core 1: Compute & Rendering Engine |
| - Preemptive Task Scheduler          | - SIO Interpolator 2D/3D Rasterizer|
| - USB CDC VT100 Interactive Shell    | - Drift-free Periodic Control Loop |
| - MPU Task Sandboxing & Recovery     | - Hardware Math Accelerators       |
+-------------------------------------------------------------------------+
                                    || (Sync.Channel & Spinlock DAG)
+-------------------------------------------------------------------------+
|                  AHB 4-Bank Crossbar Memory Partitioning                |
| - Bank 0: Core 0 OS Kernel & Task Stacks (Privileged)                   |
| - Bank 1: Core 1 Compute Workspace & Frame Cache                        |
| - Bank 2 & 3: Double-Buffered Video Scanlines (DMA Protected)           |
+-------------------------------------------------------------------------+
                                    || (Zero-Copy DMA Chaining)
+-------------------------------------------------------------------------+
|                         PIO Display Engine (Hardware)                   |
| - PIO0 SM0-SM2: 10-bit TMDS / DVI Serializer (Direct to HDMI/VGA)       |
| - 640x480 60Hz or 320x240 RGB332 / RGB565 Generation (0% CPU Load)      |
+-------------------------------------------------------------------------+
```

---

## Why C, C++, and Rust Cannot Match This in Practice

| Engineering Challenge | C / C++ (FreeRTOS / Bare-Metal) | Rust (Embassy / RTIC) | Loom (Loom-OS) |
| :--- | :--- | :--- | :--- |
| **Frame Tearing Prevention** | Requires manual mutexes/flags; developers frequently mutate scanline buffers during active DMA, causing visual artifacts. | Requires complex `'static` lifetime wrappers or `Pin<&mut [u8]>`, which complicates buffer recycling. | **Compile-Time Typestate Lock**: Accessing a buffer during in-flight DMA triggers `error[E3005]` at compile time. Zero runtime overhead. |
| **Stack Overflow Prevention** | Unchecked at compile time; an overflowing task silently corrupts adjacent stacks or kernel tables. | Relies on runtime MPU guard pages, which crash the entire MCU and consume limited MPU regions. | **Static WCSD Analysis**: Call-graph DAG computes exact frame requirements; exceeding `Stack_Limit` triggers `error[E4001]` during compilation. |
| **Dual-Core Deadlock Safety** | Spinlock deadlocks between Core 0 and Core 1 require runtime debugging with a logic analyzer. | Rust's borrow checker does not track global multi-lock acquisition ordering across threads. | **DAG Deadlock Verification**: Acquisition order of the 32 SIO spinlocks is verified at compile time; cycles trigger `error[E2005]`. |
| **PIO Protocol Integration** | Requires external Python tools (`pioasm`) emitting C header arrays; disconnected from compiler type-checking. | Uses external `pioasm` blobs or complex macros with limited compiler checking. | **First-Class In-Language DSL**: `Pio_Program` compiles directly into `.rodata` with typed labels and side-set safety. |
| **Fault Isolation on Cortex-M0+** | Any crash (null pointer, bus error) triggers an unrecoverable `HardFault_Handler` resetting the chip. | Panic handler resets the core or hangs in a loop (`loop {}`). | **MPU Sandboxed Recovery**: Cortex-M0+ 8-region MPU isolates user tasks; offending tasks are cleanly terminated and restarted while display and Core 1 continue running. |

---

## Detailed System Components

### 1. The PIO Display Engine
- **Hardware Target**: Direct connection to GPIO pins via passive resistor ladder (VGA RGB332) or direct HDMI/DVI connector (TMDS differential pairs).
- **Zero-RAM "Racing the Beam"**:
  - Instead of requiring a 150–600 KB full-frame buffer (which exceeds RP2040's 264 KB SRAM), Loom-OS renders on-the-fly into twin 320-pixel or 640-pixel scanline buffers (~1.2 KB total).
  - While PIO outputs Scanline $N$ via DMA, Core 1 synthesizes Scanline $N+1$ using SIO interpolators.
  - The Loom compiler guarantees that Core 1 cannot touch Scanline $N$ until DMA releases it.

### 2. The Interactive Microkernel Shell (Core 0)
- Exposes a clean VT100 terminal over USB CDC with zero dynamic heap allocation.
- Built-in commands:
  - `ps`: Displays active tasks, priority, core affinity, and static WCSD stack utilization.
  - `mem`: Shows AHB crossbar bank allocation and MPU region status.
  - `spawn <app>`: Spawns sandboxed user tasks dynamically.
  - `kill <id>`: Terminates tasks cleanly.
  - `crash-dump`: Dumps forensic blackbox logs from Flash Sector 511.

### 3. The Real-Time Compute & Graphics Engine (Core 1)
- Uses Loom's block-level periodic loop:
  ```loom
  Block Frame_Compositor Periodic 16666 Max_Jitter 100 # 60 Hz display refresh
  ```
- Evaluates UI widgets, window compositing, or procedural graphics (bouncing balls, cellular automata, or biological wave simulations) using SIO hardware interpolators for single-cycle alpha blending and coordinate transforms.

### 4. MPU Task Sandboxing & Forensic Recovery
- User applications run in unprivileged thread mode restricted to MPU Region 2 (stack) and Region 3 (local RAM).
- If a user application attempts an illegal memory access or dereferences an invalid pointer:
  1. The MPU traps into `HardFault_Handler`.
  2. The Loom kernel captures fault state (`PC`, `LR`, `Task_ID`) and writes a forensic record to Flash Sector 511.
  3. The kernel cleanly kills the offending process, releases its resources, prints an error message to the shell, and resumes the system without dropping a single video frame.

---

## Goals vs. Non-Goals

* **In-Scope Goals:**
  - Build a fully operational, bootable microkernel on bare RP2040 silicon.
  - Real-time video display generation (VGA or DVI) via PIO + DMA with 0% CPU display load.
  - Interactive VT100 shell over USB CDC.
  - MPU-sandboxed user task execution with isolated fault recovery.
  - 100% formal verification via Loom compiler (WCSD stack proof, DMA lock proof, spinlock DAG deadlock elimination).
* **Out-of-Scope Non-Goals:**
  - POSIX compliance or multi-gigabyte filesystem drivers.
  - Third-party binary ELF loading (apps are authored in Loom and linked with the microkernel).

---

## Alternatives Weighed

* **FreeRTOS + LittlevGL (LVGL) in C**: Rejected. Heavy dynamic heap allocations, frequent memory fragmentation, zero compile-time DMA buffer protection, and unrecoverable HardFaults on M0+.
* **Embassy Rust on RP2040**: Rejected. Does not provide compile-time WCSD stack limits, lacks first-class PIO assembly syntax, and cannot statically prove absence of spinlock deadlocks across the dual cores.

---

## Key Decisions & Staging for Next Steps

* Clarify display interface preference:
  1. **Direct VGA Output** (8 GPIO pins with passive resistor ladder for RGB332 + HSync/VSync).
  2. **Direct DVI / HDMI Output** (8 GPIO pins with 4 differential TMDS pairs connected directly to an HDMI monitor).
  3. **High-Speed Color SPI Display** (ST7789 or ILI9341 320x240 LCD module).
