#pragma once
// Update download progress: fraction, speed and easing.
// Members of App in main.cpp; included once, right after the class.

double App::update_fraction() {
    if (update_stage() == UpdateStage::Ready) return 1;
    const std::string state = update_status["state"];
    const double p = std::clamp(std::atof(update_status["progress"].c_str()) / 100.0, 0.0, 1.0),
                 size = std::atof(update_status["size"].c_str()),
                 received = std::atof(update_status["received"].c_str());
    if (state == "verifying" || state == "staging") return p;
    const double got = size > 0 ? std::clamp(received / size, 0.0, 1.0) : 0;
    return state == "downloading" ? std::max(got, p) : got;
}

std::string App::update_speed() {
    if (update_status["state"] != "downloading") return {};
    const double rate = std::atof(update_status["rate"].c_str());
    const int eta = std::atoi(update_status["eta"].c_str());
    if (rate <= 0) return {};
    char speed[32];
    if (rate >= 1048576) std::snprintf(speed, sizeof speed, "%.1f MB/s", rate / 1048576.0);
    else std::snprintf(speed, sizeof speed, "%d KB/s", std::max(1, int(rate / 1024)));
    if (eta < 0) return speed;
    const std::string left = eta < 60     ? locale.text("Less than a minute left")
                             : eta < 5400 ? locale.text("About " + std::to_string((eta + 30) / 60) + " min left")
                                          : locale.text("About " + std::to_string((eta + 1800) / 3600) + " h left");
    return std::string(speed) + "  ·  " + left;
}

void App::ease_update(Uint32 now) {
    const double target = update_fraction();
    const bool glitter = glitter_active(), arrived = glitter && !glitter_was;
    glitter_was = glitter;
    if (!glitter || arrived || update_stage() != UpdateStage::Running || target < update_shown - .05)
        update_shown = target;
    else update_shown += (target - update_shown) * std::min(1.0, (now - update_shown_at) / 220.0);
    update_shown_at = now;
}
