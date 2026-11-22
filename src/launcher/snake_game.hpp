#pragma once
// Snake, the launcher's hidden game (snake_screen.hpp draws and drives it). Pure C++, no SDL, so
// tests/snake_test.cpp can play it step by step.
// A nod to the phone game everyone had: nine speeds, an apple worth the speed, a bonus critter after every fifth
// apple that walks about and is worth less the longer it waits, the swallowed apple travelling down the body as a
// bulge, and a blink when it is over. Edges wrap unless Walls is on.
// The board fills the upper screen under the status bar: 27 x 18 cells of 24 px, 648 x 432, its outer columns
// running 4 px past each side.
// Each step also says what moved (the tail cell let go, the cell eaten, where the critter walked from), so the
// screen can glide everything between steps instead of jumping.
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace snake {

constexpr int cols = 27, rows = 18;
constexpr int levels = 9;
constexpr int bonus_every = 5;  // apples between critters
constexpr int bonus_steps = 60; // how long a critter waits, in steps
constexpr int bonus_walk = 3;   // it takes a step of its own every this many

enum class Dir { Up, Down, Left, Right };
inline bool opposite(Dir a, Dir b) {
    return (a == Dir::Up && b == Dir::Down) || (a == Dir::Down && b == Dir::Up) ||
           (a == Dir::Left && b == Dir::Right) || (a == Dir::Right && b == Dir::Left);
}
inline bool vertical(Dir d) { return d == Dir::Up || d == Dir::Down; }

struct Cell {
    int x = 0, y = 0;
    bool operator==(const Cell& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

// What one step did, for the sound, the buzz and the screen.
enum Event : unsigned { None = 0, Ate = 1, AteBonus = 2, Died = 4, BonusShown = 8, BonusGone = 16, Won = 32 };

// Milliseconds a step at each speed: about 5 cells a second at 1, 20 at 9.
inline int step_ms(int level) { return 230 - 20 * std::clamp(level, 1, levels); }

inline Cell moved(Cell c, Dir d) {
    c.x += d == Dir::Left ? -1 : d == Dir::Right ? 1 : 0;
    c.y += d == Dir::Up ? -1 : d == Dir::Down ? 1 : 0;
    return c;
}
inline Cell wrapped(Cell c) { return {(c.x % cols + cols) % cols, (c.y % rows + rows) % rows}; }

struct Game {
    std::deque<Cell> body; // body.front() is the head
    Dir dir = Dir::Right;
    // Turns pressed between steps, at most two, so a quick Up-Left inside one step is both turns, in order.
    std::array<Dir, 2> queued{};
    int queued_count = 0;
    Cell food{};
    // The critter: two cells wide, `bonus_left` steps until it goes.
    Cell bonus{};
    int bonus_left = 0;
    int level = 5, score = 0, apples = 0, grow = 0;
    bool walls = false, over = false, won = false;
    // What the last step moved, for the gliding: `steps` counts them.
    int steps = 0;
    Cell last_tail{};          // the cell the tail let go of
    bool tail_moved = false;   // false while growing: the tail stayed put
    Cell ate_cell{};           // the apple or critter cell eaten...
    int ate_step = -1;         // ...on this step
    Cell bonus_from{};         // where the critter walked from...
    int bonus_moved_step = -1; // ...on this step
    bool bonus_facing_left = false;
    // Swallowed apples on their way down: each is an index from the head, one further every step.
    std::vector<int> bulges;
    uint32_t seed = 0x2545F491;

    uint32_t random() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    }

    void reset(int speed, bool walled, uint32_t start_seed) {
        level = std::clamp(speed, 1, levels);
        walls = walled;
        seed = start_seed ? start_seed : 0x2545F491;
        body.clear();
        // Five long, along the middle row, heading right.
        for (int i = 0; i < 5; i++) body.push_back({cols / 2 - i, rows / 2});
        dir = Dir::Right;
        queued_count = 0;
        score = apples = grow = 0;
        bonus_left = 0;
        over = won = false;
        bulges.clear();
        steps = 0;
        tail_moved = false;
        ate_step = bonus_moved_step = -1;
        place_food();
    }

    bool on_body(Cell c) const { return std::find(body.begin(), body.end(), c) != body.end(); }
    bool on_bonus(Cell c) const { return bonus_left > 0 && c.y == bonus.y && (c.x == bonus.x || c.x == bonus.x + 1); }

    // A random free cell, or false when the board is full.
    bool free_cell(Cell& out, bool wide = false) {
        std::vector<Cell> open;
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols - (wide ? 1 : 0); x++) {
                const Cell c{x, y}, beside{x + 1, y};
                if (on_body(c) || on_bonus(c) || c == food) continue;
                if (wide && (on_body(beside) || on_bonus(beside) || beside == food || c == food)) continue;
                open.push_back(c);
            }
        if (open.empty()) return false;
        out = open[random() % open.size()];
        return true;
    }
    bool place_food() {
        Cell c;
        if (!free_cell(c)) return false;
        food = c;
        return true;
    }

    // The way the snake will go next: the last turn waiting, else where it is going.
    Dir heading() const { return queued_count ? queued[size_t(queued_count - 1)] : dir; }

    // A press: kept for the next step unless it is the way it already goes or straight back.
    bool turn(Dir d) {
        if (over) return false;
        // A third press replaces the second, so the newest wins; it is checked against the turn before it, never
        // against the one it replaces, or Up-Left-Down would queue a U-turn into the neck.
        const bool full = queued_count == 2;
        const Dir last = full ? queued[0] : heading();
        if (d == last || opposite(d, last)) return false;
        if (full) queued[1] = d;
        else queued[size_t(queued_count++)] = d;
        return true;
    }

    // The critter's own step: one cell any way that is free (it never wraps or steps under the snake).
    void walk_bonus() {
        const Dir ways[4] = {Dir::Up, Dir::Down, Dir::Left, Dir::Right};
        const int first = int(random() % 4);
        for (int i = 0; i < 4; i++) {
            const Dir d = ways[(first + i) % 4];
            const Cell to = moved(bonus, d), beside{to.x + 1, to.y};
            if (to.x < 0 || to.x + 1 >= cols || to.y < 0 || to.y >= rows) continue;
            if (on_body(to) || on_body(beside) || to == food || beside == food) continue;
            bonus_from = bonus;
            bonus = to;
            bonus_moved_step = steps;
            if (d == Dir::Left || d == Dir::Right) bonus_facing_left = d == Dir::Left;
            return;
        }
    }

    // Would the head be safe in `c` next step? (The tail moves away unless the snake is growing.)
    bool safe(Cell c) const {
        if (c.x < 0 || c.x >= cols || c.y < 0 || c.y >= rows) {
            if (walls) return false;
            c = wrapped(c);
        }
        for (size_t i = 0; i + (grow == 0 ? 1 : 0) < body.size(); i++)
            if (body[i] == c) return false;
        return true;
    }

    // The attract mode's pilot: of the safe ways (on, left, right) the one nearest the apple, preferring room to move.
    void autopilot() {
        if (over) return;
        const Dir options[3] = {dir, vertical(dir) ? Dir::Left : Dir::Up, vertical(dir) ? Dir::Right : Dir::Down};
        int best = -1, best_score = 1 << 30;
        for (int i = 0; i < 3; i++) {
            Cell c = moved(body.front(), options[i]);
            if (!safe(c)) continue;
            c = wrapped(c);
            int dx = std::abs(c.x - food.x), dy = std::abs(c.y - food.y);
            if (!walls) {
                dx = std::min(dx, cols - dx);
                dy = std::min(dy, rows - dy);
            }
            int room = 0;
            for (Dir d : {Dir::Up, Dir::Down, Dir::Left, Dir::Right}) room += safe(moved(c, d)) ? 1 : 0;
            const int score_here = (dx + dy) * 4 + (room <= 1 ? 60 : 0) + (i == 0 ? 0 : 1);
            if (score_here < best_score) {
                best_score = score_here;
                best = i;
            }
        }
        queued_count = 0;
        if (best > 0) turn(options[best]);
    }

    unsigned step() {
        if (over) return None;
        if (queued_count) {
            dir = queued[0];
            queued[0] = queued[1];
            queued_count--;
        }
        Cell head = moved(body.front(), dir);
        if (head.x < 0 || head.x >= cols || head.y < 0 || head.y >= rows) {
            if (walls) {
                over = true;
                return Died;
            }
            head.x = (head.x + cols) % cols;
            head.y = (head.y + rows) % rows;
        }
        // The tail moves out of the way first unless the snake is growing, so chasing it is safe.
        const bool tail_leaves = grow == 0;
        for (size_t i = 0; i + (tail_leaves ? 1 : 0) < body.size(); i++)
            if (body[i] == head) {
                over = true;
                return Died;
            }
        unsigned events = None;
        steps++;
        body.push_front(head);
        tail_moved = grow == 0;
        if (grow > 0) grow--;
        else {
            last_tail = body.back();
            body.pop_back();
        }
        for (auto& b : bulges) b++;
        bulges.erase(std::remove_if(bulges.begin(), bulges.end(), [&](int b) { return b >= int(body.size()); }),
                     bulges.end());
        if (bonus_left > 0) {
            if (on_bonus(head)) {
                // Worth more the sooner it is caught.
                score += level * (5 + bonus_left / 4);
                bonus_left = 0;
                events |= AteBonus;
                bulges.push_back(0);
                ate_cell = head;
                ate_step = steps;
            } else if (--bonus_left == 0) events |= BonusGone;
            else if (bonus_left % bonus_walk == 0) walk_bonus();
        }
        if (head == food) {
            ate_cell = head;
            ate_step = steps;
            score += level;
            apples++;
            grow++;
            bulges.push_back(0);
            events |= Ate;
            if (!place_food()) {
                over = won = true;
                return events | Won;
            }
            if (apples % bonus_every == 0 && bonus_left == 0) {
                Cell c;
                if (free_cell(c, true)) {
                    bonus = bonus_from = c;
                    bonus_left = bonus_steps;
                    events |= BonusShown;
                }
            }
        }
        return events;
    }
};

} // namespace snake
