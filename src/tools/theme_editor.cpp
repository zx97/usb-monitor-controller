// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - lcd-theme-editor: a KDE/Qt6 visual editor for
// res/themes/<name>/theme.yaml.
//
// The preview is produced by the very same ThemeRenderer as the daemon and the
// screen drivers, into an OffscreenCanvas. Moving a widget therefore shows the
// pixels the panel will receive, not an approximation.
//
// Usage: lcd-theme-editor [themes-root] [fonts-root] [theme-name]

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QTransform>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <optional>

#include "lcdctl/sensors.hpp"
#include "lcdctl/settings.hpp"
#include "lcdctl/theme.hpp"
#include "lcdctl/theme_render.hpp"

namespace {

namespace fs = std::filesystem;

QImage toQImage(const lcdctl::Image& image) {
    if (image.empty()) {
        return {};
    }
    QImage result(image.width(), image.height(), QImage::Format_RGB888);
    for (int y = 0; y < image.height(); ++y) {
        const std::uint8_t* source =
            image.data() + static_cast<std::size_t>(y) * image.width() * image.channels();
        std::uint8_t* target = result.scanLine(y);
        if (image.channels() == 3) {
            std::copy(source, source + image.width() * 3, target);
        } else {
            for (int x = 0; x < image.width(); ++x) {
                target[x * 3 + 0] = source[x * 4 + 0];
                target[x * 3 + 1] = source[x * 4 + 1];
                target[x * 3 + 2] = source[x * 4 + 2];
            }
        }
    }
    return result;
}

// Widget the theme is edited on: shows the rendered theme and lets the user drag
// the selected item around.
class PreviewWidget : public QWidget {
public:
    explicit PreviewWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(240, 320);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
    }

    void setRender(QImage image) {
        image_ = std::move(image);
        zoom_ = std::min(width() / std::max(1, image_.width()),
                         height() / std::max(1, image_.height()));
        zoom_ = std::max(1, zoom_);
        updateGeometry();
        update();
    }

    const QImage& render() const { return image_; }

    void setItems(const std::vector<lcdctl::ThemeItem>* items) { items_ = items; }
    void setSelection(const QString& key) {
        selection_ = key;
        update();
    }

    std::function<void(const QString&, int, int)> onMoved;
    std::function<void(const QString&)> onSelected;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(24, 26, 32));
        if (image_.isNull()) {
            painter.setPen(Qt::white);
            painter.drawText(rect(), Qt::AlignCenter, "No theme loaded");
            return;
        }
        const int offsetX = (width() - image_.width() * zoom_) / 2;
        const int offsetY = (height() - image_.height() * zoom_) / 2;
        origin_ = QPoint(offsetX, offsetY);
        painter.drawImage(QRect(offsetX, offsetY, image_.width() * zoom_, image_.height() * zoom_),
                          image_);

        painter.setRenderHint(QPainter::Antialiasing, true);
        if (items_) {
            for (const lcdctl::ThemeItem& item : *items_) {
                if (!item.show) {
                    continue;
                }
                const lcdctl::Rect bounds = item.bounds();
                if (bounds.width <= 0 || bounds.height <= 0) {
                    continue;
                }
                const bool selected = QString::fromStdString(item.key) == selection_;
                painter.setPen(QPen(selected ? QColor(97, 190, 255) : QColor(255, 255, 255, 90),
                                    selected ? 2 : 1, selected ? Qt::SolidLine : Qt::DashLine));
                // Never use a ternary with Qt::NoBrush: it converts to
                // QColor(Qt::color0), i.e. a solid black fill.
                if (selected) {
                    painter.setBrush(QColor(97, 190, 255, 40));
                } else {
                    painter.setBrush(Qt::NoBrush);
                }
                painter.drawRect(QRect(offsetX + bounds.x * zoom_, offsetY + bounds.y * zoom_,
                                       bounds.width * zoom_, bounds.height * zoom_));
            }
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (!items_ || image_.isNull()) {
            return;
        }
        const QPoint position((event->position().x() - origin_.x()) / zoom_,
                              (event->position().y() - origin_.y()) / zoom_);
        const lcdctl::ThemeItem* hit = nullptr;
        for (const lcdctl::ThemeItem& item : *items_) {
            if (!item.show) {
                continue;
            }
            const lcdctl::Rect bounds = item.bounds();
            if (position.x() >= bounds.x && position.y() >= bounds.y &&
                position.x() < bounds.right() && position.y() < bounds.bottom()) {
                hit = &item;  // last match wins: topmost item
            }
        }
        if (hit) {
            selection_ = QString::fromStdString(hit->key);
            dragging_ = true;
            dragOrigin_ = position;
            update();
            if (onSelected) {
                onSelected(QString::fromStdString(hit->key));
            }
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!dragging_ || !items_ || image_.isNull()) {
            return;
        }
        const QPoint position((event->position().x() - origin_.x()) / zoom_,
                              (event->position().y() - origin_.y()) / zoom_);
        const QPoint delta = position - dragOrigin_;
        if (delta.manhattanLength() == 0 || delta.x() == 0 && delta.y() == 0) {
            return;
        }
        dragOrigin_ = position;
        if (onMoved) {
            emit onMoved(selection_, delta.x(), delta.y());
        }
    }

    void mouseReleaseEvent(QMouseEvent*) override { dragging_ = false; }

