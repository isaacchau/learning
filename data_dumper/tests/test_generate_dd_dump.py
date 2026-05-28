#!/usr/bin/env python3
"""
Test suite for generate_dd_dump.py

Usage:
    python3 tests/test_generate_dd_dump.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

# Path to the generator script
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GENERATOR = os.path.join(PROJECT_ROOT, "generate_dd_dump.py")
DATA_DUMPER_H = os.path.join(PROJECT_ROOT, "data_dumper.h")


class TestGenerateDdDump(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="dd_dump_test_")
        self.generated = os.path.join(self.tmpdir, "generated.h")

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _write_header(self, name, content):
        path = os.path.join(self.tmpdir, name)
        with open(path, "w") as f:
            f.write(content)
        return path

    def _run_generator(self, headers, includes=None):
        includes = includes or []
        cmd = [sys.executable, GENERATOR]
        for inc in includes:
            cmd.extend(["-I", inc])
        for h in headers:
            cmd.append(h)
        cmd.extend(["-o", self.generated])
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(
            result.returncode, 0,
            f"Generator failed:\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )
        with open(self.generated, "r") as f:
            return f.read()

    def _compile(self, extra_sources=None):
        """Compile a small test program that includes the generated header."""
        extra_sources = extra_sources or []
        test_cpp = os.path.join(self.tmpdir, "compile_test.cpp")
        with open(test_cpp, "w") as f:
            f.write('#include "{}"\n'.format(self.generated))
            f.write("int main() { return 0; }\n")
        cmd = [
            "g++", "-std=c++14", "-Wall", "-Wextra",
            "-I", PROJECT_ROOT,
            "-I", self.tmpdir,
        ]
        cmd.append(test_cpp)
        cmd.extend(extra_sources)
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(
            result.returncode, 0,
            f"Compilation failed:\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )

    # ------------------------------------------------------------------
    # Tests
    # ------------------------------------------------------------------

    def test_simple_struct(self):
        h = self._write_header("simple.h", """
struct Point {
    int x;
    int y;
};
""")
        out = self._run_generator([h])
        self.assertIn("inline void dd_dump(const ::Point& val, DataDumper& dd)", out)
        self.assertIn('dd.field("x", val.x);', out)
        self.assertIn('dd.field("y", val.y);', out)
        self._compile()

    def test_namespaced_struct(self):
        h = self._write_header("ns.h", """
namespace api {
namespace v2 {
struct Config {
    int timeout;
    double rate;
};
}
}
""")
        out = self._run_generator([h])
        self.assertIn("namespace api {", out)
        self.assertIn("namespace v2 {", out)
        self.assertIn("inline void dd_dump(const ::api::v2::Config& val, DataDumper& dd)", out)
        self.assertIn('dd.field("timeout", val.timeout);', out)
        self._compile()

    def test_scoped_enum(self):
        h = self._write_header("enum_scoped.h", """
enum class Status { OK, ERROR, TIMEOUT };
""")
        out = self._run_generator([h])
        self.assertIn("template <>", out)
        self.assertIn("struct EnumTraits<::Status> {", out)
        self.assertIn("case ::Status::OK: return \"Status::OK\";", out)
        self.assertIn("case ::Status::ERROR: return \"Status::ERROR\";", out)
        self._compile()

    def test_unscoped_enum_in_namespace(self):
        h = self._write_header("enum_unscoped.h", """
namespace api {
enum Color { RED, GREEN, BLUE };
}
""")
        out = self._run_generator([h])
        self.assertIn("struct EnumTraits<::api::Color> {", out)
        self.assertIn("case ::api::Color::RED: return \"RED\";", out)
        self._compile()

    def test_nested_include_auto_discovery(self):
        """If header A includes header B, dumps for B should be generated too."""
        types_h = self._write_header("types.h", """
namespace api {
enum class Color { RED, GREEN };
}
""")
        api_h = self._write_header("api.h", f"""
#include "{types_h}"
namespace api {{
struct Config {{
    int timeout;
}};
}}
""")
        out = self._run_generator([api_h])
        # Both Config and Color should be present
        self.assertIn("dd_dump(const ::api::Config& val", out)
        self.assertIn("EnumTraits<::api::Color>", out)
        self._compile()

    def test_template_struct_skipped(self):
        h = self._write_header("template.h", """
template <typename T>
struct Container {
    T data;
    size_t len;
};
""")
        out = self._run_generator([h])
        self.assertNotIn("Container", out)

    def test_bitfields(self):
        h = self._write_header("bitfields.h", """
