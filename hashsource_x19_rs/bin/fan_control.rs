//! X19 Fan Control Test
//!
//! Simple fan speed ramp test with proper initialization.
//! Gradually increases fan speed from 10% to 100% with 5% increments.

use hashsource_x19::{FpgaController, FpgaError};
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::Duration;

/// Global shutdown flag for signal handling
static SHUTDOWN: AtomicBool = AtomicBool::new(false);

/// Signal handler for graceful shutdown
extern "C" fn signal_handler(_sig: libc::c_int) {
    println!("\nReceived signal, shutting down...");
    SHUTDOWN.store(true, Ordering::SeqCst);
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("=== X19 Fan Speed Ramp Test ===\n");

    // Setup signal handlers
    unsafe {
        libc::signal(libc::SIGINT, signal_handler as libc::sighandler_t);
        libc::signal(libc::SIGTERM, signal_handler as libc::sighandler_t);
    }

    // Initialize FPGA controller
    let mut fpga = FpgaController::new().map_err(|e| {
        eprintln!("Error: {}", e);
        if let FpgaError::NotRoot = e {
            eprintln!("Hint: Run with sudo");
        }
        e
    })?;

    // Perform initialization sequence
    fpga.initialize()?;

    // Run fan speed ramp test
    println!("========================================");
    println!("Fan Speed Ramp Test");
    println!("========================================");
    println!("Ramping from 10% to 100% in 5% increments");
    println!("10 second hold at each speed");
    println!("Press Ctrl+C to stop\n");

    for speed in (10..=100).step_by(5) {
        if SHUTDOWN.load(Ordering::SeqCst) {
            break;
        }

        print!("Setting fan speed to {:3}%...", speed);
        std::io::Write::flush(&mut std::io::stdout())?;

        fpga.set_fan_speed(speed);

        let pwm_value = ((speed as u32) << 16) | (100 - speed as u32);
        println!(" (PWM: 0x{:08X})", pwm_value);

        // Hold for 10 seconds (with interrupt checking)
        for _ in 0..10 {
            if SHUTDOWN.load(Ordering::SeqCst) {
                break;
            }
            thread::sleep(Duration::from_secs(1));
        }
    }

    if !SHUTDOWN.load(Ordering::SeqCst) {
        println!("\n========================================");
        println!("Test Complete!");
        println!("========================================\n");
    }

    // Set to safe default (50%)
    println!("Setting fans to 50% before exit...");
    fpga.set_fan_speed(50);

    println!("Goodbye!");

    Ok(())
}
