//! A guest that never finishes its tick -- the most common way untrusted code goes wrong. The sandbox
//! must stop each tick when its fuel runs out, mark the guest's outputs FAULTY, and quarantine it after
//! three failures in a row, while the node and every other actor carry on.
//!
//!   output 0  never published                       act/<name>/out

#![no_std]

#[path = "potluck_guest.rs"]
mod potluck;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

#[no_mangle]
pub extern "C" fn tick(now_ms: i32) {
    let mut x = now_ms;
    loop {
        x = core::hint::black_box(x.wrapping_mul(1103515245).wrapping_add(12345));
    }
}
