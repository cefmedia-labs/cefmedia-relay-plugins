// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#include "recorder_dock.h"

// Qt (OBS ships the Qt 6 runtime; the panel links Qt6Core/Gui/Widgets import libs and
// resolves to OBS's DLLs at load time — nothing Qt needs to be deployed with the plugin).
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtWidgets/QStyle> // unpolish/polish for dynamic-property restyle

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "cefmedia_output.h"
#include "cefmedia_detect.h" // CefMediaRelay install/run check (relay-missing hint row)
#include "cefmedia_api.h"
#include "channel_preview.h" // live channel preview popup + ChannelCombo (Qt TU #2)
#include "frontend_api.h" // brings obs.h + util/config-file.h (config_get_string)
#include "i18n.h"

namespace {

// Dock registration id (must be unique across the OBS main window; OBS also persists the
// dock layout under this object name).
constexpr const char* kDockId = "cefmediaRecorderDock";

// Occupancy-dot geometry: each dot is a small filled circle —
// bright when its channel is owned by a capturing tab, dim when free. Fixed
// pixel size (Qt logical pixels; scales with the OS DPI). The dots sit inline
// at the right end of the Channel row; spacing between them comes from the
// row layout's spacing.
constexpr int kDotDiameter = 10;
// Occupied / free dot colors. Occupied: OBS-primary-ish blue (same family as the
// recording button's #284CB8, readable on both light and dark themes). Free:
// mid-palette gray (dim, clearly inactive).
constexpr const char* kDotColorOn = "#284CB8";
constexpr const char* kDotColorOff = "#555555";
// Relay-missing hint color (warning amber — readable on both themes, matches
// OBS's own warning text family).
constexpr const char* kHintColorWarn = "#E8A33D";

// Channel-state icon for the Channel combo (rendered both on the closed combo and
// in the popup list): a filled circle in the same occupied/free colors as the
// inline occupancy dots, so the combo reads the same visual language as the dot
// row next to it. Painted at 2x and scaled down via devicePixelRatio for crisp
// HiDPI. The free state is intentionally the same dim-gray filled dot as the
// inline row — no new state vocabulary.
QIcon MakeChannelStateIcon(bool on)
{
    constexpr int kCanvas = 16; // logical px; matches the style's small-icon size
    QPixmap pm(kCanvas * 2, kCanvas * 2);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(on ? kDotColorOn : kDotColorOff));
    const qreal r = kDotDiameter / 2.0; // same circle size as the inline dots
    p.drawEllipse(QPointF(kCanvas / 2.0, kCanvas / 2.0), r, r);
    return QIcon(pm);
}

// Start/Stop button stylesheets (single source of truth; RefreshUI installs them
// on every state change and the constructor uses kButtonStyleIdle for the first
// paint). The Yami theme's base QPushButton rule (height + max-height:
// var(--input_height), applied to every QPushButton inside OBSDock — ours included)
// already sizes the button exactly like the official #streamButton/#recordButton;
// our inline sheet only sets padding — NO max-height here, otherwise we override the
// theme's cap with our own (wrong-by-a-few-px) number. Recording state paints the OBS
// primary blue (#recordButton.state-active = --primary #284CB8, hover --primary_light
// #476BD7).
constexpr const char* kButtonStyleIdle =
    "QPushButton { padding: 4px 12px; }";
constexpr const char* kButtonStyleRecording =
    "QPushButton { padding: 4px 12px; background-color: #284CB8; }"
    "QPushButton:hover { background-color: #476BD7; }";

// Two-state Tools menu text: the ini KEYS live in recorder_dock.h; the TRANSLATED text
// is resolved once at registration (T()) and cached here — the two-state rewrite in
// RecorderDockNotifyStateChanged matches against these cached strings, so localization
// cannot desync the menu rewrite.
QString g_menu_text_idle;
QString g_menu_text_recording;

// Latest channel-occupancy bitmap from CefMedia Relay (bit i = channel i owned).
// Written on the status receive thread, read on the UI thread (1 s timer);
// relaxed is enough — the dots are an indicator, one tick of lag is invisible.
// Declared before RecorderPanel (its constructor registers the callback).
std::atomic<unsigned int> g_panel_bitmap{0};

// QLineEdit with an embedded trailing "…" button (drawn inside the field, right side of
// the text area): a flat borderless child button repositioned on every resize so it
// tracks the field's right edge (no Q_OBJECT needed — resizeEvent is a virtual override).
class FolderEdit : public QLineEdit
{
public:
    explicit FolderEdit(QWidget* parent = nullptr) : QLineEdit(parent)
    {
        browse_btn_ = new QPushButton(QStringLiteral("…"), this);
        browse_btn_->setToolTip(T("Dock.Tooltip.Browse"));
        browse_btn_->setCursor(Qt::PointingHandCursor);
        // Flat + no focus: visually part of the field, not a separate widget. The
        // stylesheet is set on the FIELD so it covers the embedded button too (a widget
        // stylesheet applies to all its descendants) and can key the glyph color off the
        // FIELD's state: dim mid-palette tone at rest, white when the field is hovered
        // or focused (the field background darkens there and "…" would vanish), or when
        // the pointer is directly over the button (over the button the field itself is
        // not :hover — the button is the topmost widget under the cursor).
        browse_btn_->setFlat(true);
        browse_btn_->setFocusPolicy(Qt::NoFocus);
        setStyleSheet(
            "QPushButton { border: none; background: transparent; padding: 0 4px; "
            "color: palette(mid); font-weight: bold; }"
            "QLineEdit:hover QPushButton, QLineEdit:focus QPushButton, QPushButton:hover "
            "{ color: white; }");
        setClearButtonEnabled(false); // trailing area is ours
    }

