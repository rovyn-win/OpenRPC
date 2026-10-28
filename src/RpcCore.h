#ifndef OPENRPC_CORE_H
#define OPENRPC_CORE_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <float.h>

namespace OpenRpc {
#ifdef _MSC_VER
typedef unsigned __int64 U64;
#else
typedef unsigned long long U64;
#endif
typedef unsigned int U32;
enum { MaxArgs = 37, MaxRequest = 65536, MaxArgumentBytes = 8192,
       MaxOwnedBytes = 32768, MaxResult = 4096, MaxPending = 32, MaxConfig = 65536 };

struct Span {
    const char* data;
    size_t size;
    Span() : data(0), size(0) {}
    Span(const char* p, size_t n) : data(p), size(n) {}
};

inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : c; }
inline bool Equal(Span a, const char* b) {
    size_t n = strlen(b);
    if (a.size != n) return false;
    for (size_t i = 0; i < n; ++i) if (Lower(a.data[i]) != Lower(b[i])) return false;
    return true;
}
inline bool BoundedLength(const char* p, size_t limit, size_t& n) {
    if (!p) return false;
    for (n = 0; n <= limit; ++n) if (!p[n]) return true;
    return false;
}
inline int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = Lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
inline bool Unsigned(Span s, U64 maximum, U64& value, unsigned base = 0) {
    if (!s.size || s.size > 65 || !s.data) return false;
    size_t i = 0;
    if (s.data[i] == '+') { if (++i == s.size) return false; }
    if (s.data[i] == '-') return false;
    if (!base) {
        base = 10;
        if (s.data[i] == '0' && i + 1 < s.size) {
            if (Lower(s.data[i + 1]) == 'x') { base = 16; i += 2; }
            else base = 8;
        }
    } else if (base == 16 && i + 1 < s.size && s.data[i] == '0' && Lower(s.data[i+1]) == 'x') i += 2;
    if (i == s.size) return false;
    value = 0;
    for (; i < s.size; ++i) {
        int digit = Hex(s.data[i]);
        if (digit < 0 || unsigned(digit) >= base || U64(digit) > maximum || value > (maximum - digit) / base) return false;
        value = value * base + digit;
    }
    return true;
}
inline bool Integer(Span s, unsigned bits, U64& value) {
    bool negative = s.size && s.data[0] == '-';
    if (negative) { ++s.data; --s.size; if (s.size && s.data[0] == '+') return false; }
    U64 maximum = bits == 32 ? U64(0xffffffffU) : ~U64(0);
    if (negative) maximum = bits == 32 ? U64(0x80000000U) : U64(1) << 63;
    if (!Unsigned(s, maximum, value)) return false;
    if (negative) value = U64(0) - value;
    if (bits == 32) {
        value &= 0xffffffffU;
        if (value & 0x80000000U) value |= U64(0xffffffffU) << 32;
    }
    return true;
}
inline bool FloatValue(Span s, float& value) {
    if (!s.data || !s.size || s.size >= 96) return false;
    char text[96];
    memcpy(text, s.data, s.size); text[s.size] = 0;
    bool digit = false;
    for (size_t i = 0; i < s.size; ++i) {
        char c = text[i];
        if (c >= '0' && c <= '9') digit = true;
        else if (c != '+' && c != '-' && c != '.' && c != 'e' && c != 'E') return false;
    }
    if (!digit) return false;
    errno = 0;
    char* end = 0;
    double parsed = strtod(text, &end);
    if (end != text + s.size || errno == ERANGE || parsed != parsed || parsed > FLT_MAX || parsed < -FLT_MAX) return false;
    value = float(parsed);
    return true;
}

struct Allocator {
    void* context;
    void* (*allocate)(void*, size_t);
    void (*release)(void*, void*);
};
inline void* AllocateDefault(void*, size_t n) { return malloc(n); }
inline void ReleaseDefault(void*, void* p) { free(p); }
inline Allocator DefaultAllocator() {
    Allocator a = {0, AllocateDefault, ReleaseDefault}; return a;
}

struct Argument {
    unsigned type;
    unsigned char* bytes;
    size_t length;
};

