# DataDumper — Project Guide for AI Agents

## Project Overview

DataDumper is a **single-header C++ library** that provides human-readable pretty-printing for C++ data structures. It is useful for logging, debugging, and quickly inspecting nested structs, enums, containers, pointers, and atomic values at runtime.

The entire library lives in `data_dumper.h` (~400 lines). There is no external dependency beyond the C++ standard library and the compiler's ABI demangler (`abi::__cxa_demangle`).

## Technology Stack

- **Language**: C++14
- **Build Tool**: GNU Make + g++
- **Library Type**: Header-only (no compilation step needed to use the library)
- **Platform**: Linux / POSIX (relies on `abi::__cxa_demangle`, available on GCC and Clang)

## File Layout

```
.
├── data_dumper.h                  # The library — include this in your project
├── data_dumper_demo.cpp           # Standalone demo / smoke test
├── data_dumper_demo               # Pre-compiled demo binary (artifact)
├── data_dumper_upstream_demo.cpp  # Demo using generated code for upstream headers
├── generate_dd_dump.py            # Python script: generates dd_dump() from headers
├── upstream/
│   └── market_data.h              # Example upstream header (simulated, unmodifiable)
├── generated/
│   └── market_data_dumps.h        # Auto-generated registration for upstream/
└── Makefile                       # Builds the demo binaries
```

There are no package managers and no generated build files (besides `generated/`).

## Build and Test Commands

Build the original demo binary:

```bash
make
```

Run the demo:

```bash
./data_dumper_demo
```

Generate registration code for upstream headers:

```bash
make generate
```

Build the upstream demo (includes the generated header):

```bash
make upstream_demo
```

Run the upstream demo:

```bash
./data_dumper_upstream_demo
```

Clean build artifacts:

```bash
make clean
```

Compiler flags used (`Makefile`):

```
-std=c++14 -Wall -Wextra -O2 -I.
```

There is **no formal unit-test framework**. The project is validated by inspecting the output of the demo binaries, which exercises:

- Simple structs
- Nested structs
- Enums (registered and unregistered)
- `std::vector`
- `std::shared_ptr` (null and non-null)
- `std::map`
- `std::pair`
- Raw pointers (null and non-null)
- `std::atomic<T>`
- Strings with escape sequences
- **Upstream structs via generated free-function `dd_dump()`**

## How the Library Works

Users opt-in their types by implementing a `dd_dump(DataDumper& _dd) const` member function and decorating it with convenience macros.

Alternatively, for **upstream headers that cannot be modified**, the library also recognizes ADL-discovered free functions:

```cpp
inline void dd_dump(const UpstreamType& val, DataDumper& dd) {
    dd.field("field1", val.field1);
    dd.field("field2", val.field2);
}
```

### Macros

| Macro | Purpose |
|-------|---------|
| `DD_DUMPABLE()` | Declares `void dd_dump(DataDumper& _dd) const` |
| `DD_FIELD(name)` | Emits `_dd.field(#name, name)` |
| `DD_ENUM(EnumType, ...)` | Specializes `EnumTraits<EnumType>` with a `switch` block |
| `DD_ENUM_VAL(name)` | Shortcut for `case name: return #name;` |

### Example Usage (In-Project Types)

```cpp
#include "data_dumper.h"

struct Order {
    uint64_t id;
    std::string symbol;

    DD_DUMPABLE() {
        DD_FIELD(id);
        DD_FIELD(symbol);
    }
};

// Print anywhere
Order o{42, "AAPL"};
std::cout << DataDumper::dump("order", o) << "\n";
```

### Example Usage (Upstream Types — No Modification Required)

```cpp
// upstream/api.h (cannot edit)
namespace upstream {
struct Config { int timeout; double rate; };
}

// generated/api_dumps.h (auto-generated)
namespace upstream {
inline void dd_dump(const upstream::Config& val, DataDumper& dd) {
    dd.field("timeout", val.timeout);
    dd.field("rate", val.rate);
}
}

// Your code
#include "upstream/api.h"
#include "generated/api_dumps.h"

upstream::Config cfg{1000, 1.5};
std::cout << DataDumper::dump("cfg", cfg) << "\n";
```

### Supported Types (Out of the Box)

- User-defined structs/classes that implement `dd_dump()` member **or** free function
- `bool`, `char`, `signed char`, `unsigned char`
- All other arithmetic types (`int`, `float`, `double`, `uint64_t`, etc.)
- `std::string`
- Fixed `char[N]` arrays (shows length info)
- `char*` / `const char*` C-strings
- `std::atomic<T>`
- `std::shared_ptr<T>` and `std::unique_ptr<T>`
- Raw pointers (non-`char*`)
- `std::vector<T>`
- `std::map<K, V>` and `std::unordered_map<K, V>`
- `std::pair<T1, T2>`
- Enums (falls back to numeric value if no `EnumTraits` specialization)

### Recursion Guard

The dumper hard-caps nesting depth at `MAX_DEPTH = 10` to avoid infinite recursion on circular structures.

## Code Style Guidelines

- The header uses **2-space indentation**.
- Sections are separated by `// === ... ===` banner comments.
- Overloads are numbered in comments (`// 1. ...`, `// 2. ...`) to make SFINAE resolution order explicit.
- Type names are demangled at runtime; the `"struct "` and `"class "` prefixes are stripped for brevity.
- Strings are escaped: `\\`, `\"`, `\n`, `\r`, `\t`, and non-printable bytes as `\xHH`.

## Security Considerations

- The library uses `typeid(T).name()` and `abi::__cxa_demangle`, so it should only be used in environments where RTTI is enabled.
- It dereferences raw pointers and smart pointers without validation beyond a null check. If a dangling or invalid pointer is passed, behavior is undefined.
- There is no sandboxing of user-provided `dd_dump()` implementations — custom dumpers can execute arbitrary code.
- The max-depth guard (`MAX_DEPTH = 10`) prevents runaway recursion, but it does not detect cycles in graphs of raw pointers.

## Porting Notes

- `abi::__cxa_demangle` is GCC/Clang specific. On MSVC the demangling path would need to be replaced or guarded out.
- Because the library is header-only, integrating it into another C++14 (or newer) project is as simple as copying `data_dumper.h` and `#include`-ing it.
