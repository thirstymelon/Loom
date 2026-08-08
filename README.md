# ⌬ Loom

*A lightweight cooperative task scheduler for Ada.*

Loom is an embedded task scheduling framework for Ada that provides a simple, deterministic alternative to traditional RTOS task management. Inspired by the programming model of FreeRTOS tasks while embracing Ada's strengths, Loom allows applications to organize work into independent tasks without requiring a full real-time operating system.

The project focuses on portability, static memory allocation, and predictable execution, making it suitable for resource-constrained microcontrollers and safety-oriented embedded systems.

> **Project Status:** 🚧 Early development (v0.1.0)

---

# ⌬ Vision

Provide a modern, lightweight task scheduling framework for Ada that enables embedded applications to be written as independent, reusable tasks while remaining deterministic and portable.

---

# ⌬ Mission

Build a cooperative scheduler that feels familiar to developers coming from FreeRTOS, while leveraging Ada's strong typing, package system, and safety features.

---

# ⌬ Goals

- Lightweight task scheduler
- Cooperative multitasking
- Deterministic execution
- Static memory allocation only
- Zero dynamic allocation
- Portable across embedded platforms
- Hardware-independent core
- Simple and expressive Ada API
- Suitable for bare-metal systems

---

# ⌬ Non-Goals

Loom is **not** intended to:

- Replace a full RTOS
- Provide preemptive scheduling (initially)
- Manage device drivers
- Implement networking stacks
- Handle graphics or UI
- Abstract hardware peripherals

Those responsibilities belong to dedicated libraries built on top of Loom.

---

# ⌬ Planned Features

- Cooperative scheduler
- Periodic tasks
- One-shot tasks
- Software timers
- Delays
- Event notifications
- Message queues
- Synchronization primitives
- Static task creation
- Priority-based scheduling
- Idle task
- Tick source abstraction

---

# ⌬ Design Principles

- Static allocation only
- No hidden heap usage
- Predictable execution
- Small API surface
- Platform-independent scheduler
- Clear separation between scheduler and hardware
- Zero-cost abstractions wherever possible

---

# ⌬ License

Apache License 2.0
