#ifndef OPENRPC_CONFIG_H
#define OPENRPC_CONFIG_H
#include "RpcCore.h"
namespace OpenRpc {
class ConfigCursor {
public:
    Span text; size_t pos; bool valid;
    explicit ConfigCursor(Span s) : text(s), pos(0), valid(s.data && s.size <= MaxConfig) {
        if (valid && s.size >= 3 && (unsigned char)s.data[0] == 0xef &&
            (unsigned char)s.data[1] == 0xbb && (unsigned char)s.data[2] == 0xbf) pos = 3;
    }
    bool Skip() {
        while (valid && pos < text.size) {
            char c = text.data[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++pos; continue; }
            if (c == '/' && pos + 1 < text.size && text.data[pos+1] == '/') {
                pos += 2; while (pos < text.size && text.data[pos] != '\n') ++pos; continue;
            }
            if (c == '/' && pos + 1 < text.size && text.data[pos+1] == '*') {
                pos += 2;
                while (pos + 1 < text.size && !(text.data[pos] == '*' && text.data[pos+1] == '/')) ++pos;
                if (pos + 1 >= text.size) { valid = false; return false; }
                pos += 2; continue;
            }
            return true;
        }
        return false;
    }
    bool Quoted(Span& out) {
        if (!Skip() || text.data[pos] != '"') return false;
        size_t start = ++pos;
        while (pos < text.size) {
            if (text.data[pos] == '"') { out = Span(text.data + start, pos++ - start); return true; }
            if (text.data[pos] == '\\' && pos + 1 < text.size && text.data[pos+1] == '"') pos += 2;
            else ++pos;
        }
        valid = false; return false;
    }
};
inline bool ConfigSection(Span file, const char* name, Span& out) {
    ConfigCursor c(file);
    while (c.Skip()) {
        Span key;
        if (!c.Quoted(key) || !c.Skip() || file.data[c.pos++] != '{') return false;
        size_t start = c.pos; unsigned depth = 1;
        while (depth && c.Skip()) {
            char ch = file.data[c.pos];
            if (ch == '"') { Span ignored; if (!c.Quoted(ignored)) return false; }
            else {
                ++c.pos;
                if (ch == '{' && ++depth > 8) return false;
                if (ch == '}') --depth;
            }
        }
        if (depth || !c.valid) return false;
        if (Equal(key, name)) { out = Span(file.data + start, c.pos - start - 1); return true; }
    }
    return false;
}
inline bool ConfigValue(Span section, const char* name, Span& out) {
    ConfigCursor c(section);
    while (c.Skip()) {
        Span key, value;
        if (!c.Quoted(key) || !c.Skip() || section.data[c.pos++] != '=' || !c.Skip()) return false;
        if (section.data[c.pos] == '"') { if (!c.Quoted(value)) return false; }
        else {
            size_t start = c.pos;
            while (c.pos < section.size && section.data[c.pos] != '\r' && section.data[c.pos] != '\n' && section.data[c.pos] != '}' &&
                !(section.data[c.pos] == '/' && c.pos + 1 < section.size && (section.data[c.pos+1] == '/' || section.data[c.pos+1] == '*'))) ++c.pos;
            size_t end = c.pos;
            while (end > start && (section.data[end-1] == ' ' || section.data[end-1] == '\t')) --end;
            value = Span(section.data + start, end - start);
        }
        if (Equal(key, name)) { out = value; return true; }
    }
    return false;
}
inline bool ConfigString(Span section, const char* name, char* out, size_t capacity) {
    Span value;
    if (!out || !capacity || !ConfigValue(section, name, value) || value.size >= capacity) return false;
    for (size_t i = 0; i < value.size; ++i) if (value.data[i] == '\r' || value.data[i] == '\n' || !value.data[i]) return false;
    memcpy(out, value.data, value.size); out[value.size] = 0; return true;
}
inline bool ConfigBool(Span section, const char* name, bool& out) {
    Span value;
    if (!ConfigValue(section, name, value)) return false;
    if (Equal(value, "true") || Equal(value, "1")) { out = true; return true; }
    if (Equal(value, "false") || Equal(value, "0")) { out = false; return true; }
    return false;
}
}
#endif
