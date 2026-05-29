#pragma once

// ---------------------------------------------------------------------------
// TextProcessing.h
//
// Header-only port of DS4 TextProcessing utilities into the voice_runtime
// pipeline.  All original functionality is preserved:
//
//   - utf8_to_wstring()   — manual UTF-8 → wchar_t conversion
//   - wstring_to_utf8()   — manual wchar_t → UTF-8 conversion
//   - clean_phonemes_cpp()— IPA phoneme cleaning for Kokoro TTS
//   - latex_to_speech()   — full LaTeX→spoken-text conversion (80+ commands)
//
// Everything lives in  namespace voice_runtime::text_processing.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace voice_runtime {
namespace text_processing {

// ---- UTF-8 ↔ wstring helpers -----------------------------------------------

inline std::wstring utf8_to_wstring(const std::string& str) {
    std::wstring wstr;
    for (size_t i = 0; i < str.length(); ) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        size_t char_len = 1;
        uint32_t codepoint = c;
        if (c < 0x80) {
            char_len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            char_len = 2;
            if (i + 1 < str.length()) {
                codepoint = ((c & 0x1F) << 6) | (static_cast<unsigned char>(str[i+1]) & 0x3F);
            }
        } else if ((c & 0xF0) == 0xE0) {
            char_len = 3;
            if (i + 2 < str.length()) {
                codepoint = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(str[i+1]) & 0x3F) << 6) | (static_cast<unsigned char>(str[i+2]) & 0x3F);
            }
        } else if ((c & 0xF8) == 0xF0) {
            char_len = 4;
            if (i + 3 < str.length()) {
                codepoint = ((c & 0x07) << 18) | ((static_cast<unsigned char>(str[i+1]) & 0x3F) << 12) | ((static_cast<unsigned char>(str[i+2]) & 0x3F) << 6) | (static_cast<unsigned char>(str[i+3]) & 0x3F);
            }
        }
        wstr.push_back(static_cast<wchar_t>(codepoint));
        i += char_len;
    }
    return wstr;
}

inline std::string wstring_to_utf8(const std::wstring& wstr) {
    std::string str;
    for (wchar_t wc : wstr) {
        uint32_t cp = static_cast<uint32_t>(wc);
        if (cp < 0x80) {
            str.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            str.push_back(static_cast<char>((cp >> 6) | 0xC0));
            str.push_back(static_cast<char>((cp & 0x3F) | 0x80));
        } else if (cp < 0x10000) {
            str.push_back(static_cast<char>((cp >> 12) | 0xE0));
            str.push_back(static_cast<char>(((cp >> 6) & 0x3F) | 0x80));
            str.push_back(static_cast<char>((cp & 0x3F) | 0x80));
        } else {
            str.push_back(static_cast<char>((cp >> 18) | 0xF0));
            str.push_back(static_cast<char>(((cp >> 12) & 0x3F) | 0x80));
            str.push_back(static_cast<char>(((cp >> 6) & 0x3F) | 0x80));
            str.push_back(static_cast<char>((cp & 0x3F) | 0x80));
        }
    }
    return str;
}

// ---- IPA phoneme cleaning for Kokoro TTS -----------------------------------

