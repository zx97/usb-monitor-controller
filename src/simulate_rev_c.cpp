// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - revision C offline decoder.
//
// Inverts the wire protocol produced by LcdCommRevC (src/drivers/rev_c.cpp) so
// that a revision C screen can be exercised without hardware: the captured byte
// stream is parsed back into the framebuffer the panel would have shown.
//
// Stream layout
// -------------
// Every message emitted by `sendCommand()` is padded to a multiple of 250 bytes
// (0x00, except START_DISPLAY_BITMAP which pads with 0x2c), so the stream is a
// concatenation of 250-byte-multiple messages. Two message kinds carry image
// data and have a variable length; every other message is a fixed 250-byte
// frame whose first byte is the command id.
//
//   Full screen (x==0, y==0, imageWidth==getWidth(), imageHeight==getHeight()):
//       PRE_UPDATE_BITMAP      0x86 ...                 (250 B)
//       START_DISPLAY_BITMAP   0x2c (padded with 0x2c)  (250 B)
//       DISPLAY_BITMAP_xINCH   0xc8 ef 69 00 ss ss      (250 B)
//       SEND_PAYLOAD           raw image bytes          (variable)
//       QUERY_STATUS           0xcf ...                 (250 B)
//
//   Partial update (any other rectangle):
//       SEND_PAYLOAD    0xcc ef 69 00 + size(3) + 000000 + count(4)  (250 B)
//       SEND_PAYLOAD    row-encoded image bytes                      (variable)
//       QUERY_STATUS    0xcf ...                                     (250 B)
//
// A SEND_PAYLOAD message carries no command-id prefix (Command.SEND_PAYLOAD is
// skipped), so its length is derived from the geometry instead of being read
// from the wire.
//
// Pixel format
// ------------
// The encoder writes BGRA (4 bytes, order B,G,R,A) for the full-screen path
// (`generateFullImage` always calls `imageToBGRA`) and either BGRA or plain BGR
// (3 bytes) for a partial update, depending on the panel/ROM. The decoder is
// only given width/height, not the sub-revision or ROM, so for the full-screen
// path the pixel size is chosen so that the following message starts exactly
// where QUERY_STATUS (0xcf) is expected; when the choice is ambiguous BGRA is
// preferred on the 5"/8" panels and BGR on the 2" panel (which matches the
// Python default of ROM 87). For a partial update the size is inferred by
// parsing the row headers and keeping the size that consumes the payload
// exactly.
//
// The decoder reconstructs the wire layout, i.e. the (already software-rotated)
// pixels exactly as the encoder laid them out in a `width`-wide linear buffer.
// The caller is responsible for accounting for the rotation the driver applies.