    QPushButton* BrowseButton() { return browse_btn_; }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLineEdit::resizeEvent(event);
        // Pin the button to the right edge, vertically centered, and reserve the same
        // width as a right text margin so the text scrolls/stops before it (otherwise
        // long paths draw under the button).
        const int w = browse_btn_->sizeHint().width();
        const int h = browse_btn_->sizeHint().height();
        browse_btn_->setGeometry(width() - w - 2, (height() - h) / 2, w, h);
        setTextMargins(0, 0, w + 6, 0);
    }

private:
    QPushButton* browse_btn_ = nullptr;
};

// Less-frequent recorder settings (everything but Channel / Speed / Autostop, which
// stay in the dock grid): Source / Format / Encoder / bitrates / Folder. A plain
// QDialog with a QFormLayout — no OBSPropertiesView (that would drag in OBS's shared
// properties-view widget tree, moc included); no Q_OBJECT (lambda connects only), so
// no moc run either. Every control prefills from the control surface on construction;
// OK commits each value through its RecorderSet* (which validates + persists to
// recorder.cfg, effective on the next start), Cancel discards everything.
//
// Field order follows the recording pipeline: what to record (Source) -> how to encode
// (Container -> Video encoder -> bitrates) -> where to put it (Folder last, so the
// encoding chain is not interrupted by a path row).
class RecorderSettingsDialog : public QDialog
{
public:
    explicit RecorderSettingsDialog(QWidget* parent = nullptr) : QDialog(parent)
    {
        setWindowTitle(T("Dock.Settings"));
        setModal(true);
        setSizeGripEnabled(false);

        auto* form = new QFormLayout();
        form->setHorizontalSpacing(8);
        form->setVerticalSpacing(6);

        // Source: which streams the next recording captures (va / v / a).
        source_box_ = new QComboBox(this);
        source_box_->addItem(T("Props.Source.VideoAudio")); // index 0 -> "va"
        source_box_->addItem(T("Props.Source.VideoOnly"));  // index 1 -> "v"
        source_box_->addItem(T("Props.Source.AudioOnly"));  // index 2 -> "a"
        {
            const std::string src = RecorderSource();
            source_box_->setCurrentIndex(src == "v" ? 1 : (src == "a" ? 2 : 0));
        }
        form->addRow(T("Props.Source"), source_box_);

        // Container format for the output file (mkv / mp4).
        format_box_ = new QComboBox(this);
        format_box_->addItem("mkv");
        format_box_->addItem("mp4");
        format_box_->setCurrentText(QString::fromStdString(RecorderFormat()));
        form->addRow(T("Props.Container"), format_box_);

        // Video encoder preference (auto probes NVENC -> QSV -> x264 at start).
        vcodec_box_ = new QComboBox(this);
        vcodec_box_->addItem(T("Props.Encoder.Auto"), "auto");
        vcodec_box_->addItem(T("Props.Encoder.X264"), "libx264");
        vcodec_box_->addItem(T("Props.Encoder.Nvenc"), "h264_nvenc");
        vcodec_box_->addItem(T("Props.Encoder.Qsv"), "h264_qsv");
        vcodec_box_->setCurrentIndex(
            vcodec_box_->findData(QString::fromStdString(RecorderVcodec())));
        form->addRow(T("Props.Encoder"), vcodec_box_);

        // Bitrates: the same ranges/steps the output properties declare
        // (vbitrate 500..60000 step 500, abitrate 32..512 step 32, kbps).
        const int vbit = RecorderVbitrate();
        vbitrate_spin_ = new QSpinBox(this);
        vbitrate_spin_->setRange(500, 60000);
        vbitrate_spin_->setSingleStep(500);
        vbitrate_spin_->setValue(vbit > 0 ? vbit : 8000);
        vbitrate_spin_->setSuffix(" kbps");
        form->addRow(T("Props.VideoBitrate"), vbitrate_spin_);

        const int abit = RecorderAbitrate();
        abitrate_spin_ = new QSpinBox(this);
        abitrate_spin_->setRange(32, 512);
        abitrate_spin_->setSingleStep(32);
        abitrate_spin_->setValue(abit > 0 ? abit : 160);
        abitrate_spin_->setSuffix(" kbps");
        form->addRow(T("Props.AudioBitrate"), abitrate_spin_);

        // Folder: the same editor + browse + open-in-Explorer trio the grid used to
        // carry (FolderEdit has the embedded "…" browse button).
        folder_edit_ = new FolderEdit(this);
        folder_edit_->setText(QString::fromStdString(RecorderFolder()));
        auto* folder_row = new QHBoxLayout();
        folder_row->setSpacing(4);
        folder_row->addWidget(folder_edit_, 1); // edit box stretches (browse inside it)
        auto* open_btn = new QPushButton(T("Dock.Open"), this);
        open_btn->setToolTip(T("Dock.Tooltip.Open"));
        folder_row->addWidget(open_btn);
        form->addRow(T("Dock.Folder"), folder_row);
        connect(folder_edit_, &QLineEdit::editingFinished, this, [this] {
            const std::string path = folder_edit_->text().toStdString();
            if (!RecorderSetFolder(path))
            {
                folder_edit_->setText(
                    QString::fromStdString(RecorderFolder())); // rejected: revert display
            }
        });
        connect(folder_edit_->BrowseButton(), &QPushButton::clicked, this, [this] {
            const QString start = QString::fromStdString(RecorderFolder());
            const QString dir = QFileDialog::getExistingDirectory(
                this, T("Dock.Dialog.SelectFolder"), start);
            if (dir.isEmpty())
            {
                return; // cancelled
            }
            folder_edit_->setText(dir);
            if (RecorderSetFolder(dir.toStdString()))
            {
                folder_edit_->setText(QString::fromStdString(RecorderFolder()));
            }
        });
        connect(open_btn, &QPushButton::clicked, this, [this] {
            const QString dir = QString::fromStdString(RecorderFolder());
            QDir().mkpath(dir); // ensure it exists before handing it to Explorer
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        });

        // OK commits (setters validate + persist), Cancel just closes — nothing was
        // written yet. The folder editor is the exception: its edits commit through
        // RecorderSetFolder as they are made (same behavior the grid row had).
        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(T("Dock.Dialog.Ok"));
        buttons->button(QDialogButtonBox::Cancel)->setText(T("Dock.Dialog.Cancel"));
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            RecorderSetSource(source_box_->currentIndex() == 1   ? "v"
                              : source_box_->currentIndex() == 2 ? "a"
                                                                 : "va");
            RecorderSetFormat(format_box_->currentText().toStdString());
            RecorderSetVcodec(vcodec_box_->currentData().toString().toStdString());
            RecorderSetVbitrate(vbitrate_spin_->value());
            RecorderSetAbitrate(abitrate_spin_->value());
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->addLayout(form);
        layout->addWidget(buttons);
    }

private:
    QComboBox* source_box_ = nullptr;
    QComboBox* format_box_ = nullptr;
    QComboBox* vcodec_box_ = nullptr;
    QSpinBox* vbitrate_spin_ = nullptr;
    QSpinBox* abitrate_spin_ = nullptr;
    FolderEdit* folder_edit_ = nullptr;
};

