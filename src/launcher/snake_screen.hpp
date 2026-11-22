#pragma once
// Snake: the launcher's easter egg. Tap the battery on Home's upper screen five times quickly and it opens, a level
// of its own over Home like Music and the Sound toy. The board fills the upper screen under the status bar; the lower
// one has the score, the speed (1 to 9), Walls and Classic screen (the old green phone display). Steer with the
// D-pad or the stick, or swipe on either screen. The game itself is snake_game.hpp.
// The snake glides between cells: the screen draws the last step in progress (the head travels from the cell it left
// to the one the game moved it to), so a turn shows the moment the game takes it, and the body is a shaded tube that
// tapers to the tail. Classic screen draws it the old way instead: cell by cell on a dot-matrix LCD whose pixels
// fade in and out a little late and cast a faint shadow on the glass behind them.
// Members of App in main.cpp; included once, right after the class.

namespace {
// Little square-wave tunes, as a phone buzzer would play them: (frequency Hz, milliseconds), 0 Hz a rest.
std::vector<float> snake_tune(std::initializer_list<std::pair<int, int>> notes) {
    const int rate = voice_toy::rate;
    std::vector<float> out;
    for (const auto& [frequency, ms] : notes) {
        const size_t count = size_t(rate) * size_t(ms) / 1000, edge = size_t(rate) * 4 / 1000;
        double phase = 0;
        for (size_t i = 0; i < count; i++) {
            float v = 0;
            if (frequency > 0) {
                phase += double(frequency) / rate;
                if (phase >= 1) phase -= 1;
                v = phase < .5 ? .2f : -.2f;
                // a few milliseconds of ramp each end, so the notes never click
                v *= std::min({1.f, float(i) / edge, float(count - i) / edge});
            }
            out.push_back(v);
        }
    }
    return out;
}

// Digits three dots wide and five high, as the old phones drew their scores (the menu's pixel font slashes its 0).
constexpr std::array<const char*, 10> snake_digit_rows = {
    "####.##.##.####", ".#.##..#..#.###", "###..#####..###", "###..####..####", "#.##.####..#..#",
    "####..###..####", "####..####.####", "###..#..#..#..#", "####.#####.####", "####.####..####",
};
// Classic screen pictures in LCD dots (4 px, six to a cell): the apple fills a cell, the critter two, its legs
// taking turns.
constexpr std::array<const char*, 6> snake_lcd_apple = {"...#..", ".####.", "######", "######", "######", ".####."};
constexpr std::array<const char*, 6> snake_lcd_critter_a = {"#...####...#", ".#.######.#.", "..##.##.###.",
                                                            "############", ".#.######.#.", "#..#....#..#"};
constexpr std::array<const char*, 6> snake_lcd_critter_b = {".#..####..#.", ".#.######.#.", "..##.##.###.",
                                                            "############", ".#.######.#.", ".#.#....#.#."};

// The old phone's display: pale green glass, dark green dots.
constexpr Color snake_lcd{199, 240, 216}, snake_lcd_ink{67, 82, 61};

std::string snake_number(int value, int digits) {
    std::string text = std::to_string(std::max(0, value));
    if (int(text.size()) < digits) text.insert(0, size_t(digits) - text.size(), '0');
    return text;
}

float snake_smooth(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}

// A pop that overshoots a little and settles, for things appearing.
float snake_pop_in(float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return 1 + 2.2f * std::pow(t - 1, 3.f) + 1.2f * std::pow(t - 1, 2.f);
}

// One step of the snake between two cells, across the board's edge when it wraps.
std::pair<int, int> snake_delta(snake::Cell from, snake::Cell to) {
    int dx = to.x - from.x, dy = to.y - from.y;
    if (dx > 1) dx -= snake::cols;
    if (dx < -1) dx += snake::cols;
    if (dy > 1) dy -= snake::rows;
    if (dy < -1) dy += snake::rows;
    return {dx, dy};
}
} // namespace

bool App::snake_available() const { return !o.game_menu && !o.port_menu && !probe; }

void App::snake_load() {
    if (sn.loaded) return;
    sn.loaded = true;
    auto saved = fields(o.data / "snake.ini");
    const auto number = [&](const char* key, int fallback) {
        const auto found = saved.find(key);
        return found == saved.end() || found->second.empty() ? fallback : std::atoi(found->second.c_str());
    };
    sn.level = std::clamp(number("speed", 5), 1, snake::levels);
    sn.walls = number("walls", 0) == 1;
    sn.classic = number("classic", 0) == 1;
    sn.best = {std::max(0, number("best", 0)), std::max(0, number("best_walls", 0))};
}

void App::snake_save() {
    std::error_code ec;
    fs::create_directories(o.data, ec);
    const fs::path file = o.data / "snake.ini";
    std::ofstream(file.string() + ".tmp")
        << "speed=" << sn.level << "\nwalls=" << (sn.walls ? 1 : 0) << "\nclassic=" << (sn.classic ? 1 : 0)
        << "\nbest=" << sn.best[0] << "\nbest_walls=" << sn.best[1] << '\n';
    fs::rename(file.string() + ".tmp", file, ec);
}

// The ready screen: a fresh snake that plays by itself (snake::Game::autopilot) behind the card.
void App::snake_demo(Uint32 now) {
    sn.state = SnakeUi::State::Ready;
    sn.game.reset(4, sn.walls, now * 2246822519u + 7);
    sn.step_at = now;
    sn.food_at = now;
    sn.eaten_step = -1;
    sn.pending.clear();
}

void App::open_snake() {
    if (sn.open || launching || !snake_available()) return;
    cancel_navigation();
    snake_load();
    sn.open = true;
    snake_demo(s.GetTicks());
    sn.particles.clear();
    sn.pops.clear();
    sn.button = -1;
    sn.top_down = false;
    haptics.play(Haptic::Open);
    snake_sound(0);
    configure_sounds(); // no menu music under the game
    dirty = true;
}

void App::close_snake() {
    if (!sn.open) return;
    sn.open = false;
    sn.button = -1;
    sn.top_down = false;
    sn.pending.clear();
    sn.particles.clear();
    sn.pops.clear();
    pointer_down = false;
    sounds.stop_toy();
    configure_sounds();
    emit_sound(MenuCue::Back);
    dirty = true;
}

// A game in play stops where it is, part way through its step, so carrying on never jumps.
void App::snake_hold() {
    if (sn.state != SnakeUi::State::Playing) return;
    const Uint32 step = Uint32(snake::step_ms(sn.game.level));
    sn.paused_into = std::min(step, s.GetTicks() - sn.step_at);
    sn.state = SnakeUi::State::Paused;
    dirty = true;
}

void App::snake_resume() {
    sn.state = SnakeUi::State::Playing;
    sn.step_at = s.GetTicks() - sn.paused_into;
    dirty = true;
}

// The lid closed or the handheld is going to sleep: a game in play waits.
void App::snake_pause() {
    if (sn.open) snake_hold();
}

// 0 start, 1 apple, 2 critter shows, 3 critter caught, 4 over, 5 over with a new best, 6 pause, 7 bump.
void App::snake_sound(int kind) {
    if (!preferences.menu_sounds || (hardware.volume && hardware.volume_percent == 0)) return;
    static const std::array<std::vector<float>, 8> tunes = {
        snake_tune({{660, 50}, {990, 70}}),
        snake_tune({{1320, 30}, {1760, 40}}),
        snake_tune({{1568, 30}, {0, 25}, {1568, 30}}),
        snake_tune({{1047, 40}, {1319, 40}, {1568, 40}, {2093, 90}}),
        snake_tune({{150, 60}, {440, 90}, {330, 90}, {220, 220}}),
        snake_tune({{150, 60}, {523, 80}, {659, 80}, {784, 80}, {1047, 240}}),
        snake_tune({{880, 40}}),
        snake_tune({{110, 70}}),
    };
    sounds.set_master(hardware.volume ? hardware.volume_percent : 100);
    sounds.play_toy(tunes[size_t(std::clamp(kind, 0, 7))], voice_toy::Voice::Normal, voice_toy::Bend{});
}

