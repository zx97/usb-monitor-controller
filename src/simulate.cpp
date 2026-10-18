// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - offline simulation: transport, decoders for
// revisions A / B / D / WeAct and the simulated driver factory.
//
// The revision C decoder lives in simulate_rev_c.cpp.

#include "lcdctl/simulate.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "lcdctl/logging.hpp"

namespace lcdctl {
namespace {

// Quantize a full 8-bit RGB value the way RGB565 does, so a decode round-trips
// exactly against an image that was serialized to RGB565.
inline void setPixelRGB565(Image& image, int x, int y, std::uint16_t value) {
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
        return;
    }
    std::uint8_t* pixel = image.data() + (static_cast<std::size_t>(y) * image.width() + x) * 3;
    pixel[0] = static_cast<std::uint8_t>(((value >> 11) & 0x1F) << 3);
    pixel[1] = static_cast<std::uint8_t>(((value >> 5) & 0x3F) << 2);
    pixel[2] = static_cast<std::uint8_t>((value & 0x1F) << 3);
}

std::uint16_t readU16(const std::vector<std::uint8_t>& data, std::size_t pos, bool bigEndian) {
    if (bigEndian) {
        return static_cast<std::uint16_t>((data[pos] << 8) | data[pos + 1]);
    }
    return static_cast<std::uint16_t>((data[pos + 1] << 8) | data[pos]);
}

// Fill the rectangle [x0..x1] x [y0..y1] from `data` (RGB565, row-major).
void blitRGB565(Image& image, int x0, int y0, int x1, int y1,
                const std::vector<std::uint8_t>& data, std::size_t start, bool bigEndian) {
    const int width = x1 - x0 + 1;
    const int height = y1 - y0 + 1;
    if (width <= 0 || height <= 0) {
        return;
    }

    std::size_t pos = start;
    const int totalPixels = width * height;
    for (int index = 0; index < totalPixels; ++index) {
        if (pos + 2 > data.size()) {
            return;
        }
        const std::uint16_t value = readU16(data, pos, bigEndian);
        pos += 2;
        setPixelRGB565(image, x0 + (index % width), y0 + (index / width), value);
    }
}

// Revision D prefixes every `chunkData`-byte group of the image payload with a
// `prefix` byte (0x50). Rebuild the contiguous payload of `totalBytes` bytes.
std::vector<std::uint8_t> deprefixChunks(const std::vector<std::uint8_t>& data, std::size_t pos,
                                         std::size_t totalBytes, std::size_t chunkData,
                                         std::uint8_t prefix) {
    std::vector<std::uint8_t> out;
    out.reserve(totalBytes);
    while (out.size() < totalBytes && pos < data.size()) {
        if (data[pos] == prefix) {
            ++pos;  // drop the per-chunk prefix
        }
        const std::size_t take = std::min(chunkData, totalBytes - out.size());
        const std::size_t available = std::min(take, data.size() - pos);
        out.insert(out.end(), data.begin() + pos, data.begin() + pos + available);
        pos += available;
        if (available == 0) {
            break;
        }
    }
    return out;
}

}  // namespace

// ===========================================================================
// SimulatedTransport
// ===========================================================================

bool SimulatedTransport::write(const std::uint8_t* data, std::size_t length) {
    captured_.insert(captured_.end(), data, data + length);
    return true;
}

std::vector<std::uint8_t> SimulatedTransport::read(std::size_t length, int) {
    const std::size_t count = std::min<std::size_t>(length, pending_.size());
    std::vector<std::uint8_t> out(pending_.begin(), pending_.begin() + count);
    pending_.erase(pending_.begin(), pending_.begin() + count);
    return out;
}

std::vector<std::uint8_t> SimulatedTransport::readAll() {
    std::vector<std::uint8_t> out = pending_;
    pending_.clear();
    return out;
}

void SimulatedTransport::queueResponse(std::vector<std::uint8_t> response) {
    pending_.insert(pending_.end(), response.begin(), response.end());
}

