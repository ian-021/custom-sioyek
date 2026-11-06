#include "rrr_reader.h"
#include "document.h"

#include <algorithm>
#include <cmath>
#include <regex>
#include <set>

#include <qevent.h>
#include <qfileinfo.h>
#include <qfontdatabase.h>
#include <qpainter.h>
#include <qtimer.h>
#include <qvariantanimation.h>

extern int RRR_WPM;
extern int RRR_FONT_SIZE;
extern std::wstring RRR_FONT;
extern float RRR_BACKGROUND_COLOR[3];
extern float RRR_TEXT_COLOR[3];
extern float RRR_FOCUS_COLOR[3];

static const int MIN_WPM = 100;
static const int MAX_WPM = 1000;
static const int WPM_STEP = 25;
static const int MIN_FONT_SIZE = 12;
static const int MAX_FONT_SIZE = 400;
static const int FONT_SIZE_STEP = 8;
// words kept loaded ahead of the reader, so a page break never stalls playback
static const int WORDS_AHEAD = 400;

static const QString HINT = QString::fromUtf8(
    "hold space to read     ← →  word     ⌥← ⌥→  sentence     ↑ ↓  speed     ⌘+ ⌘−  size     esc  back to the page");

// ---------- words ----------

RrrWord::RrrWord(std::wstring text_, int page_, fz_rect rect_) : text(std::move(text_)), page(page_), rect(rect_) {
    std::vector<int> letters;
    for (int i = 0; i < static_cast<int>(text.size()); i++) {
        if (QChar::isLetterOrNumber(static_cast<char32_t>(text[i]))) {
            letters.push_back(i);
        }
    }
    int n = static_cast<int>(letters.size());
    if (n == 0) pivot = std::max(0, static_cast<int>(text.size() - 1) / 2);
    else if (n == 1) pivot = letters[0];
    else if (n <= 5) pivot = letters[1];
    else if (n <= 9) pivot = letters[2];
    else if (n <= 13) pivot = letters[3];
    else pivot = letters[4];

    // the last character before any closing quotes and brackets: " ' ” ’ ) ] } » * _
    size_t end = text.find_last_not_of(L"\"'”’)]}»*_");
    wchar_t last = end == std::wstring::npos ? 0 : text[end];
    ends_sentence = last != 0 && std::wstring(L".!?…").find(last) != std::wstring::npos;
    if (ends_sentence) {
        weight = 2.2f;
    }
    else if (last != 0 && std::wstring(L",;:–—").find(last) != std::wstring::npos) {
        weight = 1.6f;
    }
    if (text.size() > 8) {
        weight += (std::min(static_cast<int>(text.size()), 20) - 8) * 0.06f;
    }
}

float RrrWord::display_weight() const {
    return ends_paragraph ? std::max(weight, 2.8f) : weight;
}

bool RrrWord::ends_sentence_or_paragraph() const {
    return ends_sentence || ends_paragraph;
}

// ---------- text extraction ----------

static bool starts_lowercase(fz_stext_line* line) {
    if (line == nullptr) return false;
    for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
        if (!QChar::isSpace(static_cast<char32_t>(ch->c))) {
            return QChar::isLower(static_cast<char32_t>(ch->c));
        }
    }
    return false;
}

// Splits a page's text blocks into words, re-joining words split by a line-end hyphen.
static void collect_blocks(fz_stext_block* block, int page, std::vector<RrrBlock>& out) {
    for (; block; block = block->next) {
        if (block->type == FZ_STEXT_BLOCK_STRUCT) {
            if (block->u.s.down) {
                collect_blocks(block->u.s.down->first_block, page, out);
            }
            continue;
        }
        if (block->type != FZ_STEXT_BLOCK_TEXT) continue;

        RrrBlock result;
        std::wstring word;
        fz_rect rect = fz_empty_rect;
        // a word continued on the next line keeps the rect of its first line
        fz_stext_line* word_line = nullptr;
        auto finish_word = [&]() {
            if (!word.empty()) {
                result.words.emplace_back(word, page, rect);
            }
            word.clear();
            rect = fz_empty_rect;
            word_line = nullptr;
        };

        for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
            bool soft_hyphen = false;
            for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                char32_t c = static_cast<char32_t>(ch->c);
                if (c == 0xAD) {
                    soft_hyphen = true;
                    continue;
                }
                soft_hyphen = false;
                if (QChar::isSpace(c)) {
                    finish_word();
                    continue;
                }
                if (word.empty()) word_line = line;
                word.push_back(static_cast<wchar_t>(c));
                if (line == word_line) {
                    rect = fz_union_rect(rect, fz_rect_from_quad(ch->quad));
                }
            }
            bool hard_hyphen = word.size() > 1 && word.back() == L'-'
                && QChar::isLetterOrNumber(static_cast<char32_t>(word[word.size() - 2]));
            if ((soft_hyphen || hard_hyphen) && starts_lowercase(line->next)) {
                if (hard_hyphen) word.pop_back();
            }
            else {
                finish_word();
            }
        }
        finish_word();
        if (result.words.empty()) continue;

        result.words.back().ends_paragraph = true;
        for (const RrrWord& w : result.words) {
            if (!result.text.empty()) result.text.push_back(L' ');
            result.text += w.text;
        }
        out.push_back(std::move(result));
    }
}

