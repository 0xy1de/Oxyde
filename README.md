# 🌌 Oxyde — A Modern Frutiger Aero Desktop Environment in Rust

[![License: MPL-2.0](https://img.shields.io/badge/license-MPL--2.0-blue.svg)](LICENSE)
[![REUSE status](https://api.reuse.software/badge/github.com/0xy1de/oxyde)](https://api.reuse.software/info/github.com/0xy1de/oxyde)
[![Build Status](https://img.shields.io/github/actions/workflow/status/<your-username>/oxyde/ci.yml?branch=main)](https://github.com/<your-username>/oxyde/actions)

Oxyde is a **next-generation desktop environment** written in Rust.  
It blends the **ergonomics of macOS Aqua** with the **translucency and depth of Frutiger Aero**,  
all while staying **lightweight, safe, and optimized for workflow**.

---

<p align="center">
  <img src="/assets/icons/logo.png" alt="Oxyde Logo" width="200"/>
</p>

---

# Project Mockup:
<p>
  <img src="/assets/oxyde_reender.png" alt="Oxyde Render Mockup"/>
</p>

---

## ✨ Features (planned)

- 🖥 **Wayland compositor** powered by [smithay](https://github.com/Smithay/smithay)  
- 📑 **Global top menu bar** (Aqua-style, Fitts’s Law optimized)  
- 📂 **Dock** with optional magnification, pinned apps, and running indicators  
- 🔲 **Traffic-light window controls** (close / min / max)  
- 🗂 **Workspaces & Exposé** — live window overview and smooth workspace switching  
- 🔍 **OxySpot Launcher** — fast search for apps, files, and actions  
- ⌨️ **Global shortcuts & hot corners**  
- 🔔 **Notification center & preferences hub**  
- 🎨 **Modern Frutiger Aero theming** — glass blur, neon accents, shadows, and springy animations  

---

## 📦 Project Structure

````text
oxyde/
  crates/
    compositor/   # smithay-based WM/compositor
    panel/        # global menu bar
    dock/         # dock
    expose/       # overview/workspaces
    launcher/     # spotlight-style search
    oxykeys/      # global shortcuts
    oxymenu/      # app menu API
    oxynotify/    # notifications
    oxyconfig/    # preferences backend
    theming/      # aero-glass, blur, glow
  assets/
    icons/
    wallpapers/
    fonts/
  docs/
  LICENSE
````
---

## 🚀 Getting Started

Prerequisites

  Rust (latest stable)

  Wayland session (X11 not supported)

  Dependencies: libinput, libudev, pkg-config


## Build & Run

```bash
git clone https://github.com/<your-username>/oxyde.git
```
```bash
cd oxyde
```
```bash
cargo run --bin compositor
```


---

## 🛠 Development Workflow

Format code:

``` cargo fmt ```

Lint:

``` cargo clippy ```

Run tests:

 ``` cargo test ```



---

## 🤝 Contributing

We welcome contributions! Please see CONTRIBUTING.md for details.
All contributions must follow the MPL-2.0 license terms.


---

## 📜 License

Source code: MPL-2.0

Documentation: CC-BY-4.0

Icons & artwork: CC-BY-SA-4.0

Fonts: OFL-1.1



---

## 🌟 Vision

Oxyde is designed to be:

Beautiful — polished visuals inspired by Aqua & Aero, modernized with Rust.

Ergonomic — minimal friction, top-left menus, fast global search, sensible defaults.

Lightweight — optimized compositor, efficient GPU pipelines, low memory overhead.

Open — community-driven, transparent, and extensible.



---
