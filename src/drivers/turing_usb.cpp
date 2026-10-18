// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turing native-USB driver for the 2.8"/4.6"/5.2"/8.0"/8.8"/9.2"/12.3" models.
// These screens expose a vendor protocol over a USB bulk endpoint: every
// command is a 500-byte plaintext packet, DES-CBC encrypted into a 512-byte
// frame (key == IV == "slv3tuzx"), optionally followed by an image payload
// (PNG, or JPEG when the PNG exceeds the device limit).
//
// Ported from library/lcd/lcd_comm_turing_usb.py of
// https://github.com/mathoudebine/turing-smart-screen-python/
// (original code Copyright (C) 2021 Matthieu Houdebine (mathoudebine)).
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include "lcdctl/drivers.hpp"

#include <stdexcept>
#include <string>

#if defined(LCDCTL_WITH_LIBUSB) && defined(LCDCTL_WITH_OPENSSL)

#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include "lcdctl/image.hpp"
#include "lcdctl/logging.hpp"

#include <openssl/evp.h>
#include <openssl/opensslv.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
#endif
#include <png.h>

extern "C" {
#include <jpeglib.h>
}

namespace lcdctl {
namespace {

// The device rejects a single image payload larger than this (Python:
// MAX_CHUNK_BYTES == 1024 * 1024).
constexpr std::size_t kMaxImagePayload = 1024 * 1024;

// Vendor command ids (subset used by this driver).
constexpr int kCmdUploadJpeg = 101;
constexpr int kCmdUploadPng = 102;
constexpr int kCmdSync = 10;
constexpr int kCmdBrightness = 14;

// DES-CBC key; the vendor protocol reuses it as the IV.
const std::uint8_t kDesKey[8] = {'s', 'l', 'v', '3', 't', 'u', 'z', 'x'};

// OpenSSL 3.x moved single-DES into the optional "legacy" provider, so
// EVP_des_cbc() cannot be instantiated until that provider is loaded. Do it
// once per process (the handle is intentionally kept for the process lifetime).
void ensureDesAvailable() {
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
    static ::OSSL_PROVIDER* legacyProvider = []() {
        return ::OSSL_PROVIDER_load(nullptr, "legacy");
    }();
    (void)legacyProvider;
#endif
}

// Milliseconds elapsed since local midnight, as the firmware expects in the
// command header (packet[4..8], little-endian).
std::uint32_t millisecondsSinceLocalMidnight() {
    const std::time_t now = std::time(nullptr);
    std::tm* local = std::localtime(&now);
    if (local == nullptr) {
        return 0;
    }
    std::tm midnight = *local;
    midnight.tm_hour = 0;
    midnight.tm_min = 0;
    midnight.tm_sec = 0;
    midnight.tm_isdst = -1;
    const std::time_t start = std::mktime(&midnight);
    if (start == static_cast<std::time_t>(-1)) {
        return 0;
    }
    const double elapsedSeconds = std::difftime(now, start);
    if (elapsedSeconds <= 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(elapsedSeconds * 1000.0);
}

// -------------------------------------------------------------------------
// JPEG encoding (libjpeg memory destination), ported from
// `_encode_jpeg_under_limit()` of the Python driver.
// -------------------------------------------------------------------------

struct JpegErrorManager {
    jpeg_error_mgr base;
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX] = {0};
};

void jpegErrorExit(j_common_ptr info) {
    auto* manager = reinterpret_cast<JpegErrorManager*>(info->err);
    (*info->err->format_message)(info, manager->message);
    std::longjmp(manager->jump, 1);
}

// subsampling: 0 -> 4:4:4, 1 -> 4:2:2, 2 -> 4:2:0 (Pillow numbering).
void applySubsampling(jpeg_compress_struct& info, int subsampling) {
    int horizontal = 2;
    int vertical = 2;
    if (subsampling == 0) {
        horizontal = 1;
        vertical = 1;
    } else if (subsampling == 1) {
        horizontal = 2;
        vertical = 1;
    }
    info.comp_info[0].h_samp_factor = horizontal;
    info.comp_info[0].v_samp_factor = vertical;
}

std::vector<std::uint8_t> encodeJpegBuffer(const Image& rgb, int quality, int subsampling) {
    jpeg_compress_struct info{};
    JpegErrorManager manager{};
    info.err = jpeg_std_error(&manager.base);
    manager.base.error_exit = jpegErrorExit;

    unsigned char* outBuffer = nullptr;
    unsigned long outSize = 0;

    if (setjmp(manager.jump)) {
        jpeg_destroy_compress(&info);
        if (outBuffer != nullptr) {
            std::free(outBuffer);
        }
        throw std::runtime_error(std::string("JPEG encode error: ") + manager.message);
    }

    jpeg_create_compress(&info);
    jpeg_mem_dest(&info, &outBuffer, &outSize);
    info.image_width = static_cast<JDIMENSION>(rgb.width());
    info.image_height = static_cast<JDIMENSION>(rgb.height());
    info.input_components = 3;
    info.in_color_space = JCS_RGB;
    jpeg_set_defaults(&info);
    applySubsampling(info, subsampling);
    jpeg_set_quality(&info, quality, TRUE);
    jpeg_start_compress(&info, TRUE);

    const int rowBytes = rgb.width() * 3;
    while (info.next_scanline < info.image_height) {
        JSAMPROW row[1] = {
            const_cast<JSAMPROW>(rgb.data() + static_cast<std::size_t>(info.next_scanline) * rowBytes)};
        jpeg_write_scanlines(&info, row, 1);
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);

    std::vector<std::uint8_t> out(outBuffer, outBuffer + outSize);
    std::free(outBuffer);
    return out;
}

}  // namespace

LcdCommTuringUSB::LcdCommTuringUSB() : LcdComm(nullptr, 480, 480) {
    auto transport = std::make_unique<UsbTransport>();

    std::uint16_t pid = 0;
    int width = 0;
    int height = 0;
    if (!transport->openFirst(pid, width, height)) {
        throw std::runtime_error("Turing native-USB screen not found");
    }

    productId_ = pid;
    displayWidth_ = width;
    displayHeight_ = height;
    transport_ = std::move(transport);

    // The screen keeps its full state on our side; it is pasted into and then
    // sent as a whole after every partial update.
    currentState_ = Image(getWidth(), getHeight(), 4, 0);
}

LcdCommTuringUSB::~LcdCommTuringUSB() = default;

bool LcdCommTuringUSB::isAvailable() { return UsbTransport::available(); }

void LcdCommTuringUSB::initializeComm() { sendSyncCommand(); }

// These models intentionally do not implement a reset (see the Python driver).
void LcdCommTuringUSB::reset() {}

void LcdCommTuringUSB::clear() { clearImage(); }

void LcdCommTuringUSB::screenOff() {
    // No dedicated "screen off" command: blank the panel and drop the backlight.
    clear();
    setBrightness(0);
}

void LcdCommTuringUSB::screenOn() { setBrightness(25); }

void LcdCommTuringUSB::setBrightness(int level) {
    if (level < 0 || level > 100) {
        throw std::invalid_argument("brightness level must be within [0, 100]");
    }
    const int converted = static_cast<int>((level / 100.0) * 102);
    sendBrightnessCommand(converted);
}

void LcdCommTuringUSB::setOrientation(Orientation orientation) {
    orientation_ = orientation;
    // Recreate the state with the new width/height now that orientation changed.
    currentState_ = Image(getWidth(), getHeight(), 4, 0);
}

void LcdCommTuringUSB::displayPILImage(const Image& image, int x, int y, int imageWidth,
                                       int imageHeight) {
    int targetWidth = imageWidth;
    int targetHeight = imageHeight;

    if (targetHeight == 0) {
        targetHeight = image.height();
    }
    if (targetWidth == 0) {
        targetWidth = image.width();
    }
    if (image.height() > getHeight()) {
        targetHeight = getHeight();
    }
    if (image.width() > getWidth()) {
        targetWidth = getWidth();
    }

    Image cropped = image;
    if (targetWidth != image.width() || targetHeight != image.height()) {
        cropped = image.crop(0, 0, targetWidth, targetHeight);
    }

    // Paste the update over the retained full-screen state.
    currentState_.paste(cropped, x, y);

    // Everything sent to the screen is portrait: rotate the state accordingly.
    Image base;
    switch (orientation_) {
        case Orientation::Landscape:
            base = currentState_.rotate(270);
            break;
        case Orientation::ReverseLandscape:
            base = currentState_.rotate(90);
            break;
        case Orientation::Portrait:
            base = currentState_.rotate(180);
            break;
        case Orientation::ReversePortrait:
            base = currentState_;
            break;
    }

    // Prefer PNG; fall back to a quality-reduced JPEG when it is too large.
    const std::vector<std::uint8_t> png = encodePng(base);
    if (png.size() <= kMaxImagePayload) {
        sendImage(png);
    } else {
        const std::vector<std::uint8_t> jpeg = encodeJpegUnderLimit(base, kMaxImagePayload, 90);
        sendJpeg(jpeg);
    }
}

// ---------------------------------------------------------------------------
// Vendor protocol helpers
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> LcdCommTuringUSB::buildCommandPacketHeader(int commandId) const {
    std::vector<std::uint8_t> packet(500, 0);
    packet[0] = static_cast<std::uint8_t>(commandId & 0xFF);
    packet[2] = 0x1A;
    packet[3] = 0x6D;

    const std::uint32_t timestamp = millisecondsSinceLocalMidnight();
    packet[4] = static_cast<std::uint8_t>(timestamp & 0xFF);
    packet[5] = static_cast<std::uint8_t>((timestamp >> 8) & 0xFF);
    packet[6] = static_cast<std::uint8_t>((timestamp >> 16) & 0xFF);
    packet[7] = static_cast<std::uint8_t>((timestamp >> 24) & 0xFF);
    return packet;
}

std::vector<std::uint8_t> LcdCommTuringUSB::encryptCommandPacket(
    const std::vector<std::uint8_t>& data) const {
    ensureDesAvailable();

    // Zero-pad the plaintext to a whole number of 8-byte DES blocks.
    const std::size_t paddedLength = ((data.size() + 7) / 8) * 8;
    std::vector<std::uint8_t> plaintext(paddedLength, 0);
    if (!data.empty()) {
        std::memcpy(plaintext.data(), data.data(), data.size());
    }

    std::vector<std::uint8_t> finalPacket(512, 0);

    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        throw std::runtime_error("cannot allocate OpenSSL cipher context");
    }