// Running headers are compared with their page numbers masked out.
static std::wstring header_key(const std::wstring& text) {
    if (text.size() >= 100) return text;
    std::wstring key;
    bool in_number = false;
    for (wchar_t c : text) {
        bool digit = c >= L'0' && c <= L'9';
        if (!digit) key.push_back(static_cast<wchar_t>(QChar::toLower(static_cast<char32_t>(c))));
        else if (!in_number) key.push_back(L'#');
        in_number = digit;
    }
    return key;
}

static bool is_at_edge(size_t i, size_t count) {
    return i < 2 || i + 2 >= count;
}

static std::set<std::wstring> edge_keys(const std::vector<RrrBlock>& blocks) {
    std::set<std::wstring> keys;
    for (size_t i = 0; i < blocks.size(); i++) {
        if (is_at_edge(i, blocks.size()) && blocks[i].text.size() < 100) {
            keys.insert(header_key(blocks[i].text));
        }
    }
    return keys;
}

static bool is_page_number(const std::wstring& text) {
    static const std::wregex pattern(L"(page\\s*)?\\d+(\\s*(of|/)\\s*\\d+)?", std::regex::icase);
    return std::regex_match(text, pattern);
}

// Appends `next` to `words`. A block that starts lowercase continues the previous one (a page or
// column break): a trailing hyphen re-joins the split word, otherwise there's no paragraph pause.
static void join(std::vector<RrrWord>& words, const std::vector<RrrWord>& next, bool continues) {
    size_t start = 0;
    if (continues && !words.empty() && !next.empty()) {
        RrrWord& last = words.back();
        if (last.text.size() > 1 && last.text.back() == L'-') {
            RrrWord merged(last.text.substr(0, last.text.size() - 1) + next[0].text, last.page, last.rect);
            merged.ends_paragraph = next[0].ends_paragraph;
            last = merged;
            start = 1;
        }
        else if (std::wstring(L".!?:;\"”’)]").find(last.text.back()) == std::wstring::npos) {
            last.ends_paragraph = false;
        }
    }
    words.insert(words.end(), next.begin() + start, next.end());
}

const std::vector<RrrBlock>& RrrReader::blocks_on(int page) {
    auto found = page_blocks.find(page);
    if (found != page_blocks.end()) return found->second;

    std::vector<RrrBlock> blocks;
    if (fz_stext_page* stext = doc->get_stext_with_page_number(page)) {
        collect_blocks(stext->first_block, page, blocks);
    }
    return page_blocks.emplace(page, std::move(blocks)).first->second;
}

const RrrPageWords& RrrReader::words_on(int page) {
    auto found = page_words.find(page);
    if (found != page_words.end()) return found->second;

    // Running headers and footers: short text at the top or bottom of the page that recurs at the
    // edges of at least two of the pages up to four away (so headers that alternate between left
    // and right pages count too).
    std::vector<std::set<std::wstring>> nearby;
    for (int other = std::max(page - 4, 0); other <= std::min(page + 4, num_pages - 1); other++) {
        if (other != page) {
            nearby.push_back(edge_keys(blocks_on(other)));
        }
    }

    const std::vector<RrrBlock>& blocks = blocks_on(page);
    RrrPageWords result;
    for (size_t i = 0; i < blocks.size(); i++) {
        const RrrBlock& block = blocks[i];
        if (is_at_edge(i, blocks.size())) {
            if (is_page_number(block.text)) continue;
            if (block.text.size() < 100) {
                std::wstring key = header_key(block.text);
                int seen = std::count_if(nearby.begin(), nearby.end(), [&](const auto& keys) { return keys.count(key) > 0; });
                if (seen >= 2) continue;
            }
        }
        bool continues = QChar::isLower(static_cast<char32_t>(block.text[0]));
        if (result.words.empty()) result.continues = continues;
        join(result.words, block.words, continues);
    }
    return page_words.emplace(page, std::move(result)).first->second;
}

