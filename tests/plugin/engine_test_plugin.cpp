// An engine plugin for the test suite, built several times with one defect
// each, so the real loader and the real runner are driven against real DLLs.
//
// The defects, one per build, by compile definition:
//
//   RV_TEST_PLUGIN_OMIT_ON_EVENT   exports no revenant_engine_plugin_on_event
//   RV_TEST_PLUGIN_ABI_VERSION=n   reports ABI version n
//   RV_TEST_PLUGIN_DESCRIBE_FAILS  describe returns ERR_ARGUMENT
//   RV_TEST_PLUGIN_REFUSE_CREATE   create returns NULL
//   RV_TEST_PLUGIN_SLOW            sleeps in every on_event
//
// With none of them it is the good plugin, and it behaves like a very small
// trunk tracker, which is the shape the ABI was written for:
//
//   - On SOURCE_OPENED it opens a p25p1 receiver kCarrierOffsetHz above the
//     front end's centre, with tag 1.
//   - On the result of tag 1 it subscribes to that receiver's default
//     decoder, with tag 2.
//   - On a receiver a client opened, it tries to remove it, with tag 3,
//     which the host must refuse: a plugin may not take a client's receiver.
//   - On every event it logs one line through the host, which is how a test
//     sees what it was given: "event type=.. vrx=.. owner=.. ..." with every
//     integer field of a decoded message appended as key=value.
//
// Built by this toolchain, so /MT like the host, which means these cannot
// show the cross-CRT heap fault the ABI's caller-allocates rule prevents.
// tests/decode/vocoder_test_plugin.cpp says the same of its own fixtures.

#define RV_ENGINE_PLUGIN_BUILDING_PLUGIN
#include "core/plugin/engine_plugin_abi.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <thread>

#ifndef RV_TEST_PLUGIN_ABI_VERSION
#define RV_TEST_PLUGIN_ABI_VERSION RV_ENGINE_PLUGIN_ABI_VERSION
#endif

namespace {

// Where tests/rpc/test_rpc_engine_plugins.cpp's transmitter sits relative to
// the file's centre, which is test_rpc_decode.cpp's kCarrierHz.
constexpr std::int64_t kCarrierOffsetHz = 5'000;

}  // namespace

struct rv_engine_plugin {
    const rv_engine_host* host = nullptr;
    std::int64_t center_hz = 0;
};

namespace {

void log_line(const rv_engine_plugin& self, const std::string& line)
{
    self.host->log(self.host->ctx, RV_ENGINE_LOG_INFO, line.c_str());
}

[[nodiscard]] std::string describe_event(const rv_engine_event& ev)
{
    std::string line = "event type=" + std::to_string(ev.type) +
                       " vrx=" + std::to_string(ev.vrx) +
                       " owner=" + std::to_string(ev.owner) +
                       " tag=" + std::to_string(ev.request_tag) +
                       " code=" + std::to_string(ev.result_code) +
                       " center=" + std::to_string(ev.center_hz) +
                       " vrx_center=" + std::to_string(ev.vrx_center_hz) +
                       " demod=" + ev.demod +
                       " decoder=" + ev.decoder +
                       " kind=" + ev.kind +
                       " removed=" + std::to_string(ev.removed_count) +
                       " dropped=" + std::to_string(ev.events_dropped_before) +
                       " fields=" + std::to_string(ev.field_count);
    for (std::uint32_t i = 0; i < ev.field_count; ++i) {
        const rv_engine_field& field = ev.fields[i];
        if (field.type == RV_ENGINE_FIELD_INT) {
            line += std::string(" ") + field.key + "=" + std::to_string(field.int_value);
        }
    }
    // Last, because it is prose and may hold anything.
    line += std::string(" text=") + ev.text;
    return line;
}

}  // namespace

extern "C" {

RV_ENGINE_PLUGIN_EXPORT std::uint32_t RV_ENGINE_PLUGIN_CALL revenant_engine_plugin_abi_version(void)
{
    return RV_TEST_PLUGIN_ABI_VERSION;
}

RV_ENGINE_PLUGIN_EXPORT std::int32_t RV_ENGINE_PLUGIN_CALL
revenant_engine_plugin_describe(rv_engine_plugin_desc* out_desc)
{
#ifdef RV_TEST_PLUGIN_DESCRIBE_FAILS
    static_cast<void>(out_desc);
    return RV_ENGINE_PLUGIN_ERR_ARGUMENT;
#else
    if (out_desc == nullptr || out_desc->struct_size != sizeof(rv_engine_plugin_desc)) {
        return RV_ENGINE_PLUGIN_ERR_ARGUMENT;
    }
    out_desc->interests =
        RV_ENGINE_INTEREST_SOURCE | RV_ENGINE_INTEREST_VRX | RV_ENGINE_INTEREST_DECODED;
    std::snprintf(out_desc->name, sizeof(out_desc->name), "test-tracker");
    std::snprintf(out_desc->version, sizeof(out_desc->version), "1.0");
    return RV_ENGINE_PLUGIN_OK;
#endif
}

RV_ENGINE_PLUGIN_EXPORT rv_engine_plugin* RV_ENGINE_PLUGIN_CALL
revenant_engine_plugin_create(const rv_engine_host* host)
{
#ifdef RV_TEST_PLUGIN_REFUSE_CREATE
    static_cast<void>(host);
    return nullptr;
#else
    if (host == nullptr || host->struct_size != sizeof(rv_engine_host)) {
        return nullptr;
    }
    auto* self = new (std::nothrow) rv_engine_plugin;
    if (self == nullptr) {
        return nullptr;
    }
    self->host = host;
    log_line(*self, "created");
    return self;
#endif
}

#ifndef RV_TEST_PLUGIN_OMIT_ON_EVENT
RV_ENGINE_PLUGIN_EXPORT void RV_ENGINE_PLUGIN_CALL
revenant_engine_plugin_on_event(rv_engine_plugin* self, const rv_engine_event* ev)
{
    if (self == nullptr || ev == nullptr || ev->struct_size != sizeof(rv_engine_event)) {
        return;
    }
#ifdef RV_TEST_PLUGIN_SLOW
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
    try {
        log_line(*self, describe_event(*ev));
    } catch (...) {
        // Never across the line.
    }

    if (ev->type == RV_ENGINE_EVENT_SOURCE_OPENED) {
        self->center_hz = ev->center_hz;
        static_cast<void>(self->host->add_vrx(self->host->ctx, ev->center_hz + kCarrierOffsetHz,
                                              0, "p25p1", 1));
    } else if (ev->type == RV_ENGINE_EVENT_COMMAND_RESULT && ev->request_tag == 1 &&
               ev->result_code == RV_ENGINE_PLUGIN_OK) {
        static_cast<void>(self->host->subscribe_decoded(self->host->ctx, ev->vrx, "", 2));
    } else if (ev->type == RV_ENGINE_EVENT_VRX_ADDED && ev->owner == RV_ENGINE_OWNER_CLIENT) {
        static_cast<void>(self->host->remove_vrx(self->host->ctx, ev->vrx, 3));
    }
}
#endif

RV_ENGINE_PLUGIN_EXPORT void RV_ENGINE_PLUGIN_CALL revenant_engine_plugin_destroy(rv_engine_plugin* self)
{
    delete self;
}

}  // extern "C"