// Panel without Q_OBJECT: every interaction is a lambda-connected signal, so no moc run
// is required. OBS owns the widget once the dock is registered.
//
// Layout: a Start/Stop toggle button above a QGridLayout with a shared label column
// (col 0) and a stretching control column (col 1) — labels align left at equal width,
// controls fill the remaining width, and every row gets the same height from the grid.
// The frequently-touched settings (Channel / Speed / Settings / Autostop) stay in
// the grid (one row each since 2026-11-14); the rest (Source / Format / Encoder /
// bitrates / Folder) live in the RecorderSettingsDialog.
class RecorderPanel : public QWidget
{
public:
    explicit RecorderPanel(QWidget* parent = nullptr) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        // Match the official Yami dock rhythm (obsPadding default 4):
        // padding_container = max(4, 0.5*4) = 4px margins; spacing_base ≈ 2px.
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(2);

        toggle_btn_ = new QPushButton(T("Dock.Button.Start"), this);
        // Initial look only — RefreshUI rewrites this stylesheet on every state
        // change (and once at the first 1 s tick), so the button's real styling
        // lives in RefreshUI (single source of truth: kButtonStyleIdle/Recording
        // below). Keep this string identical to kButtonStyleIdle so the first
        // paint matches what RefreshUI will install.
        toggle_btn_->setStyleSheet(kButtonStyleIdle);
        // Lock the width to the wider of the two labels ("Start Recording" /
        // "Stop Recording") so the button does not resize when the text toggles.
        toggle_btn_->setMinimumWidth(
            qMax(toggle_btn_->sizeHint().width(),
                 QPushButton(T("Dock.Button.Stop"), this).sizeHint().width()));
        layout->addWidget(toggle_btn_);

        auto* grid = grid_ = new QGridLayout();
        // Official row rhythm: spacing_large (4px) between columns, spacing_base
        // (2px) between rows — matches the Controls/Settings dock look.
        grid->setHorizontalSpacing(4);
        grid->setVerticalSpacing(2);
        grid->setColumnStretch(0, 0); // label column: natural width (aligned)
        grid->setColumnStretch(1, 1); // control column: fills the remaining width
        layout->addLayout(grid);

        int row = 0;
        // -- settings --
        grid->addWidget(new QLabel(T("Dock.Channel"), this), row, 0);
        channel_box_ = new ChannelCombo(this);
        for (int i = 1; i <= CEFMEDIA_CHANNEL_COUNT; ++i) {
            // Label is the 1-based UI channel number ("1".."5"). CefMedia Relay's
            // favicon badge shows the same number on the owning tab. Each item
            // carries a channel-state icon (same colors as the inline dots) so
            // occupancy is readable with the popup open — and the closed combo
            // shows the selected channel's state too.
            channel_box_->addItem(MakeChannelStateIcon(false), QString::number(i));
        }
        // Channel row and Speed row share the control column. The Settings button
        // used to ride the Speed row with a trailing-width equalization dance so
        // both combos rendered equal width; since 2026-11-14 the Speed combo grew
        // wide with the VOD note, so Settings got its own grid row and the channel
        // row keeps only the (narrow) dots cluster — no width equalization needed.
        settings_btn_ = new QPushButton(T("Dock.Settings"), this);
        settings_btn_->setStyleSheet(kButtonStyleIdle);
        settings_btn_->setToolTip(T("Dock.Tooltip.Settings"));
        const int kRowSpacing = 4; // matches the QHBoxLayout spacings below

        channel_box_->setCurrentIndex(RecorderChannel()); // 0-based index == UI-1
        // Occupancy indicator rides the Channel row (right of the combo): 5 dots
        // mirroring CefMedia Relay's channel bitmap (lit = a tab owns that channel).
        auto* channel_row = new QHBoxLayout();
        channel_row->setSpacing(kRowSpacing); // gap between combo and dots cluster
        channel_row->addWidget(channel_box_, 1); // combo fills the leading width
        for (int i = 0; i < CEFMEDIA_CHANNEL_COUNT; ++i) {
            QLabel* dot = new QLabel(this);
            dot->setFixedSize(kDotDiameter, kDotDiameter);
            dot->setStyleSheet(QStringLiteral("background:%1;border-radius:%2px;")
                                   .arg(kDotColorOff)
                                   .arg(kDotDiameter / 2));
            channel_row->addWidget(dot);
            occupancy_dots_.push_back(dot);
        }
        grid->addLayout(channel_row, row++, 1);
        // The occupancy dots share the Channel row (column 1) — the settings grid
        // rows run: Channel / Speed / Settings / Autostop. Source/Format/Encoder/
        // bitrates/Folder live in the Settings dialog (less-frequent settings).