void App::snake_start() {
    const Uint32 now = s.GetTicks();
    sn.game.reset(sn.level, sn.walls, now * 2654435761u + 1);
    sn.state = SnakeUi::State::Playing;
    sn.step_at = now;
    sn.food_at = now;
    sn.eaten_step = -1;
    sn.new_best = false;
    sn.pending.clear();
    sn.pops.clear();
    haptics.play(Haptic::Select);
    snake_sound(0);
    dirty = true;
}

void App::snake_turn(snake::Dir d) {
    switch (sn.state) {
    case SnakeUi::State::Ready: snake_start(); break;
    case SnakeUi::State::Paused: snake_resume(); break;
    case SnakeUi::State::Over: return; // a direction never restarts by itself: A does
    case SnakeUi::State::Playing: break;
    }
    sn.game.turn(d);
    dirty = true;
}

// One press of A or Start.
void App::snake_go() {
    const Uint32 now = s.GetTicks();
    switch (sn.state) {
    case SnakeUi::State::Ready: snake_start(); break;
    case SnakeUi::State::Playing:
        snake_hold();
        snake_sound(6);
        break;
    case SnakeUi::State::Paused: snake_resume(); break;
    case SnakeUi::State::Over:
        if (sn.card_shown && now - sn.over_at >= 600) snake_start(); // not off the press that was still steering
        break;
    }
    dirty = true;
}

// Speed and Walls are for the next game: changed while paused, the game starts over.
void App::snake_set(int speed, bool walls, bool classic) {
    if (sn.state == SnakeUi::State::Playing) return;
    const bool changed = speed != sn.level || walls != sn.walls;
    sn.level = std::clamp(speed, 1, snake::levels);
    sn.walls = walls;
    if (classic != sn.classic) {
        sn.classic = classic;
        haptics.play(classic ? Haptic::On : Haptic::Off);
    } else if (changed) haptic_detent();
    if (changed && sn.state == SnakeUi::State::Paused) snake_demo(s.GetTicks());
    else if (changed && sn.state == SnakeUi::State::Ready) sn.game.walls = walls;
    snake_save();
    dirty = true;
}

// How far the last step has got on screen, 0 to 1.
float App::snake_progress(Uint32 now) const {
    const auto& g = sn.game;
    if (g.steps == 0 || sn.state == SnakeUi::State::Over) return 1;
    const float step = float(snake::step_ms(g.level));
    const float into = sn.state == SnakeUi::State::Paused ? float(sn.paused_into) : float(now - sn.step_at);
    return std::clamp(into / step, 0.f, 1.f);
}

// The end of a game in the normal look: a shake, then the snake bursts from the head down, then the card.
int App::snake_card_ms() const {
    if (sn.classic) return 1000;
    const int n = int(sn.game.body.size()), each = std::max(16, 700 / std::max(1, n));
    return std::min(1500, 320 + each * n + 200);
}

void App::snake_burst(float x, float y, int count, Color c, float speed, float size, Uint32 now) {
    for (int i = 0; i < count; i++) {
        const float angle = float(sn.game.random() % 6283) / 1000.f,
                    pace = speed * (.4f + float(sn.game.random() % 600) / 1000.f);
        sn.particles.push_back({x, y, std::cos(angle) * pace, std::sin(angle) * pace - speed * .35f,
                                size * (.6f + float(sn.game.random() % 500) / 1000.f), c.r, c.g, c.b, now,
                                420 + int(sn.game.random() % 260)});
    }
}

bool App::snake_tick(Uint32 now) {
    if (!sn.open) return false;
    if (!snake_available()) {
        close_snake();
        return false;
    }
    using namespace snake_layout;
    auto& g = sn.game;
    const bool live = sn.state == SnakeUi::State::Playing, demo = sn.state == SnakeUi::State::Ready;
    if (live || demo) {
        const Uint32 step = Uint32(snake::step_ms(g.level));
        // A long stall (a slow frame, the lid) never fires a burst of steps: the snake carries on from now.
        if (now - sn.step_at > step * 3) sn.step_at = now - step;
        while ((sn.state == SnakeUi::State::Playing || sn.state == SnakeUi::State::Ready) && now - sn.step_at >= step) {
            sn.step_at += step;
            if (demo) g.autopilot();
            const int before = g.score;
            const snake::Cell bonus = g.bonus;
            const bool facing = g.bonus_facing_left;
            const unsigned events = g.step();
            if (events & (snake::Ate | snake::AteBonus)) {
                // Eaten when the head gets there on screen, at the end of this step.
                const bool critter = events & snake::AteBonus;
                sn.eaten_step = g.steps;
                sn.eaten_bonus = critter;
                sn.eaten_cell = critter ? bonus : g.ate_cell;
                sn.eaten_facing_left = facing;
                const float x = float(board_x + g.ate_cell.x * cell + cell / 2),
                            y = float(board_y + g.ate_cell.y * cell + cell / 2);
                sn.pending.push_back({sn.step_at + step * 85 / 100, critter ? 1 : 0, x, y, g.score - before, demo});
                if (events & snake::Ate) sn.food_at = sn.step_at + step;
            }
            if ((events & snake::BonusShown) && live) snake_sound(2);
            if (g.over && demo) {
                snake_demo(now);
                break;
            }
            if (g.over) {
                sn.state = SnakeUi::State::Over;
                sn.over_at = now;
                sn.card_shown = false;
                sn.pending.clear();
                int& best = sn.best[sn.walls ? 1 : 0];
                sn.new_best = g.score > best;
                if (sn.new_best) {
                    best = g.score;
                    snake_save();
                }
                snake_sound(sn.new_best || g.won ? 5 : 4);
                haptics.play(Haptic::Error);
                if (!sn.classic) {
                    // Each piece of the snake bursts in turn, from the head down, once the shake has settled.
                    const int n = int(g.body.size()), each = std::max(16, 700 / std::max(1, n));
                    for (int k = 0; k < n; k++)
                        sn.pending.push_back({now + 320 + Uint32(each * k), 2,
                                              float(board_x + g.body[size_t(k)].x * cell + cell / 2),
                                              float(board_y + g.body[size_t(k)].y * cell + cell / 2), k, true});
                }
            }
        }
        if (live) last_activity = now; // a game in play is not an idle menu
    }
    // The effects whose moment has come.
    for (size_t i = 0; i < sn.pending.size();) {
        const auto p = sn.pending[i];
        if (int32_t(now - p.at) < 0) {
            i++;
            continue;
        }
        sn.pending.erase(sn.pending.begin() + long(i));
        if (p.kind == 2) {
            const float share = float(p.value) / float(std::max<size_t>(1, sn.game.body.size()));
            snake_burst(p.x, p.y, 5, blend(theme.at("accent"), theme.at("slot"), int(40 * share)), 140, 5, now);
            if (p.value % 4 == 0 && !p.quiet) haptic_detent();
            continue;
        }
        if (!sn.classic) {
            if (p.kind == 0) {
                snake_burst(p.x, p.y, 9, Color{222, 62, 58}, 190, 4.2f, now);
                snake_burst(p.x, p.y, 3, Color{88, 176, 84}, 150, 3.2f, now);
            } else {
                snake_burst(p.x, p.y, 16, Color{240, 168, 40}, 240, 4.5f, now);
                snake_burst(p.x, p.y, 6, Color{255, 255, 255}, 260, 2.5f, now);
            }
        }
        if (!p.quiet) {
            sn.pops.push_back({p.x, p.y - 10, p.value, now});
            snake_sound(p.kind == 0 ? 1 : 3);
            haptics.play(p.kind == 0 ? Haptic::Tick : Haptic::Favourite);
        }
    }
    sn.particles.erase(std::remove_if(sn.particles.begin(), sn.particles.end(),
                                      [&](const SnakeUi::Particle& p) { return int(now - p.born) >= p.life; }),
                       sn.particles.end());
    sn.pops.erase(
        std::remove_if(sn.pops.begin(), sn.pops.end(), [&](const SnakeUi::Pop& p) { return now - p.born >= 800; }),
        sn.pops.end());
    bool moving =
        live || demo || !sn.pending.empty() || !sn.particles.empty() || !sn.pops.empty() || now - sn.food_at < 400;
    if (sn.state == SnakeUi::State::Over && !sn.card_shown) {
        if (int(now - sn.over_at) >= snake_card_ms()) sn.card_shown = true;
        moving = true;
    }
    if (moving) dirty = true;
    return moving;
}