inline std::wstring clean_phonemes_cpp(const std::wstring& wph) {
    std::wstring cleaned;
    static const std::unordered_set<wchar_t> allowed = {
        L';', L':', L',', L'.', L'!', L'?', L'\u2014', L'\u2026', L'"', L'(', L')', L'\u201C', L'\u201D', L' ', L'\t', L'\n', L'\r',
        L'\u0303', L'\u02A3', L'\u02A5', L'\u02A6', L'\u02A8', L'\u1D5D', L'\uAB67', L'A', L'I', L'O', L'S', L'T', L'W', L'Y', L'\u1D4A',
        L'a', L'b', L'c', L'd', L'e', L'f', L'g', L'h', L'i', L'j', L'k', L'l', L'm', L'n', L'o', L'p', L'q', L'r', L's', L't', L'u', L'v', L'w', L'x', L'y', L'z',
        L'\u0251', L'\u0250', L'\u0252', L'\u00E6', L'\u03B2', L'\u0254', L'\u0255', L'\u00E7', L'\u0256', L'\u00F0', L'\u02A4', L'\u0259', L'\u025A', L'\u025B', L'\u025C', L'\u025F', L'\u0261', L'\u0265', L'\u0268', L'\u026A', L'\u029D', L'\u026F', L'\u0270', L'\u014B', L'\u0273', L'\u0272', L'\u0274',
        L'\u00F8', L'\u0278', L'\u03B8', L'\u0153', L'\u0279', L'\u027E', L'\u027B', L'\u0281', L'\u027D', L'\u0282', L'\u0283', L'\u0288', L'\u02A7', L'\u028A', L'\u028B', L'\u028C', L'\u0263', L'\u0264', L'\u03C7', L'\u028E', L'\u0292', L'\u0294', L'\u02C8', L'\u02CC', L'\u02D0', L'\u02B0', L'\u02B2',
        L'\u2193', L'\u2192', L'\u2197', L'\u2198'
    };

    for (wchar_t wc : wph) {
        if (wc == L'\u0361') {
            continue;
        } else if (wc == L'\u2016') {
            cleaned.push_back(L'.');
        } else if (wc == L'|') {
            cleaned.push_back(L',');
        } else if (allowed.count(wc)) {
            cleaned.push_back(wc);
        }
    }
    return cleaned;
}

// ---- LaTeX-to-speech -------------------------------------------------------

namespace detail {

// Map of LaTeX commands to spoken equivalents
inline const std::unordered_map<std::string, std::string>& latex_map() {
    static const std::unordered_map<std::string, std::string> m = {
        // Greek lowercase
        {"\\alpha", "alpha"},
        {"\\beta", "beta"},
        {"\\gamma", "gamma"},
        {"\\delta", "delta"},
        {"\\epsilon", "epsilon"},
        {"\\varepsilon", "epsilon"},
        {"\\zeta", "zeta"},
        {"\\eta", "eta"},
        {"\\theta", "theta"},
        {"\\vartheta", "theta"},
        {"\\iota", "iota"},
        {"\\kappa", "kappa"},
        {"\\lambda", "lambda"},
        {"\\mu", "mu"},
        {"\\nu", "nu"},
        {"\\xi", "xi"},
        {"\\pi", "pi"},
        {"\\rho", "rho"},
        {"\\sigma", "sigma"},
        {"\\tau", "tau"},
        {"\\upsilon", "upsilon"},
        {"\\phi", "phi"},
        {"\\varphi", "phi"},
        {"\\chi", "chi"},
        {"\\psi", "psi"},
        {"\\omega", "omega"},
        // Greek uppercase
        {"\\Gamma", "Gamma"},
        {"\\Delta", "Delta"},
        {"\\Theta", "Theta"},
        {"\\Lambda", "Lambda"},
        {"\\Xi", "Xi"},
        {"\\Pi", "Pi"},
        {"\\Sigma", "Sigma"},
        {"\\Phi", "Phi"},
        {"\\Psi", "Psi"},
        {"\\Omega", "Omega"},
        // Math operators
        {"\\nabla", "nabla"},
        {"\\partial", "partial"},
        {"\\infty", "infinity"},
        {"\\sum", "sum"},
        {"\\prod", "product"},
        {"\\int", "integral"},
        {"\\iint", "double integral"},
        {"\\iiint", "triple integral"},
        {"\\oint", "contour integral"},
        {"\\cdot", "dot"},
        {"\\times", "cross"},
        {"\\div", "divided by"},
        {"\\pm", "plus or minus"},
        {"\\mp", "minus or plus"},
        {"\\neq", "not equal"},
        {"\\approx", "approximately"},
        {"\\propto", "proportional to"},
        {"\\sim", "similar to"},
        {"\\cong", "congruent to"},
        {"\\equiv", "equivalent"},
        {"\\leq", "less than or equal"},
        {"\\geq", "greater than or equal"},
        {"\\ll", "much less"},
        {"\\gg", "much greater"},
        {"\\subset", "subset"},
        {"\\supset", "superset"},
        {"\\subseteq", "subset or equal"},
        {"\\supseteq", "superset or equal"},
        {"\\cup", "union"},
        {"\\cap", "intersection"},
        {"\\in", "in"},
        {"\\notin", "not in"},
        {"\\forall", "for all"},
        {"\\exists", "there exists"},
        {"\\emptyset", "empty set"},
        {"\\null", "null"},
        {"\\to", "to"},
        {"\\rightarrow", "to"},
        {"\\Rightarrow", "implies"},
        {"\\leftarrow", "from"},
        {"\\Leftarrow", "is implied by"},
        {"\\leftrightarrow", "corresponds to"},
        {"\\mapsto", "maps to"},
        {"\\implies", "implies"},
        {"\\because", "because"},
        {"\\therefore", "therefore"},
        // Delimiters
        {"\\lvert", "|"},
        {"\\rvert", "|"},
        {"\\lVert", "|"},
        {"\\rVert", "|"},
        // Misc
        {"\\hbar", "h bar"},
        {"\\dag", "dagger"},
        {"\\ddag", "double dagger"},
        {"\\degree", "degrees"},
        {"\\prime", "prime"},
        {"\\angle", "angle"},
        {"\\triangle", "triangle"},
        {"\\square", "square"},
    };
    return m;
}

} // namespace detail

