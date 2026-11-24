// Toasts (src/launcher/toast.hpp): the look the words pick, the stack, refreshing the same words, timing out and
// leaving, the spring settling, under the sanitizers (tests/test_toast.py builds and runs this).
#include "launcher/toast.hpp"
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    failures++;
}
} // namespace

int main() {
    using toast::Severity;
    // The words decide the look.
    check(toast::infer("Could not save all settings. Check storage.") == Severity::Error, "could not = error");
    check(toast::infer("This request did not finish. Please try again.") == Severity::Error, "did not = error");
    check(toast::infer("Connect the handheld to a computer first.") == Severity::Warning, "first = warning");
    check(toast::infer("Connect to Wi-Fi first") == Severity::Warning, "Wi-Fi first = warning");
    check(toast::infer("USB data transfer is not available on this handheld.") == Severity::Warning,
          "not available = warning");
    check(toast::infer("Stop scraping before signing in") == Severity::Warning, "stop = warning");
    check(toast::infer("Settings saved") == Severity::Success, "saved = success");
    check(toast::infer("Welcome back") == Severity::Success, "welcome back = success");
    check(toast::infer("Already connected.") == Severity::Info, "plain = info");
    check(toast::hold_ms("x", Severity::Error) > toast::hold_ms("x", Severity::Info), "errors stay longer");

    toast::Stack stack;
    const auto a = stack.post("First", Severity::Info, 1000);
    stack.post("Second", Severity::Error, 1100);
    check(stack.cards.size() == 2 && stack.cards.front().text == "Second", "newest on top");
    // The same words again refresh and nudge the card instead of stacking a copy.
    const auto again = stack.post("First", Severity::Info, 1500);
    check(again == a && stack.cards.size() == 2, "same words, same card");
    check(stack.cards.back().nudged == 1500, "the refreshed card nudges");
    check(stack.cards.back().until == 1500 + toast::hold_ms("First", Severity::Info), "and its clock restarts");
    // A fourth pushes the oldest out.
    stack.post("Third", Severity::Success, 1600);
    stack.post("Fourth", Severity::Warning, 1700);
    int staying = 0;
    for (const auto& c : stack.cards) staying += !c.left;
    check(staying == 3, "three at most");
    // Layout: each under the one above, with the gap.
    for (auto& c : stack.cards) c.height = 52;
    stack.layout(12);
    check(stack.cards[0].target_y == 12 && stack.cards[1].target_y == 12 + 52 + toast::gap, "stacked slots");
    // The spring starts above the screen, overshoots a little, and settles.
    check(toast::enter_offset(0) == 1, "starts above");
    bool overshoot = false;
    for (uint32_t t = 0; t < toast::enter_ms; t += 5) overshoot |= toast::enter_offset(t) < -0.02;
    check(overshoot, "overshoots");
    check(std::abs(toast::enter_offset(toast::enter_ms - 20)) < 0.02, "settled before it ends");
    check(toast::leave_progress(0) == 0 && toast::leave_progress(toast::leave_ms) == 1, "leaving runs 0..1");
    check(toast::nudge_offset(toast::nudge_ms) == 0, "nudge dies away");
    // Time runs out: it leaves, then it is gone.
    toast::Stack timed;
    timed.post("Hello", Severity::Info, 0);
    const uint32_t hold = toast::hold_ms("Hello", Severity::Info);
    check(timed.wait_ms(0) == int(hold), "wakes when the card is due");
    check(!timed.tick(hold - 1) && !timed.cards.front().left, "stays until due");
    check(timed.tick(hold) && timed.cards.front().left == hold, "then leaves");
    check(timed.moving(hold + 10), "leaving is motion");
    check(timed.tick(hold + toast::leave_ms) && timed.empty(), "and is gone");
    check(timed.wait_ms(0) == -1, "nothing to wait for");
    // Dismissing by tap.
    const auto id = timed.post("Tap me", Severity::Info, 0);
    timed.dismiss(id, 50);
    check(timed.cards.front().left == 50, "a tap sends it away");
    // Without animations a card goes at once.
    toast::Stack still;
    still.animations = false;
    still.post("Plain", Severity::Info, 0);
    check(!still.moving(0), "no motion without animations");
    still.tick(toast::hold_ms("Plain", Severity::Info));
    check(still.empty(), "gone at once without animations");
    // Gliding to a new slot is framerate independent and lands exactly.
    toast::Stack glide;
    glide.post("A", Severity::Info, 0);
    glide.cards[0].height = 52;
    glide.layout(12);
    glide.cards[0].target_y = 80;
    for (int i = 0; i < 60; i++) glide.glide(16);
    check(glide.cards[0].shown_y == 80, "glide lands");
    if (failures) return 1;
    std::printf("toast: ok\n");
    return 0;
}