// How long the loop may sleep: a frame while anything moves, else until something happens.
int App::snake_wait(Uint32 now) const {
    if (!sn.open) return 1000;
    const bool stepping = sn.state == SnakeUi::State::Playing || sn.state == SnakeUi::State::Ready;
    const bool moving = stepping || !sn.pending.empty() || !sn.particles.empty() || !sn.pops.empty() ||
                        now - sn.food_at < 400 || (sn.state == SnakeUi::State::Over && !sn.card_shown);
    if (!moving) return 1000;
    if (!stepping) return 16;
    const int step = snake::step_ms(sn.game.level), gone = int(now - sn.step_at);
    return std::clamp(step - gone, 1, 16);
}

// ---- drawing

// A texture baked once per renderer: 0 a soft white disc, 1 the board, 2 the LCD's dot filter.
SDL_Texture* App::snake_texture(SDL_Renderer* r, int kind, uint32_t key) {
    const auto id = std::make_tuple(r, kind, key);
    if (auto found = sn.textures.find(id); found != sn.textures.end()) return found->second;
    // A board for another theme or look goes first.
    for (auto at = sn.textures.begin(); at != sn.textures.end();) {
        if (std::get<0>(at->first) == r && std::get<1>(at->first) == kind) {
            if (at->second) s.DestroyTexture(at->second);
            at = sn.textures.erase(at);
        } else ++at;
    }
    using namespace snake_layout;
    const int w = kind == 0 ? 64 : board_w, h = kind == 0 ? 64 : board_h;
    std::vector<uint32_t> pixels(size_t(w) * size_t(h), 0);
    const auto argb = [](Color c, int a) {
        return uint32_t(std::clamp(a, 0, 255)) << 24 | uint32_t(c.r) << 16 | uint32_t(c.g) << 8 | uint32_t(c.b);
    };
    if (kind == 0) {
        // Anti-aliased: alpha falls off over the last pixel of the radius.
        for (int py = 0; py < h; py++)
            for (int px = 0; px < w; px++) {
                const float d = std::hypot(px + .5f - 32, py + .5f - 32);
                pixels[size_t(py * w + px)] = argb(Color{255, 255, 255}, int(std::clamp(32 - d, 0.f, 1.f) * 255));
            }
    } else if (kind == 1 && !sn.classic) {
        // A soft checkerboard, one square a cell: it shows the grid the snake moves on without drawing lines.
        const Color a = theme.at("slot"),
                    b = preferences.dark ? blend(a, theme.at("ink"), 5) : blend(a, theme.at("panel"), 45);
        for (int py = 0; py < h; py++)
            for (int px = 0; px < w; px++)
                pixels[size_t(py * w + px)] = argb(((px / cell) + (py / cell)) % 2 ? b : a, 255);
    } else if (kind == 1) {
        // The LCD's glass, a touch brighter at the top, with its unlit dots just visible.
        for (int py = 0; py < h; py++)
            for (int px = 0; px < w; px++) {
                Color glass = blend(snake_lcd, Color{150, 190, 165}, py * 18 / h);
                const bool gap = px % 4 == 3 || py % 4 == 3;
                pixels[size_t(py * w + px)] = argb(gap ? glass : blend(glass, snake_lcd_ink, 5), 255);
            }
    } else {
        // The dot filter over whatever is lit: thin gaps of glass between the 3 px dots.
        for (int py = 0; py < h; py++)
            for (int px = 0; px < w; px++)
                if (px % 4 == 3 || py % 4 == 3) pixels[size_t(py * w + px)] = argb(snake_lcd, 150);
    }
    SDL_Texture* texture = s.CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (texture && s.UpdateTexture(texture, nullptr, pixels.data(), w * 4) != 0) {
        s.DestroyTexture(texture);
        texture = nullptr;
    }
    if (texture) {
        s.SetTextureBlendMode(texture, kind == 1 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
        if (kind == 0) {
            // Linear sampling, so a disc drawn small stays round and soft (SDL 2.0.12 and later).
            using ScaleMode = int (*)(SDL_Texture*, int);
            static const auto scale_mode = reinterpret_cast<ScaleMode>(dlsym(s.handle, "SDL_SetTextureScaleMode"));
            if (scale_mode) scale_mode(texture, 1);
        }
    }
    sn.textures[id] = texture;
    return texture;
}

void App::snake_disc(SDL_Renderer* r, float x, float y, float radius, Color c, int alpha) {
    if (radius <= .2f || alpha <= 0) return;
    SDL_Texture* disc = snake_texture(r, 0, 0);
    if (!disc) {
        alpha_round(r, int(x - radius), int(y - radius), int(radius * 2), int(radius * 2), int(radius), c, alpha);
        return;
    }
    const int size = std::max(1, int(std::lround(radius * 2)));
    const SDL_Rect to{int(std::lround(x - size / 2.f)), int(std::lround(y - size / 2.f)), size, size};
    s.SetTextureColorMod(disc, c.r, c.g, c.b);
    s.SetTextureAlphaMod(disc, Uint8(std::clamp(alpha, 0, 255)));
    s.RenderCopy(r, disc, nullptr, &to);
}

void App::snake_fill(SDL_Renderer* r, int x, int y, int w, int h, Color c, int alpha) {
    if (alpha <= 0) return;
    s.SetRenderDrawBlendMode(r, alpha >= 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    s.SetRenderDrawColor(r, c.r, c.g, c.b, Uint8(std::clamp(alpha, 0, 255)));
    const SDL_Rect box{x, y, w, h};
    s.RenderFillRect(r, &box);
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

// A lit LCD dot block: the dark dots, and their faint shadow on the glass behind, 2 px down and right.
void App::snake_lit(SDL_Renderer* r, int x, int y, int w, int h, int alpha) {
    snake_fill(r, x + 2, y + 2, w, h, snake_lcd_ink, alpha * 45 / 255);
    snake_fill(r, x, y, w, h, snake_lcd_ink, alpha);
}

void App::snake_dots(SDL_Renderer* r, const std::string& text, int x, int y, int dot, Color ink) {
    s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, 255);
    pixel_dots(text, x, y, dot, 40, [&](const SDL_Rect& p) { s.RenderFillRect(r, &p); }, [](unsigned, int, int) {});
}

// A number in those digits, its top-left at (x, y); each digit 3 dots and a gap of one. Returns the width.
int App::snake_digits(SDL_Renderer* r, const std::string& text, int x, int y, int dot, Color ink, int alpha) {
    int at = x;
    const int gap = dot >= 4 ? std::max(1, dot / 5) : 0; // the dots stand a hair apart, as on an LCD
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            const char* rows = snake_digit_rows[size_t(c - '0')];
            for (int i = 0; i < 15; i++)
                if (rows[i] == '#')
                    snake_fill(r, at + (i % 3) * dot, y + (i / 3) * dot, dot - gap, dot - gap, ink, alpha);
        } else if (c == '+') {
            snake_fill(r, at, y + 2 * dot, 3 * dot - gap, dot - gap, ink, alpha);
            snake_fill(r, at + dot, y + dot, dot - gap, 3 * dot - gap, ink, alpha);
        }
        at += 4 * dot;
    }
    return at - x - dot;
}

