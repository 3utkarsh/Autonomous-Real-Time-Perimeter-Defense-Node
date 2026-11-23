# Autonomous Real-Time Perimeter Defense Node (ESP32-S3)

An active optical security system built on the **ESP32-S3** using **FreeRTOS Symmetric Multiprocessing (SMP)**. The node projects a focused laser tripwire across a boundary, adapts continuously to ambient room lighting, triggers a hardware-modulated acoustic siren upon intrusion, and hosts a live local Wi-Fi dashboard for remote monitoring and control.

---

## Core Architecture

To prevent network stack overhead from delaying physical intrusion detection, execution is partitioned across both Xtensa LX7 cores:

* **Core 0 (Real-Time Optical Engine):** Runs a pinned FreeRTOS task at a deterministic **1 kHz** ($1\text{ ms}$) interval via `vTaskDelayUntil`. Acquires 12-bit ADC readings from the optical front-end and executes an integer leaky-integrator baseline filter (`>> 7` bit-shift arithmetic) to absorb gradual ambient light shifts without false triggers.
* **Core 1 (Supervisor & Network Telemetry):** Serves an embedded HTTP Web Dashboard and JSON status endpoint (`/api/status`, `/api/reset`) while driving an alternating two-tone siren ($800\text{ Hz} \leftrightarrow 2200\text{ Hz}$) directly through the hardware **LEDC PWM** timer without blocking CPU cycles.

---

## Hardware Bill of Materials & Pinout

| Module | Component | ESP32-S3 Pin | Function |
| :--- | :--- | :--- | :--- |
| **Optical Emitter** | HW-493 Laser Diode | `GPIO 6` | Continuous beam projection |
| **Optical Receiver** | HW-486 (KY-018) LDR | `GPIO 4` (ADC1) | 12-bit analog light acquisition |
| **Audio Deterrent** | HW-512 Passive Buzzer | `GPIO 5` (LEDC) | Non-blocking hardware PWM siren |
| **Power Rails** | Common Bus | `3V3` / `GND` | Shared logic power and ground |

---

## Key Features

* **Sub-Millisecond Breach Detection:** Continuous $1\text{ kHz}$ sampling catches rapid beam interruptions immediately.
* **Adaptive Baseline Tracking:** Dynamically adjusts to sunlight and room illumination changes while tripping on sudden optical deltas.
* **Zero-Blocking Audio Synthesis:** Hardware LEDC timer channels modulate siren frequencies independently of the main loop.
* **Embedded Web Dashboard:** Accessible over local Wi-Fi or standalone SoftAP (`192.168.4.1`) for real-time ADC telemetry, breach counting, and remote disarm/reset.

---

## Setup & Usage

1. Wire the HW-493 laser, HW-486 LDR, and HW-512 buzzer according to the pinout table above.
2. Open the firmware in **Arduino IDE** or **PlatformIO** targeting the **ESP32-S3 Dev Module**.
3. Configure your Wi-Fi credentials (or run in standalone SoftAP mode).
4. Upload the sketch, aim the laser onto the LDR window, and open the printed IP address in any mobile or desktop browser.