private:
    QImage image_;
    const std::vector<lcdctl::ThemeItem>* items_ = nullptr;
    QString selection_;
    QPoint origin_;
    QPoint dragOrigin_;
    int zoom_ = 1;
    bool dragging_ = false;
};

class ThemeEditor : public QMainWindow {
public:
    ThemeEditor(lcdctl::Settings settings, std::string themeRoot, std::string themeName);
    ~ThemeEditor() override = default;

private:
    void buildUi();
    void clearProperties();
    void loadThemeList();
    bool loadTheme(const std::string& name, bool keepSelection = false);
    void refreshPreview();
    void refreshItemList();
    void showItem(const std::string& key);
    void showStaticText(const std::string& name);
    void applyToItem(const std::function<void(lcdctl::ThemeItem&)>& mutate);
    void save();
    void exportPng();
    void selectItem(const QString& key);
    void moveSelection(int dx, int dy);

    lcdctl::Settings settings_;
    std::string themeRoot_;
    std::string themeName_;
    lcdctl::Theme theme_;
    lcdctl::Metrics metrics_;

    PreviewWidget* preview_ = nullptr;
    QListWidget* itemList_ = nullptr;
    QListWidget* textList_ = nullptr;
    QComboBox* themeCombo_ = nullptr;
    QComboBox* orientationCombo_ = nullptr;
    QComboBox* rotationCombo_ = nullptr;
    bool loading_ = false;
    QGroupBox* properties_ = nullptr;
    QFormLayout* propertiesLayout_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    std::optional<std::string> selectedKey_;
    std::string selectedText_;
};

ThemeEditor::ThemeEditor(lcdctl::Settings settings, std::string themeRoot, std::string themeName)
    : settings_(std::move(settings)), themeRoot_(std::move(themeRoot)),
      themeName_(std::move(themeName)) {
    buildUi();
    loadThemeList();
    if (!themeName_.empty()) {
        loadTheme(themeName_);
    }
}

