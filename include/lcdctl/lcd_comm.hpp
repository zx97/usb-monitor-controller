// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - abstract display + high-level drawing helpers.
//
// Ported from library/lcd/lcd_comm.py of
// https://github.com/mathoudebine/turing-smart-screen-python/

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "lcdctl/draw.hpp"
#include "lcdctl/font.hpp"
#include "lcdctl/image.hpp"
#include "lcdctl/paths.hpp"
#include "lcdctl/transport.hpp"
#include "lcdctl/types.hpp"

namespace lcdctl {

// ---------------------------------------------------------------------------
// Text / radial style structures live in draw.hpp (shared with the offscreen
// renderer); they are re-exported here for source compatibility.
// ---------------------------------------------------------------------------
using TextStyle = ::lcdctl::TextStyle;
using RadialStyle = ::lcdctl::RadialStyle;

class LcdComm : public DrawSurface {
public:
    LcdComm(std::unique_ptr<Transport> transport, int displayWidth, int displayHeight);
    virtual ~LcdComm() = default;

    int getWidth() const;
    int getHeight() const;
    Size size() const override { return {getWidth(), getHeight()}; }
    Orientation orientation() const { return orientation_; }

    // DrawSurface: the on-screen surface forwards every crop to the transport.
    const Image& cachedImage(const std::string& path) override;
    std::shared_ptr<Font> cachedFont(const std::string& path, int size) override;

    // ------------------------------------------------------------------
    // Revision-specific interface
    // ------------------------------------------------------------------
    virtual void initializeComm() = 0;
    virtual void reset() = 0;
    virtual void clear() = 0;
    virtual void screenOff() = 0;
    virtual void screenOn() = 0;
    virtual void setBrightness(int level) = 0;
    virtual void setOrientation(Orientation orientation) = 0;

    // DrawSurface entry point: forward to the revision-specific 4-argument
    // form (imageWidth/imageHeight are used by a few revisions for scaling).
    void displayPILImage(const Image& image, int x, int y) override {
        displayPILImage(image, x, y, 0, 0);
    }
    virtual void displayPILImage(const Image& image, int x, int y, int imageWidth,
                                 int imageHeight) = 0;

    // Optional: backplate RGB LED (rev B "flagship" only).
    virtual void setBackplateLedColor(Color color) { (void)color; }

    // ------------------------------------------------------------------
    // High-level helpers, implemented on top of displayPILImage()
    // ------------------------------------------------------------------
    void displayBitmap(const std::string& path, int x = 0, int y = 0,
                       int width = 0, int height = 0);

    // Paint the whole screen with a single colour (does not use any native
    // "clear" command, so it works on revisions that ignore it).
    void fillScreen(Color color);

    void displayText(const std::string& text, int x, int y,
                     const TextStyle& style = TextStyle{});

    // Same primitives, style-structure flavour (used by the theme renderer).
    Rect displayTextStyled(const std::string& text, int x, int y, const TextStyle& style) {
        return drawText(*this, text, x, y, style);
    }
    Rect displayProgressBarStyled(int x, int y, int width, int height, int value,
                                  const ProgressBarStyle& style) {
        return drawProgressBar(*this, x, y, width, height, value, style);
    }
    Rect displayLineGraphStyled(int x, int y, int width, int height,
                                const std::vector<float>& values,
                                const LineGraphStyle& style) {
        return drawLineGraph(*this, x, y, width, height, values, style);
    }
    Rect displayRadialProgressBarStyled(int xc, int yc, int radius, int barWidth, int value,
                                        const RadialStyle& style) {
        return drawRadialProgressBar(*this, xc, yc, radius, barWidth, value, style);
    }

    void displayProgressBar(int x, int y, int width, int height,
                            int minValue, int maxValue, int value,
                            Color barColor = {0, 0, 0}, bool barOutline = true,
                            Color backgroundColor = {255, 255, 255},
                            const std::string& backgroundImage = "",
                            bool reverseDirection = false);

    void displayLineGraph(int x, int y, int width, int height,
                          const std::vector<float>& values,
                          float minValue = 0, float maxValue = 100,
                          bool autoscale = false,
                          Color lineColor = {0, 0, 0}, int lineWidth = 2,
                          bool graphAxis = true, Color axisColor = {0, 0, 0},
                          Color backgroundColor = {255, 255, 255},
                          const std::string& backgroundImage = "");

    void displayRadialProgressBar(int xc, int yc, int radius, int barWidth,
                                  int value, const RadialStyle& style = RadialStyle{});

protected:
    // Serial / USB primitives forwarded to the transport.
    void writeData(const std::vector<std::uint8_t>& data);
    void writeLine(const std::vector<std::uint8_t>& line);
    void sendLine(const std::vector<std::uint8_t>& line);
    std::vector<std::uint8_t> readData(std::size_t size);
    void flushInput();

    std::unique_ptr<Transport> transport_;
    int displayWidth_;
    int displayHeight_;
    Orientation orientation_ = Orientation::Portrait;

    std::unordered_map<std::string, Image> imageCache_;
    std::unordered_map<std::string, std::shared_ptr<Font>> fontCache_;
};

}  // namespace lcdctl
