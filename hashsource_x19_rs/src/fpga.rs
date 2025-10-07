//! FPGA Register Access and Control
//!
//! Provides low-level access to FPGA registers via `/dev/axi_fpga_dev`.
//! Handles memory mapping, initialization sequences, and PWM control.

use std::fs::OpenOptions;
use std::os::unix::io::AsRawFd;
use std::ptr::{self, NonNull};
use std::sync::atomic::{fence, Ordering};
use std::thread;
use std::time::Duration;

/// FPGA device path
pub const FPGA_DEVICE: &str = "/dev/axi_fpga_dev";

/// FPGA register space size (4608 bytes)
pub const FPGA_SIZE: usize = 0x1200;

/// Register offsets
pub mod regs {
    /// Control register (bit 30 = BM1391 init)
    pub const CTRL: usize = 0x000;

    /// Hash-on-plug detection
    pub const HASH_ON_PLUG: usize = 0x008;

    /// I2C controller
    pub const I2C_CTRL: usize = 0x030;

    /// Initialization control
    pub const INIT_CTRL: usize = 0x080;

    /// PWM main channel
    pub const PWM_MAIN: usize = 0x084;

    /// Initialization config
    pub const INIT_CFG: usize = 0x088;

    /// PWM alternate channel
    pub const PWM_ALT: usize = 0x0A0;
}

/// FPGA controller error types
#[derive(Debug)]
pub enum FpgaError {
    DeviceOpen(std::io::Error),
    Mmap(std::io::Error),
    InvalidOffset,
    NotRoot,
}

impl std::fmt::Display for FpgaError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::DeviceOpen(e) => write!(f, "Failed to open FPGA device: {}", e),
            Self::Mmap(e) => write!(f, "Failed to mmap FPGA registers: {}", e),
            Self::InvalidOffset => write!(f, "Invalid register offset (must be 4-byte aligned)"),
            Self::NotRoot => write!(f, "Must run as root to access FPGA device"),
        }
    }
}

impl std::error::Error for FpgaError {}

/// FPGA register controller
///
/// Provides safe access to FPGA registers with automatic cleanup.
pub struct FpgaController {
    /// Memory-mapped register pointer
    regs: NonNull<u32>,
    /// File descriptor (kept alive for munmap)
    _fd: std::fs::File,
}

impl FpgaController {
    /// Open and memory-map FPGA device
    ///
    /// # Errors
    /// Returns error if device cannot be opened or mapped.
    pub fn new() -> Result<Self, FpgaError> {
        Self::with_device(FPGA_DEVICE)
    }

    /// Open FPGA device at custom path (for testing)
    pub fn with_device(device: &str) -> Result<Self, FpgaError> {
        // Check for root permissions
        if unsafe { libc::geteuid() } != 0 {
            return Err(FpgaError::NotRoot);
        }

        println!("Opening {}...", device);

        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(device)
            .map_err(FpgaError::DeviceOpen)?;

        let fd = file.as_raw_fd();

        // Memory-map the FPGA register space
        let ptr = unsafe {
            libc::mmap(
                ptr::null_mut(),
                FPGA_SIZE,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd,
                0,
            )
        };

        if ptr == libc::MAP_FAILED {
            return Err(FpgaError::Mmap(std::io::Error::last_os_error()));
        }

        let regs = NonNull::new(ptr as *mut u32)
            .ok_or_else(|| FpgaError::Mmap(std::io::Error::last_os_error()))?;

        println!("FPGA registers mapped at {:p}\n", regs.as_ptr());

        Ok(Self { regs, _fd: file })
    }

    /// Read register value at byte offset
    ///
    /// # Panics
    /// Panics if offset is not 4-byte aligned or out of bounds.
    #[inline]
    pub fn read_reg(&self, offset: usize) -> u32 {
        assert!(offset % 4 == 0, "Register offset must be 4-byte aligned");
        assert!(offset < FPGA_SIZE, "Register offset out of bounds");

        unsafe { ptr::read_volatile(self.regs.as_ptr().add(offset / 4)) }
    }

    /// Write register value at byte offset with memory barrier
    ///
    /// # Panics
    /// Panics if offset is not 4-byte aligned or out of bounds.
    #[inline]
    pub fn write_reg(&mut self, offset: usize, value: u32) {
        assert!(offset % 4 == 0, "Register offset must be 4-byte aligned");
        assert!(offset < FPGA_SIZE, "Register offset out of bounds");

        unsafe {
            ptr::write_volatile(self.regs.as_ptr().add(offset / 4), value);
            // Memory barrier for ARM-FPGA coherency
            fence(Ordering::SeqCst);
        }
    }

