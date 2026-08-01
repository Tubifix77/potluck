// Potluck Frame opcodes — ARCHITECTURE.md §5.2.
//
// M0 implements the five membership/error opcodes only, per M0-BRIEF.md. The rest of §5.2 is
// listed here as commented-out constants so that the numbering stays visible and nobody
// re-allocates a value: the opcode space is wire format, and the wire format is the product.
// Uncomment a line when the milestone that owns it arrives — do not renumber.

#pragma once

#include <cstdint>

namespace pot {

// ---- Membership (M0) ----
constexpr uint8_t kOpHello = 0x01;      // node announce: id, capabilities, epoch, pubkey fp
constexpr uint8_t kOpHelloAck = 0x02;   // admission decision
constexpr uint8_t kOpHeartbeat = 0x03;  // liveness + link stats (§8)
constexpr uint8_t kOpBye = 0x04;        // intentional departure

// ---- Namespace (M1) ----
constexpr uint8_t kOpRead = 0x10;   // path → typed value
constexpr uint8_t kOpWrite = 0x11;  // path ← typed value
// constexpr uint8_t kOpSubscribe   = 0x12;
// constexpr uint8_t kOpPublish     = 0x13;
// constexpr uint8_t kOpUnsubscribe = 0x14;
// constexpr uint8_t kOpList        = 0x15;

// ---- Actors ----
// constexpr uint8_t kOpCall  = 0x20;

// The generic response. §5.2 lists it under Actors because that is where it is first needed, but
// its definition there — "response, carries request msg_id" — is not actor-specific, and §5.1 makes
// msg_id the correlation id for any request. So READ and WRITE are answered with REPLY rather than
// with two more opcodes: the opcode space is wire format, and spending two values on something an
// existing one already means would be waste that outlives the decision.
constexpr uint8_t kOpReply = 0x21;
// constexpr uint8_t kOpCast  = 0x22;

// ---- Lifecycle (M3 / M6) ----
// constexpr uint8_t kOpDeployBegin    = 0x30;
// constexpr uint8_t kOpDeployChunk    = 0x31;
// constexpr uint8_t kOpDeployCommit   = 0x32;
// constexpr uint8_t kOpDeployAbort    = 0x33;
// constexpr uint8_t kOpMigratePrepare = 0x40;
// constexpr uint8_t kOpMigrateCommit  = 0x41;

// ---- Safety (M4) ----
// Not implemented at M0, but the value is needed now: §5.4 forbids fragmenting SAFE_STATE, and
// the codec enforces that rule for every opcode it names. The rest of §5.2's SAFE_STATE
// constraints (broadcast, priority 31, ACKREQ always off) are not checked until M4 ships the
// opcode — see M0-LOG.md.
constexpr uint8_t kOpSafeState = 0x50;

// ---- Errors (M0) ----
constexpr uint8_t kOpErr = 0x7F;  // code + optional detail; never silently dropped

// Human-readable opcode name, or "?" for anything not listed above. Used by the serial JSON and
// by potluck-capture; keeping it next to the numbers is how the two stay in step.
const char* opcode_str(uint8_t opcode);

// ---------------------------------------------------------------------------------------------
// ERR codes — the payload of kOpErr. §5.2 fixes the opcode, not the code space, so this is M0's
// allocation. Codes 0x0001..0x00FF mirror pot::FrameError so that a rejected frame can be
// reported without a translation table.
// ---------------------------------------------------------------------------------------------

constexpr uint16_t kErrNone = 0x0000;
constexpr uint16_t kErrFrameBase = 0x0000;  // + FrameError value
constexpr uint16_t kErrUnknownOpcode = 0x0100;
constexpr uint16_t kErrNotAdmitted = 0x0101;
constexpr uint16_t kErrPeerTableFull = 0x0102;
constexpr uint16_t kErrPayloadTooShort = 0x0103;  // opcode's fixed payload is longer than arrived
constexpr uint16_t kErrEpochRegressed = 0x0104;   // boot_epoch older than the last one seen

// HELLO_ACK decision codes.
constexpr uint8_t kAdmitOk = 0x00;
constexpr uint8_t kAdmitRefusedTableFull = 0x01;
constexpr uint8_t kAdmitRefusedBadId = 0x02;

// BYE reason codes.
constexpr uint8_t kByeReasonShutdown = 0x00;
constexpr uint8_t kByeReasonReconfigure = 0x01;

}  // namespace pot
