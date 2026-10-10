// M7: guest actors. See pot/guest.hpp.

#include "pot/guest.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

extern "C" {
#include "wasm3.h"
}
#include "pot/wasm_imports.hpp"

namespace pot {

namespace {

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

constexpr uint8_t kWasmHeader[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};  // "\0asm", version 1

// The traps the guest API raises itself (compared by pointer, like the interpreter's).
const char* const kNoSuchInput = "[trap] no such input (potluck)";
const char* const kNoSuchOutput = "[trap] no such output (potluck)";
const char* const kWrongType = "[trap] published a value of the output's other type (potluck)";

bool fail_with(const char** why, const char* w) {
    if (why != nullptr) *why = w;
    return false;
}

// An input as the guest asked for it. A number of another type converts; anything else is NaN or 0.
float as_f32(const GuestInput& in) {
    if (!quality_has_value(in.q)) return std::numeric_limits<float>::quiet_NaN();
    float f = 0;
    int32_t i = 0;
    uint32_t u = 0;
    bool b = false;
    if (in.v.as_f32(f)) return f;
    if (in.v.as_i32(i)) return static_cast<float>(i);
    if (in.v.as_u32(u)) return static_cast<float>(u);
    if (in.v.as_bool(b)) return b ? 1.0f : 0.0f;
    return std::numeric_limits<float>::quiet_NaN();
}

int32_t as_i32(const GuestInput& in) {
    if (!quality_has_value(in.q)) return 0;
    float f = 0;
    int32_t i = 0;
    uint32_t u = 0;
    bool b = false;
    if (in.v.as_i32(i)) return i;
    if (in.v.as_u32(u)) return u > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<int32_t>(u);
    if (in.v.as_bool(b)) return b ? 1 : 0;
    if (in.v.as_f32(f)) {
        if (std::isnan(f)) return 0;
        if (f >= 2147483647.0f) return 0x7FFFFFFF;
        if (f <= -2147483648.0f) return static_cast<int32_t>(0x80000000u);
        return static_cast<int32_t>(f);
    }
    return 0;
}

}  // namespace

// ---- the blob ---------------------------------------------------------------------------------------

bool parse_guest_blob(const uint8_t* p, size_t len, GuestSpec& out, const char** why) {
    out = GuestSpec{};
    if (p == nullptr || len < kGuestFixedLen) return fail_with(why, "guest blob truncated");
    if (rd32(p) != kGuestMagic) return fail_with(why, "guest blob: bad magic");
    if (p[4] != kGuestVersion) return fail_with(why, "guest blob: unknown version");
    const uint8_t n_in = p[5];
    const uint8_t n_out = p[6];
    const uint8_t pages = p[7];
    const uint32_t fuel = rd32(p + 8);
    if (n_in > kMaxGuestInputs) return fail_with(why, "guest blob: more inputs than kMaxGuestInputs");
    if (n_out == 0 || n_out > kMaxGuestOutputs) return fail_with(why, "guest blob: outputs must be 1..kMaxGuestOutputs");
    if (pages > kGuestMaxPages) return fail_with(why, "guest blob: more memory than kGuestMaxPages");
    if (fuel < kGuestFuelMin || fuel > kGuestFuelMax) return fail_with(why, "guest blob: fuel outside its range");
    const size_t wiring = 4u * n_in + 5u * n_out;
    if (len < kGuestFixedLen + wiring + kNodeCertLen + kEdSigLen + sizeof(kWasmHeader)) {
        return fail_with(why, "guest blob truncated");
    }
    size_t at = kGuestFixedLen;
    for (uint8_t i = 0; i < n_in; ++i, at += 4) {
        out.in_hash[i] = rd32(p + at);
        if (out.in_hash[i] == 0) return fail_with(why, "guest blob: an input path hash of 0");
    }
    for (uint8_t i = 0; i < n_out; ++i, at += 5) {
        out.out_hash[i] = rd32(p + at);
        out.out_type[i] = static_cast<ValueType>(p[at + 4]);
        if (out.out_hash[i] == 0) return fail_with(why, "guest blob: an output path hash of 0");
        if (out.out_type[i] != ValueType::I32 && out.out_type[i] != ValueType::F32) {
            return fail_with(why, "guest blob: an output is neither i32 nor f32");
        }
        for (uint8_t j = 0; j < i; ++j) {
            if (out.out_hash[j] == out.out_hash[i]) return fail_with(why, "guest blob: two outputs at one path");
        }
    }
    out.cert = p + at;
    at += kNodeCertLen;
    out.sig = p + at;
    at += kEdSigLen;
    out.module = p + at;
    out.module_len = static_cast<uint32_t>(len - at);
    if (std::memcmp(out.module, kWasmHeader, sizeof(kWasmHeader)) != 0) {
        return fail_with(why, "guest blob: the module is not a WebAssembly 1.0 binary");
    }
    out.n_in = n_in;
    out.n_out = n_out;
    out.mem_pages = pages;
    out.fuel = fuel;
    return true;
}

CertError guest_image_check(const DeployImage& img, const uint8_t ca_pub[kEdPubLen], uint8_t* which) {
    for (uint8_t g = 0; g < img.guest_count; ++g) {
        if (which != nullptr) *which = g;
        GuestSpec s;
        if (!parse_guest_blob(img.guests[g].data, img.guests[g].len, s, nullptr)) return CertError::Length;
        const CertError e = guest_check(ca_pub, s.cert, s.sig, s.module, s.module_len, nullptr);
        if (e != CertError::Ok) return e;
    }
    return CertError::Ok;
}

// ---- the library --------------------------------------------------------------------------------------

bool GuestLibrary::load(const DeployImage& img, uint8_t* pool, size_t pool_len, const char** why) {
    n_ = 0;
    if (img.guest_count > kMaxGuests) return fail_with(why, "more guests than kMaxGuests");
    uint8_t refs[kMaxGuests] = {};
    for (uint8_t i = 0; i < img.actor_count; ++i) {
        const ActorDecl& a = img.actors[i];
        if (a.type != ActorType::Guest || a.cfg_len == 0) continue;
        const uint8_t g = a.cfg[a.cfg_len - 1];
        if (g >= img.guest_count) return fail_with(why, "guest actor names a guest the image does not carry");
        ++refs[g];
    }
    size_t total = 0;
    for (uint8_t g = 0; g < img.guest_count; ++g) {
        if (refs[g] != 1) {
            return fail_with(why, refs[g] == 0 ? "a guest no actor runs" : "a guest two actors run (one actor each)");
        }
        total += img.guests[g].len;
    }
    if (total > pool_len || (total > 0 && pool == nullptr)) return fail_with(why, "the guests do not fit the guest pool");
    size_t at = 0;
    for (uint8_t g = 0; g < img.guest_count; ++g) {
        std::memcpy(pool + at, img.guests[g].data, img.guests[g].len);
        if (!parse_guest_blob(pool + at, img.guests[g].len, specs_[g], why)) return false;
        at += img.guests[g].len;
    }
    n_ = img.guest_count;
    return true;
}

GuestLibrary& guest_library() {
    static GuestLibrary s_lib;
    return s_lib;
}

// ---- the guest API: what a guest may import from "potluck", and nothing else --------------------------

struct GuestApi {
    using Ctx = GuestRuntime::Ctx;
    static Ctx* ctx(IM3Runtime rt) { return static_cast<Ctx*>(m3_GetUserData(rt)); }

