#ifndef DATA_DUMPER_H
#define DATA_DUMPER_H

#include <array>
#include <atomic>
#include <cxxabi.h>
#include <deque>
#include <iomanip>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ============================================================================
// EnumTraits -- default and per-enum specializations
// ============================================================================

template <typename T>
struct EnumTraits {
    static std::string to_string(T val) {
        std::ostringstream oss;
        oss << "enum(" << static_cast<typename std::underlying_type<T>::type>(val) << ")";
        return oss.str();
    }
};

// ============================================================================
// DataDumper -- main pretty-printing engine
// ============================================================================
class DataDumper {
public:
    static constexpr int DEFAULT_MAX_DEPTH = 10;

    DataDumper() : max_depth_(DEFAULT_MAX_DEPTH) {}
    explicit DataDumper(int max_depth) : max_depth_(max_depth) {}

    void set_max_depth(int max_depth) { max_depth_ = max_depth; }
    int get_max_depth() const { return max_depth_; }

    // Begin a named struct block
    void begin(const char* name) {
        out_ << name << " {\n";
        ++depth_;
    }

    // End a struct block
    void end() {
        --depth_;
        indent();
        out_ << "}";
    }

    // Begin an array block with a size
    void begin_array(const char* name, size_t size) {
        indent();
        out_ << name << " = [" << size << "] {\n";
        ++depth_;
    }

    // End an array block
    void end_array() {
        --depth_;
        indent();
        out_ << "}\n";
    }

    // Begin an array element block
    void begin_array_element(size_t index) {
        indent();
        out_ << "[" << index << "] = {\n";
        ++depth_;
    }

    // End an array element block
    void end_array_element() {
        --depth_;
        indent();
        out_ << "}\n";
    }

    // Print one field
    template <typename T>
    void field(const char* name, const T& value) {
        if (depth_ >= max_depth_) {
            indent();
            out_ << name << " = (max depth reached)\n";
            return;
        }
        indent();
        out_ << name << " = ";
        dump_value(value);
        out_ << "\n";
    }

    // Static convenience: dump a value with a name prefix
    template <typename T>
    static std::string dump(const char* name, const T& value) {
        DataDumper dd;
        dd.out_ << name << " = ";
        dd.dump_value(value);
        dd.out_ << "\n";
        return dd.str();
    }

    // Static convenience: dump a value with a name prefix and custom max depth
    template <typename T>
    static std::string dump(const char* name, const T& value, int max_depth) {
        DataDumper dd(max_depth);
        dd.out_ << name << " = ";
        dd.dump_value(value);
        dd.out_ << "\n";
        return dd.str();
    }

    // Static convenience: dump a value without a name prefix
    template <typename T>
    static std::string dump(const T& value) {
        DataDumper dd;
        dd.dump_value(value);
        dd.out_ << "\n";
        return dd.str();
    }

    // Static convenience: dump a value without a name prefix and custom max depth
    template <typename T>
    static std::string dump(const T& value, int max_depth) {
        DataDumper dd(max_depth);
        dd.dump_value(value);
        dd.out_ << "\n";
        return dd.str();
    }

    std::string str() const { return out_.str(); }

private:
    std::ostringstream out_;
    int depth_ = 0;
    int max_depth_ = DEFAULT_MAX_DEPTH;

    void indent() {
        for (int i = 0; i < depth_; ++i) out_ << "  ";
    }

    // --- SFINAE trait: does T have dd_dump(DataDumper&) const? ---
    template <typename T>
    struct has_dd_dump {
        template <typename U>
        static auto test(int)
            -> decltype(std::declval<const U>().dd_dump(std::declval<DataDumper&>()),
                        std::true_type());
        template <typename>
        static std::false_type test(...);
        static constexpr bool value = decltype(test<T>(0))::value;
    };

    // --- SFINAE trait: does ADL find dd_dump(const U&, DataDumper&)? ---
    template <typename T>
    struct has_free_dd_dump {
        template <typename U>
        static auto test(int)
            -> decltype(dd_dump(std::declval<const U&>(), std::declval<DataDumper&>()),
                        std::true_type());
        template <typename>
        static std::false_type test(...);
        static constexpr bool value = decltype(test<T>(0))::value;
    };

