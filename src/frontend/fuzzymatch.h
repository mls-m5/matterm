#pragma once

// Forgiving search for the command palette (docs/TABS_PLAN.md section 8), in the
// spirit of VS Code's: the letters of the query must appear in the text in order,
// case does not matter, spaces in the query are ignored, and matches at the start
// of a word or in a run of consecutive letters score higher. If that fails, every
// space-separated word of the query may match on its own, in any order, at a lower
// score ("tab new" finds "New tab"). Pure C++17 on bytes: only ASCII letters fold
// case, UTF-8 sequences compare as they are.
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace frontend {

struct FuzzyMatch {
    bool matched = false;
    int score = 0;                    // higher is better
    std::vector<std::size_t> offsets; // byte offsets of the matched letters, ascending
};

namespace fuzzydetail {

inline char fold(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
inline bool isSpace(char c) { return c == ' ' || c == '\t'; }
inline bool isWordStart(std::string_view text, std::size_t i) {
    if (i == 0) return true;
    const char p = text[i - 1];
    return isSpace(p) || p == '-' || p == '_' || p == '/' || p == '.' ||
           (p >= 'a' && p <= 'z' && text[i] >= 'A' && text[i] <= 'Z');
}

// Best in-order match of `letters` in `text` (dynamic programming, O(n*m)).
inline FuzzyMatch matchLetters(std::string_view letters, std::string_view text) {
    FuzzyMatch result;
    const std::size_t m = letters.size(), n = text.size();
    if (m == 0) {
        result.matched = true;
        return result;
    }
    if (m > n || n > 512) return result; // long texts are not command names
    constexpr int kNone = -1000000;
    // best[i][j]: best score with letters[0..i] matched, letters[i] at text[j].
    std::vector<std::vector<int>> best(m, std::vector<int>(n, kNone));
    std::vector<std::vector<int>> from(m, std::vector<int>(n, -1));
    for (std::size_t i = 0; i < m; ++i) {
        for (std::size_t j = i; j < n; ++j) {
            if (fold(text[j]) != fold(letters[i])) continue;
            int gain = 1;
            if (isWordStart(text, j)) gain += 8;
            if (j == 0) gain += 4;
            if (text[j] == letters[i]) gain += 1; // exact case
            if (i == 0) {
                best[i][j] = gain - int(std::min<std::size_t>(j, 6)); // late starts cost a little
                continue;
            }
            for (std::size_t k = i - 1; k < j; ++k) {
                if (best[i - 1][k] == kNone) continue;
                int s = best[i - 1][k] + gain;
                if (k + 1 == j) s += 5;                       // consecutive letters
                else s -= int(std::min<std::size_t>(j - k - 1, 5)); // a gap costs a little
                if (s > best[i][j]) {
                    best[i][j] = s;
                    from[i][j] = int(k);
                }
            }
        }
    }
    int top = kNone;
    std::size_t at = 0;
    for (std::size_t j = m - 1; j < n; ++j) {
        if (best[m - 1][j] > top) {
            top = best[m - 1][j];
            at = j;
        }
    }
    if (top == kNone) return result;
    result.matched = true;
    result.score = top;
    result.offsets.assign(m, 0);
    for (std::size_t i = m; i-- > 0;) {
        result.offsets[i] = at;
        if (i > 0) at = std::size_t(from[i][at]);
    }
    return result;
}

} // namespace fuzzydetail

inline FuzzyMatch fuzzyMatch(std::string_view query, std::string_view text) {
    using namespace fuzzydetail;
    std::string letters;
    for (const char c : query) {
        if (!isSpace(c)) letters += c;
    }
    if (auto direct = matchLetters(letters, text); direct.matched) return direct;
    // Each word on its own, in any order.
    FuzzyMatch combined;
    combined.matched = true;
    std::size_t i = 0;
    int words = 0;
    while (i < query.size()) {
        while (i < query.size() && isSpace(query[i])) ++i;
        const std::size_t start = i;
        while (i < query.size() && !isSpace(query[i])) ++i;
        if (i == start) break;
        auto one = matchLetters(query.substr(start, i - start), text);
        if (!one.matched) return {};
        combined.score += one.score;
        combined.offsets.insert(combined.offsets.end(), one.offsets.begin(), one.offsets.end());
        ++words;
    }
    if (words < 2) return {}; // a single word that failed above cannot match
    combined.score -= 20; // below any in-order match of similar quality
    std::sort(combined.offsets.begin(), combined.offsets.end());
    combined.offsets.erase(std::unique(combined.offsets.begin(), combined.offsets.end()),
                           combined.offsets.end());
    return combined;
}

} // namespace frontend