// A glossy apple centred on (x, y): its shadow, a darker cheek, a highlight, the stem and a leaf.
void App::snake_apple(SDL_Renderer* r, float x, float y, float k) {
    if (k <= .02f) return;
    k *= snake_layout::cell / 16.f;
    snake_disc(r, x + 2, y + 4, 6.2f * k, shadow_colour(), preferences.dark ? 90 : 45);
    snake_disc(r, x, y + .5f, 6.8f * k, Color{222, 62, 58}, 255);
    snake_disc(r, x + 1.5f * k, y + 2 * k, 4.6f * k, Color{176, 36, 40}, 110);
    snake_disc(r, x - 2.3f * k, y - 1.8f * k, 2.1f * k, Color{255, 255, 255}, 175);
    snake_fill(r, int(x - k), int(y - 8.5f * k), std::max(1, int(2 * k)), std::max(1, int(4 * k)), Color{112, 72, 40},
               255);
    snake_disc(r, x + 2.8f * k, y - 7 * k, 2.4f * k, Color{88, 176, 84}, 255);
}

// The critter: a little beetle two cells long, walking on six legs, its head the way it last went.
void App::snake_critter(SDL_Renderer* r, float x, float y, bool left, float k, int alpha, Uint32 now) {
    if (k <= .02f || alpha <= 0) return;
    k *= snake_layout::cell / 16.f;
    const float f = left ? -1 : 1;
    const Color shell{240, 168, 40}, dark{64, 52, 46};
    snake_disc(r, x - 4 * k + 3, y + 4, 7 * k, shadow_colour(), preferences.dark ? 80 : 40);
    snake_disc(r, x + 4 * k + 3, y + 4, 7 * k, shadow_colour(), preferences.dark ? 80 : 40);
    // Legs: three a side, each pair stepping as the other rests.
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    s.SetRenderDrawColor(r, dark.r, dark.g, dark.b, Uint8(alpha));
    for (int i = -1; i <= 1; i++) {
        const int lift = ((now / 110) + Uint32(i + 1)) % 2 ? 2 : -1;
        const int lx = int(x + i * 6 * k);
        for (int w = 0; w < (k > 1.2f ? 2 : 1); w++) { // two pixels wide when it is drawn big
            s.RenderDrawLine(r, lx + w, int(y), lx + w + lift, int(y - 9 * k));
            s.RenderDrawLine(r, lx + w, int(y), lx + w - lift, int(y + 9 * k));
        }
    }
    // Antennae, ahead of the head.
    const int hx = int(x + f * 11 * k);
    s.RenderDrawLine(r, hx, int(y - 2), hx + int(f * 6 * k), int(y - 6 * k));
    s.RenderDrawLine(r, hx, int(y + 2), hx + int(f * 6 * k), int(y + 6 * k));
    s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    snake_disc(r, x - 4 * k, y, 7 * k, shell, alpha);
    snake_disc(r, x + 4 * k, y, 7 * k, shell, alpha);
    snake_fill(r, int(x - 10 * k), int(y), int(18 * k), 1, dark, alpha * 3 / 4); // where the wings meet
    snake_disc(r, x - 4 * k, y - 3.2f * k, 1.7f * k, dark, alpha);
    snake_disc(r, x + 3 * k, y + 3.4f * k, 1.7f * k, dark, alpha);
    snake_disc(r, x + 1 * k, y - 3.4f * k, 1.4f * k, dark, alpha);
    snake_disc(r, x - 5 * k, y + 3.2f * k, 1.4f * k, dark, alpha);
    snake_disc(r, x - 3 * k, y - 4.5f * k, 2 * k, Color{255, 255, 255}, alpha / 2); // the shell's shine
    snake_disc(r, hx, y, 4.3f * k, dark, alpha);
    snake_disc(r, hx + f * 1.5f * k, y - 1.8f * k, 1.1f * k, Color{255, 255, 255}, alpha);
    snake_disc(r, hx + f * 1.5f * k, y + 1.8f * k, 1.1f * k, Color{255, 255, 255}, alpha);
}