    int updated = 0;
    int finalized = 0;
    const bool ok =
        EVP_EncryptInit_ex(context, EVP_des_cbc(), nullptr, kDesKey, kDesKey) == 1 &&
        // Padding is already applied by hand, disable the automatic one.
        EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
        EVP_EncryptUpdate(context, finalPacket.data(), &updated, plaintext.data(),
                          static_cast<int>(plaintext.size())) == 1 &&
        EVP_EncryptFinal_ex(context, finalPacket.data() + updated, &finalized) == 1;
    EVP_CIPHER_CTX_free(context);

    if (!ok) {
        throw std::runtime_error("OpenSSL DES-CBC encryption failed");
    }
    (void)finalized;

    // Vendor trailer.
    finalPacket[510] = 161;
    finalPacket[511] = 26;
    return finalPacket;
}

std::vector<std::uint8_t> LcdCommTuringUSB::writeToDevice(
    const std::vector<std::uint8_t>& packet) {
    if (!transport_ || !transport_->isOpen()) {
        throw std::runtime_error("transport is not open");
    }
    if (!transport_->write(packet)) {
        LCDCTL_ERROR << "USB write error";
        return {};
    }

    // The reply may be empty when the command does not answer in time.
    std::vector<std::uint8_t> response = transport_->read(512, 2000);
    // Flush whatever remains queued on the IN endpoint.
    transport_->readAll();
    return response;
}