struct Flags {
    unsigned int flag1 : 1;
    unsigned int flag2 : 2;
    unsigned int reserved : 5;
};
""")
        out = self._run_generator([h])
        self.assertIn('dd.field("flag1", val.flag1);', out)
        self.assertIn('dd.field("flag2", val.flag2);', out)
        self.assertIn('dd.field("reserved", val.reserved);', out)
        self._compile()

    def test_arrays(self):
        h = self._write_header("arrays.h", """
struct Buffers {
    char name[32];
    int matrix[4][4];
};
""")
        out = self._run_generator([h])
        self.assertIn('dd.field("name", val.name);', out)
        self.assertIn('dd.field("matrix", val.matrix);', out)
        self._compile()

    def test_function_pointers_skipped(self):
        h = self._write_header("callbacks.h", """
struct Callbacks {
    void (*on_start)(int);
    void (*on_stop)(void);
    int normal_field;
};
""")
        out = self._run_generator([h])
        self.assertIn('dd.field("normal_field", val.normal_field);', out)
        self.assertNotIn("on_start", out)
        self.assertNotIn("on_stop", out)
        self._compile()

    def test_nested_struct_with_field(self):
        """Outer struct has a nested Inner struct used as a field."""
        h = self._write_header("nested.h", """
struct Outer {
    struct Inner {
        int a;
    };
    int b;
    Inner inner;
};
""")
        out = self._run_generator([h])
        # Outer should be dumped with b and inner
        self.assertIn('dd.field("b", val.b);', out)
        self.assertIn('dd.field("inner", val.inner);', out)
        self._compile()

    def test_static_members_skipped(self):
        h = self._write_header("static.h", """
struct Stats {
    int count;
    static int total;
};
""")
        out = self._run_generator([h])
        self.assertIn('dd.field("count", val.count);', out)
        self.assertNotIn("total", out)
        self._compile()

    def test_multiple_input_headers(self):
        h1 = self._write_header("a.h", "struct A { int x; };\n")
        h2 = self._write_header("b.h", "struct B { int y; };\n")
        out = self._run_generator([h1, h2])
        self.assertIn("dd_dump(const ::A& val", out)
        self.assertIn("dd_dump(const ::B& val", out)
        self._compile()

    def test_forward_declarations(self):
        h = self._write_header("forward.h", """
struct ForwardStruct;
class ForwardClass;
enum class ForwardEnum : int;

struct ValidStruct {
    int x;
};
""")
        out = self._run_generator([h])
        # Forward declarations should be skipped, and ValidStruct should be parsed and generated successfully
        self.assertNotIn("ForwardStruct", out)
        self.assertNotIn("ForwardClass", out)
        self.assertNotIn("ForwardEnum", out)
        self.assertIn("inline void dd_dump(const ::ValidStruct& val, DataDumper& dd)", out)
        self._compile()

    def test_global_namespace_collision(self):
        h = self._write_header("collision.h", """
enum CollisionEnum { VAL_A, VAL_B };
namespace app {
enum CollisionEnum { VAL_A, VAL_B };
struct TestStruct {
    CollisionEnum local_val;
    ::CollisionEnum global_val;
};
}
""")
        out = self._run_generator([h])
        # Verify fully-qualified naming is used to avoid ambiguity
        self.assertIn("struct EnumTraits<::CollisionEnum> {", out)
        self.assertIn("struct EnumTraits<::app::CollisionEnum> {", out)
        self.assertIn("case ::CollisionEnum::VAL_A: return \"VAL_A\";", out)
        self.assertIn("case ::app::CollisionEnum::VAL_A: return \"VAL_A\";", out)
        self._compile()

    def test_large_number_of_types(self):
        # Generate 75 structs and 75 enums (total 150 types) to verify no capacity capping or issues
        content = []
        for i in range(75):
            content.append(f"enum class Enum_{i} {{ VAL_A, VAL_B }};")
            content.append(f"struct Struct_{i} {{ int field_1; double field_2; Enum_{i} field_3; }};")
        
        h = self._write_header("large.h", "\n".join(content))
        out = self._run_generator([h])
        
        # Verify that all 150 types are present in the generated code
        for i in range(75):
            self.assertIn(f"struct EnumTraits<::Enum_{i}> {{", out)
            self.assertIn(f"inline void dd_dump(const ::Struct_{i}& val, DataDumper& dd)", out)
        
        self._compile()

    def test_in_class_initializers(self):
        h = self._write_header("initializers.h", """