// ===========================================================================
// Revision A decoder
// ===========================================================================
// Stream: 6-byte packets (or 16 bytes for SET_ORIENTATION), each immediately
// followed by image data when the command is DISPLAY_BITMAP (197).
Image decodeRevA(const std::vector<std::uint8_t>& stream, int width, int height) {
    Image framebuffer(width, height, 3);
    std::size_t pos = 0;
    // --- Revision A: also handle the Clear / ToBlack commands (no image data) ---
    while (pos + 6 <= stream.size()) {
        const std::uint8_t command = stream[pos + 5];
        const bool isOrientation = (command == 121);
        const std::size_t packetSize = isOrientation ? 16 : 6;

        if (command == 102 || command == 103) {  // CLEAR (white) / TO_BLACK
            const std::uint16_t color = (command == 102) ? 0xFFFF : 0x0000;
            for (int y = 0; y < framebuffer.height(); ++y) {
                for (int x = 0; x < framebuffer.width(); ++x) {
                    setPixelRGB565(framebuffer, x, y, color);
                }
            }
            pos += packetSize;
            continue;
        }

        if (command == 197) {
            const int x0 = (stream[pos + 0] << 2) | (stream[pos + 1] >> 6);
            const int y0 = ((stream[pos + 1] & 0x3F) << 4) | (stream[pos + 2] >> 4);
            const int x1 = ((stream[pos + 2] & 0x0F) << 6) | (stream[pos + 3] >> 2);
            const int y1 = ((stream[pos + 3] & 0x03) << 8) | stream[pos + 4];
            pos += packetSize;
            blitRGB565(framebuffer, x0, y0, x1, y1, stream, pos, /*bigEndian=*/false);
            pos += static_cast<std::size_t>(x1 - x0 + 1) * (y1 - y0 + 1) * 2;
            continue;
        }
        pos += packetSize;
    }
    return framebuffer;
}

// ===========================================================================
// Revision B decoder
// ===========================================================================
// Stream: 10-byte frames; DISPLAY_BITMAP (0xCC) frame carries the rectangle and
// is followed by RGB565 big-endian data.
Image decodeRevB(const std::vector<std::uint8_t>& stream, int width, int height) {
    Image framebuffer(width, height, 3);
    std::size_t pos = 0;
    while (pos + 10 <= stream.size()) {
        const std::uint8_t command = stream[pos];
        if (command == 0xCC) {
            const int x0 = (stream[pos + 1] << 8) | stream[pos + 2];
            const int y0 = (stream[pos + 3] << 8) | stream[pos + 4];
            const int x1 = (stream[pos + 5] << 8) | stream[pos + 6];
            const int y1 = (stream[pos + 7] << 8) | stream[pos + 8];
            pos += 10;
            blitRGB565(framebuffer, x0, y0, x1, y1, stream, pos, /*bigEndian=*/true);
            pos += static_cast<std::size_t>(x1 - x0 + 1) * (y1 - y0 + 1) * 2;
            continue;
        }
        pos += 10;
    }
    return framebuffer;
}