#include "lcdctl/simulate.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace lcdctl {
namespace {

// The protocol frame: every message is padded to a multiple of this many bytes.
constexpr std::size_t kFrame = 250;
// Image data is transmitted in 249-byte chunks separated by a single 0x00 byte.
constexpr std::size_t kChunk = 249;

std::size_t roundUpToFrame(std::size_t length) {
    return ((length + kFrame - 1) / kFrame) * kFrame;
}

// Length of the 0x00-joined chunk sequence the encoder produces from `rawLength`
// bytes of payload (b'\x00'.join(chunked(data, 249))).
std::size_t joinedLength(std::size_t rawLength) {
    if (rawLength == 0) {
        return 0;
    }
    const std::size_t chunks = (rawLength + kChunk - 1) / kChunk;
    return rawLength + (chunks - 1);
}

// Recover `rawLength` raw payload bytes from a 0x00-joined chunk sequence:
// copy 249 bytes, skip one separator, repeat.
std::vector<std::uint8_t> splitJoinedChunks(const std::vector<std::uint8_t>& stream,
                                            std::size_t offset, std::size_t rawLength) {
    std::vector<std::uint8_t> raw;
    raw.reserve(rawLength);
    std::size_t pos = offset;
    while (raw.size() < rawLength && pos < stream.size()) {
        const std::size_t take = std::min(kChunk, rawLength - raw.size());
        const std::size_t available = std::min(take, stream.size() - pos);
        raw.insert(raw.end(), stream.begin() + pos, stream.begin() + pos + available);
        pos += available;
        if (available < take) {  // truncated stream: keep what we have
            break;
        }
        ++pos;  // skip the 0x00 separator between chunks
    }
    return raw;
}

// Interleaved BGR/BGRA -> the framebuffer's RGB triples. Both layouts store the
// channels in B,G,R(,A) order, so R=src[2], G=src[1], B=src[0].
void putPixel(Image& framebuffer, std::size_t pixelIndex, const std::uint8_t* src) {
    const std::size_t count =
        static_cast<std::size_t>(framebuffer.width()) * framebuffer.height();
    if (pixelIndex >= count) {
        return;
    }
    const std::size_t x = pixelIndex % static_cast<std::size_t>(framebuffer.width());
    const std::size_t y = pixelIndex / static_cast<std::size_t>(framebuffer.width());
    std::uint8_t* dst = framebuffer.data() + (y * framebuffer.width() + x) * 3;
    dst[0] = src[2];
    dst[1] = src[1];
    dst[2] = src[0];
}

// 480x480 marks the 2" panels (1/3 of the 5" area); they use BGR by default.
bool isSmallPanel(int width, int height) {
    return static_cast<long long>(width) * height == 480LL * 480LL;
}

// The full-screen payload is BGRA on the wire, but older ROMs may send BGR.
// Pick the size for which QUERY_STATUS (0xcf) lands right after the payload;
// on a tie (or when neither fits) prefer BGRA except on the 2" panel.
int chooseFullPixelSize(const std::vector<std::uint8_t>& stream, std::size_t payloadPos,
                        int width, int height) {
    const std::size_t pixels =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    auto fits = [&](int pixelSize) {
        const std::size_t messageLength =
            roundUpToFrame(joinedLength(pixels * static_cast<std::size_t>(pixelSize)));
        return payloadPos + messageLength < stream.size() &&
               stream[payloadPos + messageLength] == 0xcf;
    };

    const bool fits4 = fits(4);
    const bool fits3 = fits(3);
    if (fits4 != fits3) {
        return fits4 ? 4 : 3;
    }
    return isSmallPanel(width, height) ? 3 : 4;
}

// Parse the row headers of a partial-update payload and report whether the
// encoding with `pixelSize` consumes exactly `raw.size()` bytes.
bool partialPayloadConsistent(const std::vector<std::uint8_t>& raw, int pixelSize) {
    std::size_t pos = 0;
    bool sawRow = false;
    while (pos + 5 <= raw.size()) {
        const int rowWidth = (static_cast<int>(raw[pos + 3]) << 8) | raw[pos + 4];
        const std::size_t rowBytes = static_cast<std::size_t>(rowWidth) * pixelSize;
        pos += 5;
        if (pos + rowBytes > raw.size()) {
            return false;
        }
        pos += rowBytes;
        sawRow = true;
    }
    return sawRow && pos == raw.size();
}

int choosePartialPixelSize(const std::vector<std::uint8_t>& raw, int width, int height) {
    const bool ok4 = partialPayloadConsistent(raw, 4);
    const bool ok3 = partialPayloadConsistent(raw, 3);
    if (ok4 != ok3) {
        return ok4 ? 4 : 3;
    }
    return isSmallPanel(width, height) ? 3 : 4;
}

// Rebuild the full-screen framebuffer; returns the offset of the following
// QUERY_STATUS message.
std::size_t decodeFullPayload(const std::vector<std::uint8_t>& stream, std::size_t payloadPos,
                              Image& framebuffer) {
    const int Width = framebuffer.width();
    const int Height = framebuffer.height();
    const int pixelSize = chooseFullPixelSize(stream, payloadPos, Width, Height);

    const std::size_t pixelCount =
        static_cast<std::size_t>(Width) * static_cast<std::size_t>(Height);
    const std::size_t rawLength = pixelCount * static_cast<std::size_t>(pixelSize);
    const std::vector<std::uint8_t> raw = splitJoinedChunks(stream, payloadPos, rawLength);

    for (std::size_t i = 0; i < pixelCount && (i + 1) * pixelSize <= raw.size(); ++i) {
        putPixel(framebuffer, i, raw.data() + i * pixelSize);
    }

    return payloadPos + roundUpToFrame(joinedLength(rawLength));
}

// Rebuild a partial update; returns the offset of the following QUERY_STATUS.
std::size_t decodePartialPayload(const std::vector<std::uint8_t>& stream,
                                 std::size_t headerPos, Image& framebuffer) {
    const std::size_t sizeOffset = headerPos + 4;  // after cc ef 69 00
    if (sizeOffset + 3 > stream.size()) {
        return headerPos + kFrame;
    }
    const std::size_t imageSize = (static_cast<std::size_t>(stream[sizeOffset]) << 16) |
                                  (static_cast<std::size_t>(stream[sizeOffset + 1]) << 8) |
                                  stream[sizeOffset + 2];
    const std::size_t payloadPos = headerPos + kFrame;
    if (imageSize < 2 || payloadPos >= stream.size()) {
        return payloadPos;
    }
    const std::size_t rawLength = imageSize - 2;

    std::vector<std::uint8_t> raw;
    if (rawLength > kFrame) {  // the encoder joined the chunks in 249-byte groups
        raw = splitJoinedChunks(stream, payloadPos, rawLength);
    } else {
        const std::size_t available = std::min(rawLength, stream.size() - payloadPos);
        raw.assign(stream.begin() + payloadPos, stream.begin() + payloadPos + available);
    }

    const int pixelSize = choosePartialPixelSize(raw, framebuffer.width(), framebuffer.height());

    // Each row is: 3-byte big-endian index, 2-byte big-endian width, pixels.
    // The index is a linear pixel offset in the wire layout (indexStride is the
    // native portrait pitch), so place the row at that offset.
    std::size_t pos = 0;
    while (pos + 5 <= raw.size()) {
        const std::size_t index = (static_cast<std::size_t>(raw[pos]) << 16) |
                                  (static_cast<std::size_t>(raw[pos + 1]) << 8) | raw[pos + 2];
        const int rowWidth = (static_cast<int>(raw[pos + 3]) << 8) | raw[pos + 4];
        pos += 5;

        const std::size_t rowBytes = static_cast<std::size_t>(rowWidth) * pixelSize;
        if (pos + rowBytes > raw.size()) {
            break;  // truncated payload
        }
        for (int k = 0; k < rowWidth; ++k) {
            putPixel(framebuffer, index + static_cast<std::size_t>(k),
                     raw.data() + pos + static_cast<std::size_t>(k) * pixelSize);
        }
        pos += rowBytes;
    }

    const std::size_t contentLength = rawLength > kFrame ? joinedLength(rawLength) : rawLength;
    return payloadPos + roundUpToFrame(contentLength + 2);  // +2 for the ef 69 trailer
}

// Fixed 250-byte command frames (image-carrying messages are handled separately).
bool isFixedFrameCommand(std::uint8_t command) {
    switch (command) {
        case 0x01:  // HELLO
        case 0x7d:  // OPTIONS
        case 0x84:  // RESTART
        case 0x83:  // TURNOFF / TURNON
        case 0x7b:  // SET_BRIGHTNESS
        case 0x79:  // STOP_VIDEO
        case 0x96:  // STOP_MEDIA
        case 0xcf:  // QUERY_STATUS
        case 0x86:  // PRE_UPDATE_BITMAP
        case 0x2c:  // START_DISPLAY_BITMAP
            return true;
        default:
            return false;
    }
}

}  // namespace

Image decodeRevC(const std::vector<std::uint8_t>& stream, int width, int height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("decodeRevC: width and height must be positive");
    }
    Image framebuffer(width, height, 3);

    std::size_t pos = 0;
    while (pos < stream.size()) {
        const std::uint8_t command = stream[pos];

        // DISPLAY_BITMAP_xINCH is followed by the full-screen SEND_PAYLOAD.
        if (command == 0xc8) {
            if (pos + kFrame >= stream.size()) {
                break;
            }
            pos = decodeFullPayload(stream, pos + kFrame, framebuffer);
            continue;
        }

        // UPDATE_BITMAP header (cc ef 69 00) opens a partial update.
        if (command == 0xcc && pos + 3 <= stream.size() && stream[pos + 1] == 0xef &&
            stream[pos + 2] == 0x69) {
            pos = decodePartialPayload(stream, pos, framebuffer);
            continue;
        }

        if (isFixedFrameCommand(command)) {
            pos += kFrame;
            continue;
        }

        // Unknown byte: stop to avoid desynchronizing.
        break;
    }

    return framebuffer;
}

}  // namespace lcdctl