void ThemeEditor::buildUi() {
    setWindowTitle("USB LCD theme editor");
    resize(1280, 860);

    auto* toolbar = addToolBar("Theme");
    toolbar->setMovable(false);
    themeCombo_ = new QComboBox(this);
    themeCombo_->setMinimumWidth(260);
    toolbar->addWidget(new QLabel("Theme: ", this));
    toolbar->addWidget(themeCombo_);
    connect(themeCombo_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        if (!text.isEmpty()) {
            loadTheme(text.toStdString());
        }
    });

    // Theme orientation (layout aspect). The physical rotation applied to the
    // panel is chosen in lcd-monitor or with `lcd-theme preview --orientation`.
    orientationCombo_ = new QComboBox(this);
    orientationCombo_->addItem("Portrait", 0);
    orientationCombo_->addItem("Landscape", 2);
    toolbar->addWidget(new QLabel("  Layout: ", this));
    toolbar->addWidget(orientationCombo_);
    connect(orientationCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (loading_ || index < 0) {
            return;
        }
        theme_.orientation =
            static_cast<lcdctl::Orientation>(orientationCombo_->itemData(index).toInt());
        refreshPreview();
        statusLabel_->setText("Modified (unsaved)");
    });

    // Preview rotation only: shows what the eye sees when the screen is mounted
    // on another side. Does not change the theme.
    rotationCombo_ = new QComboBox(this);
    for (int degrees : {0, 90, 180, 270}) {
        rotationCombo_->addItem(QString("%1\u00b0").arg(degrees), degrees);
    }
    toolbar->addWidget(new QLabel("  Preview rotation: ", this));
    toolbar->addWidget(rotationCombo_);
    connect(rotationCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        refreshPreview();
    });

    auto* reloadAction = toolbar->addAction("Reload");
    connect(reloadAction, &QAction::triggered, this, [this] { loadTheme(themeName_, true); });

    auto* saveAction = toolbar->addAction("Save");
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, [this] { save(); });

    auto* pngAction = toolbar->addAction("Export PNG\u2026");
    connect(pngAction, &QAction::triggered, this, [this] { exportPng(); });

    auto* splitter = new QSplitter(this);
    setCentralWidget(splitter);

    auto* leftPanel = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(8, 8, 8, 8);
    leftLayout->addWidget(new QLabel("Stats items", leftPanel));
    itemList_ = new QListWidget(leftPanel);
    leftLayout->addWidget(itemList_, 3);
    leftLayout->addWidget(new QLabel("Static texts", leftPanel));
    textList_ = new QListWidget(leftPanel);
    leftLayout->addWidget(textList_, 1);

    connect(itemList_, &QListWidget::itemSelectionChanged, this, [this] {
        const auto selected = itemList_->selectedItems();
        if (!selected.isEmpty()) {
            showItem(selected.first()->data(Qt::UserRole).toString().toStdString());
        }
    });
    connect(textList_, &QListWidget::itemSelectionChanged, this, [this] {
        const auto selected = textList_->selectedItems();
        if (!selected.isEmpty()) {
            showStaticText(selected.first()->data(Qt::UserRole).toString().toStdString());
        }
    });

    preview_ = new PreviewWidget(splitter);
    preview_->onMoved = [this](const QString& key, int dx, int dy) {
        selectedKey_ = key.toStdString();
        applyToItem([dx, dy](lcdctl::ThemeItem& item) {
            switch (item.kind) {
                case lcdctl::ThemeItem::Kind::Text: item.text.x += dx; item.text.y += dy; break;
                case lcdctl::ThemeItem::Kind::ProgressBar: item.bar.x += dx; item.bar.y += dy; break;
                case lcdctl::ThemeItem::Kind::LineGraph: item.graph.x += dx; item.graph.y += dy; break;
                case lcdctl::ThemeItem::Kind::Radial: item.radial.x += dx; item.radial.y += dy; break;
            }
        });
    };
    preview_->onSelected = [this](const QString& key) { selectItem(key); };

    auto* scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    properties_ = new QGroupBox("Properties", scroll);
    propertiesLayout_ = new QFormLayout(properties_);
    propertiesLayout_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    scroll->setWidget(properties_);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setStretchFactor(2, 1);

    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_);
}