        grid->addWidget(new QLabel(T("Dock.Speed"), this), row, 0);
        speed_box_ = new QComboBox(this);
        speed_box_->addItem("1.0x"); // index 0 -> 1000 permille
        // Speeds above 1x are Media-mode VOD only: page capture samples the
        // compositor at a wall-clock rate, so >1x is only meaningful for Media
        // mode VOD recording — the browser refuses Page switches on speed-latched
        // channels and clamps >1x rates in Page mode. The old inline per-item note
        // (2026-11-14) made every >1x item very wide; the full hint now lives as
        // the combo's own tooltip (hover only). Hint text via the plugin locale
        // (en-US is the canonical fallback for locales this key was not added to).
        speed_box_->addItem("1.5x"); // index 1 -> 1500 permille
        speed_box_->addItem("2.0x"); // index 2 -> 2000 permille
        speed_box_->addItem("3.0x"); // index 3 -> 3000 permille
        {
            const int sp = RecorderSpeed(); // permille from the control surface
            const int idx = sp == 1500 ? 1 : (sp == 2000 ? 2 : (sp == 3000 ? 3 : 0));
            speed_box_->setCurrentIndex(idx);
        }
        speed_box_->setToolTip(
            QStringLiteral("<p>%1</p>")
                .arg(QString::fromUtf8(T("Dock.SpeedHint"))));
        grid->addWidget(speed_box_, row++, 1);

        // Action rows (Settings / Autostop) span the full grid width so their left
        // edge lines up with the Start Recording button above — the panel reads as
        // two row types: labeled parameter rows (Channel / Speed / Output) and
        // full-width action rows (Start / Settings / Autostop / relay hint).
        grid->addWidget(settings_btn_, row++, 0, 1, 2);

        // Auto-stop toggle: when checked, the recording ends by itself when CefMedia Relay
        // reports the current video playback end (End-flag frame) — a full episode is
        // captured unattended. Effective on the next start (persisted to recorder.cfg).
        autostop_check_ = new QCheckBox(T("Dock.Autostop"), this);
        autostop_check_->setToolTip(
            T("Dock.Autostop"));
        autostop_check_->setChecked(RecorderAutoStop());
        grid->addWidget(autostop_check_, row++, 0, 1, 2);

        // -- recording info (single Output row: actual recorded content time / file
        //    size; hidden until the first recording starts, kept with the final values
        //    after a stop) --
        output_row_ = row;
        grid->addWidget(output_title_ = new QLabel(T("Dock.Output"), this), row, 0);
        output_label_ = new QLabel("00:00:00 / 0.0 MB", this);
        output_label_->setAlignment(Qt::AlignCenter | Qt::AlignVCenter);
        grid->addWidget(output_label_, row++, 1);

        // -- relay-missing hint row: one line, visible only when the
        //    start-time check could not find CefMedia Relay executable. A warning tone
        //    (theme warning color) so it reads as a problem, not as data. --
        hint_row_ = row;
        grid->addWidget(hint_label_ = new QLabel(T("Dock.RelayMissing"), this), row, 0,
                        1, 2); // spans both columns (no sibling label)
        hint_label_->setAlignment(Qt::AlignCenter | Qt::AlignVCenter);
        hint_label_->setWordWrap(true);
        hint_label_->setStyleSheet(QStringLiteral("color:%1;")
                                       .arg(kHintColorWarn));
        grid->addWidget(new QWidget(this), row++, 1); // keep grid geometry regular

        // Uniform perceived row spacing: grid rows are as tall as their tallest widget.
        // Raise every row's minimum height to the combo height so the text-to-text
        // rhythm is identical across all rows.
        row_height_cached_ = speed_box_->sizeHint().height();
        for (int r = 0; r < row; ++r)
        {
            grid->setRowMinimumHeight(r, row_height_cached_);
        }
        // The Output row starts collapsed (fresh OBS start, nothing recorded yet);
        // RefreshUI raises it to the uniform height on the first recording. The hint
        // row likewise starts collapsed; RefreshUI raises it only when the start-time
        // start-time check reported CefMedia Relay missing.
        grid->setRowMinimumHeight(output_row_, 0);
        grid->setRowMinimumHeight(hint_row_, 0);
        // Lock the label column width to the widest label across ALL labeled rows
        // (otherwise different label texts would give column 0 a ragged width).
        // Labeled rows: Channel (0) / Speed (1) / Output (output_row_); the action
        // rows (Settings / Autostop) and the hint row span both columns, so they
        // have no col-0 label to measure.
        int label_w = 0;
        for (int labeled_row : {0, 1, output_row_})
        {
            auto* item = grid->itemAtPosition(labeled_row, 0);
            if (!item || !item->widget())
            {
                continue;
            }
            label_w = qMax(label_w,
                           static_cast<QLabel*>(item->widget())->sizeHint().width());
        }
        grid->setColumnMinimumWidth(0, label_w);

        layout->addStretch(1);

        timer_ = new QTimer(this);
        timer_->setInterval(1000);

