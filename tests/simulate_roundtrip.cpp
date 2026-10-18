// SPDX-License-Identifier: GPL-3.0-or-later
//
// Round-trip self test: for every revision with a decoder, drive a simulated
// screen with a known pattern, decode the captured byte stream back into a
// framebuffer and check that it matches the original image. This exercises the
// real wire-protocol encoders without any hardware.

#include <cstdio>
#include <string>
#include <vector>

#include "lcdctl/drivers.hpp"
#include "lcdctl/logging.hpp"
#include "lcdctl/simulate.hpp"

namespace {

using lcdctl::Image;
using lcdctl::Orientation;
using lcdctl::Revision;

// A pixel value that survives the RGB565 round trip exactly.
std::uint8_t quantized(std::uint8_t value, std::uint8_t mask) {
    return static_cast<std::uint8_t>(value & mask);
}

Image makePattern(int width, int height) {
    Image image(width, height, 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t* pixel =
                image.data() + (static_cast<std::size_t>(y) * width + x) * 3;
            pixel[0] = quantized(static_cast<std::uint8_t>(x * 8), 0xF8);
            pixel[1] = quantized(static_cast<std::uint8_t>(y * 4), 0xFC);
            pixel[2] = quantized(static_cast<std::uint8_t>((x + y) * 8), 0xF8);
        }
    }
    return image;
}

struct Case {
    const char* name;
    Revision revision;
    int storedWidth;   // portrait size passed to the driver
    int storedHeight;
    Orientation orientation;  // orientation with no software rotation
};

bool runCase(const Case& testCase) {
    auto simulated = lcdctl::createSimulatedLcd(testCase.revision, testCase.storedWidth,
                                                testCase.storedHeight);
    simulated.lcd->initializeComm();
    simulated.lcd->setOrientation(testCase.orientation);

    const int width = simulated.lcd->getWidth();
    const int height = simulated.lcd->getHeight();

    simulated.transport->clearCaptured();  // drop the initialization traffic

    const Image pattern = makePattern(width, height);
    simulated.lcd->displayPILImage(pattern, 0, 0);

    const Image decoded = lcdctl::decodeCapturedStream(
        testCase.revision, simulated.transport->captured(), width, height);

    if (decoded.width() != width || decoded.height() != height) {
        std::printf("FAIL %-8s size mismatch: got %dx%d expected %dx%d\n", testCase.name,
                    decoded.width(), decoded.height(), width, height);
        return false;
    }
    if (decoded.channels() != 3) {
        std::printf("FAIL %-8s channel count %d\n", testCase.name, decoded.channels());
        return false;
    }

    const Image expected = pattern.convert(3);
    std::size_t mismatches = 0;
    std::size_t firstMismatch = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (decoded.data()[i] != expected.data()[i]) {
            if (mismatches == 0) {
                firstMismatch = i;
            }
            ++mismatches;
        }
    }

    if (mismatches != 0) {
        const std::size_t pixel = firstMismatch / 3;
        std::printf("FAIL %-8s %zu/%zu bytes differ (first at pixel %zu,%zu)\n", testCase.name,
                    mismatches, expected.size(), pixel % width, pixel / width);
        return false;
    }

    std::printf("PASS %-8s %dx%d %s (%zu bytes captured)\n", testCase.name, width, height,
                lcdctl::orientationName(testCase.orientation),
                simulated.transport->captured().size());
    return true;
}

}  // namespace

int main() {
    lcdctl::setLogLevel(lcdctl::LogLevel::Error);

    // Each revision is driven in an orientation where the driver performs no
    // software rotation, so the decoded framebuffer must match the input image.
    const std::vector<Case> cases = {
        {"A", Revision::A, 320, 480, Orientation::Portrait},
        {"B", Revision::B, 320, 480, Orientation::Portrait},
        // Rev C rotates in software for every orientation except landscape.
        {"C", Revision::C, 480, 800, Orientation::Landscape},
        {"D", Revision::D, 320, 480, Orientation::Portrait},
        {"WEACT_A", Revision::WeActA, 320, 480, Orientation::Portrait},
        {"WEACT_B", Revision::WeActB, 80, 160, Orientation::Portrait},
    };

    int failures = 0;
    for (const Case& testCase : cases) {
        try {
            if (!runCase(testCase)) {
                ++failures;
            }
        } catch (const std::exception& error) {
            std::printf("FAIL %-8s exception: %s\n", testCase.name, error.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("All %zu simulation round-trips passed.\n", cases.size());
    } else {
        std::printf("%d/%zu simulation round-trips failed.\n", failures, cases.size());
    }
    return failures == 0 ? 0 : 1;
}