class Request {
    Request(const Request&);
    Request& operator=(const Request&);
public:
    Allocator allocator;
    U64 params[MaxArgs];
    double fpr[13];
    Argument args[MaxArgs];
    U32 target, type, returnCount, ordinal;
    unsigned count, floatCount;
    bool system, vm;
    char module[260];
    explicit Request(Allocator a = DefaultAllocator()) : allocator(a) {
        memset(args, 0, sizeof(args)); Reset();
    }
    ~Request() { Reset(); }
    void Reset() {
        for (unsigned i = 0; i < MaxArgs; ++i) if (args[i].bytes) allocator.release(allocator.context, args[i].bytes);
        memset(args, 0, sizeof(args)); memset(params, 0, sizeof(params)); memset(fpr, 0, sizeof(fpr));
        target = type = returnCount = ordinal = count = floatCount = 0;
        system = vm = false; module[0] = 0;
    }
};

struct Field { Span name, value; bool hasValue; };
struct Fields {
    Field items[16];
    unsigned count;
    Fields() : count(0) {}
    const Field* Find(const char* name) const {
        for (unsigned i = 0; i < count; ++i) if (Equal(items[i].name, name)) return &items[i];
        return 0;
    }
};
inline bool ParseFields(Span command, Fields& out) {
    out.count = 0;
    if (!command.data || command.size > MaxRequest) return false;
    for (size_t k = 0; k < command.size; ++k) if (!command.data[k] || command.data[k] == '\r' || command.data[k] == '\n') return false;
    size_t i = 0;
    while (i < command.size && command.data[i] != ' ' && command.data[i] != '\t') ++i;
    if (!Equal(Span(command.data, i), "consolefeatures")) return false;
    while (i < command.size) {
        while (i < command.size && (command.data[i] == ' ' || command.data[i] == '\t')) ++i;
        if (i == command.size) break;
        if (out.count == 16) return false;
        Field f; size_t start = i;
        while (i < command.size && command.data[i] != '=' && command.data[i] != ' ' && command.data[i] != '\t') ++i;
        f.name = Span(command.data + start, i - start);
        if (!f.name.size) return false;
        for (unsigned n = 0; n < out.count; ++n) {
            Span a = out.items[n].name;
            if (a.size != f.name.size) continue;
            bool same = true;
            for (size_t k = 0; k < a.size; ++k) if (Lower(a.data[k]) != Lower(f.name.data[k])) same = false;
            if (same) return false;
        }
        f.hasValue = i < command.size && command.data[i] == '=';
        if (f.hasValue) {
            ++i;
            if (i < command.size && command.data[i] == '"') {
                start = ++i;
                while (i < command.size && command.data[i] != '"') ++i;
                if (i == command.size) return false;
                f.value = Span(command.data + start, i++ - start);
                if (i < command.size && command.data[i] != ' ' && command.data[i] != '\t') return false;
            } else {
                start = i;
                while (i < command.size && command.data[i] != ' ' && command.data[i] != '\t') ++i;
                f.value = Span(command.data + start, i - start);
            }
        }
        out.items[out.count++] = f;
    }
    return true;
}
inline bool FieldNumber(const Fields& fields, const char* name, U32& value, bool required) {
    const Field* f = fields.Find(name);
    if (!f) return !required;
    U64 number = 0;
    if (!f->hasValue || !Unsigned(f->value, 0xffffffffU, number)) return false;
    value = U32(number); return true;
}
inline bool ParsePayload(Span payload, Request& r, const char*& error) {
    Span parts[4 + MaxArgs * 2 + 1]; unsigned n = 0; size_t start = 0;
    for (size_t i = 0; i <= payload.size; ++i) {
        if (i != payload.size && payload.data[i] != '\\') continue;
        if (n == sizeof(parts) / sizeof(parts[0])) { error = "Too many argument fields"; return false; }
        parts[n++] = Span(payload.data + start, i - start); start = i + 1;
    }
    U64 target = 0, count = 0;
    if (n < 4 || !Equal(parts[0], "A") || !Equal(parts[2], "A") ||
        !Unsigned(parts[1], 0xffffffffU, target, 16) || !Unsigned(parts[3], MaxArgs, count, 10)) {
        error = "Invalid argument count or payload"; return false;
    }
    unsigned expected = 4 + unsigned(count) * 2;
    if (n == expected + 1 && !parts[n-1].size) --n;
    if (n != expected) { error = "Invalid argument count or payload"; return false; }
    r.target = U32(target); r.count = unsigned(count); size_t owned = 0;
    for (unsigned i = 0; i < r.count; ++i) {
        Span descriptor = parts[4 + i*2], value = parts[5 + i*2];
        if (!descriptor.size) { error = "Missing argument type"; return false; }
        unsigned kind = unsigned(descriptor.data[0] - '0'); r.args[i].type = kind;
        if (kind == 2 || kind == 7) {
            U64 bytes = 0;
            if (descriptor.size < 3 || descriptor.data[1] != '/' ||
                !Unsigned(Span(descriptor.data + 2, descriptor.size - 2), MaxArgumentBytes, bytes, 10) ||
                value.size % 2 || value.size / 2 != bytes || owned + size_t(bytes) + 1 > MaxOwnedBytes) {
                error = "Invalid array length"; return false;
            }
            for (size_t k = 0; k < value.size; ++k) if (Hex(value.data[k]) < 0) { error = "Invalid hexadecimal payload"; return false; }
            unsigned char* buffer = (unsigned char*)r.allocator.allocate(r.allocator.context, size_t(bytes) + 1);
            if (!buffer) { error = "Out of memory"; return false; }
            r.args[i].bytes = buffer; r.args[i].length = size_t(bytes); owned += size_t(bytes) + 1;
            for (size_t k = 0; k < size_t(bytes); ++k) buffer[k] = (unsigned char)((Hex(value.data[k*2]) << 4) | Hex(value.data[k*2+1]));
            buffer[size_t(bytes)] = 0; r.params[i] = U64((size_t)buffer);
        } else if (descriptor.size != 1) { error = "Invalid argument descriptor"; return false; }
        else if (kind == 1 || kind == 4 || kind == 8) {
            if (!Integer(value, kind == 8 ? 64 : 32, r.params[i])) { error = "Invalid integer argument"; return false; }
            if (kind == 4 && r.params[i] > 255) { error = "Invalid byte argument"; return false; }
        } else if (kind == 3) {
            float f = 0;
            if (!FloatValue(value, f)) { error = "Invalid floating point argument"; return false; }
            double promoted = f;
            if (r.floatCount < 13) {
                r.fpr[r.floatCount] = promoted;
                memcpy(&r.params[i], &promoted, sizeof(promoted));
            } else {
                U32 bits = 0; memcpy(&bits, &f, sizeof(f)); r.params[i] = bits;
            }
            ++r.floatCount;
        } else { error = "Unsupported argument type"; return false; }
    }
    return true;
}
inline bool Parse(const Fields& fields, Request& r, const char*& error) {
    r.Reset(); error = 0; U32 version = 0;
    if (!FieldNumber(fields, "ver", version, true)) error = "Version is not specified, expecting major version 2";
    else if (version != 2) error = "Version mismatch, expected 2";
    else if (!FieldNumber(fields, "type", r.type, true) || r.type > 18) error = "Invalid command type";
    else if (!FieldNumber(fields, "as", r.returnCount, false) || r.returnCount > MaxResult / 8) error = "Invalid result array length";
    else if (!FieldNumber(fields, "ord", r.ordinal, fields.Find("module") != 0)) error = "Invalid module ordinal";
    const Field* module = fields.Find("module");
    if (!error && module) {
        if (!module->hasValue || !module->value.size || module->value.size >= sizeof(r.module)) error = "Invalid module name";
        else { memcpy(r.module, module->value.data, module->value.size); r.module[module->value.size] = 0; }
    }
    r.system = fields.Find("system") != 0; r.vm = fields.Find("VM") != 0;
    const Field* payload = fields.Find("params");
    if (!error && (!payload || !payload->hasValue)) error = "The paramaters were not found";
    if (!error) ParsePayload(payload->value, r, error);
    if (error) { r.Reset(); return false; }
    return true;
}