        // Channel selection: persists immediately, effective on the next start
        // (a running recording keeps its start-time channel snapshot). Disabled
        // while recording — same "next start" semantics as the dialog settings,
        // made visible instead of silently ignored.
        connect(channel_box_, &QComboBox::currentIndexChanged, this, [this](int index) {
            RecorderSetChannel(index); // 0-based index == 0-based channel
            channel_box_->setEnabled(!RecorderIsRecording());
        });
        // Live channel preview while the dropdown is open (ChannelPreviewWindow):
        // the popup follows the highlighted item (hover/keyboard), shows black
        // for a free channel, and closes with the dropdown itself. Keyboard
        // navigation arrives through QComboBox::highlighted; mouse hover does
        // NOT reliably fire it — QAbstractItemView::entered (with mouse
        // tracking on the popup view) is the hover path.
        connect(channel_box_, &QComboBox::highlighted, this,
                [this](int index) { UpdatePreview(index); });
        if (QAbstractItemView* popup_view = channel_box_->view())
        {
            popup_view->setMouseTracking(true);
            connect(popup_view, &QAbstractItemView::entered, this,
                    [this](const QModelIndex& index) { UpdatePreview(index.row()); });
        }
        static_cast<ChannelCombo*>(channel_box_)->popup_shown = [this] {
            UpdatePreview(channel_box_->currentIndex());
        };
        static_cast<ChannelCombo*>(channel_box_)->popup_hidden = [this] {
            ClosePreview();
        };

        connect(settings_btn_, &QPushButton::clicked, this, [this] {
            RecorderSettingsDialog dialog(this);
            dialog.exec(); // modal; the 1 s timer keeps running in the nested loop
        });

        // Auto-stop toggle: persist immediately; effective on the next start (a running
        // recording keeps its start-time snapshot).
        connect(autostop_check_, &QCheckBox::toggled, this, [](bool checked) {
            RecorderSetAutoStop(checked);
        });

        // Phase B: the speed selector drives CefMedia Relay — but only WHILE RECORDING.
        // A change while not recording just persists (recorder.cfg) and takes effect on
        // the next start; stopping a recording restores 1x in CefMedia Relay (control
        // surface) while this combo keeps its selection for the next run. RateSet
        // no-ops when CefMedia Relay / control export is absent, so a missing relay is
        // safe. The 1 s timer's HeartbeatTick re-applies every 1 s while recording and
        // not 1x (covers page navigation / new tabs resetting the site player's rate;
        // tightened from 3 s on 2026-11-14 — the browser-side speed latch refreshes
        // per RateSet, so a faster heartbeat shortens the unprotected window).
        connect(speed_box_, &QComboBox::currentIndexChanged, this, [this](int index) {
            static constexpr int kPermille[] = {1000, 1500, 2000, 3000};
            const int permille = (index >= 0 && index < 4) ? kPermille[index] : 1000;
            RecorderSetSpeed(permille); // persist only
            if (RecorderIsRecording())
            {
                ApplyRateNow(); // immediate apply; the 1 s heartbeat takes over
            }
        });

        // (the Settings dialog owns its own folder editor/browse/open wiring; the
        // source/format/encoder/bitrates persist through the dialog's OK button).

        connect(toggle_btn_, &QPushButton::clicked, this, [this] {
            if (RecorderIsRecording())
            {
                RecorderStopCurrent();
            }
            else
            {
                RecorderStartNew();
            }
            RefreshUI();
        });
        connect(timer_, &QTimer::timeout, this, [this] {
            RecorderFinalizeIfPending(); // complete an auto-stop requested by the reader
            RefreshUI();
            HeartbeatTick();
        });

        RefreshUI();
        timer_->start();

        // Occupancy indicator: subscribe to CefMedia Relay's channel bitmap. The
        // callback runs on the ipc client's status receive thread — it must not
        // touch widgets; it only stores the bitmap (relaxed is fine: worst case
        // the dots lag one 1 s timer tick behind). The panel's timer repaints
        // the dots every second (UpdateOccupancyDots in RefreshUI), so there is
        // no cross-thread widget access and no Q_OBJECT/slot machinery needed.
        // The status thread is held for the dock's lifetime.
        CefMediaApi::Instance().SetStatusCallback(
            [](unsigned int bitmap, void* /*obj*/) {
                g_panel_bitmap.store(bitmap, std::memory_order_relaxed);
            },
            nullptr);
        CefMediaApi::Instance().StatusStart();
    }

private:
    static QString FormatDuration(int64_t ms)
    {
        const int64_t total_seconds = ms / 1000;
        const int64_t h = total_seconds / 3600;
        const int64_t m = (total_seconds % 3600) / 60;
        const int64_t s = total_seconds % 60;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld",
                 (long long)h, (long long)m, (long long)s);
        return QString::fromUtf8(buf);
    }

    static QString FormatSize(int64_t bytes)
    {
        if (bytes <= 0)
        {
            return QStringLiteral("0.0 MB"); // idle state: the Output row stays visible
        }
        const double mb = (double)bytes / (1024.0 * 1024.0);
        if (mb >= 1024.0)
        {
            return QString::asprintf("%.2f GB", mb / 1024.0);
        }
        return QString::asprintf("%.1f MB", mb);
    }

