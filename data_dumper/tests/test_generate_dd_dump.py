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


if __name__ == "__main__":
    unittest.main(verbosity=2)
