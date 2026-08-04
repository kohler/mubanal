// util.cc -- UTF-8 helpers

#include "mubanal.hh"

namespace mubanal {

char32_t u8_next(std::string_view s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    char32_t v;
    int extra;
    if (c < 0x80) {
        v = c;
        extra = 0;
    } else if ((c & 0xE0) == 0xC0) {
        v = c & 0x1F;
        extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
        v = c & 0x0F;
        extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
        v = c & 0x07;
        extra = 3;
    } else {
        v = c;
        extra = 0;
    }
    if (i + extra > s.size()) {
        i = s.size();
        return 0xFFFD; /* REPLACEMENT CHARACTER */
    }
    ++i;
    for (int k = 0; k < extra; ++k, ++i) {
        v = (v << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    }
    return v;
}

size_t u8_prev(std::string_view s, size_t i) {
    while (i > 0) {
        --i;
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            break;
        }
    }
    return i;
}

void u8_append(std::string& s, char32_t c) {
    unsigned v = unsigned(c);
    if (v < 0x80) {
        s.push_back(char(v));
    } else if (v < 0x800) {
        s.push_back(char(0xC0 | (v >> 6)));
        s.push_back(char(0x80 | (v & 0x3F)));
    } else if (v < 0x10000) {
        s.push_back(char(0xE0 | (v >> 12)));
        s.push_back(char(0x80 | ((v >> 6) & 0x3F)));
        s.push_back(char(0x80 | (v & 0x3F)));
    } else {
        s.push_back(char(0xF0 | (v >> 18)));
        s.push_back(char(0x80 | ((v >> 12) & 0x3F)));
        s.push_back(char(0x80 | ((v >> 6) & 0x3F)));
        s.push_back(char(0x80 | (v & 0x3F)));
    }
}

size_t u8_length(std::string_view s) {
    size_t n = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

bool eq_ascii_ci(std::string_view s, std::string_view ascii) {
    if (s.size() != ascii.size()) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(s[i]);
        unsigned char b = static_cast<unsigned char>(ascii[i]);
        if (a >= 'A' && a <= 'Z') {
            a += 32;
        }
        if (b >= 'A' && b <= 'Z') {
            b += 32;
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool in_ascii_ci(std::string_view s, std::initializer_list<std::string_view> asciis) {
    for (auto ascii : asciis) {
        if (eq_ascii_ci(s, ascii)) {
            return true;
        }
    }
    return false;
}

}  // namespace mubanal