public:
    ~RecorderPanel() override
    {
        // The popup is a TOP-LEVEL window (not a widget child), so OBS freeing
        // the dock would leak it — destroy it here. Direct delete (not
        // deleteLater): OBS is tearing the dock down and the deferred-delete
        // queue may never run again; the destructor unbinds synchronously
        // (removes the frame callback + joins the receive threads).
        if (preview_)
        {
            ChannelPreviewWindow* window = preview_;
            preview_ = nullptr;
            delete window;
        }
    }

    // One refresh covers both the periodic timer and the button click; the button state
    // is re-derived every tick so a Tools-menu toggle (or an error stop) is reflected
    // within 1 s without any cross-notification. Duration shows the ACTUAL recorded
    // content time (file timeline — it stops while the CefMedia Relay pauses), not wall clock.
    // Public: RecorderDockNotifyStateChanged (outside the class) calls it on
    // state transitions so the dock button follows the Tools menu immediately.
    void RefreshUI()
    {
        const bool recording = RecorderIsRecording();
        toggle_btn_->setText(recording ? T("Dock.Button.Stop") : T("Dock.Button.Start"));
        // Recording state: background turns OBS's primary blue (the official Stop
        // Recording look — Yami theme paints #recordButton.state-active with
        // --primary = --blue3 #284CB8, hover --primary_light = #476BD7, text keeps
        // the theme's --text). Matching that instead of red text keeps the dock
        // consistent with the main-window recording button. Applied via the same
        // dynamic-property mechanism OBS themes use (.state-active) so the native
        // palette still drives everything the stylesheet doesn't override.
        const bool was_recording = toggle_btn_->property("state-active").toBool();
        if (was_recording != recording)
        {
            toggle_btn_->setProperty("state-active", recording);
            toggle_btn_->style()->unpolish(toggle_btn_);
            toggle_btn_->style()->polish(toggle_btn_);
        }
        toggle_btn_->setStyleSheet(recording ? kButtonStyleRecording : kButtonStyleIdle);

        // Channel combo: disabled while recording (the binding is a start-time
        // snapshot; same next-start semantics as Format/Folder, made visible).
        if (channel_box_->isEnabled() == recording)
        {
            channel_box_->setEnabled(!recording);
        }
        UpdateOccupancyDots();

        // Recording info row: hidden until the FIRST recording starts; while recording
        // it live-updates; after a stop the control surface returns 0, so the LAST
        // values are cached and kept on display (the row summarizes the finished
        // recording; a fresh start overwrites within 1 s once frames are encoded).
        if (grid_)
        {
            if (recording)
            {
                has_recorded_ = true;
            }
            // Row visible once any recording has happened (recording now, or stopped
            // with a finished file to show); collapsed on a fresh OBS start.
            grid_->setRowMinimumHeight(output_row_, has_recorded_ ? row_height_cached_
                                                                  : 0);
            output_title_->setVisible(has_recorded_);
            output_label_->setVisible(has_recorded_);
            // CefMediaRelay-missing hint: visible only when the start-time check
            // reported it missing (kMissing); hides once configured/found — the
            // state is start-time latched, so this stays stable within a session.
            const bool relay_missing =
                CefMediaRelayLaunchStateNow() == CefMediaRelayLaunchState::kMissing;
            grid_->setRowMinimumHeight(hint_row_, relay_missing ? row_height_cached_
                                                                : 0);
            hint_label_->setVisible(relay_missing);
        }
        if (!has_recorded_)
        {
            return; // never recorded: keep the row hidden and the values at zero
        }
        const int64_t duration_ms = RecorderRecordedDurationUs() / 1000;
        const int64_t size_bytes = RecorderFileSize();
        if (recording)
        {
            last_duration_ms_ = duration_ms;
            last_size_bytes_ = size_bytes;
        }
        output_label_->setText(FormatDuration(recording ? duration_ms : last_duration_ms_) +
                               " / " +
                               FormatSize(size_bytes > 0 ? size_bytes : last_size_bytes_));
    }

    // Phase B control: push the selected speed to CefMedia Relay over the libipc
    // rate channel (permille: 1000/1500/2000/3000), routed to the recorded
    // channel's tab. Callers gate this on RecorderIsRecording —
    // CefMedia Relay rate only changes while recording (stop paths restore 1x via
    // the control surface). recording=true rides the message (recording-state
    // signalling): the browser latches per-channel recording state from the
    // heartbeat and locks its toolbar UI while it is fresh. No-op when the
    // CefMedia Relay is absent; the heartbeat retries.
    void ApplyRateNow()
    {
        CefMediaApi::Instance().RateSet(RecorderSpeed(), RecorderChannel(),
                                        /*recording=*/true);
    }

    // Called from the 1 s timer: while RECORDING, re-send every second (any
    // speed — 1x included, 2026-11-15) so page navigations / new tabs (which
    // reset the site player's rate) get re-applied and the browser-side
    // recording latch (toolbar lock + recording badge) stays fresh. Not
    // recording: stay silent — the stop path already sent recording=false and
    // restored 1x (a heartbeat would fight it; the combo selection only applies
    // from the next start).
    void HeartbeatTick()
    {
        if (!RecorderIsRecording())
        {
            return;
        }
        ApplyRateNow(); // 1 s heartbeat while recording (covers navigation /
                        // new-tab rate resets and keeps the browser-side speed
                        // latch + recording latch alive)
    }

    // ---- channel preview popup (ChannelPreviewWindow / ChannelCombo, see channel_preview.h) ----
    // Live preview tied to the Channel dropdown's open state: shown as soon as
    // the dropdown opens, following the highlighted item (hover/keyboard),
    // black for a free channel, gone when the dropdown closes. Frames flow only
    // while the dropdown is open — the committed selection is untouched.
    void UpdatePreview(int channel)
    {
        if (!preview_)
        {
            preview_ = new ChannelPreviewWindow();
            preview_->setAttribute(Qt::WA_DeleteOnClose);
            // Keep the panel pointer honest when the popup is dismissed
            // (deleteLater runs the destructor — and the mandatory unbind —
            // back on the UI thread).
            connect(preview_, &QObject::destroyed, this,
                    [this] { preview_ = nullptr; });
        }
        preview_->show();
        // Occupancy decision stays on the panel side (the CefMedia Relay
        // bitmap is dock state): the window only consumes the verdict.
        const unsigned int bitmap = g_panel_bitmap.load(std::memory_order_relaxed);
        preview_->FollowChannel(channel,
                                channel >= 0 && ((bitmap >> channel) & 1u));
        // Snug against the Channel combo's right edge, tops aligned. move() on
        // a top-level window positions the frame's top-left corner, which is
        // exactly the alignment we want.
        preview_->move(channel_box_->mapToGlobal(
            QPoint(channel_box_->width() + 4, 0)));
        preview_->raise();
    }

    void ClosePreview()
    {
        if (!preview_)
        {
            return;
        }
        ChannelPreviewWindow* window = preview_;
        preview_ = nullptr; // before close(): destroyed() fires via deleteLater
        window->close();    // WA_DeleteOnClose -> unbind in the destructor
    }

    // Occupancy dots + Channel combo icons: repaint from the latest bitmap
    // (0 = idle/all dark — CefMedia Relay gone or not capturing; every dot/icon
    // mirrors one channel). setStyleSheet/setIcon no-op on identical input, so
    // the 1 s tick repaints nothing unless the bitmap changed.
    void UpdateOccupancyDots()
    {
        const unsigned int bitmap = g_panel_bitmap.load(std::memory_order_relaxed);
        for (size_t i = 0; i < occupancy_dots_.size(); ++i)
        {
            const bool on = (bitmap >> i) & 1u;
            QLabel* dot = occupancy_dots_[i];
            const char* color = on ? kDotColorOn : kDotColorOff;
            const QString want = QStringLiteral("background:%1;border-radius:%2px;")
                                     .arg(color)
                                     .arg(kDotDiameter / 2);
            if (dot->styleSheet() != want)
            {
                dot->setStyleSheet(want);
            }
            // The combo's per-item icon mirrors the same state (closed combo shows
            // the selected channel's icon; the popup shows all five). Cache the
            // last painted state to avoid rebuilding the QIcon every tick.
            if (i < channel_states_.size() && channel_states_[i] != on)
            {
                channel_states_[i] = on;
                channel_box_->setItemIcon(static_cast<int>(i), MakeChannelStateIcon(on));
            }
        }
    }

    QPushButton* toggle_btn_ = nullptr;
    QPushButton* settings_btn_ = nullptr;
    QComboBox* channel_box_ = nullptr;
    QComboBox* speed_box_ = nullptr;
    QCheckBox* autostop_check_ = nullptr;
    QGridLayout* grid_ = nullptr;
    QLabel* output_title_ = nullptr;
    QLabel* output_label_ = nullptr;
    QLabel* hint_label_ = nullptr; // relay-missing hint (spans both columns)
    QTimer* timer_ = nullptr;
    ChannelPreviewWindow* preview_ = nullptr; // channel preview popup (top-level; may be null)
    int hint_row_ = 0;             // grid row of the relay-missing hint (show/hide)
    std::vector<QLabel*> occupancy_dots_;
    // Last painted combo-icon state per channel (constructor starts every item
    // with the free icon, so the cache starts all-false and the first tick with
    // a lit channel swaps in the occupied icon).
    std::vector<bool> channel_states_ = std::vector<bool>(CEFMEDIA_CHANNEL_COUNT, false);
    int output_row_ = 0;           // grid row of the Output info (for show/hide)
    int row_height_cached_ = 0;    // uniform row height (combo sizeHint)
    bool has_recorded_ = false;    // any recording started this OBS session
    int64_t last_duration_ms_ = 0; // last displayed Output duration (kept after stop)
    int64_t last_size_bytes_ = 0;  // last displayed Output size (kept after stop)
};