void LcdCommTuringUSB::sendSyncCommand() {
    LCDCTL_DEBUG << "Sending sync command (ID " << kCmdSync << ")";
    writeToDevice(encryptCommandPacket(buildCommandPacketHeader(kCmdSync)));
}

void LcdCommTuringUSB::sendBrightnessCommand(int brightness) {
    std::vector<std::uint8_t> packet = buildCommandPacketHeader(kCmdBrightness);
    packet[8] = static_cast<std::uint8_t>(brightness & 0xFF);
    writeToDevice(encryptCommandPacket(packet));
}

void LcdCommTuringUSB::sendImage(const std::vector<std::uint8_t>& pngData) {
    const std::size_t imageSize = pngData.size();

    std::vector<std::uint8_t> packet = buildCommandPacketHeader(kCmdUploadPng);
    packet[8] = static_cast<std::uint8_t>((imageSize >> 24) & 0xFF);
    packet[9] = static_cast<std::uint8_t>((imageSize >> 16) & 0xFF);
    packet[10] = static_cast<std::uint8_t>((imageSize >> 8) & 0xFF);
    packet[11] = static_cast<std::uint8_t>(imageSize & 0xFF);

    std::vector<std::uint8_t> payload = encryptCommandPacket(packet);
    payload.insert(payload.end(), pngData.begin(), pngData.end());
    writeToDevice(payload);
}