void ThemeEditor::loadThemeList() {
    themeCombo_->blockSignals(true);
    themeCombo_->clear();
    for (const lcdctl::ThemeInfo& info : lcdctl::listThemes(themeRoot_)) {
        themeCombo_->addItem(QString::fromStdString(info.name));
    }
    themeCombo_->blockSignals(false);
}

bool ThemeEditor::loadTheme(const std::string& name, bool keepSelection) {
    try {
        theme_ = lcdctl::Theme::loadByName(name, themeRoot_, settings_.fontsRoot);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Theme", QString::fromUtf8(error.what()));
        return false;
    }
    themeName_ = name;
    if (!keepSelection) {
        selectedKey_.reset();
        selectedText_.clear();
    }
    loading_ = true;
    orientationCombo_->setCurrentIndex(
        lcdctl::isPortraitOrientation(theme_.orientation) ? 0 : 1);
    loading_ = false;
    preview_->setItems(&theme_.items);

    // Deterministic values so the preview is stable while editing.
    auto sensors = lcdctl::makeStubSensors("static", settings_.sensorSettings());
    sensors->refresh(metrics_);

    refreshItemList();
    refreshPreview();
    if (!selectedKey_ && !theme_.items.empty()) {
        // Show the first item's properties immediately: the panel must never
        // look broken/empty on start.
        selectItem(QString::fromStdString(theme_.items.front().key));
    }
    themeCombo_->blockSignals(true);
    themeCombo_->setCurrentText(QString::fromStdString(name));
    themeCombo_->blockSignals(false);
    return true;
}

void ThemeEditor::refreshPreview() {
    lcdctl::Size size = theme_.declaredSize();
    if (size.width <= 0 || size.height <= 0) {
        size = {320, 480};
    }
    lcdctl::OffscreenCanvas canvas(size, lcdctl::Color{0, 0, 0});
    lcdctl::ThemeRenderer renderer(theme_, canvas);
    renderer.drawBackground();
    renderer.drawAll(metrics_);
    QImage image = toQImage(canvas.framebuffer());
    const int degrees = rotationCombo_ ? rotationCombo_->currentData().toInt() : 0;
    if (degrees == 90 || degrees == 180 || degrees == 270) {
        image = image.transformed(QTransform().rotate(degrees));
    }
    preview_->setRender(image);
    if (selectedKey_) {
        preview_->setSelection(QString::fromStdString(*selectedKey_));
    }
}

void ThemeEditor::refreshItemList() {
    itemList_->blockSignals(true);
    itemList_->clear();
    for (const lcdctl::ThemeItem& item : theme_.items) {
        auto* entry = new QListWidgetItem(
            QString("%1%2").arg(item.show ? "" : "(hidden) ").arg(QString::fromStdString(item.key)));
        entry->setData(Qt::UserRole, QString::fromStdString(item.key));
        if (!item.show) {
            entry->setForeground(QColor(140, 140, 140));
        }
        itemList_->addItem(entry);
    }
    itemList_->blockSignals(false);

    textList_->blockSignals(true);
    textList_->clear();
    for (const lcdctl::ThemeText& text : theme_.texts) {
        auto* entry = new QListWidgetItem(QString::fromStdString(text.name));
        entry->setData(Qt::UserRole, QString::fromStdString(text.name));
        textList_->addItem(entry);
    }
    textList_->blockSignals(false);
}

void ThemeEditor::selectItem(const QString& key) {
    for (int row = 0; row < itemList_->count(); ++row) {
        auto* entry = itemList_->item(row);
        if (entry->data(Qt::UserRole).toString() == key) {
            itemList_->blockSignals(true);
            itemList_->setCurrentRow(row);
            itemList_->blockSignals(false);
            preview_->setSelection(key);
            showItem(key.toStdString());
            return;
        }
    }
}