// Valid between Create/Destroy (ownership: OBS). The status callback compares
// against it, so it must exist before the callback can fire.
RecorderPanel* g_panel = nullptr; // valid between Create/Destroy (ownership: OBS)

// (g_panel_bitmap is declared above the panel class — the callback in its
// constructor references it.)

// Startup geometry settling: re-apply the saved dock layout once the main
// window has reached its final size.
//
// QMainWindow::restoreState restores the splitter positions recorded at exit,
// but those absolute widths only survive the layout pass when the window is at
// the same size it had at save time. A plugin dock misses OBS's own startup
// restore (it does not exist yet when OBSBasic restores its state in the
// constructor), so CreateRecorderDock below re-applies the saved state itself —
// and that re-apply runs while the window may still be settling towards its
// restored geometry (a restored maximize state, for instance, is applied by
// the window manager after show()). When the re-apply fits the layout into a
// smaller-than-saved window, the dock is clamped to its minimumSizeHint; the
// surplus width arriving with the final window size is then spread evenly
// across the dock row by Qt's geometry calculator, so the dock comes back
// wider than the user left it — and that widened width is what gets recorded
// at exit. Re-running restoreState once the window geometry has gone quiet
// restores the recorded widths exactly (zero surplus at the saved window
// size), which is what this watcher does. Docks OBS restored at startup need
// no such fix: their state application is deferred by Qt until the window
// reaches its final size.
//
// Not a Q_OBJECT: eventFilter() is a plain virtual override and both timers are
// lambda-connected, so no moc run is needed. The watcher is parented to the
// main window, holds only the DockState bytes, and removes itself after one
// re-apply (or the hard cap), so it needs no cleanup on module unload.
class DockSettleWatcher : public QObject
{
public:
    DockSettleWatcher(QMainWindow* main, QByteArray dock_state)
        : QObject(main), main_(main), dock_state_(dock_state)
    {
        main_->installEventFilter(this);
        quiet_.setSingleShot(true);
        quiet_.setInterval(kQuietMs);
        connect(&quiet_, &QTimer::timeout, this, [this] { Finish(); });
        cap_.setSingleShot(true);
        cap_.setInterval(kCapMs);
        connect(&cap_, &QTimer::timeout, this, [this] { Finish(); });
        quiet_.start();
        cap_.start();
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == main_ && event->type() == QEvent::Resize)
        {
            resized_ = true;
            quiet_.start(); // geometry still moving; wait for it to go quiet
        }
        return false;
    }