    // --- Type name helper (demangle via abi::__cxa_demangle) ---
    template <typename T>
    static std::string type_name() {
#if defined(__GXX_RTTI) || defined(_CPPRTTI)
        int status = 0;
        char* demangled = abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status);
        std::string result = (status == 0 && demangled) ? demangled : typeid(T).name();
        free(demangled);
#else
        std::string result = "(unknown type)";
#endif
        if (result.size() >= 7 && result.substr(0, 7) == "struct ")
            result = result.substr(7);
        else if (result.size() >= 6 && result.substr(0, 6) == "class ")
            result = result.substr(6);
        return result;
    }

    // --- dump_value overloads (most-specific first) ---

    // 1. User-defined types with dd_dump() member
    template <typename T>
    auto dump_value(const T& val)
        -> typename std::enable_if<has_dd_dump<T>::value, void>::type {
        out_ << type_name<T>() << " {\n";
        ++depth_;
        val.dd_dump(*this);
        --depth_;
        indent();
        out_ << "}";
    }

    // 1b. User-defined types with free dd_dump(const T&, DataDumper&) via ADL
    template <typename T>
    auto dump_value(const T& val)
        -> typename std::enable_if<
            has_free_dd_dump<T>::value && !has_dd_dump<T>::value, void>::type {
        out_ << type_name<T>() << " {\n";
        ++depth_;
        dd_dump(val, *this);
        --depth_;
        indent();
        out_ << "}";
    }

    // 2. bool
    void dump_value(bool val) { out_ << (val ? "true" : "false"); }

    // 3. char (single character, printable style)
    void dump_value(char val) { out_ << "'" << val << "'"; }

    // 4. signed char (numeric, not character)
    void dump_value(signed char val) { out_ << static_cast<int>(val); }

    // 5. unsigned char (numeric, not character)
    void dump_value(unsigned char val) { out_ << static_cast<unsigned>(val); }

    // 6. Generic arithmetic types (int, long, float, double, uint64_t, etc.)
    template <typename T>
    auto dump_value(const T& val)
        -> typename std::enable_if<
            std::is_arithmetic<T>::value && !std::is_same<T, char>::value &&
                !std::is_same<T, bool>::value && !std::is_same<T, signed char>::value &&
                !std::is_same<T, unsigned char>::value,
            void>::type {
        out_ << val;
    }

    // 7. std::string
    void dump_value(const std::string& val) { out_ << "\"" << escape(val) << "\""; }

    // 8. Fixed char arrays: char[N]
    template <size_t N>
    void dump_value(const char (&val)[N]) {
        size_t len = 0;
        while (len < N && val[len] != '\0') ++len;
        out_ << "\"" << escape(std::string(val, len)) << "\"";
        out_ << " (" << len << "/" << N << ")";
    }

    // 9. char pointer (C string) -- template so char[N] arrays prefer overload #8
    template <typename T>
    auto dump_value(T val)
        -> typename std::enable_if<
            std::is_pointer<T>::value &&
                std::is_same<
                    typename std::remove_cv<
                        typename std::remove_pointer<T>::type>::type,
                    char>::value,
            void>::type {
        if (!val) {
            out_ << "(null)";
            return;
        }
        out_ << "\"" << escape(val) << "\"";
    }

    // 10. std::atomic<T>
    template <typename T>
    void dump_value(const std::atomic<T>& val) {
        dump_value(val.load(std::memory_order_relaxed));
    }

    // 11. std::shared_ptr<T>
    template <typename T>
    void dump_value(const std::shared_ptr<T>& val) {
        if (!val) {
            out_ << "(null)";
            return;
        }
        dump_value(*val);
    }

    // 12. std::unique_ptr<T>
    template <typename T, typename Deleter>
    void dump_value(const std::unique_ptr<T, Deleter>& val) {
        if (!val) {
            out_ << "(null)";
            return;
        }
        dump_value(*val);
    }

    // 12b. Generic C-style arrays: T[N]
    template <typename T, size_t N>
    void dump_value(const T (&val)[N]) {
        out_ << "[" << N << "] {\n";
        ++depth_;
        for (size_t i = 0; i < N; ++i) {
            indent();
            out_ << "[" << i << "] = ";
            dump_value(val[i]);
            out_ << "\n";
        }
        --depth_;
        indent();
        out_ << "}";
    }

    // 13. Raw pointer (not char* -- char* is handled by overload #9)
    template <typename T>
    auto dump_value(T val)
        -> typename std::enable_if<
            std::is_pointer<T>::value &&
            !std::is_same<typename std::remove_cv<typename std::remove_pointer<T>::type>::type, char>::value,
            void>::type {
        if (!val) {
            out_ << "(null)";
            return;
        }
        dump_value(*val);
    }

    // 14. std::vector<T>
    template <typename T>
    void dump_value(const std::vector<T>& val) {
        out_ << "[" << val.size() << "] {\n";
        ++depth_;
        for (size_t i = 0; i < val.size(); ++i) {
            indent();
            out_ << "[" << i << "] = ";
            dump_value(val[i]);
            out_ << "\n";
        }
        --depth_;
        indent();
        out_ << "}";
    }

    // 14b. std::array<T, N>
    template <typename T, size_t N>
    void dump_value(const std::array<T, N>& val) {
        out_ << "[" << N << "] {\n";
        ++depth_;
        for (size_t i = 0; i < N; ++i) {
            indent();
            out_ << "[" << i << "] = ";
            dump_value(val[i]);
            out_ << "\n";
        }
        --depth_;
        indent();
        out_ << "}";
    }

    // 14c. std::list<T>
    template <typename T, typename Alloc>
    void dump_value(const std::list<T, Alloc>& val) {
        out_ << "[" << val.size() << "] {\n";
        ++depth_;
        size_t i = 0;
        for (const auto& item : val) {
            indent();
            out_ << "[" << i++ << "] = ";
            dump_value(item);
            out_ << "\n";
        }
        --depth_;
        indent();
        out_ << "}";
    }

    // 14d. std::deque<T>
    template <typename T, typename Alloc>
    void dump_value(const std::deque<T, Alloc>& val) {
        out_ << "[" << val.size() << "] {\n";
        ++depth_;
        size_t i = 0;
        for (const auto& item : val) {
            indent();
            out_ << "[" << i++ << "] = ";
            dump_value(item);
            out_ << "\n";
        }
        --depth_;
        indent();
        out_ << "}";
    }

    // 15. std::map<K, V>
    template <typename K, typename V>
    void dump_value(const std::map<K, V>& val) {
        out_ << "[" << val.size() << "] {\n";
        ++depth_;
        for (const auto& p : val) {
            indent();
            out_ << "key = ";
            dump_value(p.first);
            out_ << "\n";
            indent();
            out_ << "val = ";
            dump_value(p.second);
            out_ << "\n";
        }
        --depth_;
        indent();
        out_ << "}";
    }

    // 16. std::unordered_map<K, V>
    template <typename K, typename V, typename H, typename E>
    void dump_value(const std::unordered_map<K, V, H, E>& val) {
        out_ << "[" << val.size() << "] {\n";
        ++depth_;
        for (const auto& p : val) {
            indent();
            out_ << "key = ";
            dump_value(p.first);
            out_ << "\n";
            indent();
            out_ << "val = ";
            dump_value(p.second);
            out_ << "\n";
        }
        --depth_;
        indent();
        out_ << "}";
    }

    // 16b. std::set<T>
    template <typename T, typename Compare, typename Alloc>
    void dump_value(const std::set<T, Compare, Alloc>& val) {
        out_ << "[" << val.size() << "] {\n";
        ++depth_;
        for (const auto& item : val) {
            indent();
            dump_value(item);
            out_ << "\n";
        }
        --depth_;
        indent();
        out_ << "}";
    }

    // 16c. std::unordered_set<T>
    template <typename T, typename Hash, typename KeyEqual, typename Alloc>
    void dump_value(const std::unordered_set<T, Hash, KeyEqual, Alloc>& val) {
        out_ << "[" << val.size() << "] {\n";
        ++depth_;
        for (const auto& item : val) {
            indent();
            dump_value(item);
            out_ << "\n";
        }
        --depth_;
        indent();
        out_ << "}";
    }

    // 17. std::pair<T1, T2>
    template <typename T1, typename T2>
    void dump_value(const std::pair<T1, T2>& val) {
        out_ << "(\n";
        ++depth_;
        indent();
        out_ << "first = ";
        dump_value(val.first);
        out_ << "\n";
        indent();
        out_ << "second = ";
        dump_value(val.second);
        out_ << "\n";
        --depth_;
        indent();
        out_ << ")";
    }

    // 18. Enums
    template <typename T>
    auto dump_value(const T& val)
        -> typename std::enable_if<std::is_enum<T>::value, void>::type {
        out_ << EnumTraits<T>::to_string(val);
    }

    // --- String escaping ---
    static std::string escape(const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            switch (c) {
            case '\\':
                r += "\\\\";
                break;
            case '"':
                r += "\\\"";
                break;
            case '\n':
                r += "\\n";
                break;
            case '\r':
                r += "\\r";
                break;
            case '\t':
                r += "\\t";
                break;
            default:
                if (c >= 0x20 && c < 0x7F) {
                    r += c;
                } else {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned char>(c));
                    r += buf;
                }
            }
        }
        return r;
    }

    static std::string escape(const char* s) { return escape(std::string(s ? s : "")); }
};

// ============================================================================
// Macros for easy struct registration
// ============================================================================

#define DD_DUMPABLE() void dd_dump(DataDumper& _dd) const
#define DD_FIELD(name) _dd.field(#name, name)

// ============================================================================
// Enum registration macro
// ============================================================================

#define DD_ENUM(EnumType, ...)                                              \
    template <> struct EnumTraits<EnumType> {                               \
        static std::string to_string(EnumType val) {                        \
            switch (val) {                                                  \
                __VA_ARGS__                                                 \
            default:                                                        \
                break;                                                      \
            }                                                               \
            std::ostringstream oss;                                         \
            oss << #EnumType << "("                                         \
                << static_cast<typename std::underlying_type<EnumType>::type>(val) \
                << ")";                                                    \
            return oss.str();                                               \
        }                                                                   \
    };

#define DD_ENUM_VAL(name) case name: return #name;

#endif // DATA_DUMPER_H