// The normal look's snake: a tube through the cells' centres, the head part way along its last step.
void App::snake_body(SDL_Renderer* r, int y, float ox, float oy, Uint32 now) {
    using namespace snake_layout;
    const auto& g = sn.game;
    const size_t n = g.body.size();
    if (n < 2) return;
    const float c = float(cell), t = snake_progress(now);
    const float bx = float(board_x) + ox, by = float(y + board_y) + oy, W = float(board_w), H = float(board_h);
    // The cells' centres along the body, unwrapped so a body across the edge stays in one piece.
    std::vector<std::pair<float, float>> nodes(n);
    nodes[0] = {bx + g.body[0].x * c + c / 2, by + g.body[0].y * c + c / 2};
    for (size_t k = 1; k < n; k++) {
        const auto [dx, dy] = snake_delta(g.body[k - 1], g.body[k]);
        nodes[k] = {nodes[k - 1].first + dx * c, nodes[k - 1].second + dy * c};
    }
    std::vector<std::pair<float, float>> path;
    // The head: from the cell it left toward the one it is going into, and a lunge into whatever ended it.
    const auto [hdx, hdy] = snake_delta(g.body[1], g.body[0]);
    float hx = nodes[1].first + (nodes[0].first - nodes[1].first) * t,
          hy = nodes[1].second + (nodes[0].second - nodes[1].second) * t;
    if (sn.state == SnakeUi::State::Over) {
        const float a = float(now - sn.over_at), lunge = a < 180 ? std::sin(a / 180 * 3.14159f) * .45f : 0;
        const float ddx = g.dir == snake::Dir::Left    ? -1
                          : g.dir == snake::Dir::Right ? 1
                                                       : 0,
                    ddy = g.dir == snake::Dir::Up     ? -1
                          : g.dir == snake::Dir::Down ? 1
                                                      : 0;
        hx += ddx * c * lunge;
        hy += ddy * c * lunge;
    }
    path.push_back({hx, hy});
    for (size_t k = 1; k < n; k++) path.push_back(nodes[k]);
    // The tail: still leaving the cell it let go of.
    if (g.tail_moved && g.steps > 0) {
        const auto [tdx, tdy] = snake_delta(g.body[n - 1], g.last_tail);
        const float px = nodes[n - 1].first + tdx * c, py = nodes[n - 1].second + tdy * c;
        path.push_back({px + (nodes[n - 1].first - px) * t, py + (nodes[n - 1].second - py) * t});
    }
    // Points every few pixels from head to tail, with the distance from the head.
    struct Stamp {
        float x, y, s;
    };
    std::vector<Stamp> stamps;
    float along = 0, length_px = 0;
    for (size_t i = 0; i + 1 < path.size(); i++)
        length_px += std::hypot(path[i + 1].first - path[i].first, path[i + 1].second - path[i].second);
    // A stamp every few pixels, fewer on a very long snake, so a frame stays a few hundred draws.
    const float spacing = std::max(3.5f, length_px / 500);
    for (size_t i = 0; i + 1 < path.size(); i++) {
        const float sx = path[i].first, sy = path[i].second, ex = path[i + 1].first, ey = path[i + 1].second;
        const float length = std::hypot(ex - sx, ey - sy);
        for (float d = i == 0 ? 0 : spacing - std::fmod(along, spacing); d < length; d += spacing)
            stamps.push_back({sx + (ex - sx) * d / length, sy + (ey - sy) * d / length, along + d});
        along += length;
    }
    if (!path.empty()) stamps.push_back({path.back().first, path.back().second, along});
    const float total = std::max(1.f, along);
    // The end: the snake bursts from the head down, one piece a moment.
    float gone = -1;
    if (sn.state == SnakeUi::State::Over && !sn.classic) {
        const int each = std::max(16, 700 / std::max<int>(1, int(n)));
        gone = (float(now - sn.over_at) - 320) / float(each) * c;
    }
    const float z = c / 16, head_r = 6.8f * z, body_r = 6.f * z, tail_r = 2.6f * z;
    const auto radius = [&](float s) {
        // Full along the body, narrowing over the last three cells to a point of a tail.
        const float to_tail = std::clamp((total - s) / std::min(total * .7f, 3 * c), 0.f, 1.f);
        float rad = tail_r + (body_r - tail_r) * std::sqrt(to_tail);
        rad += (head_r - body_r) * std::max(0.f, 1 - s / c); // swelling into the head
        // Swallowed food, a bulge travelling down.
        for (int b : g.bulges) {
            const float at = (t + float(b) - 1) * c;
            if (at >= 0) rad += 3.2f * z * std::max(0.f, 1 - std::abs(s - at) / (.9f * c));
        }
        return rad;
    };
    const Color accent = theme.at("accent"), well = theme.at("slot"), shade = shadow_colour();
    // Drawn twice where it crosses an edge, so it slides off one side and on at the other.
    const auto wrapped = [&](float x0, float y0, float rad, auto&& draw) {
        float x = x0, yy = y0;
        while (x < bx) x += W;
        while (x >= bx + W) x -= W;
        while (yy < by) yy += H;
        while (yy >= by + H) yy -= H;
        for (int i = -1; i <= 1; i++)
            for (int j = -1; j <= 1; j++) {
                const float px = x + i * W, py = yy + j * H;
                if (px + rad + 4 < bx || px - rad - 4 > bx + W || py + rad + 4 < by || py - rad - 4 > by + H) continue;
                draw(px, py);
            }
    };
    // The stamps overlap, so the shadow's faint layers add up: soft at its edges, deeper under the middle.
    const int shadow_alpha = preferences.dark ? 26 : 13;
    for (size_t i = stamps.size(); i-- > 0;) {
        const auto& p = stamps[i];
        if (p.s < gone || (i % 2 && i + 1 != stamps.size())) continue; // the soft shadow needs only every other
        const float rad = radius(p.s);
        wrapped(p.x, p.y, rad,
                [&](float x, float yy) { snake_disc(r, x + 3, yy + 4.5f, rad, shade, shadow_alpha * 3 / 2); });
    }
    for (size_t i = stamps.size(); i-- > 0;) {
        const auto& p = stamps[i];
        if (p.s < gone) continue;
        const float rad = radius(p.s), share = p.s / total;
        // Accent at the head, easing toward the board at the tail.
        const Color body = blend(accent, well, int(38 * share));
        wrapped(p.x, p.y, rad, [&](float x, float yy) { snake_disc(r, x, yy, rad, body, 255); });
    }
    // The shine along its back: an opaque lighter line (translucent stamps would pile up into beads).
    for (size_t i = stamps.size(); i-- > 0;) {
        const auto& p = stamps[i];
        if (p.s < gone) continue;
        const float rad = radius(p.s);
        const Color shine = blend(blend(accent, well, int(38 * p.s / total)), Color{255, 255, 255}, 26);
        wrapped(p.x, p.y, rad,
                [&](float x, float yy) { snake_disc(r, x - rad * .2f, yy - rad * .32f, rad * .36f, shine, 255); });
    }
    if (gone > 0) return; // the head went first
    // The head: a touch wider, eyes that follow the apple and blink, a tongue that flicks.
    float vx = float(hdx), vy = float(hdy);
    const float fx = bx + g.food.x * c + c / 2, fy = by + g.food.y * c + c / 2;
    const float near = std::hypot(fx - hx, fy - hy) / c;
    const float hungry = sn.state == SnakeUi::State::Over ? 0 : snake_smooth(3, 1, near);
    const float hr = head_r + (.8f + hungry * .9f) * z;
    const Color head = accent;
    wrapped(hx, hy, hr, [&](float x, float yy) {
        snake_disc(r, x, yy, hr, head, 255);
        snake_disc(r, x - hr * .2f, yy - hr * .32f, hr * .4f, blend(head, Color{255, 255, 255}, 26), 255);
        const float px = -vy, py = vx;
        const bool dead = sn.state == SnakeUi::State::Over;
        // A tongue now and then, and all the time when the apple is close.
        if (!dead && ((now % 2600) < 170 || (hungry > .5f && (now % 520) < 200))) {
            const Color tongue{214, 54, 84};
            s.SetRenderDrawColor(r, tongue.r, tongue.g, tongue.b, 255);
            const float reach = 7 * z;
            const int sx = int(x + vx * hr), sy = int(yy + vy * hr), ex = int(x + vx * (hr + reach)),
                      ey = int(yy + vy * (hr + reach));
            for (int w = 0; w < 2; w++) {
                const int wx = int(px * w), wy = int(py * w);
                s.RenderDrawLine(r, sx + wx, sy + wy, ex + wx, ey + wy);
                s.RenderDrawLine(r, ex + wx, ey + wy, int(ex + wx + vx * 4 * z + px * 3 * z),
                                 int(ey + wy + vy * 4 * z + py * 3 * z));
                s.RenderDrawLine(r, ex + wx, ey + wy, int(ex + wx + vx * 4 * z - px * 3 * z),
                                 int(ey + wy + vy * 4 * z - py * 3 * z));
            }
        }
        for (int side = -1; side <= 1; side += 2) {
            const float ex = x + vx * 2.2f * z + px * 3.6f * z * side, ey = yy + vy * 2.2f * z + py * 3.6f * z * side;
            if (dead) {
                // Crosses for eyes.
                s.SetRenderDrawColor(r, 255, 255, 255, 255);
                const int e = int(2 * z);
                for (int w = 0; w < 2; w++) {
                    s.RenderDrawLine(r, int(ex) - e + w, int(ey) - e, int(ex) + e + w, int(ey) + e);
                    s.RenderDrawLine(r, int(ex) - e + w, int(ey) + e, int(ex) + e + w, int(ey) - e);
                }
                continue;
            }
            if ((now % 3400) < 120) {
                snake_fill(r, int(ex - 2.5f * z), int(ey), int(5 * z), 2, Color{255, 255, 255}, 220); // a blink
                continue;
            }
            snake_disc(r, ex, ey, 2.7f * z, Color{255, 255, 255}, 255);
            float lx = fx - ex, ly = fy - ey;
            const float d = std::max(1.f, std::hypot(lx, ly));
            snake_disc(r, ex + lx / d * 1.1f * z, ey + ly / d * 1.1f * z, 1.5f * z, Color{24, 28, 36}, 255);
        }
    });
}

