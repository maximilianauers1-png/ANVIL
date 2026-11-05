#include "anvil/expr.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <numbers>

namespace anvil {
namespace {

enum class Tok { Num, Ident, Op, LParen, RParen, Comma, End };

struct Token {
    Tok kind;
    std::string_view text;
    double num = 0.0;
    std::size_t pos = 0;
};

class Lexer {
  public:
    explicit Lexer(std::string_view s) : s_(s) { advance(); }
    const Token& peek() const { return cur_; }
    Token take() {
        Token t = cur_;
        advance();
        return t;
    }

  private:
    void advance() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
        std::size_t start = i_;
        if (i_ >= s_.size()) {
            cur_ = {Tok::End, {}, 0.0, start};
            return;
        }
        char c = s_[i_];
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i_ + 1 < s_.size() &&
                                                            std::isdigit(static_cast<unsigned char>(s_[i_ + 1])))) {
            std::size_t j = i_;
            while (j < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[j])) || s_[j] == '.')) ++j;
            // exponent only if followed by digits ("1e3", not "1 em")
            if (j < s_.size() && (s_[j] == 'e' || s_[j] == 'E')) {
                std::size_t k = j + 1;
                if (k < s_.size() && (s_[k] == '+' || s_[k] == '-')) ++k;
                if (k < s_.size() && std::isdigit(static_cast<unsigned char>(s_[k]))) {
                    while (k < s_.size() && std::isdigit(static_cast<unsigned char>(s_[k]))) ++k;
                    j = k;
                }
            }
            double v = 0.0;
            auto r = std::from_chars(s_.data() + i_, s_.data() + j, v);
            if (r.ec != std::errc() || r.ptr != s_.data() + j)
                throw ExprError("invalid number '" + std::string(s_.substr(i_, j - i_)) + "'", start);
            cur_ = {Tok::Num, s_.substr(i_, j - i_), v, start};
            i_ = j;
            return;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i_;
            while (j < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '_')) ++j;
            cur_ = {Tok::Ident, s_.substr(i_, j - i_), 0.0, start};
            i_ = j;
            return;
        }
        // Unicode micro sign / degree sign (UTF-8) as unit shorthands.
        if (s_.substr(i_, 2) == "\xc2\xb5" || s_.substr(i_, 2) == "\xce\xbc") {  // µ, μ
            if (s_.substr(i_ + 2, 1) == "m") {
                cur_ = {Tok::Ident, "um", 0.0, start};
                i_ += 3;
                return;
            }
        }
        if (s_.substr(i_, 2) == "\xc2\xb0") {  // °
            cur_ = {Tok::Ident, "deg", 0.0, start};
            i_ += 2;
            return;
        }
        ++i_;
        switch (c) {
            case '+': case '-': case '*': case '/': case '^':
                cur_ = {Tok::Op, s_.substr(start, 1), 0.0, start};
                return;
            case '(': cur_ = {Tok::LParen, s_.substr(start, 1), 0.0, start}; return;
            case ')': cur_ = {Tok::RParen, s_.substr(start, 1), 0.0, start}; return;
            case ',': cur_ = {Tok::Comma, s_.substr(start, 1), 0.0, start}; return;
            case '"': cur_ = {Tok::Ident, "in", 0.0, start}; return;  // 2" = 2 in
            default:
                throw ExprError(std::string("unexpected character '") + c + "'", start);
        }
    }
    std::string_view s_;
    std::size_t i_ = 0;
    Token cur_{Tok::End, {}, 0.0, 0};
};

std::optional<RtQuantity> builtin(std::string_view id) {
    using namespace std::numbers;
    if (id == "mm") return RtQuantity{1.0, dims::kLength};
    if (id == "um") return RtQuantity{1e-3, dims::kLength};
    if (id == "cm") return RtQuantity{10.0, dims::kLength};
    if (id == "m") return RtQuantity{1000.0, dims::kLength};
    if (id == "in" || id == "inch") return RtQuantity{units::kMmPerInch, dims::kLength};
    if (id == "deg") return RtQuantity{pi / 180.0, dims::kAngle};
    if (id == "rad") return RtQuantity{1.0, dims::kAngle};
    if (id == "pi") return RtQuantity{pi, dims::kNone};
    return std::nullopt;
}

class Parser {
  public:
    Parser(std::string_view s, const VariableResolver& v, std::vector<std::string>* idents)
        : lx_(s), vars_(v), idents_(idents) {}

    RtQuantity parse() {
        RtQuantity r = expr();
        if (lx_.peek().kind != Tok::End)
            throw ExprError("unexpected '" + std::string(lx_.peek().text) + "'", lx_.peek().pos);
        return r;
    }

  private:
    static bool starts_primary(const Token& t) {
        return t.kind == Tok::Num || t.kind == Tok::Ident || t.kind == Tok::LParen;
    }

    RtQuantity expr() {
        RtQuantity a = term();
        while (lx_.peek().kind == Tok::Op && (lx_.peek().text == "+" || lx_.peek().text == "-")) {
            Token op = lx_.take();
            RtQuantity b = term();
 if (!(a.dim == b.dim) && !idents_)
                throw ExprError("cannot " + std::string(op.text == "+" ? "add " : "subtract ") +
                                    dim_name(b.dim) + (op.text == "+" ? " to " : " from ") + dim_name(a.dim),
                                op.pos);
            a.value = op.text == "+" ? a.value + b.value : a.value - b.value;
        }
        return a;
    }

