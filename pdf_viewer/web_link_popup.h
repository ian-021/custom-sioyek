#pragma once

#include <qwidget.h>
#include <qstring.h>
#include <qurl.h>

class QLabel;
class QTimer;
class QPushButton;
class QWebEngineView;

// A floating, in-window preview of a web link, in the spirit of gwern.net's link popups.
// Clicking an http(s) link in the document opens the page here instead of in the system
// browser. Escape closes it, and the header has a button to hand the page off to the browser.
class WebLinkPopup : public QWidget {
    Q_OBJECT

public:
    explicit WebLinkPopup(QWidget* parent);

    // true when this build was compiled with Qt WebEngine and can actually render pages
    static bool is_available();

    void open_url(const QUrl& url);
    // start loading a url in the hidden popup after a short delay, so that a following click
    // shows an already (partially) loaded page. Only the most recently hovered url is loaded.
    void preload_url(const QUrl& url);
    void cancel_preload();
    void close_popup();
    void open_current_in_browser();
    QUrl current_url() const;

    // resize/reposition relative to the parent window using the web_link_popup_size config
    void update_geometry(int parent_width, int parent_height);

signals:
    void closed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void apply_style();
    void set_title_text(const QString& text);

    QLabel* title_label = nullptr;
    QPushButton* back_button = nullptr;
    QPushButton* open_button = nullptr;
    QPushButton* close_button = nullptr;
#ifdef SIOYEK_HAS_WEBENGINE
    QWebEngineView* web_view = nullptr;
#endif
    QUrl url_;              // what the web view is currently showing
    QUrl requested_url_;    // the last url we asked it to load (differs from url_ after redirects)
    QUrl pending_preload_url_;
    QTimer* preload_timer = nullptr;
    void start_loading(const QUrl& url);
};
