// SPDX-License-Identifier: GPL-3.0-or-later
//
// Round-trip self test: for every revision with a decoder, drive a simulated
// screen with a known pattern, decode the captured byte stream back into a
// framebuffer and check that it matches the original image. This exercises the
// real wire-protocol encoders without any hardware.

#include <cstdio>
#include <string>
#include <vector>

#include "lcdctl/console.hpp"
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

bool isUniform(const Image& image, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const Image rgb = image.convert(3);
    for (std::size_t i = 0; i + 2 < rgb.size(); i += 3) {
        if (rgb.data()[i] != r || rgb.data()[i + 1] != g || rgb.data()[i + 2] != b) {
            return false;
        }
    }
    return true;
}

// clear() must blank the whole screen, whatever the revision.
bool runClearCase(const Case& testCase, lcdctl::Color expected) {
    // Revisions that transmit RGB565 quantize the colour (255 -> 248/252/248);
    // revision C sends 8-bit BGR/BGRA and keeps the exact value.
    if (testCase.revision != Revision::C) {
        expected.r = static_cast<std::uint8_t>((expected.r >> 3) << 3);
        expected.g = static_cast<std::uint8_t>((expected.g >> 2) << 2);
        expected.b = static_cast<std::uint8_t>((expected.b >> 3) << 3);
    }

    auto simulated = lcdctl::createSimulatedLcd(testCase.revision, testCase.storedWidth,
                                                testCase.storedHeight);
    simulated.lcd->initializeComm();
    simulated.lcd->setOrientation(Orientation::Portrait);
    simulated.transport->clearCaptured();

    simulated.lcd->clear();

    const int width = simulated.lcd->getWidth();
    const int height = simulated.lcd->getHeight();
    const Image frame = lcdctl::decodeCapturedStream(testCase.revision,
                                                    simulated.transport->captured(), width, height);

    if (!isUniform(frame, expected.r, expected.g, expected.b)) {
        std::printf("FAIL %-8s clear() did not blank the screen\n", testCase.name);
        return false;
    }
    std::printf("PASS %-8s clear() -> blank %dx%d\n", testCase.name, width, height);
    return true;
}

// The scrolling console: once full, adding a line drops the oldest one.
bool runConsoleCase() {
    auto simulated = lcdctl::createSimulatedLcd(Revision::A, 320, 480);
    simulated.lcd->initializeComm();
    simulated.lcd->setOrientation(Orientation::Portrait);

    lcdctl::TextConsole::Options options;
    options.x = 0;
    options.y = 0;
    options.width = 200;
    options.height = 100;
    options.fontSize = 12;
    lcdctl::TextConsole console(*simulated.lcd, options);
    console.clear();
    simulated.transport->clearCaptured();

    const int capacity = console.capacity();
    if (capacity < 2) {
        std::printf("FAIL console capacity=%d is too small for the test\n", capacity);
        return false;
    }
    const int total = capacity + 3;
    for (int i = 0; i < total; ++i) {
        console.addLine("line " + std::to_string(i));
    }

    const std::string expectedFirst = "line " + std::to_string(total - capacity);
    const std::string expectedLast = "line " + std::to_string(total - 1);
    if (static_cast<int>(console.lineCount()) != capacity ||
        console.lines().front() != expectedFirst || console.lines().back() != expectedLast) {
        std::printf("FAIL console scroll: %zu lines, first='%s' (expected '%s')\n",
                    console.lineCount(), console.lines().front().c_str(), expectedFirst.c_str());
        return false;
    }

    // The last frame must actually contain text (some bright pixels).
    const Image frame = lcdctl::decodeCapturedStream(Revision::A, simulated.transport->captured(),
                                                     320, 480);
    std::size_t bright = 0;
    const Image rgb = frame.convert(3);
    for (std::size_t i = 0; i + 2 < rgb.size(); i += 3) {
        if (rgb.data()[i] + rgb.data()[i + 1] + rgb.data()[i + 2] > 300) {
            ++bright;
        }
    }
    if (bright == 0) {
        std::printf("FAIL console: decoded frame is blank\n");
        return false;
    }

    std::printf("PASS console  %d lines visible, scroll drops the oldest, %zu bright px\n", capacity,
                bright);
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
    int checks = 0;
    for (const Case& testCase : cases) {
        ++checks;
        try {
            if (!runCase(testCase)) {
                ++failures;
            }
        } catch (const std::exception& error) {
            std::printf("FAIL %-8s exception: %s\n", testCase.name, error.what());
            ++failures;
        }
    }

    // clear() must blank the screen (white for A/B/C/D, black for WeAct).
    for (const Case& testCase : cases) {
        ++checks;
        const bool weact = testCase.revision == Revision::WeActA ||
                           testCase.revision == Revision::WeActB;
        const lcdctl::Color expected = weact ? lcdctl::Color{0, 0, 0}
                                             : lcdctl::Color{255, 255, 255};
        try {
            if (!runClearCase(testCase, expected)) {
                ++failures;
            }
        } catch (const std::exception& error) {
            std::printf("FAIL %-8s clear exception: %s\n", testCase.name, error.what());
            ++failures;
        }
    }

    // Scrolling console behaviour.
    ++checks;
    try {
        if (!runConsoleCase()) {
            ++failures;
        }
    } catch (const std::exception& error) {
        std::printf("FAIL console exception: %s\n", error.what());
        ++failures;
    }

    if (failures == 0) {
        std::printf("All %d simulation checks passed.\n", checks);
    } else {
        std::printf("%d/%d simulation checks failed.\n", failures, checks);
    }
    return failures == 0 ? 0 : 1;
}
