// Fuzzy command search for the Command Bar.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

/// Subsequence score of `query` in `text` (case-insensitive). Returns a
/// negative value if `query` is not a subsequence. Bonuses: word starts (+8),
/// consecutive matches (+5), early first match; penalty per skipped character.
double fuzzy_score(std::string_view query, std::string_view text);

struct FuzzyEntry {
    std::string id;
    std::string text;   // searchable text (label + keywords)
    double usage = 0;   // how often the user ran it (adds log2(1+usage))
};
/// Indices of entries sorted by descending score (only matches), at most `limit`.
std::vector<std::size_t> fuzzy_rank(std::string_view query, const std::vector<FuzzyEntry>& entries,
                                    std::size_t limit = 20);

}  // namespace anvil
