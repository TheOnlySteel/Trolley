# Breathe

A polished coherent-breathing timer for the M5Stack Core2 (5.5 s inhale / 5.5 s exhale).

## Features

- **Home screen** — session length chips (1 / 3 / 5 / 10 min or Open-ended), a play button,
  corner icon toggles for Sound and Haptics (settings persist across power-off via NVS),
  a power-off button top-left, battery readout top-right.
- **Flicker-free rendering** — full-screen off-screen canvas pushed atomically each frame
  (~30 fps, limited by the panel's 40 MHz SPI bus).
- **3-2-1 countdown** to settle in before the first breath (tap to cancel).
- **Session screen** — numberless and minimal: a glowing layered orb with eased grow/shrink
  motion, color crossfade between inhale (teal) and exhale (periwinkle), and a ring that
  fills from the bottom up as you inhale, then splits at the top and drains down as you
  exhale. Only a small dim time readout sits in the corner.
- **Eyes-closed friendly** — soft directional tones (higher = inhale, lower = exhale) and a
  short haptic tap at every phase change; both non-blocking, both mutable.
- **Pause anywhere** — tap the screen (or any touch button) for a Resume / End panel.
- **Graceful endings** — timed sessions finish at the end of a full breath, never mid-exhale,
  then show a summary (breaths + duration) with a gentle three-note chime.
- Flicker-free full-screen canvas rendering at ~30 fps; screen dims during sessions.

## Build

1. Arduino IDE with the ESP32 board package and the **M5Unified** library (Library Manager).
2. Board: **M5Core2**. Open `Breathe.ino`, upload.