#include <vector>
#define DEFAULT_PRIORITY 2
struct Config {
    int timeout = 1000;
    double rate{1.5};
    int priority = DEFAULT_PRIORITY;
    std::vector<int> values = {1, 2, 3};
};
""")
        out = self._run_generator([h])
        # Verify that all fields with in-class initializers are correctly parsed with their correct names
        self.assertIn('dd.field("timeout", val.timeout);', out)
        self.assertIn('dd.field("rate", val.rate);', out)
        self.assertIn('dd.field("priority", val.priority);', out)
        self.assertIn('dd.field("values", val.values);', out)
        self._compile()

    def test_namespace_aliases_and_using_directives(self):
        h = self._write_header("aliases.h", """
namespace outer {
    namespace inner {
        struct Target {
            int val;
        };
    }
}

namespace alias_ns = outer::inner;
using namespace outer;

namespace outer {
    namespace inner {
        struct SecondTarget {
            int val;
        };
    }
}
""")
        out = self._run_generator([h])
        # Verify both Target and SecondTarget are generated under the correct qualified names,
        # meaning namespace alias and using directives did not corrupt the namespace stack.
        self.assertIn("inline void dd_dump(const ::outer::inner::Target& val, DataDumper& dd)", out)
        self.assertIn("inline void dd_dump(const ::outer::inner::SecondTarget& val, DataDumper& dd)", out)
        self._compile()

    def test_user_reproduction(self):
        h1 = self._write_header("h1.h", """
#pragma once
namespace app {
enum Status { ACTIVE, INACTIVE };
}
""")
        h2 = self._write_header("h2.h", f"""
#pragma once
#include "{h1}"
namespace app {{
namespace files {{
struct FileInfo {{
    int id;
    app::Status status;
}};
}}
}}
""")
        h3 = self._write_header("h3.h", f"""
#pragma once
#include "{h2}"
namespace app {{
namespace internal {{
struct InternalInfo {{
    int code;
    app::Status status;
}};
}}
}}
""")
        out = self._run_generator([h1, h2, h3])
        # Verify both structs are generated under the correct qualified names
        self.assertIn("FileInfo", out)
        self.assertIn("InternalInfo", out)
        self._compile()

    def test_cpp17_nested_namespace(self):
        h = self._write_header("nested_ns.h", """
#pragma once
namespace app::files {
struct FileInfo {
    int id;
};
}
""")
        out = self._run_generator([h])
        self.assertIn("inline void dd_dump(const ::app::files::FileInfo& val, DataDumper& dd)", out)
        self._compile()

    def test_typedef_struct(self):
        h = self._write_header("typedef_struct.h", """
#pragma once
typedef struct {
    int x;
    int y;
} Point;

namespace app {
typedef struct {
    double price;
    int quantity;
} Order;
}

typedef struct Line_ {
    Point start;
    Point end;
} Line;
""")
        out = self._run_generator([h])
        # Verify Point
        self.assertIn("inline void dd_dump(const ::Point& val, DataDumper& dd)", out)
        self.assertIn('dd.field("x", val.x);', out)
        self.assertIn('dd.field("y", val.y);', out)
        # Verify app::Order
        self.assertIn("inline void dd_dump(const ::app::Order& val, DataDumper& dd)", out)
        self.assertIn('dd.field("price", val.price);', out)
        self.assertIn('dd.field("quantity", val.quantity);', out)
        # Verify Line / Line_
        self.assertTrue(
            "inline void dd_dump(const ::Line_& val, DataDumper& dd)" in out or
            "inline void dd_dump(const ::Line& val, DataDumper& dd)" in out
        )
        self._compile()

    def test_inline_anonymous_union(self):
        h = self._write_header("anonymous_union.h", """
#pragma once
struct TypeA { int val; };
struct TypeB { double val; };
typedef struct {
    union {
        TypeA varA;
        TypeB varB;
    };
    int normalField;
} MyStruct;
""")
        out = self._run_generator([h])
        self.assertIn("inline void dd_dump(const ::MyStruct& val, DataDumper& dd)", out)
        self.assertIn('dd.field("varA", val.varA);', out)
        self.assertIn('dd.field("varB", val.varB);', out)
        self.assertIn('dd.field("normalField", val.normalField);', out)
        self._compile()

    def test_nested_anonymous_struct(self):
        h = self._write_header("anonymous_struct.h", """
#pragma once
typedef struct {
    struct {
        int a;
        int b;
    } inline_var;
    double c;
} MyStruct;
""")
        out = self._run_generator([h])
        self.assertIn("inline void dd_dump(const ::MyStruct& val, DataDumper& dd)", out)
        self.assertIn('dd.field("inline_var.a", val.inline_var.a);', out)
        self.assertIn('dd.field("inline_var.b", val.inline_var.b);', out)
        self.assertIn('dd.field("c", val.c);', out)
        self._compile()

    def test_deeply_nested_anonymous_struct(self):
        h = self._write_header("deeply_nested.h", """
