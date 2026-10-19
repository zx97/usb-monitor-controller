// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - terminal-like scrolling text area.

#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <string>

#include "lcdctl/font.hpp"
#include "lcdctl/lcd_comm.hpp"
#include "lcdctl/types.hpp"

namespace lcdctl {

// A fixed screen area behaving like a terminal / teleprompter:
//   * lines are appended from the top;
//   * while there is room, only the new line is drawn (cheap);
//   * once the area is full, adding a line shifts everything up by one line,
//     so the top line disappears and the new line appears at the bottom.
class TextConsole {
public:
    struct Options {
        int x = 0;
        int y = 0;
        int width = 320;
        int height = 320;          // visible text area height
        std::string font;          // empty => bundled default font
        int fontSize = 14;
        int lineGap = 2;
        Color textColor{255, 255, 255};
        Color background{0, 0, 0};
    };

    // Throws std::invalid_argument if the area does not fit the display.
    TextConsole(LcdComm& lcd, Options options);

    int lineHeight() const { return lineHeight_; }
    int capacity() const { return capacity_; }
    std::size_t lineCount() const { return lines_.size(); }
    const std::deque<std::string>& lines() const { return lines_; }

    // Blank the whole area and forget the lines.
    void clear();

    // Append a line, scrolling the area up when it is already full.
    void addLine(const std::string& text);

private:
    void draw(int firstLine, int count, int drawHeight);

    LcdComm& lcd_;
    Options options_;
    std::shared_ptr<Font> font_;
    int lineHeight_ = 1;
    int capacity_ = 1;
    std::deque<std::string> lines_;
};

}  // namespace lcdctl