void LcdCommTuringUSB::sendJpeg(const std::vector<std::uint8_t>& jpegData) {
    const std::size_t imageSize = jpegData.size();

    std::vector<std::uint8_t> packet = buildCommandPacketHeader(kCmdUploadJpeg);
    packet[8] = static_cast<std::uint8_t>((imageSize >> 24) & 0xFF);
    packet[9] = static_cast<std::uint8_t>((imageSize >> 16) & 0xFF);
    packet[10] = static_cast<std::uint8_t>((imageSize >> 8) & 0xFF);
    packet[11] = static_cast<std::uint8_t>(imageSize & 0xFF);

    std::vector<std::uint8_t> payload = encryptCommandPacket(packet);
    payload.insert(payload.end(), jpegData.begin(), jpegData.end());
    writeToDevice(payload);
}

// Hard-coded "blank" PNG uploaded by the Python `clear_image()` (command 102).
void LcdCommTuringUSB::clearImage() {
    std::vector<std::uint8_t> image = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x01, 0xe0, 0x00, 0x00, 0x07, 0x80, 0x08, 0x06, 0x00, 0x00, 0x00, 0x16,
        0xf0, 0x84, 0xf5, 0x00, 0x00, 0x00, 0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xae, 0xce, 0x1c,
        0xe9, 0x00, 0x00, 0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc,
        0x61, 0x05, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0e, 0xc3, 0x00,
        0x00, 0x0e, 0xc3, 0x01, 0xc7, 0x6f, 0xa8, 0x64, 0x00, 0x00, 0x0e, 0x0c, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x5e, 0xed, 0xc1, 0x01, 0x0d, 0x00, 0x00, 0x00, 0xc2, 0xa0, 0xf7, 0x4f, 0x6d,
        0x0f, 0x07, 0x14, 0x00, 0x00, 0x00, 0x00,
    };
    // The Python payload is zero-padded to the 480x1920 IDAT size.
    image.insert(image.end(), 3568, 0x00);

    const std::uint8_t trailer[] = {
        0x00, 0xf0, 0x66, 0x4a, 0xc8, 0x00, 0x01, 0x11, 0x9d, 0x82, 0x0a, 0x00,
        0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
    image.insert(image.end(), trailer, trailer + sizeof(trailer));

    const std::size_t imageSize = image.size();

    std::vector<std::uint8_t> packet = buildCommandPacketHeader(kCmdUploadPng);
    packet[8] = static_cast<std::uint8_t>((imageSize >> 24) & 0xFF);
    packet[9] = static_cast<std::uint8_t>((imageSize >> 16) & 0xFF);
    packet[10] = static_cast<std::uint8_t>((imageSize >> 8) & 0xFF);
    packet[11] = static_cast<std::uint8_t>(imageSize & 0xFF);

    std::vector<std::uint8_t> payload = encryptCommandPacket(packet);
    payload.insert(payload.end(), image.begin(), image.end());
    writeToDevice(payload);
}

