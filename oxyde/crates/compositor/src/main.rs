fn main() {
    println!("Oxyde compositor starting...");

    oxyde_panel::init();
    oxyde_theming::init();
    oxyde_oxynotify::init();
    oxyde_oxymenu::init();
    oxyde_oxykeys::init();
    oxyde_oxyconfig::init();
    oxyde_launcher::init();
    oxyde_expose::init();
    oxyde_dock::init();
}