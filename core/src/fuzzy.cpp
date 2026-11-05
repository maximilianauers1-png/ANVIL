#include "anvil/fuzzy.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace anvil {

namespace {
char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
bool word_start(std::string_view t, std::size_t i) {
    if (i == 0) return true;
    char p = t[i - 1], c = t[i];
    return !std::isalnum(static_cast<unsigned char>(p)) ||
           (std::islower(static_cast<unsigned char>(p)) && std::isupper(static_cast<unsigned char>(c)));
}
}  // namespace

double fuzzy_score(std::string_view q, std::string_view t) {
    if (q.empty()) return 0.0;
    // Greedy left-to-right matching that prefers word starts: for each query
    // char, take the next word-start occurrence if one exists before the next
    // ordinary occurrence of the following query char; simple and O(|t|).
    double score = 0;
    std::size_t ti = 0, first = std::string_view::npos;
    long last = -2;
    for (std::size_t qi = 0; qi < q.size(); ++qi) {
        char qc = lower(q[qi]);
        if (qc == ' ') continue;
        std::size_t found = std::string_view::npos, ws = std::string_view::npos;
        for (std::size_t k = ti; k < t.size(); ++k) {
            if (lower(t[k]) != qc) continue;
            if (found == std::string_view::npos) found = k;
            if (word_start(t, k)) {
                ws = k;
                break;
            }
            if (static_cast<long>(k) == last + 1) break;  // consecutive beats a later word start
        }
        std::size_t k = (ws != std::string_view::npos && !(static_cast<long>(found) == last + 1)) ? ws : found;
        if (k == std::string_view::npos) return -1.0;
        if (first == std::string_view::npos) first = k;
        score += 1.0;
        if (word_start(t, k)) score += 8.0;
        if (static_cast<long>(k) == last + 1) score += 5.0;
        else if (last >= 0) score -= 0.5 * std::min<double>(static_cast<double>(k - last - 1), 10.0);
        last = static_cast<long>(k);
        ti = k + 1;
    }
    score -= 0.2 * static_cast<double>(std::min<std::size_t>(first, 20));
    score -= 0.01 * static_cast<double>(t.size());  // prefer shorter labels on ties
    return std::max(score, 0.0);
}

std::vector<std::size_t> fuzzy_rank(std::string_view q, const std::vector<FuzzyEntry>& e, std::size_t limit) {
    std::vector<std::pair<double, std::size_t>> s;
    for (std::size_t i = 0; i < e.size(); ++i) {
        double v = fuzzy_score(q, e[i].text);
        if (v < 0) continue;
        s.push_back({v + std::log2(1.0 + e[i].usage), i});
    }
    std::stable_sort(s.begin(), s.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < s.size() && i < limit; ++i) out.push_back(s[i].second);
    return out;
}

}  // namespace anvil