void RrrReader::load_range(int first, int last) {
    words.clear();
    for (int page = first; page <= last; page++) {
        const RrrPageWords& next = words_on(page);
        join(words, next.words, next.continues);
    }
    first_page = first;
    last_page = last;
}

bool RrrReader::load_next_page() {
    while (last_page + 1 < num_pages) {
        last_page++;
        const RrrPageWords& next = words_on(last_page);
        if (!next.words.empty()) {
            join(words, next.words, next.continues);
            return true;
        }
    }
    return false;
}

// Reloads from the previous page with text, staying on the current word. False at the start.
bool RrrReader::load_previous_page() {
    int previous = first_page - 1;
    while (previous >= 0 && words_on(previous).words.empty()) previous--;
    if (previous < 0) return false;

    RrrWord current = words[index];
    load_range(previous, last_page);
    auto same = std::find_if(words.begin(), words.end(), [&](const RrrWord& w) {
        return w.page == current.page && w.rect.x0 == current.rect.x0 && w.rect.y0 == current.rect.y0;
    });
    if (same != words.end()) {
        index = static_cast<int>(same - words.begin());
    }
    else {
        // it was the first word of its page and merged into the hyphenated last word of the page before
        auto later = std::find_if(words.begin(), words.end(), [&](const RrrWord& w) { return w.page >= current.page; });
        index = std::max(0, static_cast<int>(later - words.begin()) - 1);
    }
    return true;
}

void RrrReader::ensure_ahead() {
    while (static_cast<int>(words.size()) - index < WORDS_AHEAD && load_next_page()) {}
}

// ---------- widget ----------

RrrReader::RrrReader(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // the document underneath sets its own cursor (e.g. a hand over links), which children inherit
    setCursor(Qt::ArrowCursor);

    timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, &RrrReader::advance);

    bar_animation = new QVariantAnimation(this);
    bar_animation->setDuration(250);
    bar_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(bar_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        bar_opacity = value.toDouble();
        update();
    });

    hide();
}

bool RrrReader::open(Document* document, int page, float x, float y) {
    pause();
    doc = document;
    num_pages = doc->num_pages();
    // read the text afresh each time: the file may have been reloaded since
    page_blocks.clear();
    page_words.clear();
    words.clear();
    index = 0;
    title = QFileInfo(QString::fromStdWString(doc->get_path())).completeBaseName();

    int start = std::max(page, 0);
    while (start < num_pages && words_on(start).words.empty()) start++;
    if (start >= num_pages) return false;
    load_range(start, start);

    if (start == page) {
        // the clicked word, or the nearest one, preferring words on the clicked line
        float best = INFINITY;
        for (int i = 0; i < static_cast<int>(words.size()); i++) {
            const fz_rect& r = words[i].rect;
            float dx = std::max({ r.x0 - x, 0.0f, x - r.x1 });
            float dy = std::max({ r.y0 - y, 0.0f, y - r.y1 });
            float distance = dx * dx + 4 * dy * dy;
            if (distance < best) {
                best = distance;
                index = i;
            }
        }
    }
    ensure_ahead();
    wpm_at_open = RRR_WPM;
    font_size_at_open = RRR_FONT_SIZE;

    bar_animation->stop();
    bar_opacity = 1.0;
    if (parentWidget()) {
        update_geometry(parentWidget()->width(), parentWidget()->height());
    }
    show();
    raise();
    setFocus();
    update();
    return true;
}

void RrrReader::close_reader() {
    if (!isVisible()) return;
    pause();
    hide();
    emit closed();
}

std::optional<RrrWord> RrrReader::current_word() const {
    if (words.empty()) return {};
    return words[index];
}