    static m3ApiRawFunction(input_f32) {
        (void)_ctx, (void)_mem;
        m3ApiReturnType(float);
        m3ApiGetArg(int32_t, idx);
        Ctx* c = ctx(runtime);
        if (idx < 0 || idx >= c->spec->n_in) m3ApiTrap(kNoSuchInput);
        m3ApiReturn(as_f32(c->req.in[idx]));
    }
    static m3ApiRawFunction(input_i32) {
        (void)_ctx, (void)_mem;
        m3ApiReturnType(int32_t);
        m3ApiGetArg(int32_t, idx);
        Ctx* c = ctx(runtime);
        if (idx < 0 || idx >= c->spec->n_in) m3ApiTrap(kNoSuchInput);
        m3ApiReturn(as_i32(c->req.in[idx]));
    }
    static m3ApiRawFunction(quality) {
        (void)_ctx, (void)_mem;
        m3ApiReturnType(int32_t);
        m3ApiGetArg(int32_t, idx);
        Ctx* c = ctx(runtime);
        if (idx < 0 || idx >= c->spec->n_in) m3ApiTrap(kNoSuchInput);
        m3ApiReturn(static_cast<int32_t>(c->req.in[idx].q));
    }
    static m3ApiRawFunction(publish_f32) {
        (void)_ctx, (void)_mem;
        m3ApiGetArg(int32_t, idx);
        m3ApiGetArg(float, v);
        Ctx* c = ctx(runtime);
        if (idx < 0 || idx >= c->spec->n_out) m3ApiTrap(kNoSuchOutput);
        if (c->spec->out_type[idx] != ValueType::F32) m3ApiTrap(kWrongType);
        c->res.out[idx] = Value::of_f32(v);
        c->res.out_set[idx] = true;
        m3ApiSuccess();
    }
    static m3ApiRawFunction(publish_i32) {
        (void)_ctx, (void)_mem;
        m3ApiGetArg(int32_t, idx);
        m3ApiGetArg(int32_t, v);
        Ctx* c = ctx(runtime);
        if (idx < 0 || idx >= c->spec->n_out) m3ApiTrap(kNoSuchOutput);
        if (c->spec->out_type[idx] != ValueType::I32) m3ApiTrap(kWrongType);
        c->res.out[idx] = Value::of_i32(v);
        c->res.out_set[idx] = true;
        m3ApiSuccess();
    }
    // save(ptr, len) -> 0 kept, -1 refused (longer than kMaxCheckpoint). Out of bounds is a trap.
    static m3ApiRawFunction(save) {
        (void)_ctx;
        m3ApiReturnType(int32_t);
        m3ApiGetArgMem(const uint8_t*, p);
        m3ApiGetArg(int32_t, n);
        Ctx* c = ctx(runtime);
        if (n < 0 || static_cast<size_t>(n) > kMaxCheckpoint) m3ApiReturn(-1);
        m3ApiCheckMem(p, static_cast<size_t>(n));
        std::memcpy(c->res.save, p, static_cast<size_t>(n));
        c->res.save_len = static_cast<uint8_t>(n);
        c->res.save_set = true;
        m3ApiReturn(0);
    }
    // restore(ptr, cap) -> bytes copied, -1 if there is no checkpoint.
    static m3ApiRawFunction(restore) {
        (void)_ctx;
        m3ApiReturnType(int32_t);
        m3ApiGetArgMem(uint8_t*, p);
        m3ApiGetArg(int32_t, cap);
        Ctx* c = ctx(runtime);
        if (!c->req.has_ck || cap < 0) m3ApiReturn(-1);
        const size_t n = static_cast<size_t>(cap) < c->req.ck_len ? static_cast<size_t>(cap) : c->req.ck_len;
        m3ApiCheckMem(p, n);
        std::memcpy(p, c->req.ck, n);
        m3ApiReturn(static_cast<int32_t>(n));
    }
    static m3ApiRawFunction(log) {
        (void)_ctx, (void)_mem;
        m3ApiGetArg(int32_t, v);
        Ctx* c = ctx(runtime);
        ++c->res.logs;
        c->res.log_last = v;
        m3ApiSuccess();
    }
    static m3ApiRawFunction(now_ms) {
        (void)_ctx, (void)_mem;
        m3ApiReturnType(int32_t);
        m3ApiReturn(static_cast<int32_t>(ctx(runtime)->req.now_ms));
    }
};

namespace {
const WasmImport kGuestImports[] = {
    {"input_f32", "f(i)", &GuestApi::input_f32},
    {"input_i32", "i(i)", &GuestApi::input_i32},
    {"quality", "i(i)", &GuestApi::quality},
    {"publish_f32", "v(if)", &GuestApi::publish_f32},
    {"publish_i32", "v(ii)", &GuestApi::publish_i32},
    {"save", "i(*i)", &GuestApi::save},
    {"restore", "i(*i)", &GuestApi::restore},
    {"log", "v(i)", &GuestApi::log},
    {"now_ms", "i()", &GuestApi::now_ms},
};
constexpr size_t kGuestImportCount = sizeof(kGuestImports) / sizeof(kGuestImports[0]);

// A load that failed for want of memory may succeed later; anything else about a module will not change.
bool transient(const char* why) {
    return why == m3Err_mallocFailed || std::strncmp(why, "m3_New", 6) == 0;
}
}  // namespace

// ---- the runtime ----------------------------------------------------------------------------------------

void GuestRuntime::set_ca(const uint8_t* ca_pub) { ca_ = ca_pub; }

uint32_t GuestRuntime::activate(uint8_t i) {
    Ctx& c = ctx_[i];
    uint32_t g = ++c.next_gen;
    if (g == 0) g = ++c.next_gen;  // 0 means "nobody"
    c.active_gen.store(g);
    uint8_t done = 3;
    c.state.compare_exchange_strong(done, 0);  // an older activation's finished run: nobody reads it
    if (wake_ != nullptr) wake_(wake_ctx_);
    return g;
}

void GuestRuntime::release(uint8_t i, uint32_t gen) {
    Ctx& c = ctx_[i];
    uint32_t g = gen;
    if (!c.active_gen.compare_exchange_strong(g, 0)) return;  // a newer activation owns it now
    uint8_t done = 3;
    c.state.compare_exchange_strong(done, 0);
    if (wake_ != nullptr) wake_(wake_ctx_);  // so the guest task unloads it
}

GuestRequest* GuestRuntime::request(uint8_t i) {
    return ctx_[i].state.load(std::memory_order_acquire) == 0 ? &ctx_[i].req : nullptr;
}

void GuestRuntime::send(uint8_t i) {
    ctx_[i].state.store(1, std::memory_order_release);
    if (wake_ != nullptr) wake_(wake_ctx_);
}

const GuestResult* GuestRuntime::result(uint8_t i, uint32_t gen) {
    Ctx& c = ctx_[i];
    if (c.state.load(std::memory_order_acquire) != 3) return nullptr;
    if (c.res.gen != gen) {
        c.state.store(0, std::memory_order_release);
        return nullptr;
    }
    return &c.res;
}

void GuestRuntime::consumed(uint8_t i) { ctx_[i].state.store(0, std::memory_order_release); }

bool GuestRuntime::service() {
    bool ran = false;
    for (uint8_t i = 0; i < kMaxGuests; ++i) {
        Ctx& c = ctx_[i];
        uint8_t requested = 1;
        if (c.state.compare_exchange_strong(requested, 2, std::memory_order_acq_rel)) {
            run(i);
            ran = true;
        } else if (c.sb.loaded() && c.state.load(std::memory_order_acquire) == 0 &&
                   c.active_gen.load() != c.loaded_gen) {
            c.sb.unload();  // its activation ended: give the memory back
            c.loaded_gen = 0;
        }
    }
    return ran;
}

void GuestRuntime::run(uint8_t i) {
    Ctx& c = ctx_[i];
    GuestResult& r = c.res;
    r = GuestResult{};
    r.gen = c.req.gen;
    const GuestSpec* sp = guest_library().spec(i);
    c.spec = sp;
    const uint32_t t0 = now_us_ != nullptr ? now_us_(meter_ctx_) : 0;
    auto finish = [this, &c, &r, t0]() {
        if (now_us_ != nullptr) r.run_us = now_us_(meter_ctx_) - t0;
        if (c.active_gen.load() != r.gen) {
            // Nobody wants this run any more (the actor is gone, or a newer activation took over).
            c.sb.unload();
            c.loaded_gen = 0;
            c.state.store(0, std::memory_order_release);
        } else {
            c.state.store(3, std::memory_order_release);
        }
    };
    if (sp == nullptr) {
        r.error = "no such guest in the image";
        r.fatal = true;
        finish();
        return;
    }
    if (c.loaded_gen != c.req.gen) {
        c.sb.unload();  // section 7.7: a fresh instance per activation
        c.loaded_gen = 0;
    }
    if (!c.sb.loaded()) {
        if (c.verified == 0 && ca_ != nullptr) {
            // Once per boot: the author's signature, under the cluster CA. The image's own signature was
            // checked when it arrived; this one says who wrote the code in it.
            c.verified = guest_check(ca_, sp->cert, sp->sig, sp->module, sp->module_len, nullptr) == CertError::Ok ? 1 : 2;
        }
        if (c.verified != 1) {
            r.error = ca_ == nullptr ? "this node is not enrolled, so it cannot tell who wrote the guest"
                                     : "the author's signature does not verify under the cluster CA";
            r.fatal = true;
            finish();
            return;
        }
        const size_t heap0 = heap_free_ != nullptr ? heap_free_(meter_ctx_) : 0;
        WasmLimits lim;
        lim.memory_bytes = static_cast<uint32_t>(sp->mem_pages) * 65536u;
        lim.value_stack_bytes = kGuestValueStack;
        const char* why = c.sb.load(sp->module, sp->module_len, lim, kGuestImports, kGuestImportCount, &c);
        if (why != nullptr) {
            r.error = why;
            r.fatal = !transient(why);
            finish();
            return;
        }
        c.loaded_gen = c.req.gen;
        r.fresh = true;
        const WasmCallResult in = c.sb.call("init", nullptr, 0, sp->fuel, kGuestNativeStack);
        r.fuel_init = in.fuel_used;
        if (heap_free_ != nullptr) {
            const size_t heap1 = heap_free_(meter_ctx_);
            r.load_heap_b = heap0 > heap1 ? static_cast<uint32_t>(heap0 - heap1) : 0;
        }
        if (in.error != nullptr && in.error != m3Err_functionLookupFailed) {  // no init() is fine
            r.error = in.error;
            if (in.error != pot_wasm_busy) {
                c.sb.unload();
                c.loaded_gen = 0;
            }
            finish();
            return;
        }
    }
    const uint32_t now = c.req.now_ms;
    const WasmCallResult t = c.sb.call("tick", &now, 1, sp->fuel, kGuestNativeStack);
    r.fuel_tick = t.fuel_used;
    if (t.error != nullptr) {
        r.error = t.error;
        if (t.error == m3Err_functionLookupFailed) {
            r.error = "the module exports no tick(i32)";
            r.fatal = true;
        }
        if (t.error != pot_wasm_busy) {
            c.sb.unload();  // a trap leaves the instance in a state nobody chose: start afresh next time
            c.loaded_gen = 0;
        }
    }
    finish();
}

void GuestRuntime::reset() {
    for (Ctx& c : ctx_) {
        c.sb.unload();
        c.state.store(0);
        c.active_gen.store(0);
        c.loaded_gen = 0;
        c.verified = 0;
        c.spec = nullptr;
    }
    ca_ = nullptr;
    wake_ = nullptr;
    wake_ctx_ = nullptr;
    now_us_ = nullptr;
    heap_free_ = nullptr;
    meter_ctx_ = nullptr;
}

GuestRuntime& guest_runtime() {
    static GuestRuntime s_rt;
    return s_rt;
}

// ---- the actor ------------------------------------------------------------------------------------------

GuestActor::GuestActor(Node& node, const TickerConfig& place, uint8_t guest, const CheckpointStore* ck)
    : node_(node), place_(place), guest_(guest), spec_(guest_library().spec(guest)), ck_store_(ck) {}

GuestActor::~GuestActor() {
    if (gen_ != 0) guest_runtime().release(guest_, gen_);
}

bool GuestActor::start(uint32_t now_ms) {
    if (spec_ == nullptr) return false;
    if (ck_store_ != nullptr && ck_store_->load != nullptr) {
        size_t len = 0;
        uint32_t age = 0;
        if (ck_store_->load(ck_store_->ctx, ck_, sizeof(ck_), &len, &age)) {
            ck_len_ = static_cast<uint8_t>(len);
            has_ck_ = true;
        }
    }
    gen_ = guest_runtime().activate(guest_);
    next_ms_ = now_ms;
    started_ = true;
    return true;
}

void GuestActor::mark_faulty(uint32_t now_ms) {
    for (uint8_t j = 0; j < spec_->n_out; ++j) node_.ns().publish_faulty(spec_->out_hash[j], now_ms);
}

void GuestActor::fail(const char* why, bool fatal, uint32_t now_ms) {
    ++stats_.failed;
    last_error_ = why;
    mark_faulty(now_ms);
    strikes_ = fatal ? kGuestStrikes : static_cast<uint8_t>(strikes_ + 1);
    if (strikes_ >= kGuestStrikes) quarantined_ = true;
    char err[96];
    size_t k = 0;
    for (const char* s = why; *s != '\0' && k + 1 < sizeof(err); ++s) err[k++] = (*s == '"' || *s == '\\') ? '\'' : *s;
    err[k] = '\0';
    std::printf("{\"t\":\"guest_fault\",\"node\":%u,\"guest\":%u,\"key\":%u,\"strikes\":%u,\"quarantined\":%u,"
                "\"error\":\"%s\"}\n",
                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(guest_),
                static_cast<unsigned>(place_.out_hash), static_cast<unsigned>(strikes_), quarantined_ ? 1u : 0u, err);
}

void GuestActor::refresh_inputs(uint32_t now_ms) {
    for (uint8_t k = 0; k < spec_->n_in; ++k) {
        bool local = false;
        Reading rd;
        if (node_.read(spec_->in_hash[k], rd, &local) != NsError::Ok || local) continue;
        if (rd.quality == Quality::Good && rd.age_ms < place_.period_ms) continue;  // fresh enough
        if (asked_ms_[k] != 0 && now_ms - asked_ms_[k] < place_.period_ms) continue;
        const NsEntry* e = node_.ns().find(spec_->in_hash[k]);
        if (e == nullptr || e->owner_node == 0) continue;
        if (node_.request_read(e->owner_node, spec_->in_hash[k]) != 0) {
            asked_ms_[k] = now_ms == 0 ? 1 : now_ms;
            ++stats_.reads;
        }
    }
}

void GuestActor::tick(uint32_t now_ms) {
    if (!started_ || quarantined_) return;
    GuestRuntime& rt = guest_runtime();
    if (const GuestResult* r = rt.result(guest_, gen_)) {
        ++stats_.runs;
        stats_.logs += r->logs;
        if (r->logs != 0) stats_.log_last = r->log_last;
        if (r->fresh) ++stats_.fresh;
        if (r->fresh && r->load_heap_b != 0) stats_.load_heap_b = r->load_heap_b;
        stats_.run_us_last = r->run_us;
        if (r->run_us > stats_.run_us_max) stats_.run_us_max = r->run_us;
        if (r->error == pot_wasm_busy) {
            ++stats_.overruns;  // another sandbox was running (a test instrument): not the guest's fault
        } else if (r->error != nullptr) {
            fail(r->error, r->fatal, now_ms);
        } else {
            strikes_ = 0;
            ++stats_.ok;
            stats_.fuel_last = r->fuel_tick;
            // One timestamp for the set: the outputs of one tick belong together (see Actor::tick).
            for (uint8_t j = 0; j < spec_->n_out; ++j) {
                if (r->out_set[j]) node_.ns().publish(spec_->out_hash[j], r->out[j], now_ms);
            }
            if (r->save_set) {
                std::memcpy(ck_, r->save, r->save_len);
                ck_len_ = r->save_len;
                has_ck_ = true;
                ck_dirty_ = true;
            }
        }
        rt.consumed(guest_);
        if (quarantined_) return;
    }
    if (ck_dirty_ && ck_store_ != nullptr && ck_store_->save != nullptr &&
        ck_store_->save(ck_store_->ctx, ck_, ck_len_)) {
        ck_dirty_ = false;
        ++stats_.ck_saved;
    }
    refresh_inputs(now_ms);
    if (static_cast<int32_t>(now_ms - next_ms_) < 0) return;
    next_ms_ += place_.period_ms;
    if (static_cast<int32_t>(now_ms - next_ms_) >= 0) next_ms_ = now_ms + place_.period_ms;  // fell behind: no burst
    GuestRequest* q = rt.request(guest_);
    if (q == nullptr) {
        ++stats_.overruns;  // the last run has not finished: skip this period rather than queue
        return;
    }
    q->gen = gen_;
    q->now_ms = now_ms;
    for (uint8_t k = 0; k < spec_->n_in; ++k) {
        Reading rd;
        if (node_.read(spec_->in_hash[k], rd) == NsError::Ok) {
            q->in[k].q = rd.quality;
            q->in[k].v = rd.value;
        } else {
            q->in[k].q = Quality::Unavailable;
            q->in[k].v = Value{};
        }
    }
    q->has_ck = has_ck_;
    q->ck_len = ck_len_;
    std::memcpy(q->ck, ck_, ck_len_);
    rt.send(guest_);
}

size_t GuestActor::stats_json(char* buf, size_t cap, uint32_t) {
    char err[64];
    size_t k = 0;
    if (last_error_ != nullptr) {
        for (const char* s = last_error_; *s != '\0' && k + 1 < sizeof(err); ++s) {
            err[k++] = (*s == '"' || *s == '\\') ? '\'' : *s;
        }
    }
    err[k] = '\0';
    const int n = std::snprintf(
        buf, cap,
        "{\"t\":\"guest\",\"node\":%u,\"guest\":%u,\"key\":%u,\"runs\":%u,\"ok\":%u,\"failed\":%u,\"overruns\":%u,"
        "\"fresh\":%u,\"reads\":%u,\"ck_saved\":%u,\"fuel\":%llu,\"logs\":%u,\"log_last\":%d,\"strikes\":%u,"
        "\"run_us\":%u,\"run_us_max\":%u,\"load_heap_b\":%u,\"quarantined\":%u,\"error\":\"%s\"}",
        static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(guest_),
        static_cast<unsigned>(place_.out_hash), static_cast<unsigned>(stats_.runs), static_cast<unsigned>(stats_.ok),
        static_cast<unsigned>(stats_.failed), static_cast<unsigned>(stats_.overruns),
        static_cast<unsigned>(stats_.fresh), static_cast<unsigned>(stats_.reads),
        static_cast<unsigned>(stats_.ck_saved), static_cast<unsigned long long>(stats_.fuel_last),
        static_cast<unsigned>(stats_.logs), static_cast<int>(stats_.log_last), static_cast<unsigned>(strikes_),
        static_cast<unsigned>(stats_.run_us_last), static_cast<unsigned>(stats_.run_us_max),
        static_cast<unsigned>(stats_.load_heap_b), quarantined_ ? 1u : 0u, err);
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

// ---- the row -------------------------------------------------------------------------------------------

namespace {
bool guest_header(const ActorDecl& d, TickerConfig& pc, uint8_t& guest) {
    size_t hl = 0;
    if (d.node_id != kPortableNode || !portable_header(d, pc, hl) || d.cfg_len != hl + 1) return false;
    guest = d.cfg[hl];
    return true;
}

Actor* guest_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(GuestActor) <= kActorSlotBytes, "GuestActor outgrew its actor slot");
    TickerConfig pc{};
    uint8_t g = 0;
    if (env.node == nullptr || !guest_header(d, pc, g) || guest_library().spec(g) == nullptr) return nullptr;
    return new (mem) GuestActor(*env.node, pc, g, env.checkpoint);
}

bool guest_placement(const ActorDecl& d, TickerConfig& out) {
    uint8_t g = 0;
    return guest_header(d, out, g);
}
}  // namespace

