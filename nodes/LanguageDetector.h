#pragma once

// ---------------------------------------------------------------------------
// LanguageDetector.h
//
// Header-only port of DS4 LanguageDetector into the voice_runtime pipeline.
// Provides lightweight stop-word-based language detection with:
//
//   - Stop-words sets for: it, es, fr, de, en
//   - CJK/Hiragana/Katakana Unicode codepoint detection  (→ "zh")
//   - Preferred-language bias (PREF_LANG_BIAS = 3)
//   - detect(text, preferredLang) → ISO 639-1 code
//   - Dynamic stop-word configuration via set_stopwords()
//
// Namespace: voice_runtime
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace voice_runtime {

class LanguageDetector {
public:
    /// Bias added to the preferred language's score to break ties
    /// and resist false detections on short/ambiguous chunks.
    static constexpr int PREF_LANG_BIAS = 3;

    LanguageDetector() {
        // Default stop-words configuration.
        // Each set includes words that are DISTINCTIVE for that language.
        // Words shared across languages are intentionally excluded to avoid ambiguity.

        stop_words_["it"] = {
            // Unique Italian words (not present in Spanish/English stop-word sets)
            "il", "lo", "la", "i", "gli", "le",
            "un", "uno", "una",
            "di", "a", "da", "in", "con", "su", "per", "tra", "fra",
            "mi", "ti", "ci", "vi", "si", "ne",
            "che", "chi", "cui", "quale", "quali",
            "perche", "perch\xC3\xA9", "poiche", "poich\xC3\xA9",
            "ma", "o", "oppure", "invece",
            "stai", "ciao", "grazie", "sono", "sei",
            "ho", "hai", "ha", "abbiamo", "avete", "hanno",
            "e", "\xC3\xA8", "era", "erano", "sar\xC3\xA0",
            "dove", "quando", "come",
            "piu", "pi\xC3\xB9", "meno",
            "tutto", "molto", "bene", "male",
            "puo", "pu\xC3\xB2", "fare", "dire",
            "essere", "avere", "volere", "potere", "dovere",
            "questo", "questa", "quello", "quella",
            "noi", "voi", "loro",
            "mio", "mia", "miei", "mie", "tuo", "suo",
            "nostro", "vostro",
            "ogni", "alcuni", "alcune", "nessuno", "nessuna",
            "primo", "dopo", "prima", "poi",
            "sempre", "mai", "ancora", "gi\xC3\xA0",
            "qui", "qua", "l\xC3\xAC", "l\xC3\xA0",
            "solo", "anche", "pure",
            "percio", "perci\xC3\xB2",
            "dunque", "inoltre",
            "allora", "davvero",
            "forse", "certo", "sicuro"
        };

        stop_words_["es"] = {
            "el", "los", "las", "unos", "unas",
            "de", "por", "para",
            "como", "donde", "cuando",
            "porque", "que", "qu\xC3\xA9",
            "hola", "gracias",
            "estas", "est\xC3\xA1s", "c\xC3\xB3mo",
            "es", "son",
            "y", "pero",
            "tengo", "tienes", "tiene",
            "puedo", "puedes", "puede",
            "hacer", "decir",
            "ser", "estar", "haber",
            "este", "ese", "aquel",
            "nosotros", "vosotros", "ellos",
            "mi", "tu", "su",
            "nuestro", "vuestro",
            "cada", "algunos", "algunas",
            "primero", "despues", "despu\xC3\xA9s",
            "siempre", "nunca",
            "aqui", "aqu\xC3\xAD", "ahi", "ah\xC3\xAD",
            "solo", "tambien", "tambi\xC3\xA9n",
            "porque", "entonces",
            "ademas", "adem\xC3\xA1s",
            "entonces", "realmente",
            "quizas", "quiz\xC3\xA1s", "cierto"
        };

        stop_words_["fr"] = {
            "les", "des", "dans", "avec", "pour", "sur",
            "comme", "o\xC3\xB9", "quand",
            "pourquoi", "qu'est",
            "bonjour", "merci",
            "suis", "est", "sont",
            "et", "mais", "ne", "pas",
            "ai", "as", "avons", "avez", "ont",
            "peux", "peut",
            "faire", "dire",
            "\xC3\xAAtre", "avoir", "vouloir",
            "ce", "cet", "cette",
            "nous", "vous", "ils", "elles",
            "mon", "ton", "son",
            "notre", "votre",
            "chaque", "quelques",
            "premier", "apr\xC3\xA8s",
            "toujours", "jamais",
            "ici",
            "seulement", "aussi",
            "car", "donc",
            "alors", "vraiment",
            "peut-\xC3\xAAtre", "certain"
        };

        stop_words_["de"] = {
            "der", "die", "das", "ein", "eine", "eines", "einem", "einen",
            "von", "an", "zu", "mit", "f\xC3\xBCr", "auf",
            "wie", "wo", "wann",
            "warum", "dass",
            "ist", "sind",
            "hallo", "danke",
            "und", "aber", "nicht",
            "habe", "hast", "hat",
            "kann", "kannst", "k\xC3\xB6nnen",
            "machen", "sagen",
            "sein", "haben", "werden",
            "dieser", "diese", "dieses",
            "wir", "ihr", "sie",
            "mein", "dein", "sein",
            "unser", "euer",
            "jeder", "einige",
            "erste", "nach",
            "immer", "nie",
            "hier",
            "nur", "auch",
            "denn", "deshalb",
            "dann", "wirklich",
            "vielleicht", "gewi\xC3\x9F", "gewi\xC3\x9F"
        };

        stop_words_["en"] = {
            "the", "an", "of", "to", "for", "with", "on", "at", "by", "from",
            "about", "as", "how", "where", "when", "why",
            "that", "are", "am", "was", "were",
            "hello", "thanks", "thank", "you",
            "and", "but", "not",
            "have", "has", "had",
            "can", "could", "would", "should",
            "do", "does", "did",
            "make", "say",
            "be", "will", "been",
            "this", "that", "these", "those",
            "we", "you", "they",
            "my", "your", "his", "her", "its",
            "our", "their",
            "each", "some", "any",
            "first", "after",
            "always", "never",
            "here", "there",
            "only", "also",
            "because", "so",
            "then", "really",
            "maybe", "certain"
        };
    }

    /// Detects language of text, prioritising pref_lang if score is ambiguous.
    /// Returns ISO 639-1 code.
    std::string detect(const std::string& text, const std::string& pref_lang) const {
        if (contains_zh_ja_char(text)) {
            return "zh";
        }

        // Tokenize: extract alphanumeric words (including apostrophes and backticks)
        std::vector<std::string> words;
        std::string current_word;
        for (char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '\'' || c == '`') {
                current_word += static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            } else {
                if (!current_word.empty()) {
                    words.push_back(current_word);
                    current_word.clear();
                }
            }
        }
        if (!current_word.empty()) {
            words.push_back(current_word);
        }

        // Short text: trust the preferred language
        if (words.size() <= 1 || text.length() < 10) {
            if (stop_words_.count(pref_lang) > 0) {
                return pref_lang;
            }
            return "en";
        }

        // Score each language. The preferred language gets a bias so that
        // short/ambiguous chunks stay in the correct language.
        std::string best_lang = pref_lang;
        int max_score = -1;

        for (const auto& pair : stop_words_) {
            int score = 0;
            for (const auto& w : words) {
                if (pair.second.count(w)) {
                    score++;
                }
            }

            // Apply bias for the preferred language
            if (pair.first == pref_lang) {
                score += PREF_LANG_BIAS;
            }

            if (score > max_score) {
                max_score = score;
                best_lang = pair.first;
            }
        }

        if (max_score <= 0) {
            // No stop-words matched at all; fall back to preferred language
            if (stop_words_.count(pref_lang) > 0) {
                return pref_lang;
            }
            return "en";
        }

        return best_lang;
    }

    /// Allows dynamic configuration of stop-words for future extensions.
    void set_stopwords(const std::string& lang,
                       const std::unordered_set<std::string>& words) {
        stop_words_[lang] = words;
    }

private:
    /// Checks whether the text contains any CJK Unified Ideograph,
    /// Hiragana, or Katakana codepoint.
    bool contains_zh_ja_char(const std::string& text) const {
        for (size_t i = 0; i < text.length(); ) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            size_t char_len = 1;
            uint32_t codepoint = c;
            if (c < 0x80) {
                char_len = 1;
            } else if ((c & 0xE0) == 0xC0) {
                char_len = 2;
                if (i + 1 < text.length()) {
                    codepoint = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[i+1]) & 0x3F);
                }
            } else if ((c & 0xF0) == 0xE0) {
                char_len = 3;
                if (i + 2 < text.length()) {
                    codepoint = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(text[i+1]) & 0x3F) << 6) | (static_cast<unsigned char>(text[i+2]) & 0x3F);
                }
            } else if ((c & 0xF8) == 0xF0) {
                char_len = 4;
                if (i + 3 < text.length()) {
                    codepoint = ((c & 0x07) << 18) | ((static_cast<unsigned char>(text[i+1]) & 0x3F) << 12) | ((static_cast<unsigned char>(text[i+2]) & 0x3F) << 6) | (static_cast<unsigned char>(text[i+3]) & 0x3F);
                }
            }

            // Chinese/Japanese ranges:
            // CJK Unified Ideographs: 4E00-9FFF
            // Hiragana: 3040-309F
            // Katakana: 30A0-30FF
            if ((codepoint >= 0x4e00 && codepoint <= 0x9fff) ||
                (codepoint >= 0x3040 && codepoint <= 0x30ff)) {
                return true;
            }
            i += char_len;
        }
        return false;
    }

    std::unordered_map<std::string, std::unordered_set<std::string>> stop_words_;
};

} // namespace voice_runtime
