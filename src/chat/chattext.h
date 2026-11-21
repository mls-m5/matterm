#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// The single place that decides which text may travel through the chat. The
// server rejects what does not fit (it never repairs), the client sanitizes
// what it receives before it reaches a screen. Pure functions: no I/O.
namespace chat {

constexpr std::size_t kMaxTextChars = 200;
constexpr std::size_t kMinNameChars = 2;
constexpr std::size_t kMaxNameChars = 16;
constexpr std::size_t kMaxRun = 8; // Longest run of one repeated character.

// Strict UTF-8: no overlong forms, surrogates, values above U+10FFFF or
// truncated sequences. Returns false (and leaves `out` unspecified) otherwise.
bool decodeUtf8Strict(std::string_view bytes, std::u32string &out);
std::string encodeUtf8(const std::u32string &text);

// Characters a chat message may contain: printable ASCII and the Latin letters
// of Latin-1 and Latin Extended-A (å ä ö é ...), without the signs x and /.
bool isAllowedChar(char32_t c);
// Controls (C0, DEL, C1) and invisible or reordering format characters: bidi
// controls, zero-width characters, line separators, BOM, soft hyphen,
// variation selectors and tag characters. Never shown, always dropped.
bool isControlOrFormat(char32_t c);

enum class TextError {
    None,
    BadUtf8,
    Empty,
    TooLong,
    Forbidden, // A character that is not allowed.
    OnlySpaces,
    EdgeSpace, // Leading or trailing space where not allowed.
    Repeated,  // A run of more than kMaxRun identical characters.
};

struct TextRules {
    std::size_t maxChars = kMaxTextChars;
    bool allowEdgeSpaces = false; // A draft may have them; a message may not.
    bool allowEmpty = false;
};

struct TextResult {
    TextError error = TextError::None;
    std::size_t chars = 0; // Code points, when the text was valid UTF-8.
    bool ok() const noexcept {
        return error == TextError::None;
    }
};

// A message: 1..kMaxTextChars allowed characters, no edge spaces.
TextResult checkMessage(std::string_view utf8);
// A draft: like a message, but may be empty and may have edge spaces.
TextResult checkDraft(std::string_view utf8);
TextResult checkText(std::string_view utf8, const TextRules &rules);

// Removes leading and trailing spaces (the lock step of a draft).
std::string trimSpaces(std::string_view text);

enum class NameError { None, Length, Characters, Reserved };
// 2..16 of [A-Za-z0-9_-], not one of the reserved names (server, admin, ...).
NameError checkName(std::string_view name);
// The form used to compare names: ASCII lower case.
std::string nameKey(std::string_view name);
// What a name looks like: lower case, 0/o, 1/i/l, 5/s, 3/e, rn/m and vv/w
// folded together, and `_` and `-` dropped. Two names with the same skeleton
// count as the same name, and the reserved names are matched by it too.
std::string nameSkeleton(std::string_view name);

// For text received from the network: always yields valid UTF-8 of at most
// `maxChars` code points with no control or format character. Invalid UTF-8 and
// characters that are not allowed become U+FFFD; controls and format characters
// are dropped (so no escape sequence can form). Idempotent.
std::string sanitizeIncoming(std::string_view bytes,
                             std::size_t maxChars = kMaxTextChars);

} // namespace chat