std::vector<std::wstring> RrrReader::changed_configs() const {
    std::vector<std::wstring> names;
    if (RRR_WPM != wpm_at_open) names.push_back(L"rrr_wpm");
    if (RRR_FONT_SIZE != font_size_at_open) names.push_back(L"rrr_font_size");
    return names;
}

void RrrReader::update_geometry(int parent_width, int parent_height) {
    setGeometry(0, 0, parent_width, parent_height);
}

// ---------- playback ----------

void RrrReader::play() {
    if (words.empty() || playing) return;
    playing = true;
    animate_bar(0.25);
    schedule_next();
}

void RrrReader::pause() {
    if (!playing) return;
    playing = false;
    timer->stop();
    animate_bar(1.0);
}

void RrrReader::schedule_next() {
    double ms = words[index].display_weight() * 60000.0 / RRR_WPM;
    timer->start(std::max(1, static_cast<int>(std::lround(ms))));
}

void RrrReader::advance() {
    if (index + 1 >= static_cast<int>(words.size())) {
        ensure_ahead();
        if (index + 1 >= static_cast<int>(words.size())) {
            pause();
            return;
        }
    }
    index++;
    schedule_next();
    repaint();
    ensure_ahead();
}

void RrrReader::step(int delta) {
    if (words.empty()) return;
    int target = index + delta;
    if (target < 0 && load_previous_page()) target = index + delta;
    index = std::clamp(target, 0, static_cast<int>(words.size()) - 1);
    ensure_ahead();
    update();
}

// Back to the start of the current sentence, or the previous one if already there.
void RrrReader::previous_sentence() {
    if (words.empty()) return;
    if (index == 0 && !load_previous_page()) return;
    int i = index - 1;
    // a sentence can begin on an earlier page; look back a few pages at most
    for (int loads = 0; ; loads++) {
        while (i > 0 && !words[i - 1].ends_sentence_or_paragraph()) i--;
        if (i > 0 || loads == 3) break;
        int before = index;
        if (!load_previous_page()) break;
        i += index - before;
    }
    index = std::max(i, 0);
    update();
}

void RrrReader::next_sentence() {
    if (words.empty()) return;
    int i = index;
    while (!words[i].ends_sentence_or_paragraph()) {
        if (i + 1 >= static_cast<int>(words.size()) && !load_next_page()) break;
        i++;
    }
    if (i + 1 >= static_cast<int>(words.size())) load_next_page();
    index = std::min(i + 1, static_cast<int>(words.size()) - 1);
    ensure_ahead();
    update();
}

// Jumps to a fraction of the whole document, reloading from that page if it isn't loaded.
void RrrReader::seek(double fraction) {
    if (words.empty()) return;
    double target = std::clamp(fraction, 0.0, 1.0) * num_pages;
    int page = std::min(static_cast<int>(target), num_pages - 1);
    double within = target - page;

    if (page < first_page || page > last_page) {
        // skip pages without text (figures, blank pages), forwards first
        int with_text = page;
        while (with_text < num_pages && words_on(with_text).words.empty()) with_text++;
        if (with_text == num_pages) {
            with_text = page;
            while (with_text >= 0 && words_on(with_text).words.empty()) with_text--;
        }
        if (with_text < 0) return;
        if (with_text != page) within = 0;
        page = with_text;
        load_range(page, page);
    }

    auto first = std::find_if(words.begin(), words.end(), [&](const RrrWord& w) { return w.page >= page; });
    auto last = std::find_if(first, words.end(), [&](const RrrWord& w) { return w.page > page; });
    int offset = static_cast<int>(within * (last - first));
    index = std::clamp(static_cast<int>(first - words.begin()) + offset, 0, static_cast<int>(words.size()) - 1);
    ensure_ahead();
    update();
}

void RrrReader::change_speed(int delta) {
    RRR_WPM = std::clamp(RRR_WPM + delta, MIN_WPM, MAX_WPM);
    update();
}

void RrrReader::change_font_size(int delta) {
    RRR_FONT_SIZE = std::clamp(RRR_FONT_SIZE + delta, MIN_FONT_SIZE, MAX_FONT_SIZE);
    update();
}