void ThemeEditor::applyToItem(const std::function<void(lcdctl::ThemeItem&)>& mutate) {
    if (!selectedKey_) {
        return;
    }
    lcdctl::ThemeItem* item = theme_.findItem(*selectedKey_);
    if (!item) {
        return;
    }
    mutate(*item);
    refreshPreview();
    refreshItemList();
    if (selectedKey_) {
        preview_->setSelection(QString::fromStdString(*selectedKey_));
    }
    statusLabel_->setText("Modified (unsaved)");
}

void ThemeEditor::clearProperties() {
    while (QLayoutItem* entry = propertiesLayout_->takeAt(0)) {
        if (QWidget* widget = entry->widget()) {
            widget->deleteLater();
        }
        delete entry;
    }
}

void ThemeEditor::showItem(const std::string& key) {
    lcdctl::ThemeItem* item = theme_.findItem(key);
    if (!item) {
        return;
    }
    selectedKey_ = key;
    clearProperties();
    properties_->setTitle(QString::fromStdString(key));

    auto* showBox = new QCheckBox(properties_);
    showBox->setChecked(item->show);
    connect(showBox, &QCheckBox::toggled, this, [this](bool checked) {
        applyToItem([checked](lcdctl::ThemeItem& target) { target.show = checked; });
    });
    propertiesLayout_->addRow("Show", showBox);

    auto* intervalBox = new QSpinBox(properties_);
    intervalBox->setRange(0, 3600);
    intervalBox->setSuffix(" s");
    intervalBox->setValue(item->interval);
    connect(intervalBox, &QSpinBox::valueChanged, this, [this](int value) {
        applyToItem([value](lcdctl::ThemeItem& target) { target.interval = value; });
    });
    propertiesLayout_->addRow("Interval", intervalBox);

    // Geometry: one pair of spin boxes per kind.
    int* x = nullptr;
    int* y = nullptr;
    int* w = nullptr;
    int* h = nullptr;
    switch (item->kind) {
        case lcdctl::ThemeItem::Kind::Text:
            x = &item->text.x; y = &item->text.y;
            w = &item->text.width; h = &item->text.height;
            break;
        case lcdctl::ThemeItem::Kind::ProgressBar:
            x = &item->bar.x; y = &item->bar.y;
            w = &item->bar.width; h = &item->bar.height;
            break;
        case lcdctl::ThemeItem::Kind::LineGraph:
            x = &item->graph.x; y = &item->graph.y;
            w = &item->graph.width; h = &item->graph.height;
            break;
        case lcdctl::ThemeItem::Kind::Radial: {
            auto* xBox = new QSpinBox(properties_);
            xBox->setRange(-10000, 10000);
            xBox->setValue(item->radial.x);
            connect(xBox, &QSpinBox::valueChanged, this, [this](int value) {
                applyToItem([value](lcdctl::ThemeItem& target) { target.radial.x = value; });
            });
            propertiesLayout_->addRow("Center X", xBox);

            auto* yBox = new QSpinBox(properties_);
            yBox->setRange(-10000, 10000);
            yBox->setValue(item->radial.y);
            connect(yBox, &QSpinBox::valueChanged, this, [this](int value) {
                applyToItem([value](lcdctl::ThemeItem& target) { target.radial.y = value; });
            });
            propertiesLayout_->addRow("Center Y", yBox);

            auto* radiusBox = new QSpinBox(properties_);
            radiusBox->setRange(1, 4000);
            radiusBox->setValue(item->radial.radius);
            connect(radiusBox, &QSpinBox::valueChanged, this, [this](int value) {
                applyToItem([value](lcdctl::ThemeItem& target) { target.radial.radius = value; });
            });
            propertiesLayout_->addRow("Radius", radiusBox);

            auto* widthBox = new QSpinBox(properties_);
            widthBox->setRange(1, 400);
            widthBox->setValue(item->radial.barWidth);
            connect(widthBox, &QSpinBox::valueChanged, this, [this](int value) {
                applyToItem([value](lcdctl::ThemeItem& target) { target.radial.barWidth = value; });
            });
            propertiesLayout_->addRow("Bar width", widthBox);
            break;
        }
    }

    if (x && y && w && h) {
        struct Field {
            const char* label;
            int* value;
            int minimum;
            int maximum;
        };
        const Field fields[] = {{"X", x, -10000, 10000},
                                {"Y", y, -10000, 10000},
                                {"Width", w, 0, 4000},
                                {"Height", h, 0, 4000}};
        for (const Field& field : fields) {
            auto* box = new QSpinBox(properties_);
            box->setRange(field.minimum, field.maximum);
            box->setValue(*field.value);
            const bool isWidth = std::string(field.label) == std::string("Width");
            const bool isHeight = std::string(field.label) == std::string("Height");
            connect(box, &QSpinBox::valueChanged, this, [this, isWidth, isHeight](int value) {
                applyToItem([isWidth, isHeight, value](lcdctl::ThemeItem& target) {
                    switch (target.kind) {
                        case lcdctl::ThemeItem::Kind::Text:
                            if (isWidth) target.text.width = value;
                            else if (isHeight) target.text.height = value;
                            break;
                        case lcdctl::ThemeItem::Kind::ProgressBar:
                            if (isWidth) target.bar.width = value;
                            else if (isHeight) target.bar.height = value;
                            break;
                        case lcdctl::ThemeItem::Kind::LineGraph:
                            if (isWidth) target.graph.width = value;
                            else if (isHeight) target.graph.height = value;
                            break;
                        case lcdctl::ThemeItem::Kind::Radial:
                            break;
                    }
                });
            });
            propertiesLayout_->addRow(field.label, box);
        }
    }

    if (item->kind == lcdctl::ThemeItem::Kind::Text) {
        auto* sizeBox = new QSpinBox(properties_);
        sizeBox->setRange(4, 200);
        sizeBox->setValue(item->text.fontSize);
        connect(sizeBox, &QSpinBox::valueChanged, this, [this](int value) {
            applyToItem([value](lcdctl::ThemeItem& target) { target.text.fontSize = value; });
        });
        propertiesLayout_->addRow("Font size", sizeBox);

        auto* anchorBox = new QComboBox(properties_);
        for (const char* anchor : {"lt", "lm", "lb", "mt", "mm", "mb", "rt", "rm", "rb", "la"}) {
            anchorBox->addItem(anchor);
        }
        anchorBox->setCurrentText(QString::fromStdString(item->text.anchor));
        connect(anchorBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
            applyToItem([text](lcdctl::ThemeItem& target) { target.text.anchor = text.toStdString(); });
        });
        propertiesLayout_->addRow("Anchor", anchorBox);
    }

    auto* metricLabel = new QLabel(QString::fromStdString(item->metric), properties_);
    metricLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    propertiesLayout_->addRow("Metric", metricLabel);
}

