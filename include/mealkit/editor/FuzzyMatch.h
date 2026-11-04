#pragma once

#include <algorithm>
#include <string_view>
#include <vector>

// Command palette matching: letters in order with gaps allowed, ignoring case. A Korean initial consonant in the query also matches
// a syllable starting with it ("ㅅㅈ" finds "씬 저장"-style names by their first consonants). Header only so unit tests can use it without the editor.
namespace mealkit::editor {

inline std::vector<char32_t> decodeUtf8Lower(std::string_view s) {
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        char32_t cp = length == 1 ? c : c & (0xFF >> (length + 1));
        for (size_t k = 1; k < length && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        if (cp >= U'A' && cp <= U'Z') cp += 32;
        out.push_back(cp);
        i += length;
    }
    return out;
}

inline bool fuzzyCharMatches(char32_t query, char32_t text) {
    if (query == text) return true;
    constexpr char32_t kInitials[19] = {0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, 0x3145,
                                        0x3146, 0x3147, 0x3148, 0x3149, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E};
    if (text < 0xAC00 || text > 0xD7A3) return false;
    return kInitials[(text - 0xAC00) / 588] == query;
}

// -1 = no match. Higher is better: runs of letters, a match at the start or after a separator, and shorter text score more. An empty query scores 0.
inline int fuzzyScore(std::string_view textUtf8, std::string_view queryUtf8) {
    std::vector<char32_t> query = decodeUtf8Lower(queryUtf8);
    std::erase(query, U' ');
    if (query.empty()) return 0;
    const std::vector<char32_t> text = decodeUtf8Lower(textUtf8);
    int best = -1;
    for (size_t start = 0; start < text.size(); ++start) {
        if (!fuzzyCharMatches(query[0], text[start])) continue;
        int score = 0;
        size_t q = 0, previous = start;
        for (size_t t = start; t < text.size() && q < query.size(); ++t) {
            if (!fuzzyCharMatches(query[q], text[t])) continue;
            score += 1;
            if (q > 0 && t == previous + 1) score += 5;
            if (t == 0) score += 10;
            else if (text[t - 1] == U' ' || text[t - 1] == U'/' || text[t - 1] == U'_' || text[t - 1] == U'.') score += 3;
            previous = t;
            ++q;
        }
        if (q == query.size()) best = std::max(best, score);
    }
    if (best < 0) return -1;
    return std::max(0, best * 4 - static_cast<int>(text.size()) / 8);
}

} // namespace mealkit::editor
