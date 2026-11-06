#include "web_link_popup.h"
#include "utils.h"

#include <qboxlayout.h>
#include <qlabel.h>
#include <qpushbutton.h>
#include <qevent.h>
#include <qpainter.h>
#include <qstyleoption.h>
#include <qshortcut.h>
#include <qapplication.h>
#include <qtimer.h>

#ifdef SIOYEK_HAS_WEBENGINE
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineHistory>
#endif

extern float WEB_LINK_POPUP_SIZE[2];
extern int WEB_LINK_POPUP_PRELOAD_DELAY_MS;
extern float UI_TEXT_COLOR[3];
extern float UI_BACKGROUND_COLOR[3];
extern float UI_SELECTED_BACKGROUND_COLOR[3];

static QString color_to_css(const float* color) {
    QColor c = QColor::fromRgbF(color[0], color[1], color[2]);
    return c.name(QColor::HexRgb);
}

bool WebLinkPopup::is_available() {
#ifdef SIOYEK_HAS_WEBENGINE
    return true;
#else
    return false;
#endif
}

WebLinkPopup::WebLinkPopup(QWidget* parent) : QWidget(parent) {
    setObjectName("web_link_popup");
    setAttribute(Qt::WA_StyledBackground, true);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(3, 3, 3, 3);
    root->setSpacing(0);

    QWidget* header = new QWidget(this);
    header->setObjectName("web_link_popup_header");
    QHBoxLayout* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(6, 3, 6, 3);
    header_layout->setSpacing(6);

    back_button = new QPushButton(QString::fromUtf8("←"), header);
    back_button->setToolTip("Back");
    back_button->setFlat(true);
    back_button->setFixedWidth(28);
    back_button->setFocusPolicy(Qt::NoFocus);

    title_label = new QLabel(header);
    title_label->setTextInteractionFlags(Qt::NoTextInteraction);
    title_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    open_button = new QPushButton(QString::fromUtf8("Open in browser ↗"), header);
    open_button->setToolTip("Open this page in the system browser (Shift+click a link to skip the popup)");
    open_button->setFlat(true);
    open_button->setFocusPolicy(Qt::NoFocus);

    close_button = new QPushButton(QString::fromUtf8("✕"), header);
    close_button->setToolTip("Close (Esc)");
    close_button->setFlat(true);
    close_button->setFixedWidth(28);
    close_button->setFocusPolicy(Qt::NoFocus);

    header_layout->addWidget(back_button);
    header_layout->addWidget(title_label, 1);
    header_layout->addWidget(open_button);
    header_layout->addWidget(close_button);
    root->addWidget(header);

#ifdef SIOYEK_HAS_WEBENGINE
    web_view = new QWebEngineView(this);
    web_view->setContextMenuPolicy(Qt::NoContextMenu);
    // The web view creates its render widget lazily; watch for it so we can catch Escape
    // even when the page has keyboard focus.
    web_view->installEventFilter(this);
    root->addWidget(web_view, 1);

    connect(web_view, &QWebEngineView::titleChanged, this, [this](const QString& title) {
        set_title_text(title.isEmpty() ? url_.toString() : title);
    });
    connect(web_view, &QWebEngineView::urlChanged, this, [this](const QUrl& url) {
        url_ = url;
        if (web_view->title().isEmpty()) {
            set_title_text(url.toString());
        }
        back_button->setEnabled(web_view->history()->canGoBack());
    });
    connect(web_view, &QWebEngineView::loadProgress, this, [this](int progress) {
        if (progress < 100) {
            setWindowTitle(QString("Loading %1%").arg(progress));
        }
    });
    connect(back_button, &QPushButton::clicked, this, [this]() { web_view->back(); });
#else
    QLabel* unavailable = new QLabel("This build of sioyek was compiled without Qt WebEngine, so links open in the browser.", this);
    unavailable->setAlignment(Qt::AlignCenter);
    unavailable->setWordWrap(true);
    root->addWidget(unavailable, 1);
    back_button->setEnabled(false);
#endif

    connect(open_button, &QPushButton::clicked, this, &WebLinkPopup::open_current_in_browser);
    connect(close_button, &QPushButton::clicked, this, &WebLinkPopup::close_popup);

    QShortcut* escape_shortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escape_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape_shortcut, &QShortcut::activated, this, &WebLinkPopup::close_popup);

    preload_timer = new QTimer(this);
    preload_timer->setSingleShot(true);
    connect(preload_timer, &QTimer::timeout, this, [this]() {
        if (!isVisible() && !pending_preload_url_.isEmpty()) {
            start_loading(pending_preload_url_);
        }
    });

    apply_style();
    hide();
}

