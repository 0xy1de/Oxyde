// SPDX-License-Identifier: MPL-2.0
use std::ffi::OsString;

use calloop::EventLoop;
use wayland_server::{Display, backend::calloop::add_display};



fn main() {
    env_logger::init();
    println!("Oxyde compositor starting...");

    // 1) Create Wayland display
    let mut display = Display::new();

    // 2) Bind a listening socket (auto-pick name like "wayland-0")
    let socket_name: OsString = display
        .add_socket_auto()
        .expect("Failed to create Wayland listening socket");

    println!(
        "Wayland socket ready: WAYLAND_DISPLAY={}",
        socket_name.to_string_lossy()
    );

    // 3) Event loop (use smithay's reexport to avoid version skew)
    let mut event_loop: EventLoop<()> = EventLoop::try_new().expect("calloop init failed");
    let handle = event_loop.handle();

    // 4) Integrate display with the loop (dispatch/flush wiring)
    let _src = add_display(&mut display, &handle)
        .expect("add_display failed (Wayland display -> event loop)");

    println!("Oxyde compositor initialized. Waiting for clients...");

    // 5) Run loop; closure signature matches the calloop version smithay uses
    event_loop
        .run(None, &mut (), |_| {})
        .expect("event loop crashed");
}