    /// Perform FPGA initialization sequence
    ///
    /// This is the stock firmware initialization sequence reverse-engineered
    /// from bmminer. Must be called before fan/PSU control will work.
    pub fn initialize(&mut self) -> Result<(), FpgaError> {
        println!("========================================");
        println!("FPGA Initialization Sequence");
        println!("========================================\n");

        println!("Current register state:");
        println!("  0x000 = 0x{:08X}", self.read_reg(regs::CTRL));
        println!("  0x080 = 0x{:08X}", self.read_reg(regs::INIT_CTRL));
        println!("  0x088 = 0x{:08X}\n", self.read_reg(regs::INIT_CFG));

        // Stage 1: Boot-time initialization
        println!("Stage 1: Boot-time initialization");

        // Set bit 30 in register 0 (BM1391 init)
        let reg0 = self.read_reg(regs::CTRL);
        if reg0 & 0x4000_0000 == 0 {
            self.write_reg(regs::CTRL, reg0 | 0x4000_0000);
            thread::sleep(Duration::from_millis(100));
            println!(
                "  Set 0x000 = 0x{:08X} (bit 30 set)",
                self.read_reg(regs::CTRL)
            );
        } else {
            println!("  0x000 = 0x{:08X} (already correct)", reg0);
        }

        self.write_reg(regs::INIT_CTRL, 0x0080_800F);
        thread::sleep(Duration::from_millis(100));
        println!("  Set 0x080 = 0x{:08X}", self.read_reg(regs::INIT_CTRL));

        self.write_reg(regs::INIT_CFG, 0x8000_01C1);
        thread::sleep(Duration::from_millis(100));
        println!("  Set 0x088 = 0x{:08X}\n", self.read_reg(regs::INIT_CFG));

        // Stage 2: Bmminer startup sequence
        println!("Stage 2: Bmminer startup sequence");

        self.write_reg(regs::INIT_CTRL, 0x8080_800F);
        thread::sleep(Duration::from_millis(50));
        println!(
            "  Set 0x080 = 0x{:08X} (bit 31 set)",
            self.read_reg(regs::INIT_CTRL)
        );

        self.write_reg(regs::INIT_CFG, 0x0000_9C40);
        thread::sleep(Duration::from_millis(50));
        println!("  Set 0x088 = 0x{:08X}", self.read_reg(regs::INIT_CFG));

        self.write_reg(regs::INIT_CTRL, 0x0080_800F);
        thread::sleep(Duration::from_millis(50));
        println!(
            "  Set 0x080 = 0x{:08X} (bit 31 clear)",
            self.read_reg(regs::INIT_CTRL)
        );

        self.write_reg(regs::INIT_CFG, 0x8001_FFFF);
        thread::sleep(Duration::from_millis(100));
        println!(
            "  Set 0x088 = 0x{:08X} (final config)\n",
            self.read_reg(regs::INIT_CFG)
        );

        println!("Initialization complete!\n");

        Ok(())
    }

    /// Set fan speed (0-100%)
    ///
    /// Uses stock firmware PWM format: `(percent << 16) | (100 - percent)`
    pub fn set_fan_speed(&mut self, percent: u8) {
        let percent = percent.min(100) as u32;

        // Stock firmware format
        let pwm_value = (percent << 16) | (100 - percent);

        self.write_reg(regs::PWM_MAIN, pwm_value);
        self.write_reg(regs::PWM_ALT, pwm_value);
    }
}

impl Drop for FpgaController {
    fn drop(&mut self) {
        unsafe {
            libc::munmap(self.regs.as_ptr() as *mut _, FPGA_SIZE);
        }
    }
}

// Make FpgaController thread-safe (raw pointers are !Send by default)
unsafe impl Send for FpgaController {}
unsafe impl Sync for FpgaController {}

#[cfg(test)]
mod tests {
    #[test]
    fn test_pwm_format() {
        // Verify PWM format matches C implementation
        let percent = 50u32;
        let pwm = (percent << 16) | (100 - percent);
        assert_eq!(pwm, 0x0032_0032);
    }
}
