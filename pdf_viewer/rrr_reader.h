#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <qwidget.h>
#include <qstring.h>
#include <mupdf/fitz.h>

class Document;
class QTimer;
class QVariantAnimation;

// One displayed word, with its focus letter and how long it stays on screen.
struct RrrWord {
    std::wstring text;
    int page = 0;
    // in page coordinates, so closing the reader can show where it stopped
    fz_rect rect = fz_empty_rect;
    // index of the letter the eye fixes on (the "optimal recognition point")
    int pivot = 0;
    // display time in beats (60 / wpm seconds), not counting a paragraph pause
    float weight = 1.0f;
    bool ends_sentence = false;
    bool ends_paragraph = false;

    RrrWord(std::wstring text, int page, fz_rect rect);
    float display_weight() const;
    bool ends_sentence_or_paragraph() const;
};

// A text block of a page: its unwrapped text (to spot running headers and page numbers) and its words.
struct RrrBlock {
    std::wstring text;
    std::vector<RrrWord> words;
};

// A page's words with running headers and page numbers removed.
struct RrrPageWords {
    std::vector<RrrWord> words;
    // the page's first block starts lowercase, so it continues the previous page's last paragraph
    bool continues = false;
};

// rrr, the RSVP speed reader, as an overlay over the document: one word at a time, its focus letter
// pinned to the center. Hold space to read, release to stop. Text is pulled from the document a page
// at a time, starting at the clicked word.
class RrrReader : public QWidget {
    Q_OBJECT

public:
    explicit RrrReader(QWidget* parent);

    // starts at the word at (or nearest to) x, y on the page; false if there is no text from there on
    bool open(Document* doc, int page, float x, float y);
    void close_reader();
    std::optional<RrrWord> current_word() const;
    // configs changed from the keyboard while open (speed, font size), for the caller to remember
    std::vector<std::wstring> changed_configs() const;

    void update_geometry(int parent_width, int parent_height);

signals:
    void closed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    // loading
    const std::vector<RrrBlock>& blocks_on(int page);
    const RrrPageWords& words_on(int page);
    void load_range(int first, int last);
    bool load_next_page();
    bool load_previous_page();
    void ensure_ahead();

    // playback
    void play();
    void pause();
    void advance();
    void schedule_next();
    void step(int delta);
    void previous_sentence();
    void next_sentence();
    void seek(double fraction);
    void change_speed(int delta);
    void change_font_size(int delta);

    double progress() const;
    double seconds_left() const;
    QRectF progress_bar_rect() const;
    void animate_bar(double to);

    Document* doc = nullptr;
    int num_pages = 0;
    QString title;
    std::map<int, std::vector<RrrBlock>> page_blocks;
    std::map<int, RrrPageWords> page_words;

    // words of pages first_page..last_page, joined across page breaks
    std::vector<RrrWord> words;
    int index = 0;
    int first_page = 0;
    int last_page = 0;

    bool playing = false;
    bool seeking = false;
    int wpm_at_open = 0;
    int font_size_at_open = 0;
    QTimer* timer = nullptr;
    // the status bar fades while reading
    double bar_opacity = 1.0;
    QVariantAnimation* bar_animation = nullptr;
};