#pragma once
struct DeepStruct {
    struct {
        struct {
            int x;
        } inner_inner;
        int y;
    } inner;
};
""")
        out = self._run_generator([h])
        self.assertIn("inline void dd_dump(const ::DeepStruct& val, DataDumper& dd)", out)
        self.assertIn('dd.field("inner.inner_inner.x", val.inner.inner_inner.x);', out)
        self.assertIn('dd.field("inner.y", val.inner.y);', out)
        self._compile()

    def test_filler_fields_skipped(self):
        h = self._write_header("filler.h", """
#pragma once
struct Config {
    int value;
    char filler_0[4];
    int another_value;
    char FILLER_1[8];
};
""")
        out = self._run_generator([h])
        self.assertIn('dd.field("value", val.value);', out)
        self.assertIn('dd.field("another_value", val.another_value);', out)
        self.assertNotIn("filler_0", out)
        self.assertNotIn("FILLER_1", out)
        self._compile()

    def test_nested_anonymous_struct_array(self):
        h = self._write_header("anon_array.h", """
#pragma once
typedef struct {
    int id;
} Price;

typedef struct {
    int val;
} Qty64;

typedef enum {
    TYPE_A,
    TYPE_B
} ItemType;

typedef struct {
    struct {
        union { 
            Price price;
            Qty64 qty;
        };
        ItemType   type;
        char       filler_0[1];
    } items[3];
} ItemBody;
""")
        out = self._run_generator([h])
        self.assertIn("inline void dd_dump(const ::ItemBody& val, DataDumper& dd)", out)
        self.assertIn('dd.begin_array("items", sizeof(val.items) / sizeof(val.items[0]));', out)
        self.assertIn('dd.begin_array_element(i);', out)
        self.assertIn('dd.field("price", val.items[i].price);', out)
        self.assertIn('dd.field("qty", val.items[i].qty);', out)
        self.assertIn('dd.field("type", val.items[i].type);', out)
        self.assertNotIn("filler_0", out)
        self.assertIn('dd.end_array_element();', out)
        self.assertIn('dd.end_array();', out)
        self._compile()

    def test_nested_anonymous_struct_array_execution(self):
        h = self._write_header("anon_array_exec.h", """
#pragma once
typedef struct {
    int id;
} Price;

typedef struct {
    int val;
} Qty64;

typedef enum {
    TYPE_A,
    TYPE_B
} ItemType;

typedef struct {
    struct {
        union { 
            Price price;
            Qty64 qty;
        };
        ItemType   type;
        char       filler_0[1];
    } items[3];
} ItemBody;
""")
        self._run_generator([h])
        
        # Compile and run a full validation binary
        test_cpp = os.path.join(self.tmpdir, "exec_test.cpp")
        with open(test_cpp, "w") as f:
            f.write(f'''
#include "{self.generated}"
#include <iostream>
#include <cassert>

int main() {{
    ItemBody body;
    body.items[0].price.id = 100;
    body.items[0].type = TYPE_A;
    body.items[1].qty.val = 200;
    body.items[1].type = TYPE_B;
    body.items[2].price.id = 300;
    body.items[2].type = TYPE_A;

    std::string dump = DataDumper::dump("body", body);
    std::cout << dump << std::endl;

    assert(dump.find("items = [3] {{") != std::string::npos);
    assert(dump.find("[0] = {{") != std::string::npos);
    assert(dump.find("price = Price {{") != std::string::npos);
    assert(dump.find("id = 100") != std::string::npos);
    assert(dump.find("filler") == std::string::npos);
    return 0;
}}
''')
        
        bin_path = os.path.join(self.tmpdir, "exec_test_bin")
        cmd_compile = [
            "g++", "-std=c++14", "-Wall", "-Wextra",
            "-I", PROJECT_ROOT,
            "-I", self.tmpdir,
            test_cpp, "-o", bin_path
        ]
        result = subprocess.run(cmd_compile, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, f"Compilation failed: {result.stderr}")
        
        result_run = subprocess.run([bin_path], capture_output=True, text=True)
        self.assertEqual(result_run.returncode, 0, f"Execution failed: {result_run.stderr}")
        self.assertIn("items = [3] {", result_run.stdout)
        self.assertIn("price = Price {", result_run.stdout)
        self.assertIn("id = 100", result_run.stdout)
        self.assertNotIn("filler", result_run.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