private:
    void Finish()
    {
        if (finished_)
        {
            return;
        }
        finished_ = true;
        main_->removeEventFilter(this);
        if (resized_)
        {
            // Idempotent for the docks OBS already laid out at startup, and
            // exact for the widths at the settled window size. Known cost: a
            // manual dock drag within the first kCapMs of startup is reverted
            // by this re-apply.
            main_->restoreState(dock_state_);
        }
        deleteLater();
    }

    // Silence window before the re-apply fires after the last window resize.
    static constexpr int kQuietMs = 300;
    // Hard cap on how long after startup the re-apply may still fire.
    static constexpr int kCapMs = 10000;

    QMainWindow* main_;
    QByteArray dock_state_;
    QTimer quiet_;
    QTimer cap_;
    bool resized_ = false;
    bool finished_ = false;
};

} // namespace

void CreateRecorderDock()
{
    FrontendApi& fe = FrontendApi::Instance();
    if (!fe.add_dock_by_id || g_panel)
    {
        return; // no frontend environment / already created
    }
    auto* panel = new RecorderPanel();
    // Dock title + the two menu strings resolve here (locale is set by OBS before
    // obs_module_load); the menu rewrite caches them (g_menu_text_*).
    g_menu_text_idle = QString::fromUtf8(T(kMenuIdle));
    g_menu_text_recording = QString::fromUtf8(T(kMenuRecording));
    if (!fe.add_dock_by_id(kDockId, T("Dock.Title"), panel))
    {
        delete panel; // id conflict — OBS keeps its existing dock
        return;
    }
    g_panel = panel; // ownership moved to OBS (shared_ptr in the main window)

    // OBS restores the saved dock layout (BasicWindow/DockState in global.ini) at
    // startup BEFORE plugins get FINISHED_LOADING, and a plugin dock registered later
    // starts hidden (add_dock_by_id ends with setVisible(false) + setFloating(true),
    // OBSStudioAPI.cpp) — so the dock misses both its saved geometry and visibility
    // on every start. Fix (same approach as OBS's own auth docks, YoutubeAuth.cpp):
    // re-apply the already-saved DockState now that our dock exists — QMainWindow
    // restoreState matches by objectName and is idempotent for the docks that were
    // already laid out — then, if the layout doesn't mark the dock visible (first
    // run ever / dock was closed last session), open it once so it's discoverable.
    QTimer::singleShot(0, panel, [panel] {
        QMainWindow* main = static_cast<QMainWindow*>(
            FrontendApi::Instance().get_main_window
                ? FrontendApi::Instance().get_main_window()
                : nullptr);
        if (!main)
        {
            return;
        }
        const char* dock_state =
            config_get_string(FrontendApi::Instance().get_user_config(), "BasicWindow",
                              "DockState");
        bool restored = false;
        if (dock_state && *dock_state)
        {
            restored = main->restoreState(QByteArray::fromBase64(dock_state));
        }
        if (!restored)
        {
            if (QWidget* dock = panel->parentWidget()) // the OBSDock wrapper OBS created
            {
                dock->show();
                dock->raise();
            }
        }
        if (dock_state && *dock_state)
        {
            // The re-apply above can land while the window is still settling
            // towards its restored geometry, which loses the recorded dock
            // widths (they get clamped to the minimum and the arriving surplus
            // is spread across the row). Watch the geometry go quiet and
            // re-apply once more at the final size — see DockSettleWatcher.
            new DockSettleWatcher(main, QByteArray::fromBase64(dock_state));
        }
    });
}

void DestroyRecorderDock()
{
    if (!g_panel)
    {
        return;
    }
    g_panel = nullptr; // the callback's g_panel_bitmap write is atomic-safe;
                       // no new widget updates can be scheduled afterwards
    // Release the status receive thread (the dock was its only holder).
    CefMediaApi::Instance().StatusStop();
    FrontendApi& fe = FrontendApi::Instance();
    if (fe.remove_dock)
    {
        fe.remove_dock(kDockId); // OBS frees the dock + widget
    }
}

void RecorderDockNotifyStateChanged()
{
    // Two-state Tools menu text: OBS's add_tools_menu_item keeps the created QAction in
    // the main window's Tools menu (object name "menuTools", OBSBasic.ui) — find it by
    // its current text and rewrite it. Runs on the UI thread (all state transitions do).
    QMainWindow* main = static_cast<QMainWindow*>(
        FrontendApi::Instance().get_main_window
            ? FrontendApi::Instance().get_main_window()
            : nullptr);
    if (!main)
    {
        return;
    }
    QMenu* tools = main->findChild<QMenu*>("menuTools");
    if (!tools)
    {
        return;
    }
    const QString wanted = RecorderIsRecording() ? g_menu_text_recording
                                                 : g_menu_text_idle;
    for (QAction* action : tools->actions())
    {
        // Match the CACHED translated strings (set at dock creation); matching either
        // state so a missed update self-heals. When the dock was never created the
        // cache is empty — fall back to comparing against freshly-resolved text.
        const bool match =
            (!g_menu_text_idle.isEmpty() &&
             (action->text() == g_menu_text_idle ||
              action->text() == g_menu_text_recording)) ||
            (g_menu_text_idle.isEmpty() &&
             (action->text() == QString::fromUtf8(T(kMenuIdle)) ||
              action->text() == QString::fromUtf8(T(kMenuRecording))));
        if (match)
        {
            action->setText(wanted);
        }
    }
    if (g_panel)
    {
        g_panel->RefreshUI(); // sync the dock button immediately (no 1 s wait)
    }
}