// ===========================================================================
// Revision D decoder
// ===========================================================================
// Variable-length commands (0x43 sub-commands, 0x44 INTOPICMODE, 0x41
// OUTPICMODE); after INTOPICMODE the RGB565 big-endian data arrives in 63-byte
// groups each prefixed with 0x50.
Image decodeRevD(const std::vector<std::uint8_t>& stream, int width, int height) {
    Image framebuffer(width, height, 3);
    std::size_t pos = 0;
    int blockX0 = 0;
    int blockY0 = 0;
    int blockX1 = 0;
    int blockY1 = 0;

    while (pos < stream.size()) {
        const std::uint8_t command = stream[pos];
        if (command == 0x43 && pos + 2 <= stream.size()) {  // 'C' + sub-command
            const std::uint8_t sub = stream[pos + 1];
            if (sub == 0x41 && pos + 10 <= stream.size()) {  // BLOCKWRITE
                blockX0 = (stream[pos + 2] << 8) | stream[pos + 3];
                blockX1 = (stream[pos + 4] << 8) | stream[pos + 5];
                blockY0 = (stream[pos + 6] << 8) | stream[pos + 7];
                blockY1 = (stream[pos + 8] << 8) | stream[pos + 9];
                pos += 10;
            } else if (sub == 0x42 && pos + 4 <= stream.size()) {  // DISPCOLOR (fill)
                const std::uint16_t color = readU16(stream, pos + 2, /*bigEndian=*/true);
                for (int y = 0; y < framebuffer.height(); ++y) {
                    for (int x = 0; x < framebuffer.width(); ++x) {
                        setPixelRGB565(framebuffer, x, y, color);
                    }
                }
                pos += 4;
            } else {
                pos += 4;  // SETBL / orientation commands
            }
            continue;
        }
        if (command == 0x44) {  // INTOPICMODE: 4 bytes then the image data
            pos += 4;
            const std::size_t totalBytes =
                static_cast<std::size_t>(blockX1 - blockX0 + 1) * (blockY1 - blockY0 + 1) * 2;
            const std::vector<std::uint8_t> payload =
                deprefixChunks(stream, pos, totalBytes, /*chunkData=*/63, /*prefix=*/0x50);
            blitRGB565(framebuffer, blockX0, blockY0, blockX1, blockY1, payload, 0,
                       /*bigEndian=*/true);
            // Advance past the data plus one prefix byte per chunk.
            const std::size_t prefixes = (totalBytes + 62) / 63;
            pos += totalBytes + prefixes;
            continue;
        }
        if (command == 0x41) {  // OUTPICMODE
            pos += 4;
            continue;
        }
        // Unknown byte: stop to avoid desynchronizing.
        break;
    }
    return framebuffer;
}

// ===========================================================================
// WeAct decoder
// ===========================================================================
// CMD + payload + CMD_END (0x0A); SET_BITMAP (0x05) is followed by RGB565
// little-endian data.
Image decodeWeAct(const std::vector<std::uint8_t>& stream, int width, int height) {
    Image framebuffer(width, height, 3);
    std::size_t pos = 0;
    while (pos < stream.size()) {
        const std::uint8_t command = stream[pos];
        if (command == 0x05) {  // SET_BITMAP
            if (pos + 10 > stream.size()) {
                break;
            }
            const int x0 = stream[pos + 1] | (stream[pos + 2] << 8);
            const int y0 = stream[pos + 3] | (stream[pos + 4] << 8);
            const int x1 = stream[pos + 5] | (stream[pos + 6] << 8);
            const int y1 = stream[pos + 7] | (stream[pos + 8] << 8);
            pos += 10;
            blitRGB565(framebuffer, x0, y0, x1, y1, stream, pos, /*bigEndian=*/false);
            pos += static_cast<std::size_t>(x1 - x0 + 1) * (y1 - y0 + 1) * 2;
            continue;
        }
        if (command == 0x04) {  // FULL: single-colour fill of the whole screen
            if (pos + 12 > stream.size()) {
                break;
            }
            const std::uint16_t color = readU16(stream, pos + 9, /*bigEndian=*/false);
            for (int y = 0; y < framebuffer.height(); ++y) {
                for (int x = 0; x < framebuffer.width(); ++x) {
                    setPixelRGB565(framebuffer, x, y, color);
                }
            }
            pos += 12;
            continue;
        }
        if (command == 0x02) {  // SET_ORIENTATION
            pos += 3;
            continue;
        }
        if (command == 0x03) {  // SET_BRIGHTNESS
            pos += 5;
            continue;
        }
        if (command == 0x07 || command == 0xC2) {  // FREE / SYSTEM_VERSION|READ
            pos += 2;
            continue;
        }
        break;
    }
    return framebuffer;
}

// ===========================================================================
// Dispatch + simulated driver factory
// ===========================================================================

bool simulationSupported(Revision revision) { return revision != Revision::TuringUsb; }