void WebLinkPopup::apply_style() {
    QString bg = color_to_css(UI_BACKGROUND_COLOR);
    QString fg = color_to_css(UI_TEXT_COLOR);
    QString border = color_to_css(UI_SELECTED_BACKGROUND_COLOR);

    setStyleSheet(QString(
        // a thick frame in the UI background colour plus a thin inner line in the accent colour,
        // so the popup reads as a floating window on both light and dark documents
        "QWidget#web_link_popup { background-color: %1; border: 3px solid %1; border-radius: 4px; }"
        "QWidget#web_link_popup_header { background-color: %1; border: 1px solid %3; border-bottom: none; }"
        "QWidget#web_link_popup_header QLabel { color: %2; background: transparent; }"
        "QWidget#web_link_popup_header QPushButton { color: %2; background: transparent; border: none; padding: 2px 6px; }"
        "QWidget#web_link_popup_header QPushButton:hover { background-color: %3; color: %1; }"
        "QWidget#web_link_popup_header QPushButton:disabled { color: %3; }"
    ).arg(bg, fg, border));
}

void WebLinkPopup::set_title_text(const QString& text) {
    QFontMetrics metrics(title_label->font());
    int available = std::max(50, title_label->width() - 10);
    title_label->setText(metrics.elidedText(text, Qt::ElideMiddle, available));
    title_label->setToolTip(url_.toString());
}

void WebLinkPopup::start_loading(const QUrl& url) {
    if (url == requested_url_) {
        // already loading or loaded (possibly preloaded on hover): nothing to do
        return;
    }
    requested_url_ = url;
    url_ = url;
    set_title_text(url.toString());
#ifdef SIOYEK_HAS_WEBENGINE
    web_view->setUrl(url);
#endif
}

void WebLinkPopup::preload_url(const QUrl& url) {
    if (isVisible() || url == requested_url_ || url == pending_preload_url_) {
        return;
    }
    pending_preload_url_ = url;
    preload_timer->start(std::max(0, WEB_LINK_POPUP_PRELOAD_DELAY_MS));
}

void WebLinkPopup::cancel_preload() {
    pending_preload_url_ = QUrl();
    preload_timer->stop();
}

void WebLinkPopup::open_url(const QUrl& url) {
    cancel_preload();
    start_loading(url);
    if (parentWidget()) {
        update_geometry(parentWidget()->width(), parentWidget()->height());
    }
    show();
    raise();
#ifdef SIOYEK_HAS_WEBENGINE
    web_view->setFocus();
#endif
}

void WebLinkPopup::close_popup() {
    if (!isVisible()) {
        return;
    }
    hide();
    cancel_preload();
    requested_url_ = QUrl();
#ifdef SIOYEK_HAS_WEBENGINE
    // stop any audio/video while hidden; the next hover/click loads a fresh page
    web_view->setUrl(QUrl("about:blank"));
#endif
    emit closed();
}

void WebLinkPopup::open_current_in_browser() {
    if (!url_.isEmpty()) {
        open_web_url(url_.toString().toStdWString());
    }
    close_popup();
}

QUrl WebLinkPopup::current_url() const {
    return url_;
}

void WebLinkPopup::update_geometry(int parent_width, int parent_height) {
    float wf = std::clamp(WEB_LINK_POPUP_SIZE[0], 0.1f, 1.0f);
    float hf = std::clamp(WEB_LINK_POPUP_SIZE[1], 0.1f, 1.0f);
    int w = static_cast<int>(parent_width * wf);
    int h = static_cast<int>(parent_height * hf);
    int x = (parent_width - w) / 2;
    int y = (parent_height - h) / 2;
    setGeometry(x, y, w, h);
    if (!url_.isEmpty()) {
        set_title_text(
#ifdef SIOYEK_HAS_WEBENGINE
            web_view->title().isEmpty() ? url_.toString() : web_view->title()
#else
            url_.toString()
#endif
        );
    }
}

void WebLinkPopup::paintEvent(QPaintEvent*) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

// clicks on the popup's own chrome must not reach the document underneath
void WebLinkPopup::mousePressEvent(QMouseEvent* event) {
    event->accept();
}

void WebLinkPopup::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
}

// wheel events the page did not consume (e.g. at the top/bottom of the page) must not scroll
// the document underneath
void WebLinkPopup::wheelEvent(QWheelEvent* event) {
    event->accept();
}

void WebLinkPopup::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close_popup();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool WebLinkPopup::eventFilter(QObject* watched, QEvent* event) {
#ifdef SIOYEK_HAS_WEBENGINE
    if (watched == web_view && event->type() == QEvent::ChildAdded) {
        QChildEvent* child_event = static_cast<QChildEvent*>(event);
        if (child_event->child() && child_event->child()->isWidgetType()) {
            child_event->child()->installEventFilter(this);
        }
    }
    else if (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride) {
        QKeyEvent* key_event = static_cast<QKeyEvent*>(event);
        if (key_event->key() == Qt::Key_Escape) {
            if (event->type() == QEvent::KeyPress) {
                close_popup();
            }
            event->accept();
            return true;
        }
    }
#endif
    return QWidget::eventFilter(watched, event);
}
