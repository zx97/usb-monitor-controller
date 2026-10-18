// SPDX-License-Identifier: GPL-3.0-or-later
//
// Protocol parity check: capture the bytes the C++ revision A driver would send
// for a solid red 320x480 image and dump them to a file. See run.sh.

#include <cstdio>
#include <memory>
#include <string>

#include "lcdctl/drivers.hpp"

namespace {

// A transport that records everything written and never blocks.
class CaptureTransport : public lcdctl::Transport {
public:
    bool isOpen() const override { return true; }
    void close() override {}
    bool write(const std::uint8_t* data, std::size_t length) override {
        data_.insert(data_.end(), data, data + length);
        return true;
    }
    std::vector<std::uint8_t> read(std::size_t, int) override { return {}; }
    std::vector<std::uint8_t> readAll() override { return {}; }

    const std::vector<std::uint8_t>& data() const { return data_; }

private:
    std::vector<std::uint8_t> data_;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <output.bin>\n", argv[0]);
        return 2;
    }

    auto transport = std::make_unique<CaptureTransport>();
    CaptureTransport* capture = transport.get();
    lcdctl::LcdCommRevA lcd(std::move(transport), 320, 480);

    lcdctl::Image image(320, 480, 3);
    for (std::size_t i = 0; i < image.size(); i += 3) {
        image.data()[i + 0] = 255;  // R
        image.data()[i + 1] = 0;    // G
        image.data()[i + 2] = 0;    // B
    }
    lcd.displayPILImage(image, 0, 0);

    std::FILE* out = std::fopen(argv[1], "wb");
    if (!out) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::fwrite(capture->data().data(), 1, capture->data().size(), out);
    std::fclose(out);
    std::printf("cpp: %zu bytes\n", capture->data().size());
    return 0;
}
