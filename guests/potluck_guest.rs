//! The guest API: what a Potluck guest -- a WebAssembly program from a party the cluster's owner does
//! not trust (M7) -- can call, and nothing else. Include it in a guest with
//!
//!     #[path = "potluck_guest.rs"] mod potluck;
//!
//! and export `tick(now_ms: i32)` (required, called every period) and `init()` (optional, once per
//! activation, after a checkpoint has been restored if there is one).
//!
//! The guest sees its inputs and outputs by index, in the order the deploy manifest lists them: the
//! cluster's owner wires them, the guest only computes. It cannot name a pin, the radio, another
//! node, or any address outside its own memory. Every loop iteration and call costs fuel; past the
//! owner's budget for one tick the guest is stopped, and after three such failures in a row it is
//! quarantined for the rest of the deployment.

#![allow(dead_code)]

#[link(wasm_import_module = "potluck")]
extern "C" {
    #[link_name = "input_f32"]
    fn sys_input_f32(index: i32) -> f32;
    #[link_name = "input_i32"]
    fn sys_input_i32(index: i32) -> i32;
    #[link_name = "quality"]
    fn sys_quality(index: i32) -> i32;
    #[link_name = "publish_f32"]
    fn sys_publish_f32(index: i32, value: f32);
    #[link_name = "publish_i32"]
    fn sys_publish_i32(index: i32, value: i32);
    #[link_name = "save"]
    fn sys_save(data: *const u8, len: i32) -> i32;
    #[link_name = "restore"]
    fn sys_restore(data: *mut u8, cap: i32) -> i32;
    #[link_name = "log"]
    fn sys_log(value: i32);
    #[link_name = "now_ms"]
    fn sys_now_ms() -> i32;
}

/// An input's quality, as the cluster reports it (section 4's tuple): only `Good` and `Stale` carry
/// a value.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Quality {
    Good,
    Stale,
    Unavailable,
    NoData,
    Faulty,
    Other,
}

pub fn quality(index: i32) -> Quality {
    match unsafe { sys_quality(index) } {
        0 => Quality::Good,
        1 => Quality::Stale,
        2 => Quality::Unavailable,
        3 => Quality::NoData,
        4 => Quality::Faulty,
        _ => Quality::Other,
    }
}

/// The input's value as an f32, or None when it carries none (or is not a number).
pub fn input_f32(index: i32) -> Option<f32> {
    match quality(index) {
        Quality::Good | Quality::Stale => {
            let v = unsafe { sys_input_f32(index) };
            if v.is_nan() { None } else { Some(v) }
        }
        _ => None,
    }
}

pub fn input_i32(index: i32) -> Option<i32> {
    match quality(index) {
        Quality::Good | Quality::Stale => Some(unsafe { sys_input_i32(index) }),
        _ => None,
    }
}

pub fn publish_f32(index: i32, value: f32) {
    unsafe { sys_publish_f32(index, value) }
}

pub fn publish_i32(index: i32, value: i32) {
    unsafe { sys_publish_i32(index, value) }
}

/// Keep up to 128 bytes for the next activation -- here after a reboot, or on another node after a
/// failover. True if kept.
pub fn save(data: &[u8]) -> bool {
    unsafe { sys_save(data.as_ptr(), data.len() as i32) == 0 }
}

/// The last saved bytes, if any; returns how many were copied.
pub fn restore(buf: &mut [u8]) -> Option<usize> {
    let n = unsafe { sys_restore(buf.as_mut_ptr(), buf.len() as i32) };
    if n < 0 { None } else { Some(n as usize) }
}

pub fn log(value: i32) {
    unsafe { sys_log(value) }
}

pub fn now_ms() -> i32 {
    unsafe { sys_now_ms() }
}