// The Classic screen's snake: whole cells, lit a little late and fading out a little late, as an LCD does.
void App::snake_body_lcd(SDL_Renderer* r, int y, int ox, int oy, Uint32 now) {
    using namespace snake_layout;
    const auto& g = sn.game;
    const int bx = board_x + ox, by = y + board_y + oy;
    const int into = sn.state == SnakeUi::State::Paused ? int(sn.paused_into)
                     : sn.state == SnakeUi::State::Over ? 1000
                                                        : int(now - sn.step_at);
    if (sn.state == SnakeUi::State::Over && now - sn.over_at < 900 && ((now - sn.over_at) / 150) % 2 == 0) return;
    // The cell the tail let go of, fading.
    if (g.tail_moved && g.steps > 0 && into < 160)
        snake_lit(r, bx + g.last_tail.x * cell, by + g.last_tail.y * cell, cell, cell, 255 - into * 255 / 160);
    for (size_t k = 0; k < g.body.size(); k++) {
        const auto cl = g.body[k];
        const int alpha = k == 0 && g.steps > 0 ? std::min(255, 60 + into * 255 / 50) : 255;
        // A swallowed apple shows as a fatter segment; the rest leave a dot of glass at the middle.
        const bool fat = std::find(g.bulges.begin(), g.bulges.end(), int(k)) != g.bulges.end();
        const int x = bx + cl.x * cell, yy = by + cl.y * cell;
        snake_lit(r, x, yy, cell, cell, alpha);
        if (!fat && k > 0 && k % 2 == 0) snake_fill(r, x + 8, yy + 8, 8, 8, snake_lcd, alpha * 3 / 5);
    }
    // The head's eye: one dot of glass, toward the way it goes.
    const auto head = g.body.front();
    const int ex = g.dir == snake::Dir::Left    ? 4
                   : g.dir == snake::Dir::Right ? 16
                                                : 4,
              ey = g.dir == snake::Dir::Up     ? 4
                   : g.dir == snake::Dir::Down ? 16
                                               : 4;
    snake_fill(r, bx + head.x * cell + ex, by + head.y * cell + ey, 4, 4, snake_lcd, 255);
    snake_fill(r, bx + head.x * cell + (snake::vertical(g.dir) ? 16 : ex),
               by + head.y * cell + (snake::vertical(g.dir) ? ey : 16), 4, 4, snake_lcd, 255);
}

void App::snake_board(SDL_Renderer* r, int y) {
    using namespace snake_layout;
    const Uint32 now = s.GetTicks();
    const bool classic = sn.classic;
    const auto& g = sn.game;
    const SDL_Rect board{board_x, y + board_y, board_w, board_h}, view{view_x, y + board_y, view_w, board_h};
    // The board: a soft checkerboard, or the phone's glass with its dots.
    const Color slot = theme.at("slot"), panel = theme.at("panel");
    const uint32_t key = classic
                             ? 1u
                             : (uint32_t(slot.r) << 16 | uint32_t(slot.g) << 8 | slot.b) ^ (uint32_t(panel.r) << 20) ^
                                   (uint32_t(panel.g) << 4) ^ (uint32_t(panel.b) << 27) ^ 2u;
    s.RenderSetClipRect(r, &view);
    if (SDL_Texture* back = snake_texture(r, 1, key)) s.RenderCopy(r, back, nullptr, &board);
    else rect(r, view.x, view.y, view.w, view.h, "slot");
    s.RenderSetClipRect(r, &view);
    // A crash shakes the board for a moment.
    float ox = 0, oy = 0;
    if (sn.state == SnakeUi::State::Over && now - sn.over_at < 360) {
        const float a = float(now - sn.over_at), amp = 7 * (1 - a / 360);
        ox = amp * std::sin(a * .11f);
        oy = amp * std::cos(a * .15f) * .6f;
    }
    const int bx = board_x + int(ox), by = y + board_y + int(oy);
    const Color ink = classic ? snake_lcd_ink : theme.at("ink");
    // The score in the corner, faint, as part of the board; a critter's countdown in the other.
    const int shown = sn.state == SnakeUi::State::Ready ? 0 : g.score;
    snake_digits(r, snake_number(shown, 4), view_x + 12, y + board_y + 12, 4, ink, classic ? 150 : 40);
    if (g.bonus_left > 0) {
        const std::string left = snake_number(g.bonus_left, 2);
        snake_digits(r, left, view_x + view_w - 12 - 28, y + board_y + 12, 4, ink, classic ? 150 : 40);
    }
    // Walls: a frame round the board.
    if (g.walls) {
        const Color wall = classic ? snake_lcd_ink : blend(theme.at("accent"), slot, 35);
        const int thick = 4;
        // Along the panel's own edges (the outer columns run past them).
        if (classic) {
            snake_lit(r, view_x, y + board_y, view_w, thick, 255);
            snake_lit(r, view_x, y + board_y + board_h - thick, view_w, thick, 255);
            snake_lit(r, view_x, y + board_y, thick, board_h, 255);
            snake_lit(r, view_x + view_w - thick, y + board_y, thick, board_h, 255);
        } else {
            snake_fill(r, view_x, y + board_y, view_w, thick, wall, 255);
            snake_fill(r, view_x, y + board_y + board_h - thick, view_w, thick, wall, 255);
            snake_fill(r, view_x, y + board_y, thick, board_h, wall, 255);
            snake_fill(r, view_x + view_w - thick, y + board_y, thick, board_h, wall, 255);
        }
    }
    const float t = snake_progress(now);
    const bool eating_now = sn.eaten_step == g.steps && g.steps > 0 && t < 1 && sn.state != SnakeUi::State::Over;
    // The apple: pops in once the last one is gone, and bobs a little.
    {
        const float k = snake_pop_in(float(int(now - sn.food_at)) / 280.f);
        const float fx = float(bx + g.food.x * cell + cell / 2), fy = float(by + g.food.y * cell + cell / 2);
        if (classic) {
            if (int(now - sn.food_at) >= 0)
                for (int row = 0; row < 6; row++)
                    for (int col = 0; col < 6; col++)
                        if (snake_lcd_apple[size_t(row)][col] == '#')
                            snake_lit(r, bx + g.food.x * cell + col * 4, by + g.food.y * cell + row * 4, 4, 4, 255);
        } else snake_apple(r, fx, fy + std::sin(float(now % 6283) * .004f) * .8f, k);
    }
    // What the head is about to swallow, shrinking into its mouth.
    if (eating_now) {
        const float k = 1 - snake_smooth(.45f, .95f, t);
        const auto at = sn.eaten_cell;
        if (classic) {
            // An LCD just switches it off when the head arrives.
        } else if (sn.eaten_bonus)
            snake_critter(r, float(bx + at.x * cell + cell), float(by + at.y * cell + cell / 2), sn.eaten_facing_left,
                          k, 255, now);
        else snake_apple(r, float(bx + at.x * cell + cell / 2), float(by + at.y * cell + cell / 2), k);
    }
    // The critter, walking from cell to cell; it blinks in its last few steps.
    if (g.bonus_left > 0) {
        const bool walked = g.bonus_moved_step == g.steps;
        const float w = walked ? t : 1;
        const float cx = float(bx) + (g.bonus_from.x + (g.bonus.x - g.bonus_from.x) * w) * cell + cell,
                    cy = float(by) + (g.bonus_from.y + (g.bonus.y - g.bonus_from.y) * w) * cell + cell / 2.f;
        const bool shown = g.bonus_left > 10 || (now / 110) % 2 == 0;
        if (classic) {
            if (shown) {
                const auto& art = (now / 160) % 2 ? snake_lcd_critter_a : snake_lcd_critter_b;
                const int left = int(cx) - cell, top = int(cy) - cell / 2;
                for (int row = 0; row < 6; row++)
                    for (int col = 0; col < 12; col++)
                        if (art[size_t(row)][g.bonus_facing_left ? 11 - col : col] == '#')
                            snake_lit(r, left + col * 4, top + row * 4, 4, 4, 255);
            }
        } else {
            const float k = snake_pop_in(float(std::min<Uint32>(now - sn.step_at, 400)) / 280.f);
            snake_critter(r, cx, cy, g.bonus_facing_left, g.bonus_left == snake::bonus_steps ? k : 1, shown ? 255 : 90,
                          now);
        }
    }
    // The snake.
    if (classic) snake_body_lcd(r, y, int(ox), int(oy), now);
    else snake_body(r, y, ox, oy, now);
    // Crumbs, and the points floating up from where they were won.
    for (const auto& p : sn.particles) {
        const float a = float(now - p.born) / 1000.f, life = float(p.life) / 1000.f;
        const float px = p.x + p.vx * a + ox, py = float(y) + p.y + p.vy * a + 520 * a * a + oy;
        const float fade = 1 - a / life;
        snake_disc(r, px, py, p.size * (cell / 16.f) * (.4f + .6f * fade), Color{p.r, p.g, p.b},
                   int(255 * std::min(1.f, fade * 1.6f)));
    }
    for (const auto& p : sn.pops) {
        const float a = float(now - p.born) / 800.f;
        const int rise = int(26 * snake_smooth(0, 1, a)), alpha = int(255 * (1 - snake_smooth(.55f, 1, a)));
        const std::string text = "+" + std::to_string(p.value);
        const int dot = 4, width = int(text.size()) * 4 * dot - dot;
        int px = std::clamp(int(p.x) - width / 2, view_x + 6, view_x + view_w - width - 6);
        const int py = y + std::max(board_y + 4, int(p.y) - 12 - rise);
        if (!classic) snake_digits(r, text, px + 1, py + 1, dot, shadow_colour(), alpha / 3);
        snake_digits(r, text, px, py, dot, classic ? snake_lcd_ink : theme.at("accent"), alpha);
    }
    s.RenderSetClipRect(r, nullptr);
    // The LCD's dots over everything lit.
    if (classic)
        if (SDL_Texture* dots = snake_texture(r, 2, 0)) {
            s.RenderSetClipRect(r, &view);
            s.RenderCopy(r, dots, nullptr, &board);
            s.RenderSetClipRect(r, nullptr);
        }
}