    RtQuantity term() {
        RtQuantity a = unary();
        for (;;) {
            const Token& t = lx_.peek();
            if (t.kind == Tok::Op && (t.text == "*" || t.text == "/")) {
                Token op = lx_.take();
                RtQuantity b = unary();
                if (op.text == "*") {
                    a = {a.value * b.value, a.dim + b.dim};
                } else {
                    if (b.value == 0.0) throw ExprError("division by zero", op.pos);
                    a = {a.value / b.value, a.dim - b.dim};
                }
            } else if (starts_primary(t)) {  // juxtaposition: "12.5 cm"
                RtQuantity b = power();
                a = {a.value * b.value, a.dim + b.dim};
            } else {
                return a;
            }
        }
    }

    RtQuantity unary() {
        const Token& t = lx_.peek();
        if (t.kind == Tok::Op && (t.text == "-" || t.text == "+")) {
            bool neg = t.text == "-";
            lx_.take();
            RtQuantity a = unary();
            if (neg) a.value = -a.value;
            return a;
        }
        return power();
    }

    RtQuantity power() {
        RtQuantity base = primary();
        if (lx_.peek().kind == Tok::Op && lx_.peek().text == "^") {
            Token op = lx_.take();
            RtQuantity e = unary();
            if (!e.dim.dimensionless()) throw ExprError("exponent must be dimensionless", op.pos);
            if (!base.dim.dimensionless()) {
                double k = std::round(e.value);
                if (k != e.value) throw ExprError("dimensioned quantity needs an integer exponent", op.pos);
                base.dim = base.dim.scaled(static_cast<int>(k));
            }
            base.value = std::pow(base.value, e.value);
        }
        return base;
    }

    RtQuantity primary() {
        Token t = lx_.take();
        switch (t.kind) {
            case Tok::Num:
                return {t.num, dims::kNone};
            case Tok::LParen: {
                RtQuantity r = expr();
                if (lx_.peek().kind != Tok::RParen) throw ExprError("missing ')'", lx_.peek().pos);
                lx_.take();
                return r;
            }
            case Tok::Ident: {
                if (lx_.peek().kind == Tok::LParen) return call(t);
                if (auto b = builtin(t.text)) return *b;
                if (idents_) {
                    idents_->emplace_back(t.text);
                    return {1.0, dims::kNone};
                }
                if (vars_) {
                    if (auto v = vars_(t.text)) return *v;
                }
                throw ExprError("unknown name '" + std::string(t.text) + "'", t.pos);
            }
            case Tok::End:
                throw ExprError("unexpected end of expression", t.pos);
            default:
                throw ExprError("unexpected '" + std::string(t.text) + "'", t.pos);
        }
    }

    RtQuantity call(const Token& fn) {
        lx_.take();  // '('
        std::vector<RtQuantity> args;
        if (lx_.peek().kind != Tok::RParen) {
            for (;;) {
                args.push_back(expr());
                if (lx_.peek().kind == Tok::Comma) {
                    lx_.take();
                    continue;
                }
                break;
            }
        }
        if (lx_.peek().kind != Tok::RParen) throw ExprError("missing ')'", lx_.peek().pos);
        lx_.take();
        auto need = [&](std::size_t n) {
            if (args.size() != n)
                throw ExprError(std::string(fn.text) + "() takes " + std::to_string(n) + " argument(s)", fn.pos);
        };
        std::string_view f = fn.text;
        if (f == "sqrt") {
            need(1);
            const Dim& d = args[0].dim;
            if (d.L % 2 || d.M % 2 || d.T % 2 || d.Th % 2 || d.I % 2 || d.A % 2)
                throw ExprError("sqrt() of " + dim_name(d) + " has no integer dimension", fn.pos);
            if (args[0].value < 0) throw ExprError("sqrt() of a negative value", fn.pos);
            Dim h{std::int8_t(d.L / 2), std::int8_t(d.M / 2), std::int8_t(d.T / 2),
                  std::int8_t(d.Th / 2), std::int8_t(d.I / 2), std::int8_t(d.A / 2)};
            return {std::sqrt(args[0].value), h};
        }
        if (f == "abs") {
            need(1);
            return {std::abs(args[0].value), args[0].dim};
        }
        if (f == "min" || f == "max") {
            if (args.empty()) throw ExprError(std::string(f) + "() needs arguments", fn.pos);
            RtQuantity r = args[0];
            for (auto& a : args) {
                if (!(a.dim == r.dim) && !idents_) throw ExprError(std::string(f) + "() of mixed dimensions", fn.pos);
                r.value = f == "min" ? std::min(r.value, a.value) : std::max(r.value, a.value);
            }
            return r;
        }
        if (f == "sin" || f == "cos" || f == "tan") {
            need(1);
            if (!(args[0].dim == dims::kAngle || args[0].dim.dimensionless()))
                throw ExprError(std::string(f) + "() needs an angle", fn.pos);
            double x = args[0].value;
            double v = f == "sin" ? std::sin(x) : f == "cos" ? std::cos(x) : std::tan(x);
            return {v, dims::kNone};
        }
        throw ExprError("unknown function '" + std::string(f) + "'", fn.pos);
    }

    Lexer lx_;
    const VariableResolver& vars_;
    std::vector<std::string>* idents_;
};

}  // namespace

RtQuantity eval_expr(std::string_view text, const VariableResolver& vars) {
    return Parser(text, vars, nullptr).parse();
}

double eval_length_mm(std::string_view text, const VariableResolver& vars, LengthUnit default_unit) {
    RtQuantity q = eval_expr(text, vars);
    if (q.dim.dimensionless()) return q.value * unit_info(default_unit).mm_per_unit;
    if (!(q.dim == dims::kLength)) throw ExprError("expected a Length, got " + dim_name(q.dim), 0);
    return q.value;
}

std::vector<std::string> expr_identifiers(std::string_view text) {
    std::vector<std::string> ids;
    VariableResolver none;
    Parser(text, none, &ids).parse();
    return ids;
}

}  // namespace anvil