std::vector<std::uint8_t> LcdCommTuringUSB::encodePng(const Image& image) const {
    const Image rgba = image.convert(4);

    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = static_cast<png_uint_32>(rgba.width());
    png.height = static_cast<png_uint_32>(rgba.height());
    png.format = PNG_FORMAT_RGBA;

    // First pass with a NULL buffer: libpng reports the required output size.
    png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&png, nullptr, &size, 0, rgba.data(), 0, nullptr) ||
        size == 0) {
        throw std::runtime_error("PNG encoding failed");
    }

    std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
    png_alloc_size_t written = size;
    if (!png_image_write_to_memory(&png, out.data(), &written, 0, rgba.data(), 0, nullptr)) {
        throw std::runtime_error("PNG encoding failed");
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

std::vector<std::uint8_t> LcdCommTuringUSB::encodeJpegUnderLimit(const Image& image,
                                                                 std::size_t maxBytes,
                                                                 int quality) const {
    const Image rgb = image.convert(3);

    // Try 4:2:0, then 4:2:2, then 4:4:4, lowering quality until the payload fits.
    const int subsamplings[] = {2, 1, 0};

    std::vector<std::uint8_t> best;
    for (const int subsampling : subsamplings) {
        int currentQuality = quality;
        while (currentQuality >= 1) {
            std::vector<std::uint8_t> data =
                encodeJpegBuffer(rgb, currentQuality, subsampling);
            if (best.empty() || data.size() < best.size()) {
                best = data;
            }
            if (data.size() <= maxBytes) {
                return data;
            }
            currentQuality = (currentQuality > 10) ? currentQuality - 5 : currentQuality - 1;
        }
    }

    throw std::runtime_error("could not transcode JPEG under the image size limit (" +
                             std::to_string(best.size()) + " > " + std::to_string(maxBytes) + ")");
}

}  // namespace lcdctl

#else  // !(LIBUSB && OPENSSL)

// ---------------------------------------------------------------------------
// Stub build: native USB is unavailable. The file must still compile and link
// so the rest of the library keeps building without libusb/OpenSSL.
// ---------------------------------------------------------------------------

namespace lcdctl {

bool LcdCommTuringUSB::isAvailable() { return false; }

LcdCommTuringUSB::LcdCommTuringUSB() : LcdComm(nullptr, 480, 480) {
    throw std::runtime_error("native USB support not built");
}

LcdCommTuringUSB::~LcdCommTuringUSB() = default;

void LcdCommTuringUSB::initializeComm() {}

void LcdCommTuringUSB::reset() {}

void LcdCommTuringUSB::clear() {}

void LcdCommTuringUSB::screenOff() {}

void LcdCommTuringUSB::screenOn() {}

void LcdCommTuringUSB::setBrightness(int /*level*/) {}

void LcdCommTuringUSB::setOrientation(Orientation /*orientation*/) {}

void LcdCommTuringUSB::displayPILImage(const Image& /*image*/, int /*x*/, int /*y*/,
                                       int /*imageWidth*/, int /*imageHeight*/) {}

}  // namespace lcdctl

#endif  // LCDCTL_WITH_LIBUSB && LCDCTL_WITH_OPENSSL
