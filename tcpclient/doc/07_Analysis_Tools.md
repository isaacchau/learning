# Installing Analysis Tools on Ubuntu

This guide shows how to install the optional analysis tools used by `Makefile.analysis`.

## Quick Install (All Tools)

```bash
sudo apt update
sudo apt install -y clang clang-tidy valgrind cppcheck
```

## Individual Tools

### 1. AddressSanitizer & ThreadSanitizer (clang)
These are **built into clang/gcc**, no extra install needed! Just the compiler:

```bash
sudo apt install -y clang
```

Verify installation:
```bash
clang --version
```

### 2. Valgrind (Memory Analysis)
```bash
sudo apt install -y valgrind
```

Verify installation:
```bash
valgrind --version
```

### 3. clang-tidy (Static Analysis)
```bash
sudo apt install -y clang-tidy
```

Verify installation:
```bash
clang-tidy --version
```

### 4. cppcheck (Static Analysis - Basic)
```bash
sudo apt install -y cppcheck
```

Verify installation:
```bash
cppcheck --version
```

### 5. scan-build (Clang Static Analyzer)
Usually comes with clang, but if missing:
```bash
sudo apt install -y clang-tools
```

Verify installation:
```bash
scan-build --help
```

## Verify All Tools

```bash
echo "=== Checking installed tools ==="
which clang && echo "✓ clang installed"
which clang-tidy && echo "✓ clang-tidy installed"
which valgrind && echo "✓ valgrind installed"
which cppcheck && echo "✓ cppcheck installed"
which scan-build && echo "✓ scan-build installed"
echo "=== Done ==="
```

## Usage After Installation

```bash
# Memory leak detection
make -f Makefile.analysis asan
./msg_client_asan --host 127.0.0.1 --port 8888

# Race condition detection
make -f Makefile.analysis tsan
./msg_client_tsan --host 127.0.0.1 --port 8888

# Static analysis
make -f Makefile.analysis tidy

# Detailed memory analysis
make -f Makefile.analysis valgrind
```

## Disk Space

Approximate install sizes:
- `clang`: ~200 MB
- `clang-tidy`: ~50 MB
- `valgrind`: ~30 MB
- `cppcheck`: ~5 MB
- **Total**: ~300 MB

## Uninstall

```bash
sudo apt remove -y clang clang-tidy valgrind cppcheck clang-tools
sudo apt autoremove -y
```
