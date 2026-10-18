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

#include "lcdctl/font.hpp"
#include "lcdctl/image.hpp"
#include "lcdctl/paths.hpp"
#include "lcdctl/transport.hpp"
#include "lcdctl/types.hpp"

namespace lcdctl {

// Options for displayText(). Defaults mirror the Python project.
struct TextStyle {
    int width = 0;                 // 0 => whole display width
    int height = 0;                // 0 => derived from the font size
    std::string font;              // empty => bundled Roboto Mono
    int fontSize = 20;
    Color fontColor{0, 0, 0};
    Color backgroundColor{255, 255, 255};
    std::string backgroundImage;   // empty => solid backgroundColor
    std::string align = "left";    // "left" | "center" | "right"
};

struct RadialStyle {
    int minValue = 0;
    int maxValue = 100;
    double angleStart = 0;
    double angleEnd = 360;
    int angleSep = 5;
    int angleSteps = 10;
    bool clockwise = true;
    bool withText = true;
    std::string text;              // empty => "NN%"
    std::string font;
    int fontSize = 20;
    Color fontColor{0, 0, 0};
    Color barColor{0, 0, 0};
    Color backgroundColor{255, 255, 255};
    std::string backgroundImage;
    Color barBackgroundColor{0, 0, 0};
    bool drawBarBackground = false;
};

class LcdComm {
public:
    LcdComm(std::unique_ptr<Transport> transport, int displayWidth, int displayHeight);
    virtual ~LcdComm() = default;

    int getWidth() const;
    int getHeight() const;
    Size size() const { return {getWidth(), getHeight()}; }
    Orientation orientation() const { return orientation_; }

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
    virtual void displayPILImage(const Image& image, int x = 0, int y = 0,
                                 int imageWidth = 0, int imageHeight = 0) = 0;

    // Optional: backplate RGB LED (rev B "flagship" only).
    virtual void setBackplateLedColor(Color color) { (void)color; }

    // ------------------------------------------------------------------
    // High-level helpers, implemented on top of displayPILImage()
    // ------------------------------------------------------------------
    void displayBitmap(const std::string& path, int x = 0, int y = 0,
                       int width = 0, int height = 0);

    void displayText(const std::string& text, int x, int y,
                     const TextStyle& style = TextStyle{});

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

    const Image& cachedImage(const std::string& path);
    std::shared_ptr<Font> cachedFont(const std::string& path, int size);

    std::unique_ptr<Transport> transport_;
    int displayWidth_;
    int displayHeight_;
    Orientation orientation_ = Orientation::Portrait;

    std::unordered_map<std::string, Image> imageCache_;
    std::unordered_map<std::string, std::shared_ptr<Font>> fontCache_;
};

}  // namespace lcdctl