void ThemeEditor::showStaticText(const std::string& name) {
    for (lcdctl::ThemeText& text : theme_.texts) {
        if (text.name != name) {
            continue;
        }
        selectedText_ = name;
        clearProperties();
        properties_->setTitle(QString("static_text/%1").arg(QString::fromStdString(name)));

        auto* contentEdit = new QLineEdit(QString::fromStdString(text.text), properties_);
        connect(contentEdit, &QLineEdit::textEdited, this, [this](const QString& value) {
            for (lcdctl::ThemeText& target : theme_.texts) {
                if (target.name == selectedText_) {
                    target.text = value.toStdString();
                }
            }
            refreshPreview();
            statusLabel_->setText("Modified (unsaved)");
        });
        propertiesLayout_->addRow("Text", contentEdit);

        struct Field {
            const char* label;
            int* value;
        };
        Field fields[] = {{"X", &text.x}, {"Y", &text.y}, {"Font size", &text.style.fontSize}};
        for (Field& field : fields) {
            auto* box = new QSpinBox(properties_);
            box->setRange(0, 4000);
            box->setValue(*field.value);
            int* target = field.value;
            connect(box, &QSpinBox::valueChanged, this, [this, target](int value) {
                *target = value;
                for (lcdctl::ThemeText& entry : theme_.texts) {
                    if (entry.name == selectedText_) {
                        entry.style.x = entry.x;
                        entry.style.y = entry.y;
                    }
                }
                refreshPreview();
                statusLabel_->setText("Modified (unsaved)");
            });
            propertiesLayout_->addRow(field.label, box);
        }
        return;
    }
}

