//! HashSource X19 Hardware Control Library
//!
//! Shared library for FPGA, I2C, GPIO, and ASIC control on Bitmain Antminer X19 miners.

pub mod fpga;

pub use fpga::{FpgaController, FpgaError};