class Text {
    char* data_; size_t capacity_, used_; bool ok_;
public:
    Text(char* data, size_t capacity) : data_(data), capacity_(capacity), used_(0), ok_(data && capacity) { if (ok_) data_[0] = 0; }
    bool Append(const char* text, size_t n) {
        if (!ok_ || n >= capacity_ - used_) { ok_ = false; return false; }
        memcpy(data_ + used_, text, n); used_ += n; data_[used_] = 0; return true;
    }
    bool Append(const char* text) { return text && Append(text, strlen(text)); }
    bool Format(const char* format, ...) {
        if (!ok_) return false;
        va_list ap; va_start(ap, format);
#ifdef _MSC_VER
        int n = _vsnprintf(data_ + used_, capacity_ - used_, format, ap);
#else
        int n = vsnprintf(data_ + used_, capacity_ - used_, format, ap);
#endif
        va_end(ap);
        if (n < 0 || size_t(n) >= capacity_ - used_) { data_[used_] = 0; ok_ = false; return false; }
        used_ += size_t(n); return true;
    }
    bool Good() const { return ok_; }
    size_t Size() const { return used_; }
};
inline bool Range(U32 address, size_t size, U32 alignment = 1) {
    return address >= 0x10000U && alignment && !(address % alignment) && size <= 0xffffffffU &&
        (!size || U64(address) + size - 1 <= 0xffffffffU);
}
struct MemorySetting { U32 address, value, expected, title; bool conditional, titleFilter; };
inline bool RegisterSetting(MemorySetting* entries, unsigned& count, const MemorySetting& value) {
    unsigned index = count;
    for (unsigned i = 0; i < count; ++i) if (entries[i].address == value.address && entries[i].value == value.value) { index = i; break; }
    if (index >= 40) return false;
    entries[index] = value; if (index == count) ++count; return true;
}
template<class Memory> void WriteSetting(const MemorySetting& value, Memory& memory) {
    if (value.conditional) memory.CompareExchange(value.address, value.value, value.expected);
    else memory.Exchange(value.address, value.value);
}
inline bool IntegerArg(const Request& r, unsigned i) {
    return i < r.count && (r.args[i].type == 1 || r.args[i].type == 4 || r.args[i].type == 8);
}
inline bool StringArg(const Request& r, unsigned i) {
    return i < r.count && r.args[i].bytes && (r.args[i].type == 2 || r.args[i].type == 7) &&
        memchr(r.args[i].bytes, 0, r.args[i].length) == 0;
}
inline bool CustomArguments(const Request& r) {
    switch (r.type) {
    case 9: return StringArg(r,0) && r.args[0].length < 260 && IntegerArg(r,1);
    case 12: return StringArg(r,0) && r.args[0].length < 512 && IntegerArg(r,1);
    case 14: for (unsigned i = 0; i < 4; ++i) if (!IntegerArg(r,i)) return false; return true;
    case 15: return IntegerArg(r,0) && r.params[0] <= 3;
    case 18: for (unsigned i = 0; i < 5; ++i) if (!IntegerArg(r,i)) return false; return true;
    default: return r.type >= 10 && r.type <= 17;
    }
}