void ThemeEditor::moveSelection(int dx, int dy) {
    applyToItem([dx, dy](lcdctl::ThemeItem& item) {
        switch (item.kind) {
            case lcdctl::ThemeItem::Kind::Text: item.text.x += dx; item.text.y += dy; break;
            case lcdctl::ThemeItem::Kind::ProgressBar: item.bar.x += dx; item.bar.y += dy; break;
            case lcdctl::ThemeItem::Kind::LineGraph: item.graph.x += dx; item.graph.y += dy; break;
            case lcdctl::ThemeItem::Kind::Radial: item.radial.x += dx; item.radial.y += dy; break;
        }
    });
    lcdctl::ThemeItem* item = selectedKey_ ? theme_.findItem(*selectedKey_) : nullptr;
    if (item) {
        showItem(item->key);
    }
}

void ThemeEditor::save() {
    if (theme_.name.empty()) {
        return;
    }
    const std::string path = (fs::path(theme_.directory) / "theme.yaml").string();
    std::error_code ec;
    fs::copy_file(path, path + ".bak", fs::copy_options::overwrite_existing, ec);
    std::ofstream stream(path);
    if (!stream) {
        QMessageBox::warning(this, "Save", QString::fromStdString("cannot write " + path));
        return;
    }
    stream << theme_.toYaml() << '\n';
    stream.close();
    statusLabel_->setText(QString("Saved %1 (backup: theme.yaml.bak)").arg(QString::fromStdString(path)));
}

void ThemeEditor::exportPng() {
    const QString path = QFileDialog::getSaveFileName(this, "Export preview", "theme-preview.png",
                                                      "PNG image (*.png)");
    if (path.isEmpty()) {
        return;
    }
    if (!preview_->render().save(path)) {
        QMessageBox::warning(this, "Export", "could not write the image");
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setApplicationName("lcd-theme-editor");
    application.setQuitOnLastWindowClosed(true);

    lcdctl::Settings settings =
        lcdctl::Settings::load(lcdctl::defaultConfigPath());
    std::string themeRoot = settings.themeRoot;
    std::string fontsRoot = settings.fontsRoot;
    std::string themeName;
    std::string screenshot;

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--screenshot" && i + 1 < argc) {
            screenshot = argv[++i];
        } else {
            positional.push_back(argument);
        }
    }
    if (positional.size() >= 1) {
        themeRoot = positional[0];
    }
    if (positional.size() >= 2) {
        fontsRoot = positional[1];
    }
    if (positional.size() >= 3) {
        themeName = positional[2];
    }
    settings.themeRoot = themeRoot;
    settings.fontsRoot = fontsRoot;

    ThemeEditor window(settings, themeRoot, themeName);
    window.show();

    if (!screenshot.empty()) {
        // Debug/CI: grab the window once, then exit. Works with
        // QT_QPA_PLATFORM=offscreen, so the editor can be checked without a desktop.
        const QString path = QString::fromStdString(screenshot);
        QTimer::singleShot(600, &window, [&window, path, &application] {
            const QPixmap pixmap = window.grab();
            if (!pixmap.save(path)) {
                std::fprintf(stderr, "cannot write %s\n", path.toUtf8().constData());
            }
            application.quit();
        });
    }
    return application.exec();
}