// Position in the whole document, by page and by word within the page.
double RrrReader::progress() const {
    if (words.empty() || num_pages == 0) return 0;
    int page = words[index].page;
    int first = index;
    int last = index;
    while (first > 0 && words[first - 1].page == page) first--;
    while (last + 1 < static_cast<int>(words.size()) && words[last + 1].page == page) last++;
    double within = static_cast<double>(index - first) / std::max(1, last - first);
    return std::min(1.0, (page + within) / num_pages);
}

// The rest of the loaded words, plus the pages not loaded yet at the loaded pages' average.
double RrrReader::seconds_left() const {
    double ahead = 0;
    double total = 0;
    for (int i = 0; i < static_cast<int>(words.size()); i++) {
        double weight = words[i].display_weight();
        total += weight;
        if (i >= index) ahead += weight;
    }
    int loaded_pages = last_page - first_page + 1;
    ahead += total / loaded_pages * (num_pages - 1 - last_page);
    return ahead * 60 / RRR_WPM;
}

void RrrReader::animate_bar(double to) {
    bar_animation->stop();
    bar_animation->setStartValue(bar_opacity);
    bar_animation->setEndValue(to);
    bar_animation->start();
}

// ---------- drawing ----------

static QColor config_color(const float* color, double alpha = 1.0) {
    return QColor::fromRgbF(color[0], color[1], color[2], alpha);
}

static QFont word_font() {
    QFont font = RRR_FONT.empty() ? QFontDatabase::systemFont(QFontDatabase::GeneralFont) : QFont(QString::fromStdWString(RRR_FONT));
    font.setPixelSize(std::max(1, RRR_FONT_SIZE));
    font.setWeight(QFont::Medium);
    return font;
}

static QFont status_font() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setPixelSize(12);
    font.setWeight(QFont::Medium);
    // tabular digits, so the numbers don't jitter as they change
    font.setFeature(QFont::Tag("tnum"), 1);
    return font;
}

// "45 min" or "2h 10m"
static QString format_duration(double seconds) {
    int minutes = std::max(1, static_cast<int>(std::ceil(seconds / 60)));
    return minutes < 60 ? QString("%1 min").arg(minutes) : QString("%1h %2m").arg(minutes / 60).arg(minutes % 60);
}

// The status bar, from the bottom: title and numbers, the progress bar, the key hint.
QRectF RrrReader::progress_bar_rect() const {
    qreal row_height = QFontMetricsF(status_font()).height();
    return QRectF(20, height() - 14 - row_height - 10 - 12, width() - 40, 12);
}

