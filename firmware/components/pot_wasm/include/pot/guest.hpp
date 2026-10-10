// M7: guest actors -- a program from a party the cluster's owner does not trust, run as a workload of the
// cluster (ARCHITECTURE section 13, M7).
//
// A guest is a WebAssembly module carried in the deploy image's guest section (pot/deploy.hpp, version 2)
// with what the owner decided about it: its inputs and outputs (namespace paths, wired by the owner's
// manifest), how much fuel one tick may burn, how much memory it may have. Its author signs the module
// (a role-3 certificate from the cluster CA, pot_trust's guest_check); the owner signs the image. Each
// node verifies the author's signature before the module runs, and runs it only in the sandbox
// (pot/wasm_sandbox.hpp): it reaches the cluster through the guest API below and nothing else.
//
// A guest actor is portable (section 7.7): the reconciler places it, moves it when its node dies, and
// hands it its checkpoint (up to 128 bytes the guest saved) on the new node. Its code runs on a worker
// of its own (GuestServices::submit), never on the link task: the actor's tick only snapshots the
// inputs, hands the work over, and publishes what the last run produced.
//
// Faults are the guest's, never the node's. A trap -- out of fuel, out of bounds, out