bool guest_check_decl(const ActorDecl& d, const ActorEnv&, const char** why) {
    TickerConfig pc{};
    uint8_t g = 0;
    if (!guest_header(d, pc, g)) return fail_with(why, "guest must be portable: the portable header, then a guest index");
    if (guest_library().spec(g) == nullptr) return fail_with(why, "guest actor names a guest the library does not hold");
    return true;
}

size_t guest_outputs(const ActorDecl& d, uint16_t, uint16_t peer, NsDecl* out, size_t cap) {
    TickerConfig pc{};
    uint8_t g = 0;
    const GuestSpec* s = guest_header(d, pc, g) ? guest_library().spec(g) : nullptr;
    if (s == nullptr || peer != 0) return 0;
    size_t n = 0;
    for (uint8_t j = 0; j < s->n_out && n < cap; ++j) {
        NsDecl& o = out[n++];
        o = NsDecl{};
        o.path_hash = s->out_hash[j];
        o.type = s->out_type[j];
        o.unit = Unit::None;
        o.kind = ResourceKind::Sampled;
        o.access = Access::Read;  // never writable: a guest is never between an actuator and anything
        o.latency_class = kClassL3;
        o.staleness_bound_ms = 5u * pc.period_ms;  // as the reconciler's default: one lost tick is not staleness
        o.staleness_policy = StalenessPolicy::Informative;
    }
    return n;
}

const ActorKind kGuestKind = {ActorType::Guest, "guest", &guest_check_decl, &guest_create, &guest_placement,
                              &guest_outputs};

}  // namespace pot