inline std::string latex_to_speech(const std::string& text) {
    std::string result = text;

    // ---- Process \frac{a}{b} → "a over b" ---------------------------------
    {
        std::string buffer;
        size_t i = 0;
        while (i < result.size()) {
            if (result.compare(i, 5, "\\frac") == 0) {
                i += 5;
                // First argument {numerator}
                if (i < result.size() && result[i] == '{') {
                    i++; // skip '{'
                    int depth = 1;
                    size_t num_start = i;
                    size_t num_end = i;
                    while (num_end < result.size() && depth > 0) {
                        if (result[num_end] == '{') depth++;
                        else if (result[num_end] == '}') depth--;
                        if (depth > 0) num_end++;
                    }
                    buffer += result.substr(num_start, num_end - num_start);
                    i = num_end + 1; // skip '}'
                }
                // Second argument {denominator}
                if (i < result.size() && result[i] == '{') {
                    i++; // skip '{'
                    int depth = 1;
                    size_t den_start = i;
                    size_t den_end = i;
                    while (den_end < result.size() && depth > 0) {
                        if (result[den_end] == '{') depth++;
                        else if (result[den_end] == '}') depth--;
                        if (depth > 0) den_end++;
                    }
                    buffer += " over ";
                    buffer += result.substr(den_start, den_end - den_start);
                    i = den_end + 1; // skip '}'
                }
                continue;
            }
            buffer += result[i];
            i++;
        }
        result = buffer;
    }

    // ---- Process \sqrt{...} → "square root of ..." ------------------------
    {
        std::string buffer;
        size_t i = 0;
        while (i < result.size()) {
            if (result.compare(i, 5, "\\sqrt") == 0) {
                buffer += "square root of ";
                i += 5;
                // Optional [n] root degree
                if (i < result.size() && result[i] == '[') {
                    i++; // skip '['
                    size_t n_start = i;
                    size_t n_end = i;
                    while (n_end < result.size() && result[n_end] != ']') n_end++;
                    buffer += result.substr(n_start, n_end - n_start);
                    buffer += " root of ";
                    i = n_end + 1; // skip ']'
                }
                // Argument {content}
                if (i < result.size() && result[i] == '{') {
                    i++; // skip '{'
                    int depth = 1;
                    size_t arg_start = i;
                    size_t arg_end = i;
                    while (arg_end < result.size() && depth > 0) {
                        if (result[arg_end] == '{') depth++;
                        else if (result[arg_end] == '}') depth--;
                        if (depth > 0) arg_end++;
                    }
                    buffer += result.substr(arg_start, arg_end - arg_start);
                    i = arg_end + 1; // skip '}'
                }
                continue;
            }
            buffer += result[i];
            i++;
        }
        result = buffer;
    }

    // ---- Remove \left/\right when followed by a known delimiter ------------
    {
        std::string buffer;
        size_t i = 0;
        auto is_delim = [](char c) -> bool {
            return c == '(' || c == ')' || c == '[' || c == ']' ||
                   c == '{' || c == '}' || c == '|' || c == '.' ||
                   c == '/' || c == '<' || c == '>' || c == '\\' ||
                   c == '"' || c == '\'';
        };
        while (i < result.size()) {
            // Check for \left followed by a delimiter
            if (result.compare(i, 5, "\\left") == 0 && i + 5 < result.size()) {
                size_t j = i + 5;
                while (j < result.size() && result[j] == ' ') j++;
                if (j < result.size() && is_delim(result[j])) {
                    if (result[j] == '\\' && j + 1 < result.size()) j++; // skip escaped brace
                    i = j + 1;
                    continue;
                }
            }
            // Check for \right followed by a delimiter
            if (result.compare(i, 6, "\\right") == 0 && i + 6 < result.size()) {
                size_t j = i + 6;
                while (j < result.size() && result[j] == ' ') j++;
                if (j < result.size() && is_delim(result[j])) {
                    if (result[j] == '\\' && j + 1 < result.size()) j++;
                    i = j + 1;
                    continue;
                }
            }
            buffer += result[i];
            i++;
        }
        result = buffer;
    }

    // ---- Remove \displaystyle, \textstyle, etc. ----------------------------
    {
        const char* style_cmds[] = {"\\displaystyle", "\\textstyle", "\\scriptstyle", "\\scriptscriptstyle"};
        for (const char* cmd : style_cmds) {
            size_t pos = 0;
            while ((pos = result.find(cmd, pos)) != std::string::npos) {
                result.erase(pos, std::strlen(cmd));
            }
        }
    }

    // ---- Remove \tag{...} and \label{...} ----------------------------------
    {
        const char* tag_cmds[] = {"\\tag", "\\label"};
        for (const char* cmd : tag_cmds) {
            size_t pos = 0;
            size_t cmd_len = std::strlen(cmd);
            while ((pos = result.find(cmd, pos)) != std::string::npos) {
                if (pos + cmd_len < result.size() && result[pos + cmd_len] == '{') {
                    size_t end = pos + cmd_len + 1;
                    int depth = 1;
                    while (end < result.size() && depth > 0) {
                        if (result[end] == '{') depth++;
                        else if (result[end] == '}') depth--;
                        end++;
                    }
                    result.erase(pos, (end + 1) - pos);
                } else {
                    pos += cmd_len;
                }
            }
        }
    }

    // ---- Replace accent commands: \hat{x} → "x hat", etc. -----------------
    {
        struct AccentCmd { const char* cmd; const char* spoken; };
        const AccentCmd accent_cmds[] = {
            {"\\hat", " hat "},
            {"\\tilde", " tilde "},
            {"\\bar", " bar "},
            {"\\dot", " dot "},
            {"\\ddot", " double dot "},
            {"\\vec", " vec "},
            {"\\widehat", " wide hat "},
            {"\\widetilde", " wide tilde "},
        };
        for (auto& ac : accent_cmds) {
            size_t cmd_len = std::strlen(ac.cmd);
            size_t pos = 0;
            while ((pos = result.find(ac.cmd, pos)) != std::string::npos) {
                size_t next = pos + cmd_len;
                if (next < result.size() && result[next] == '{') {
                    // \hat{...}
                    size_t start = next + 1;
                    int depth = 1;
                    size_t end = start;
                    while (end < result.size() && depth > 0) {
                        if (result[end] == '{') depth++;
                        else if (result[end] == '}') depth--;
                        if (depth > 0) end++;
                    }
                    std::string inner = result.substr(start, end - start);
                    result.replace(pos, (end + 1) - pos, inner + ac.spoken);
                    pos = pos + inner.size() + std::strlen(ac.spoken);
                } else if (next < result.size() && result[next] != '{' && result[next] != '\\') {
                    // \hat x  (single character argument)
                    std::string inner(1, result[next]);
                    result.replace(pos, cmd_len + 1, inner + ac.spoken);
                    pos = pos + inner.size() + std::strlen(ac.spoken);
                } else {
                    pos = next;
                }
            }
        }
    }

    // ---- Process subscript _{} and superscript ^{} -------------------------
    {
        std::string buffer;
        size_t i = 0;
        while (i < result.size()) {
            if (result[i] == '_' && i + 1 < result.size()) {
                if (result[i+1] == '{') {
                    i += 2; // skip _{
                    int depth = 1;
                    size_t start = i;
                    size_t end = i;
                    while (end < result.size() && depth > 0) {
                        if (result[end] == '{') depth++;
                        else if (result[end] == '}') depth--;
                        if (depth > 0) end++;
                    }
                    buffer += " sub ";
                    buffer += result.substr(start, end - start);
                    i = end + 1;
                } else {
                    buffer += " sub ";
                    buffer += result[i+1];
                    i += 2;
                }
                continue;
            }
            if (result[i] == '^' && i + 1 < result.size()) {
                if (result[i+1] == '{') {
                    i += 2; // skip ^{
                    int depth = 1;
                    size_t start = i;
                    size_t end = i;
                    while (end < result.size() && depth > 0) {
                        if (result[end] == '{') depth++;
                        else if (result[end] == '}') depth--;
                        if (depth > 0) end++;
                    }
                    buffer += " sup ";
                    buffer += result.substr(start, end - start);
                    i = end + 1;
                } else {
                    buffer += " sup ";
                    buffer += result[i+1];
                    i += 2;
                }
                continue;
            }
            buffer += result[i];
            i++;
        }
        result = buffer;
    }

    // ---- Remove math-style commands (\mathbb{}, \textbf{}, etc.) -----------
    {
        const char* style_cmds2[] = {"\\mathbb", "\\mathbf", "\\mathcal", "\\mathit", "\\mathrm", "\\mathsf", "\\mathtt", "\\textbf", "\\textit"};
        for (const char* cmd : style_cmds2) {
            size_t cmd_len = std::strlen(cmd);
            size_t pos = 0;
            while ((pos = result.find(cmd, pos)) != std::string::npos) {
                if (pos + cmd_len < result.size() && result[pos + cmd_len] == '{') {
                    size_t start = pos + cmd_len + 1;
                    int depth = 1;
                    size_t end = start;
                    while (end < result.size() && depth > 0) {
                        if (result[end] == '{') depth++;
                        else if (result[end] == '}') depth--;
                        if (depth > 0) end++;
                    }
                    std::string inner = result.substr(start, end - start);
                    result.replace(pos, (end + 1) - pos, inner);
                    pos = pos + inner.size();
                } else {
                    pos += cmd_len;
                }
            }
        }
    }

    // ---- Replace known LaTeX commands with spoken equivalents ---------------
    // Sort by length descending to match \varepsilon before \epsilon etc.
    {
        const auto& lm = detail::latex_map();
        std::vector<std::pair<std::string, std::string>> sorted(lm.begin(), lm.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });

        for (auto& kv : sorted) {
            size_t pos = 0;
            while ((pos = result.find(kv.first, pos)) != std::string::npos) {
                result.replace(pos, kv.first.size(), kv.second);
                pos += kv.second.size();
            }
        }
    }

    // ---- Remove remaining isolated backslashes -----------------------------
    {
        std::string buffer;
        size_t i = 0;
        while (i < result.size()) {
            if (result[i] == '\\' && i + 1 < result.size() && result[i+1] == ' ') {
                buffer += result[i];
                i++;
                continue;
            }
            if (result[i] == '\\' && (i + 1 >= result.size() || result[i+1] == ' ' || result[i+1] == ',' || result[i+1] == '.' || result[i+1] == ';' || result[i+1] == ')' || result[i+1] == '(')) {
                // orphan backslash, skip it
                i++;
                continue;
            }
            buffer += result[i];
            i++;
        }
        result = buffer;
    }

    // ---- Strip equation delimiters ($$...$$, \[...\]) ----------------------
    {
        size_t pos = 0;
        while ((pos = result.find("$$", pos)) != std::string::npos) {
            result.erase(pos, 2);
        }
        pos = 0;
        while ((pos = result.find("\\[", pos)) != std::string::npos) {
            result.erase(pos, 2);
        }
        pos = 0;
        while ((pos = result.find("\\]", pos)) != std::string::npos) {
            result.erase(pos, 2);
        }
    }

    return result;
}

} // namespace text_processing
} // namespace voice_runtime