Image decodeCapturedStream(Revision revision, const std::vector<std::uint8_t>& stream,
                           int width, int height) {
    switch (revision) {
        case Revision::A:
            return decodeRevA(stream, width, height);
        case Revision::B:
            return decodeRevB(stream, width, height);
        case Revision::C:
            return decodeRevC(stream, width, height);
        case Revision::D:
            return decodeRevD(stream, width, height);
        case Revision::WeActA:
        case Revision::WeActB:
            return decodeWeAct(stream, width, height);
        case Revision::TuringUsb:
            break;
    }
    throw std::invalid_argument("no simulator for this revision");
}

namespace {

void queueDefaultResponses(Revision revision, int width, int height, SimulatedTransport& sim) {
    if (revision == Revision::A) {
        // Pick the HELLO answer matching the requested resolution so that the
        // driver keeps the simulated size.
        if (width == 480 && height == 800) {
            sim.queueResponse({0x02, 0x02, 0x02, 0x02, 0x02, 0x02});  // UsbMonitor 5"
        } else if (width == 600 && height == 1024) {
            sim.queueResponse({0x03, 0x03, 0x03, 0x03, 0x03, 0x03});  // UsbMonitor 7"
        } else {
            sim.queueResponse({0x01, 0x01, 0x01, 0x01, 0x01, 0x01});  // UsbMonitor 3.5"
        }
    } else if (revision == Revision::B) {
        // HELLO answer: [0xCA, 'H','E','L','L','O', 0x0A, subrev, x, 0xCA].
        sim.queueResponse({0xCA, 'H', 'E', 'L', 'L', 'O', 0x0A, 0x12, 0x00, 0xCA});
    } else if (revision == Revision::C) {
        // 23-byte printable ID starting with "chs_"; "chs_5inch.1.87" -> ROM 87.
        std::string id = "chs_5inch.1.87";
        id.resize(23, '\0');
        sim.queueResponse(std::vector<std::uint8_t>(id.begin(), id.end()));
    } else if (revision == Revision::WeActA || revision == Revision::WeActB) {
        // 19-byte version answer: [0]=cmd, [1..9) ascii version, then padding.
        std::vector<std::uint8_t> version(19, 0);
        const std::string text = "V1.0.0.0";
        std::copy(text.begin(), text.end(), version.begin() + 1);
        sim.queueResponse(version);
    }
}

}  // namespace

SimulatedLcd createSimulatedLcd(Revision revision, int width, int height) {
    if (!simulationSupported(revision)) {
        throw std::invalid_argument("this revision cannot be simulated");
    }

    Size size{width, height};
    if (size.width == 0 || size.height == 0) {
        size = defaultSize(revision);
    }

    auto transport = std::make_unique<SimulatedTransport>();
    SimulatedTransport* raw = transport.get();
    queueDefaultResponses(revision, size.width, size.height, *raw);

    std::unique_ptr<LcdComm> lcd;
    switch (revision) {
        case Revision::A:
            lcd = std::make_unique<LcdCommRevA>(std::move(transport), size.width, size.height);
            break;
        case Revision::B:
            lcd = std::make_unique<LcdCommRevB>(std::move(transport), size.width, size.height);
            break;
        case Revision::C:
            lcd = std::make_unique<LcdCommRevC>(std::move(transport), size.width, size.height);
            break;
        case Revision::D:
            lcd = std::make_unique<LcdCommRevD>(std::move(transport), size.width, size.height);
            break;
        case Revision::WeActA:
            lcd = std::make_unique<LcdCommWeActA>(std::move(transport));
            break;
        case Revision::WeActB:
            lcd = std::make_unique<LcdCommWeActB>(std::move(transport));
            break;
        case Revision::TuringUsb:
            break;  // rejected above
    }

    LCDCTL_DEBUG << "Simulated " << revisionName(revision) << " " << size.width << "x"
                 << size.height;
    return {std::move(lcd), raw, revision};
}

}  // namespace lcdctl
