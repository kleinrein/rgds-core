#pragma once
// Toasts: the launcher's one way to say what just happened (toast_screen.hpp draws them on the lower screen and feeds
// them). A card drops in from the top edge on a spring, holds long enough to read, and drifts back up as it fades.
// Up to three stack, the newest on top; the older ones glide down to make room. Saying the same words again while
// they are up does not stack a copy: the card nudges and its clock starts over.
// Pure C++, no SDL, so tests/toast_test.cpp can step it through time.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace toast {

enum class Severity { Info, Success, Warning, Error };

// Words the launcher already uses decide the look when a caller does not say: "Could not save…" is an error,
// "Connect to Wi-Fi first" a warning, "Settings saved" a success. Matched on the English source text.
inline Severity infer(const std::string& source) {
    std::string t;
    t.reserve(source.size());
    for (char c : source) t.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    auto has = [&](const char* word) { return t.find(word) != std::string::npos; };
    auto starts = [&](const char* word) { return t.rfind(word, 0) == 0; };
    if (has("could not") || has("couldn't") || has("did not") || has("didn't") || has("cannot") || has("can't") ||
        has("failed") || has("error") || has("not finish") || has("not be "))
        return Severity::Error;
    if (has(" first") || starts("wait") || has("wait for") || starts("stop ") || has("not available") ||
        has("unavailable") || has("not on this profile") || has("is full") || has("room is full") ||
        starts("one moment") || starts("finishing") || has("requires") || has("one download at a time") ||
        has("no button detected") || has("only "))
        return Severity::Warning;
    if (has("saved") || starts("added") || has("removed") || has("erased") || starts("copied") || starts("moved") ||
        has("welcome back") || has("cover now") || has("plays next") || has("added to") || has(" on.") ||
        starts("done") || starts("using "))
        return Severity::Success;
    return Severity::Info;
}

// How long a card stays: long enough to read at a glance, more for longer words, more again for errors.
inline uint32_t hold_ms(const std::string& text, Severity severity) {
    const uint32_t chars = uint32_t(text.size());
    uint32_t hold = std::clamp<uint32_t>(2200 + chars * 42, 2800, 6500);
    if (severity == Severity::Error) hold += 1500;
    else if (severity == Severity::Warning) hold += 700;
    return hold;
}

constexpr uint32_t enter_ms = 620; // the spring has settled to under a pixel by then
constexpr uint32_t leave_ms = 300; // drift up and fade
constexpr uint32_t nudge_ms = 420; // the shake when the same words come again
constexpr size_t max_shown = 3;    // a fourth pushes the oldest out
constexpr int gap = 8;             // between stacked cards

struct Card {
    uint64_t id = 0;
    std::string text; // already translated
    Severity severity = Severity::Info;
    uint32_t born = 0, until = 0, nudged = 0, left = 0; // left: when it started leaving, 0 while it stays
    int height = 0;                                     // set by the screen once it has laid the words out
    double shown_y = 0;                                 // where the stack slot is drawn, gliding towards target_y
    double target_y = 0;
    bool placed = false; // shown_y has been set once
};

// The spring of a card coming in: 1 is "still above the screen", 0 is "home". Damped, so it overshoots a few
// pixels and settles; ζ .52, ω 21 rad/s.
inline double enter_offset(uint32_t age) {
    if (age >= enter_ms) return 0;
    const double t = age / 1000.0, zeta = .52, omega = 21, damped = omega * std::sqrt(1 - zeta * zeta);
    return std::exp(-zeta * omega * t) * std::cos(damped * t);
}
inline double enter_alpha(uint32_t age) { return std::min(1.0, age / 170.0); }
// Leaving: 0 at the start, 1 when gone; eased in, so it lifts off gently and then goes.
inline double leave_progress(uint32_t since) {
    const double k = std::min(1.0, since / double(leave_ms));
    return k * k * (3 - 2 * k);
}
// The nudge: a few quick swings that die away, in pixels.
inline double nudge_offset(uint32_t since) {
    if (since >= nudge_ms) return 0;
    const double t = since / 1000.0;
    return 9 * std::exp(-t * 11) * std::sin(t * 2 * M_PI * 9);
}

struct Stack {
    std::vector<Card> cards; // newest first
    uint64_t next_id = 1;
    bool animations = true;

    // Returns the card's id. The same words while their card is still up refresh it instead.
    uint64_t post(const std::string& text, Severity severity, uint32_t now) {
        if (text.empty()) return 0;
        for (auto& card : cards)
            if (!card.left && card.text == text) {
                card.severity = severity;
                card.until = now + hold_ms(text, severity);
                card.nudged = animations ? now : 0;
                return card.id;
            }
        Card card;
        card.id = next_id++;
        card.text = text;
        card.severity = severity;
        card.born = now;
        card.until = now + hold_ms(text, severity);
        cards.insert(cards.begin(), card);
        size_t staying = 0;
        for (auto& c : cards)
            if (!c.left && ++staying > max_shown) c.left = now;
        return card.id;
    }
    void dismiss(uint64_t id, uint32_t now) {
        for (auto& card : cards)
            if (card.id == id && !card.left) card.left = now;
    }
    void dismiss_all(uint32_t now) {
        for (auto& card : cards)
            if (!card.left) card.left = now;
    }
    // Starts cards whose time is up on their way out and drops the ones that are gone. True when anything changed.
    bool tick(uint32_t now) {
        bool changed = false;
        for (auto& card : cards)
            if (!card.left && int32_t(now - card.until) >= 0) {
                card.left = now;
                changed = true;
            }
        const auto before = cards.size();
        cards.erase(std::remove_if(cards.begin(), cards.end(),
                                   [&](const Card& c) {
                                       return c.left && (!animations || int32_t(now - c.left) >= int32_t(leave_ms));
                                   }),
                    cards.end());
        return changed || cards.size() != before;
    }
    // Slots from `top` down; a leaving card keeps its place so the ones under it move only once it is gone.
    void layout(int top) {
        double y = top;
        for (auto& card : cards) {
            card.target_y = y;
            if (!card.placed) {
                card.shown_y = y;
                card.placed = true;
            }
            y += card.height + gap;
        }
    }
    // The slot glide, framerate independent (time constant 70 ms).
    void glide(uint32_t dt) {
        const double k = animations ? 1 - std::exp(-double(dt) / 70.0) : 1;
        for (auto& card : cards) {
            card.shown_y += (card.target_y - card.shown_y) * k;
            if (std::abs(card.target_y - card.shown_y) < .5) card.shown_y = card.target_y;
        }
    }
    bool moving(uint32_t now) const {
        if (!animations) return false;
        for (const auto& card : cards) {
            if (now - card.born < enter_ms || card.left || (card.nudged && now - card.nudged < nudge_ms) ||
                card.shown_y != card.target_y)
                return true;
        }
        return false;
    }
    // Milliseconds until a card has to start leaving; -1 with nothing up.
    int wait_ms(uint32_t now) const {
        int wait = -1;
        for (const auto& card : cards)
            if (!card.left) {
                const int left = std::max(1, int(int32_t(card.until - now)));
                wait = wait < 0 ? left : std::min(wait, left);
            }
        return wait;
    }
    bool empty() const { return cards.empty(); }
};

} // namespace toast