void App::snake_top(SDL_Renderer* r, int y) {
    using namespace snake_layout;
    status_bar(r, y);
    snake_board(r, y);
    // A card over the board when it is not moving: the start, a pause, the end.
    if (sn.state == SnakeUi::State::Playing || (sn.state == SnakeUi::State::Over && !sn.card_shown)) return;
    const bool classic = sn.classic;
    const int w = 384, h = 140, x = 320 - w / 2, top = y + board_y + (board_h - h) / 2;
    const Color plate = classic ? snake_lcd_ink : Color{20, 24, 30}, words = classic ? snake_lcd : Color{255, 255, 255};
    alpha_round(r, x + 3, top + 5, w, h, card_radius, shadow_colour(), preferences.dark ? 90 : 40);
    alpha_round(r, x, top, w, h, card_radius, plate, classic ? 255 : sn.state == SnakeUi::State::Ready ? 210 : 232);
    std::string title, note;
    switch (sn.state) {
    case SnakeUi::State::Ready:
        title = "SNAKE";
        note = locale.text("Press A or a direction to start");
        break;
    case SnakeUi::State::Paused:
        title = locale.text("Paused");
        note = locale.text("Press A to carry on");
        break;
    default:
        title = sn.game.won   ? locale.text("You win!")
                : sn.new_best ? locale.text("New best!")
                              : locale.text("Game over");
        note = locale.text("Score") + " " + std::to_string(sn.game.score) + "  ·  " + locale.text("Best") + " " +
               std::to_string(sn.best[sn.walls ? 1 : 0]);
        break;
    }
    if (sn.state == SnakeUi::State::Ready) {
        // The name in the dots of the old phones, big, with a little bounce to each letter.
        const int dot = 6, width = 5 * 6 * dot - dot, left = 320 - width / 2;
        const Uint32 now = s.GetTicks();
        for (int i = 0; i < 5; i++) {
            const float phase = float(now % 1800) / 1800.f * 6.2832f - float(i) * .55f;
            const int lift = int(std::max(0.f, std::sin(phase)) * 5);
            snake_dots(r, title.substr(size_t(i), 1), left + i * 6 * dot, top + 6 - lift, dot,
                       classic ? snake_lcd : theme.at("accent"));
        }
    } else {
        const int width = std::min(w - 32, label_width(title, 28, false));
        raw_label_colour(r, title, 320 - width / 2, centred_label_y(title, 28, top + 42), 28, words, w - 32);
        if (sn.new_best && !classic) sparkle(r, 320 - width / 2 - 20, top + 42, 9, theme.at("accent"), 255);
    }
    const int nw = std::min(w - 32, label_width(note, 16, false));
    raw_label_colour(r, note, 320 - nw / 2,
                     centred_label_y(note, 16, top + (sn.state == SnakeUi::State::Ready ? 100 : 84)), 16,
                     classic ? snake_lcd : Color{214, 220, 228}, w - 32);
    if (sn.state == SnakeUi::State::Over) {
        const std::string again = "A  " + locale.text("Play again") + "   ·   B  " + locale.text("Leave");
        const int aw = std::min(w - 32, label_width(again, 14, false));
        raw_label_colour(r, again, 320 - aw / 2, centred_label_y(again, 14, top + 114), 14,
                         classic ? snake_lcd : Color{170, 178, 190}, w - 32);
    }
}

void App::snake_bottom(SDL_Renderer* r, int y) {
    using namespace snake_layout;
    rect(r, 0, y, 640, 480, "background");
    pattern(r, y, 480, true);
    const bool held = pointer_down && sn.button >= 0, playing = sn.state == SnakeUi::State::Playing;
    back_button(r, 28, y + 20, held && sn.button == 0);
    label(r, "Snake", 88, y + 25, 28, "ink", 400);
    rect(r, 24, y + 77, 592, 1, "line");
    rect(r, 24, y + 76, 44, 2, "accent");
    // Score and best, in the dots of the old phones.
    settings_card(r, 24, y + card_y, 592, card_h);
    rect(r, 320, y + card_y + 16, 1, card_h - 32, "line");
    for (int side = 0; side < 2; side++) {
        const int x = side ? 344 : 48;
        label(r, side ? "Best" : "Score", x, y + card_y + 14, 16, "muted", 240);
        const int value = side ? sn.best[sn.walls ? 1 : 0] : sn.state == SnakeUi::State::Ready ? 0 : sn.game.score;
        snake_digits(r, std::to_string(value), x, y + card_y + 44, 8, theme.at(side ? "ink" : "accent"));
        if (side && sn.new_best && sn.state == SnakeUi::State::Over)
            sparkle(r, 590, y + card_y + 26, 9, theme.at("accent"), 255);
    }
    if (sn.walls) {
        const std::string note = locale.text("With walls");
        const int width = std::min(140, label_width(note, 14, false));
        raw_label(r, note, 592 - width, centred_label_y(note, 14, y + card_y + 24), 14, "muted", 140);
    }
    // Speed: nine steps, as the phone had them. Locked while a game is moving.
    label(r, "Speed", 36, y + speeds_y - 28, 16, "muted", 300);
    for (int i = 0; i < snake::levels; i++) {
        const int x = speed_x(i);
        const bool chosen = sn.level == i + 1, pressed = held && sn.button == 10 + i;
        const Color fill = theme.at(chosen ? "accent" : pressed ? "focus" : "panel");
        raised_pill(r, x, y + speeds_y, speed_w, speed_h, control_radius,
                    playing && !chosen ? blend(fill, theme.at("background"), 40) : fill, pressed ? 1 : 0);
        const std::string digit = std::to_string(i + 1);
        const Color ink = chosen ? theme.at("on_accent") : theme.at(playing ? "muted" : "ink");
        raw_label_colour(r, digit, x + (speed_w - label_width(digit, 22, false)) / 2,
                         centred_label_y(digit, 22, y + speeds_y + speed_h / 2 + (pressed ? 2 : 0)), 22, ink, speed_w);
    }
    // Walls and Classic screen: lit while on.
    for (int i = 0; i < 2; i++) {
        const int x = i ? 328 : 24;
        const bool on = i ? sn.classic : sn.walls, pressed = held && sn.button == 20 + i;
        const Color fill = theme.at(on ? "accent" : pressed ? "focus" : "panel");
        raised_pill(r, x, y + switches_y, switch_w, switch_h, control_radius + 2,
                    playing && i == 0 ? blend(fill, theme.at("background"), 40) : fill, pressed ? 1 : 0);
        const int sink = pressed ? 2 : 0, cy = y + switches_y + switch_h / 2 + sink;
        const Color ink = theme.at(on ? "on_accent" : "ink");
        // A little picture of each: a framed square for walls, the phone's two greens for the classic display.
        if (i == 0) {
            const Color line = theme.at(on ? "on_accent" : "accent");
            tint_rect(r, x + 18, cy - 11, 22, 3, line);
            tint_rect(r, x + 18, cy + 8, 22, 3, line);
            tint_rect(r, x + 18, cy - 11, 3, 22, line);
            tint_rect(r, x + 37, cy - 11, 3, 22, line);
            tint_rect(r, x + 26, cy - 3, 6, 6, line);
        } else {
            fill_round(r, x + 16, cy - 12, 26, 24, 5, snake_lcd);
            tint_rect(r, x + 21, cy - 2, 12, 4, snake_lcd_ink);
            tint_rect(r, x + 29, cy - 7, 4, 9, snake_lcd_ink);
        }
        const std::string name = locale.text(i ? "Classic screen" : "Walls");
        raw_label_colour(r, name, x + 54, centred_label_y(name, 20, cy), 20, ink, switch_w - 110);
        const std::string key = i ? "X" : "Y";
        fill_round(r, x + switch_w - 36, cy - 11, 22, 22, 11, on ? ink : theme.at("slot"));
        raw_label_colour(r, key, x + switch_w - 36 + (22 - label_width(key, 14, false)) / 2,
                         centred_label_y(key, 14, cy), 14, on ? fill : theme.at("muted"), 22);
    }
    // What the shoulders and the screens do.
    if (playing) {
        const std::string note = locale.text("Swipe on either screen to steer");
        raw_label(r, note, 320 - std::min(560, label_width(note, 16, false)) / 2, centred_label_y(note, 16, y + 396),
                  16, "muted", 560);
    } else key_hints(r, 320, y + 396, {{"L", "Slower"}, {"R", "Faster"}}, 0, 16, 560);
    rect(r, 24, y + 424, 592, 1, "line");
    footer_button(r, footer_x[0], y + footer_y, footer_w, "Back", "B", false, held && sn.button == 1);
    const char* go = sn.state == SnakeUi::State::Ready     ? "Start"
                     : sn.state == SnakeUi::State::Playing ? "Pause"
                     : sn.state == SnakeUi::State::Paused  ? "Resume"
                                                           : "Play again";
    footer_button(r, footer_x[1], y + footer_y, footer_w, go, "A", true, held && sn.button == 2);
}

