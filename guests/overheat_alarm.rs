//! M7's example system: a vendor's logic on someone else's cluster.
//!
//! "Acme" sells an overheat alarm. The cluster's owner runs it without reading its code: it gets one
//! input (a temperature, wired by the owner's manifest) and three outputs, and nothing else. It smooths
//! the temperature, raises an alarm with hysteresis, and counts its ticks -- and keeps its state in a
//! checkpoint, so a failover to another board continues where it left off instead of starting cold.
//!
//!   input 0   a temperature, degC
//!   output 0  the smoothed temperature (f32)          act/<name>/out
//!   output 1  1 while overheated, else 0 (i32)        act/<name>/alarm
//!   output 2  ticks since first deployed (i32)       act/<name>/count -- survives a failover

#![no_std]

#[path = "potluck_guest.rs"]
mod potluck;

const ON_ABOVE: f32 = 38.0;
const OFF_BELOW: f32 = 37.0;
const ALPHA: f32 = 0.2;

static mut SMOOTHED: f32 = f32::NAN;
static mut ALARM: i32 = 0;
static mut COUNT: i32 = 0;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

#[no_mangle]
pub extern "C" fn init() {
    let mut buf = [0u8; 12];
    if potluck::restore(&mut buf) == Some(12) {
        unsafe {
            SMOOTHED = f32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]);
            ALARM = i32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]);
            COUNT = i32::from_le_bytes([buf[8], buf[9], buf[10], buf[11]]);
        }
        potluck::log(1); // restored
    }
}

#[no_mangle]
pub extern "C" fn tick(_now_ms: i32) {
    unsafe {
        COUNT += 1;
        if let Some(t) = potluck::input_f32(0) {
            SMOOTHED = if SMOOTHED.is_nan() { t } else { SMOOTHED + ALPHA * (t - SMOOTHED) };
            if ALARM == 0 && SMOOTHED > ON_ABOVE {
                ALARM = 1;
            } else if ALARM == 1 && SMOOTHED < OFF_BELOW {
                ALARM = 0;
            }
        }
        if !SMOOTHED.is_nan() {
            potluck::publish_f32(0, SMOOTHED);
        }
        potluck::publish_i32(1, ALARM);
        potluck::publish_i32(2, COUNT);
        let mut buf = [0u8; 12];
        buf[0..4].copy_from_slice(&SMOOTHED.to_le_bytes());
        buf[4..8].copy_from_slice(&ALARM.to_le_bytes());
        buf[8..12].copy_from_slice(&COUNT.to_le_bytes());
        potluck::save(&buf);
    }
}