inline bool ScalarResult(Text& text, unsigned type, U64 integer, double floating) {
    switch (type) {
    case 0: return text.Append("S_OK");
    case 1: return text.Format("%X", U32(integer));
    case 4: return text.Format("%X", U32(integer & 255));
    case 3: return text.Format("%f", double(float(floating)));
    case 8: return text.Format("%llX", integer);
    default: return false;
    }
}
inline bool ArrayElement(Text& text, unsigned type, U64 bits, char separator) {
    switch (type) {
    case 5: return text.Format("%X%c", U32(bits), separator);
    case 6: {
        U32 raw = U32(bits); float value; memcpy(&value, &raw, 4);
        return text.Format("%f%c", double(value), separator);
    }
    case 7: return text.Format("%i%c", int(bits & 255), separator);
    case 9: return text.Format("%llX%c", bits, separator);
    default: return false;
    }
}
template<class Memory> bool StringResult(Text& text, U32 address, Memory& memory) {
    for (unsigned i = 0; i < MaxResult; ++i) {
        U64 bits = 0;
        if (!Range(address,size_t(i)+1) || !memory.Read(address+i,1,bits)) return false;
        char ch = char(bits);
        if (!ch) return text.Good();
        if (ch == '\r' || ch == '\n' || !text.Append(&ch,1)) return false;
    }
    return false;
}
template<class Memory> bool ArrayResult(Text& text, unsigned type, unsigned count, U32 address, Memory& memory) {
    if (count > MaxResult/8 || (type != 5 && type != 6 && type != 7 && type != 9)) return false;
    unsigned width = type == 7 ? 1 : type == 9 ? 8 : 4;
    if (count && !Range(address,size_t(count)*width)) return false;
    for (unsigned i = 0; i < count; ++i) {
        U64 bits = 0;
        if (!memory.Read(address+i*width,width,bits) || !ArrayElement(text,type,bits,i+1 == count ? ';' : ',')) return false;
    }
    return text.Good();
}
inline void Jump(U32* words, U32 target) {
    words[0] = 0x3D600000 | (((target >> 16) + ((target & 0x8000) ? 1 : 0)) & 0xffff);
    words[1] = 0x396B0000 | (target & 0xffff);
    words[2] = 0x7D6903A6; words[3] = 0x4E800420;
}
typedef bool (*ResolveSave)(void*, U32, U32&);
inline bool Trampoline(const U32* original, U32 address, U32 stub, U32* out, ResolveSave resolve, void* context) {
    if (!Range(address, 20, 4) || !Range(stub, 32, 4)) return false;
    bool nops = true;
    for (unsigned i = 0; i < 4; ++i) if (original[i] != 0x60000000) nops = false;
    if (!nops && original[0] != 0x7D8802A6) return false;
    Jump(out, address + 16);
    for (unsigned i = 0; i < 4; ++i) {
        U32 word = original[i]; unsigned op = word >> 26;
        if (op == 16 || op == 19 || (op == 18 && (word & 3) != 1)) return false;
        U32 spr = word & 0xfc1fffff;
        if (op == 17 || spr == 0x7c0903a6 || spr == 0x7c0902a6 || (i && spr == 0x7c0802a6)) return false;
        if (op == 18) {
            U32 displacement = word & 0x03fffffc;
            if (displacement & 0x02000000) displacement |= 0xfc000000;
            U32 destination = 0;
            if (!resolve || !resolve(context, address + i * 4 + displacement, destination)) return false;
            U32 delta = destination - (stub + (i + 3) * 4);
            if ((delta & 3) || (delta > 0x01fffffc && delta < 0xfe000000)) return false;
            word = 0x48000001 | (delta & 0x03fffffc);
        }
        out[i+3] = word;
    }
    out[7] = 0x4E800420; return true;
}
struct Layout { U32 marker, expected, features, file; };
typedef bool (*ReadMarker)(void*, U32, U32&);
inline unsigned SelectLayout(U32 base, U32 size, ReadMarker read, void* context, Layout& result) {
    static const Layout layouts[] = {
        {0x9101BD30,0x91001920,0x9100BE30,0x9100E008},
        {0x9101ABF8,0x910037C8,0x9100B820,0x9100DB20},
        {0x91F05900,0x636F6E73,0x91F4CE20,0}
    };
    if (!read || !Range(base,size) || size < 4) return 0;
    for (unsigned i = 0; i < sizeof(layouts)/sizeof(layouts[0]); ++i) {
        const Layout& value = layouts[i]; U32 marker = 0;
        if (value.marker < base || U64(value.marker) + 4 > U64(base) + size || !read(context,value.marker,marker) || marker != value.expected) continue;
        result = value; return i + 1;
    }
    return 0;
}
template<class Installer> bool InstallPair(Installer& installer, bool second) {
    if (!installer.Install(0)) { installer.Restore(0); return false; }
    if (second && !installer.Install(1)) { installer.Restore(1); installer.Restore(0); return false; }
    return true;
}

template<class T, unsigned Capacity> class PendingTable {
    T* slots_[Capacity]; U32 tokens_[Capacity], next_;
public:
    PendingTable() : next_(0) { memset(slots_, 0, sizeof(slots_)); memset(tokens_, 0, sizeof(tokens_)); }
    U32 Insert(T* value) {
        if (!value || next_ == 0xffffffffU) return 0;
        for (unsigned i = 0; i < Capacity; ++i) if (!slots_[i]) { slots_[i] = value; tokens_[i] = ++next_; return next_; }
        return 0;
    }
    T* Find(U32 token) const { for (unsigned i = 0; i < Capacity; ++i) if (slots_[i] && tokens_[i] == token) return slots_[i]; return 0; }
    T* Remove(U32 token) { for (unsigned i = 0; i < Capacity; ++i) if (slots_[i] && tokens_[i] == token) { T* p = slots_[i]; slots_[i] = 0; return p; } return 0; }
    T* At(unsigned i) const { return i < Capacity ? slots_[i] : 0; }
    U32 TokenAt(unsigned i) const { return i < Capacity && slots_[i] ? tokens_[i] : 0; }
};
}
#endif