void RrrReader::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.fillRect(rect(), config_color(RRR_BACKGROUND_COLOR));
    if (words.empty()) return;

    // the word, its focus letter centered in the window, between two ticks
    const RrrWord& word = words[index];
    QString before = QString::fromStdWString(word.text.substr(0, word.pivot));
    QString focus = QString::fromStdWString(word.text.substr(word.pivot, 1));
    QString after = QString::fromStdWString(word.text.substr(word.pivot + 1));

    QFont font = word_font();
    QFontMetricsF metrics(font);
    qreal center_x = width() / 2.0;
    qreal center_y = height() / 2.0;
    qreal x = center_x - metrics.horizontalAdvance(before) - metrics.horizontalAdvance(focus) / 2;
    qreal baseline = center_y + (metrics.ascent() - metrics.descent()) / 2;

    p.setFont(font);
    p.setPen(config_color(RRR_TEXT_COLOR));
    p.drawText(QPointF(x, baseline), before);
    p.setPen(config_color(RRR_FOCUS_COLOR));
    p.drawText(QPointF(x + metrics.horizontalAdvance(before), baseline), focus);
    p.setPen(config_color(RRR_TEXT_COLOR));
    p.drawText(QPointF(x + metrics.horizontalAdvance(before + focus), baseline), after);

    qreal tick_height = RRR_FONT_SIZE * 0.22;
    qreal gap = RRR_FONT_SIZE * 0.18;
    QColor tick = config_color(RRR_FOCUS_COLOR, 0.45);
    p.fillRect(QRectF(center_x - 1, center_y - metrics.height() / 2 - gap - tick_height, 2, tick_height), tick);
    p.fillRect(QRectF(center_x - 1, center_y + metrics.height() / 2 + gap, 2, tick_height), tick);

    // the status bar, faded while reading; the hint fades out completely
    QFont small = status_font();
    QFontMetricsF small_metrics(small);
    QRectF bar = progress_bar_rect();
    QRectF row(bar.left(), bar.bottom() + 10, bar.width(), small_metrics.height());
    QRectF hint(bar.left(), bar.top() - 10 - small_metrics.height(), bar.width(), small_metrics.height());
    QColor dim = config_color(RRR_TEXT_COLOR, 0.45);
    p.setFont(small);
    p.setPen(dim);

    double hint_opacity = std::clamp((bar_opacity - 0.25) / 0.75, 0.0, 1.0);
    if (hint_opacity > 0) {
        p.setOpacity(bar_opacity * hint_opacity);
        p.drawText(hint, Qt::AlignCenter, HINT);
    }
    p.setOpacity(bar_opacity);

    double done = progress();
    QRectF track(bar.left(), bar.center().y() - 1.5, bar.width(), 3);
    QRectF filled(track.left(), track.top(), std::max(3.0, track.width() * done), track.height());
    p.setPen(Qt::NoPen);
    p.setBrush(config_color(RRR_TEXT_COLOR, 0.15));
    p.drawRoundedRect(track, 1.5, 1.5);
    p.setBrush(config_color(RRR_TEXT_COLOR, 0.6));
    p.drawRoundedRect(filled, 1.5, 1.5);

    QString numbers = QString("p. %1/%2   %3 wpm   %4%   %5 left")
        .arg(word.page + 1)
        .arg(num_pages)
        .arg(RRR_WPM)
        .arg(static_cast<int>(done * 100))
        .arg(format_duration(seconds_left()));
    p.setPen(dim);
    p.drawText(row, Qt::AlignRight | Qt::AlignVCenter, numbers);
    qreal title_width = std::max(0.0, row.width() - small_metrics.horizontalAdvance(numbers) - 24);
    p.drawText(row, Qt::AlignLeft | Qt::AlignVCenter, small_metrics.elidedText(title, Qt::ElideMiddle, title_width));
}

// ---------- input ----------

// While the reader is open, keys never reach sioyek's own bindings.
void RrrReader::keyPressEvent(QKeyEvent* event) {
    event->accept();
    // ⌘+ and ⌘− (Qt reports ⌘ as Control on macOS); = and _ are the same keys without shift
    if (event->modifiers() & Qt::ControlModifier) {
        switch (event->key()) {
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            change_font_size(FONT_SIZE_STEP);
            return;
        case Qt::Key_Minus:
        case Qt::Key_Underscore:
            change_font_size(-FONT_SIZE_STEP);
            return;
        default:
            break;
        }
    }
    bool alt = event->modifiers() & Qt::AltModifier;
    switch (event->key()) {
    case Qt::Key_Space:
        if (!event->isAutoRepeat()) play();
        break;
    case Qt::Key_Left:
        if (alt) previous_sentence();
        else step(-1);
        break;
    case Qt::Key_Right:
        if (alt) next_sentence();
        else step(1);
        break;
    case Qt::Key_Up:
        change_speed(WPM_STEP);
        break;
    case Qt::Key_Down:
        change_speed(-WPM_STEP);
        break;
    case Qt::Key_Escape:
        close_reader();
        break;
    default:
        break;
    }
}

void RrrReader::keyReleaseEvent(QKeyEvent* event) {
    event->accept();
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        pause();
    }
}

// Click or drag the progress bar to jump.
void RrrReader::mousePressEvent(QMouseEvent* event) {
    event->accept();
    QRectF bar = progress_bar_rect();
    if (event->button() == Qt::LeftButton && bar.adjusted(0, -4, 0, 4).contains(event->position())) {
        seeking = true;
        seek((event->position().x() - bar.left()) / bar.width());
    }
}

void RrrReader::mouseMoveEvent(QMouseEvent* event) {
    event->accept();
    if (seeking) {
        QRectF bar = progress_bar_rect();
        seek((event->position().x() - bar.left()) / bar.width());
    }
}

void RrrReader::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
    seeking = false;
}

void RrrReader::wheelEvent(QWheelEvent* event) {
    event->accept();
}

// a space release that happens elsewhere (e.g. after switching apps) would never reach us
void RrrReader::focusOutEvent(QFocusEvent* event) {
    pause();
    QWidget::focusOutEvent(event);
}