// ---- keys and touch

void App::snake_action(int code) {
    last_activity = s.GetTicks();
    dirty = true;
    switch (code) {
    case 0: snake_turn(snake::Dir::Left); break;
    case 1: snake_turn(snake::Dir::Right); break;
    case 8: snake_turn(snake::Dir::Up); break;
    case 9: snake_turn(snake::Dir::Down); break;
    case 2:
    case 4: snake_go(); break;
    case 3:
        if (sn.state == SnakeUi::State::Playing) snake_go(); // B pauses first; a second B leaves
        else close_snake();
        break;
    case 7: close_snake(); break;
    case 13:
    case 14: snake_set(sn.level + (code == 13 ? -1 : 1), sn.walls, sn.classic); break;
    case 12: snake_set(sn.level, !sn.walls, sn.classic); break;
    case 11: snake_set(sn.level, sn.walls, !sn.classic); break;
    default: break;
    }
}

// The hat on a diagonal: the way that turns the snake wins, so a sloppy press still turns it.
void App::snake_hat(int value) {
    const bool left = value & SDL_HAT_LEFT, right = value & SDL_HAT_RIGHT, up = value & SDL_HAT_UP,
               down = value & SDL_HAT_DOWN;
    const bool across = left || right, along = up || down;
    if (!across && !along) return;
    const bool turn_across = across && (!along || snake::vertical(sn.game.heading()));
    snake_action(turn_across ? (left ? 0 : 1) : (up ? 8 : 9));
}

// Five quick taps on the battery (Home's upper screen) open the game. True when the tap was on the battery.
bool App::snake_secret_tap(int kind, int x, int y, Uint32 now) {
    if (kind != 0 || !snake_available() || !session_initialized || welcome_active || launching ||
        settings.page != Page::Library || !home_menu || systems_menu || picker_open || filter_sheet_active() ||
        chat_open || music_open || vt.open)
        return false;
    if (x < 548 || y >= 48) return false;
    if (now - sn.tap_at > 650) sn.taps = 0;
    sn.tap_at = now;
    if (++sn.taps >= 3) haptic_detent(); // something stirs
    if (sn.taps >= 5) {
        sn.taps = 0;
        open_snake();
    }
    return true;
}

// A swipe of 22 px turns the snake, then counts again from there, so one long stroke can turn twice.
bool App::snake_swipe(int& ax, int& ay, int x, int y) {
    const int dx = x - ax, dy = y - ay;
    if (std::max(std::abs(dx), std::abs(dy)) < 22) return false;
    if (std::abs(dx) > std::abs(dy)) snake_turn(dx < 0 ? snake::Dir::Left : snake::Dir::Right);
    else snake_turn(dy < 0 ? snake::Dir::Up : snake::Dir::Down);
    ax = x;
    ay = y;
    last_activity = s.GetTicks();
    return true;
}

void App::snake_top_touch(int kind, int x, int y) {
    if (kind == 0) {
        sn.top_down = true;
        sn.top_swiped = false;
        sn.top_x = x;
        sn.top_y = y;
    } else if (kind == 1 && sn.top_down) {
        if (snake_swipe(sn.top_x, sn.top_y, x, y)) sn.top_swiped = true;
    } else if (kind >= 2 && sn.top_down) {
        sn.top_down = false;
        // A plain tap on the board starts, carries on or plays again.
        if (!sn.top_swiped && sn.state != SnakeUi::State::Playing) snake_go();
    }
    dirty = true;
}

int App::snake_button_at(int x, int y) {
    using namespace snake_layout;
    if (x < 84 && y < 78) return 0;
    if (y >= footer_y - 8) return x < 320 ? 1 : 2;
    if (sn.state == SnakeUi::State::Playing) return -1; // the rest of the screen steers
    for (int i = 0; i < snake::levels; i++)
        if (x >= speed_x(i) - 4 && x < speed_x(i) + speed_w + 4 && y >= speeds_y - 6 && y < speeds_y + speed_h + 6)
            return 10 + i;
    if (y >= switches_y - 6 && y < switches_y + switch_h + 6) return x < 320 ? 20 : 21;
    return -1;
}

void App::snake_press(int x, int y) {
    sn.button = snake_button_at(x, y);
    sn.anchor_x = x;
    sn.anchor_y = y;
    sn.swiped = false;
    last_activity = s.GetTicks();
    dirty = true;
}

void App::snake_drag(int x, int y) {
    if (sn.button >= 10 || (sn.button < 0)) {
        // Off a speed or a switch by a swipe: it steers instead.
        if (sn.button >= 0 && std::max(std::abs(x - sn.anchor_x), std::abs(y - sn.anchor_y)) >= 22) sn.button = -1;
        if (sn.button < 0 && snake_swipe(sn.anchor_x, sn.anchor_y, x, y)) sn.swiped = true;
    }
    dirty = true;
}

void App::snake_release(int x, int y) {
    const int button = sn.button;
    sn.button = -1;
    dirty = true;
    if (button < 0 || snake_button_at(x, y) != button) return;
    if (button == 0 || button == 1) snake_action(3);
    else if (button == 2) snake_go();
    else if (button >= 10 && button < 10 + snake::levels) snake_set(button - 9, sn.walls, sn.classic);
    else if (button == 20) snake_set(sn.level, !sn.walls, sn.classic);
    else if (button == 21) snake_set(sn.level, sn.walls, !sn.classic);
}
