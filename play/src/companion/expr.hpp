// SPDX-License-Identifier: GPL-3.0-or-later
// A number a view works out from the game's memory, written in a profile the
// way its addresses are: hex numbers, mem8/mem16/mem32[ADDRESS], parentheses
// and + - * / << >> & |, with C's precedence. Where the screen is in the world:
//
//   x = (mem8[50] << 8) + mem8[FD]
//   y = (mem8[EB] >> 4) * B0 - 40
//   x = f32[800F69A4]          # a 32-bit float (an N64's places), its whole part
#pragma once
#include "companion/profile.hpp"
#include "core/core.hpp"

#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace play {

class Expr {
public:
    bool parse(const std::string& text, std::string& error) {
        text_ = text;
        at_ = 0;
        nodes_.clear();
        root_ = parse_binary(0, error);
        skip();
        if (root_ >= 0 && at_ != text_.size()) error = "unexpected '" + text_.substr(at_) + "'";
        if (!error.empty()) {
            error = "in '" + text + "': " + error;
            return false;
        }
        return true;
    }
    bool empty() const { return root_ < 0; }
    long eval(Core& core) const { return root_ < 0 ? 0 : eval(core, root_); }

private:
    struct Node {
        char op = 0; // 0: a number, 'm': memory, 'f': a float in memory, else the operator ('<' <<, '>' >>)
        long value = 0;
        uint32_t address = 0;
        int size = 1, a = -1, b = -1;
    };
    void skip() {
        while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_]))) ++at_;
    }
    static int precedence(char op) {
        switch (op) {
        case '*':
        case '/': return 5;
        case '+':
        case '-': return 4;
        case '<':
        case '>': return 3;
        case '&': return 2;
        case '|': return 1;
        default: return -1;
        }
    }
    char peek_op() {
        skip();
        if (at_ >= text_.size()) return 0;
        const char c = text_[at_];
        if ((c == '<' || c == '>') && at_ + 1 < text_.size() && text_[at_ + 1] == c) return c;
        return precedence(c) > 0 && c != '<' && c != '>' ? c : 0;
    }
    int parse_binary(int min, std::string& error) {
        int left = parse_operand(error);
        for (char op; left >= 0 && (op = peek_op()) && precedence(op) >= min;) {
            at_ += (op == '<' || op == '>') ? 2 : 1;
            const int right = parse_binary(precedence(op) + 1, error);
            if (right < 0) return -1;
            nodes_.push_back({op, 0, 0, 1, left, right});
            left = int(nodes_.size()) - 1;
        }
        return left;
    }
    int parse_operand(std::string& error) {
        skip();
        if (at_ >= text_.size()) {
            error = "a number or mem8[...] is missing";
            return -1;
        }
        if (text_[at_] == '(') {
            ++at_;
            const int inner = parse_binary(0, error);
            skip();
            if (inner < 0) return -1;
            if (at_ >= text_.size() || text_[at_] != ')') {
                error = "a ')' is missing";
                return -1;
            }
            ++at_;
            return inner;
        }
        const bool real = text_.compare(at_, 4, "f32[") == 0;
        if (real || text_.compare(at_, 3, "mem") == 0) {
            const size_t open = text_.find('[', at_), close = text_.find(']', at_);
            if (open == std::string::npos || close == std::string::npos || close < open) {
                error = "expected mem8[ADDRESS]";
                return -1;
            }
            Node n;
            n.op = real ? 'f' : 'm';
            const std::string width = text_.substr(at_ + 3, open - at_ - 3);
            n.size = real ? 4 : width == "8" ? 1 : width == "16" ? 2 : width == "32" ? 4 : 0;
            if (!n.size || !parse_address(text_.substr(open + 1, close - open - 1), n.address)) {
                error = "bad memory read '" + text_.substr(at_, close - at_ + 1) + "'";
                return -1;
            }
            at_ = close + 1;
            nodes_.push_back(n);
            return int(nodes_.size()) - 1;
        }
        size_t end = at_;
        while (end < text_.size() && std::isxdigit(static_cast<unsigned char>(text_[end]))) ++end;
        if (end == at_) {
            error = "unexpected '" + text_.substr(at_) + "'";
            return -1;
        }
        Node n;
        n.value = std::stol(text_.substr(at_, end - at_), nullptr, 16);
        at_ = end;
        nodes_.push_back(n);
        return int(nodes_.size()) - 1;
    }
    long eval(Core& core, int i) const {
        const Node& n = nodes_[size_t(i)];
        if (n.op == 0) return n.value;
        if (n.op == 'm') return long(peek_address(core, n.address, n.size));
        if (n.op == 'f') {
            const uint32_t bits = peek_address(core, n.address, 4);
            float f;
            std::memcpy(&f, &bits, 4);
            return std::isfinite(f) && std::fabs(f) < 1e9f ? long(f) : 0;
        }
        const long a = eval(core, n.a), b = eval(core, n.b);
        switch (n.op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b ? a / b : 0;
        case '<': return a << (b & 31);
        case '>': return a >> (b & 31);
        case '&': return a & b;
        case '|': return a | b;
        default: return 0;
        }
    }

    std::string text_;
    size_t at_ = 0;
    std::vector<Node> nodes_;
    int root_ = -1;
};

// A view's key read as an expression; false with `error` set when it is there but doesn't parse.
inline bool view_expr(const ViewSpec& spec, const char* key, Expr& out, bool required, std::string& error) {
    auto found = spec.values.find(key);
    if (found == spec.values.end()) {
        if (required) error = "[view] " + spec.format + " needs " + key;
        return !required;
    }
    std::string why;
    if (!out.parse(found->second, why)) {
        error = "[view] " + spec.format + " " + key + ": " + why;
        return false;
    }
    return true;
}

} // namespace play
