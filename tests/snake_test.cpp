// Snake, the launcher's easter egg (src/launcher/snake_game.hpp): turning, eating, growing, wrapping, walls, the
// critter and its countdown, under the sanitizers (tests/test_snake.py builds and runs this).
#include "launcher/snake_game.hpp"
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    failures++;
}

snake::Game fresh(bool walls = false, int level = 5) {
    snake::Game g;
    g.reset(level, walls, 7);
    return g;
}

// The food put right where the head goes next.
void feed(snake::Game& g) {
    auto ahead = g.body.front();
    const auto d = g.heading();
    ahead.x += d == snake::Dir::Left ? -1 : d == snake::Dir::Right ? 1 : 0;
    ahead.y += d == snake::Dir::Up ? -1 : d == snake::Dir::Down ? 1 : 0;
    ahead.x = (ahead.x + snake::cols) % snake::cols;
    ahead.y = (ahead.y + snake::rows) % snake::rows;
    g.food = ahead;
}
} // namespace

int main() {
    {
        auto g = fresh();
        check(g.body.size() == 5 && g.dir == snake::Dir::Right, "starts five long, heading right");
        check(!g.on_body(g.food), "the first apple is on a free cell");
        check(!g.turn(snake::Dir::Left), "straight back is refused");
        check(!g.turn(snake::Dir::Right), "the way it already goes is not a turn");
        check(g.turn(snake::Dir::Up) && g.turn(snake::Dir::Left), "two quick turns both wait");
        const auto head = g.body.front();
        g.step();
        check(g.body.front().x == head.x && g.body.front().y == head.y - 1, "the first turn is taken first");
        g.step();
        check(g.body.front().x == head.x - 1 && g.body.front().y == head.y - 1, "then the second");
        check(g.body.size() == 5, "no apple, no growth");
    }
    {
        // A third press replaces the second, so the newest wish wins.
        auto g = fresh();
        g.turn(snake::Dir::Up);
        g.turn(snake::Dir::Left);
        g.turn(snake::Dir::Right);
        check(g.queued_count == 2 && g.queued[1] == snake::Dir::Right, "a third press replaces the second");
        auto u = fresh();
        u.turn(snake::Dir::Up);
        u.turn(snake::Dir::Left);
        check(!u.turn(snake::Dir::Down), "never a U-turn queued behind the first turn");
        u.step();
        u.step();
        check(!u.over, "so quick presses never bite the neck");
    }
    {
        auto g = fresh(false, 3);
        feed(g);
        const unsigned events = g.step();
        check(events & snake::Ate, "eating is reported");
        check(g.score == 3, "an apple is worth the speed");
        check(g.bulges.size() == 1 && g.bulges[0] == 0, "the swallowed apple starts at the head");
        g.step();
        check(g.body.size() == 6, "one longer a step after eating");
        check(g.bulges.size() == 1 && g.bulges[0] == 1, "the bulge moves down the body");
        for (int i = 0; i < 10; i++) g.step();
        check(g.bulges.empty(), "the bulge leaves at the tail");
        check(!g.on_body(g.food), "the next apple is on a free cell");
    }
    {
        // Without walls the edge wraps; with them it is the end.
        auto g = fresh(false);
        for (int i = 0; i < snake::cols; i++) {
            g.food = {0, 0};
            g.step();
        }
        check(!g.over && g.body.front().x == snake::cols / 2, "a full lap comes back round");
        auto w = fresh(true);
        unsigned events = 0;
        for (int i = 0; i < snake::cols && !w.over; i++) {
            w.food = {0, 0};
            events = w.step();
        }
        check(w.over && (events & snake::Died), "a wall ends it");
        check(w.body.front().x == snake::cols - 1, "the head stops at the last cell");
        check(w.step() == snake::None && !w.turn(snake::Dir::Up), "nothing moves once it is over");
    }
    {
        // Into its own body is the end; into the cell the tail is leaving is not.
        auto g = fresh();
        g.body = {{5, 5}, {5, 6}, {4, 6}, {4, 5}};
        g.dir = snake::Dir::Up;
        g.food = {0, 0};
        g.turn(snake::Dir::Left);
        g.step();
        check(!g.over, "chasing the tail is safe");
        g.body = {{5, 5}, {5, 6}, {4, 6}, {4, 5}, {4, 4}};
        g.dir = snake::Dir::Up;
        g.queued_count = 0;
        g.turn(snake::Dir::Left);
        g.step();
        check(g.over, "biting the body ends it");
    }
    {
        // Every fifth apple brings the critter; it is worth more caught early and leaves when its time is up.
        auto g = fresh(false, 2);
        unsigned events = 0;
        for (int i = 0; i < snake::bonus_every; i++) {
            feed(g);
            events = g.step();
        }
        check((events & snake::BonusShown) && g.bonus_left == snake::bonus_steps, "the critter comes after five");
        check(!g.on_body(g.bonus) && g.bonus.x + 1 < snake::cols, "it sits on two free cells");
        check(!(g.bonus == g.food), "not on the apple");
        auto caught = g;
        // Put the head just left of the critter, heading right.
        caught.body.front() = {caught.bonus.x - 1, caught.bonus.y};
        caught.dir = snake::Dir::Right;
        caught.queued_count = 0;
        caught.food = {caught.bonus.x == 0 ? 5 : 0, (caught.bonus.y + 3) % snake::rows};
        const int before = caught.score;
        if (caught.bonus.x > 0) {
            events = caught.step();
            check(events & snake::AteBonus, "catching it is reported");
            check(caught.score - before == 2 * (5 + snake::bonus_steps / 4), "caught at once: the most points");
            check(caught.bonus_left == 0, "it is gone once caught");
        }
        // Left alone (the snake on a lap of its own row, the critter on another), it goes when its time is up.
        bool gone = false;
        for (int i = 0; i < snake::bonus_steps && !gone && !g.over; i++) {
            g.food = {-5, -5}; // nowhere it can be eaten
            g.bonus.y = (g.body.front().y + 4) % snake::rows;
            g.queued_count = 0;
            gone = g.step() & snake::BonusGone;
        }
        check(gone && g.bonus_left == 0, "it leaves by itself");
    }
    {
        // What a step reports for the gliding: the tail cell let go of, nothing while growing, the cell eaten.
        auto g = fresh();
        const auto tail = g.body.back();
        g.food = {0, 0};
        g.step();
        check(g.steps == 1 && g.tail_moved && g.last_tail == tail, "the tail let go of its cell");
        feed(g);
        g.step();
        check(g.ate_step == g.steps && g.ate_cell == g.body.front(), "the cell eaten, on this step");
        g.food = {0, 0};
        g.step();
        check(!g.tail_moved, "growing, the tail stays put");
    }
    {
        // The critter walks: every few steps one free cell, never off the board or under the snake.
        auto g = fresh();
        for (int i = 0; i < snake::bonus_every; i++) {
            feed(g);
            g.step();
        }
        int walks = 0;
        for (int i = 0; i < 30 && g.bonus_left > 0 && !g.over; i++) {
            g.food = {-5, -5};
            g.queued_count = 0;
            const auto before = g.bonus;
            if (std::abs(g.body.front().y - g.bonus.y) <= 1) g.turn(snake::Dir::Up); // keep clear of it
            g.step();
            if (g.bonus_moved_step == g.steps) {
                walks++;
                check(std::abs(g.bonus.x - before.x) + std::abs(g.bonus.y - before.y) == 1, "a walk is one cell");
                check(g.bonus.x >= 0 && g.bonus.x + 1 < snake::cols && g.bonus.y >= 0 && g.bonus.y < snake::rows,
                      "it stays on the board");
                check(!g.on_body(g.bonus) && !g.on_body({g.bonus.x + 1, g.bonus.y}), "never under the snake");
            }
        }
        check(walks >= 5, "it walks about");
    }
    {
        // The attract mode's pilot eats and stays alive for a good while on an open board.
        auto g = fresh(false, 4);
        int steps = 0;
        while (steps < 3000 && !g.over) {
            g.autopilot();
            g.step();
            steps++;
        }
        check(g.apples >= 10, "the pilot finds apples");
        check(steps >= 150, "and does not crash at once");
    }
    check(snake::step_ms(1) > snake::step_ms(5) && snake::step_ms(5) > snake::step_ms(9) && snake::step_ms(9) >= 50,
          "faster with each speed, never silly");
    if (failures) return 1;
    std::printf("snake: ok\n");
    return 0;
}
