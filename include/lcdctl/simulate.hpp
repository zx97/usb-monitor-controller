// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - offline simulation ("SIMU" mode).
//
// Lets a real driver run without hardware: a SimulatedTransport records every
// byte the driver writes and replays canned responses, and a per-revision
// decoder turns the captured byte stream back into the framebuffer the screen
// would have shown. The result can be written to a PNG for inspection.
//
// This is the C++ equivalent of the Python project's `LcdSimulated` revision,
// but it also exercises the real wire-protocol encoders.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "lcdctl/drivers.hpp"
#include "lcdctl/factory.hpp"
#include "lcdctl/image.hpp"
#include "lcdctl/transport.hpp"

namespace lcdctl {

// A Transport that captures writes and replays canned responses for reads.
class SimulatedTransport : public Transport {
public:
    bool isOpen() const override { return true; }
    void close() override {}
    bool write(const std::uint8_t* data, std::size_t length) override;
    std::vector<std::uint8_t> read(std::size_t length, int timeoutMs) override;
    std::vector<std::uint8_t> readAll() override;
    void flushInput() override {}

    // Append bytes that the next read() calls will return (FIFO).
    void queueResponse(std::vector<std::uint8_t> response);

    const std::vector<std::uint8_t>& captured() const { return captured_; }
    void clearCaptured() { captured_.clear(); }

private:
    std::vector<std::uint8_t> captured_;
    std::vector<std::uint8_t> pending_;
};

// True when a captured-stream decoder exists for this revision.
bool simulationSupported(Revision revision);

// Rebuild the framebuffer from a captured byte stream. `width`/`height` are the
// display dimensions in the currently active orientation (see LcdComm::getWidth()).
Image decodeCapturedStream(Revision revision, const std::vector<std::uint8_t>& stream,
                           int width, int height);

// Per-revision decoders.
Image decodeRevA(const std::vector<std::uint8_t>& stream, int width, int height);
Image decodeRevB(const std::vector<std::uint8_t>& stream, int width, int height);
Image decodeRevC(const std::vector<std::uint8_t>& stream, int width, int height);
Image decodeRevD(const std::vector<std::uint8_t>& stream, int width, int height);
Image decodeWeAct(const std::vector<std::uint8_t>& stream, int width, int height);

// A driver running on a SimulatedTransport. `transport` is owned by `lcd`.
struct SimulatedLcd {
    std::unique_ptr<LcdComm> lcd;
    SimulatedTransport* transport = nullptr;
    Revision revision = Revision::A;
};

// Instantiate a driver backed by a SimulatedTransport, with the canned HELLO /
// version responses its initializeComm() expects. `width`/`height` of 0 use the
// revision default. Throws std::invalid_argument for revisions without a decoder.
SimulatedLcd createSimulatedLcd(Revision revision, int width = 0, int height = 0);

}  // namespace lcdctl
