#!/usr/bin/env python3
"""
generate_dd_dump.py

Reads C++ header files by preprocessing them with g++ -E, then parses the
preprocessed output with a lightweight brace-level scanner to extract structs,
classes, and enums. Emits a header with free dd_dump() functions and
EnumTraits specializations for use with DataDumper.

This allows upstream headers to remain unmodified and does NOT require clang.

Usage:
    python3 generate_dd_dump.py -I/path/to/includes upstream.h -o generated.h
"""

import argparse
import os
import re
import subprocess
import sys


def run_cpp_preprocessor(header_paths, includes):
    """Run g++ -E and return the preprocessed text."""
    cmd = ["g++", "-E", "-std=c++14"]
    for inc in includes:
        cmd.append("-I" + inc)
    cmd.extend(header_paths)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("g++ preprocessor failed:\n" + result.stderr, file=sys.stderr)
        sys.exit(1)
    return result.stdout


def get_system_include_paths():
    """Ask g++ for its system #include search paths."""
    result = subprocess.run(
        ["g++", "-xc++", "-E", "-v", "-"],
        input="", capture_output=True, text=True
    )
    paths = []
    in_search = False
    for line in result.stderr.splitlines():
        if "#include <...> search starts here:" in line:
            in_search = True
            continue
        if "End of search list." in line:
            in_search = False
            continue
        if in_search:
            paths.append(line.strip())
    return paths


SYSTEM_INCLUDE_PATHS = get_system_include_paths()


def is_system_header(path):
    """Return True if the given path lives under a compiler system include dir."""
    if path.startswith("<"):
        return True
    real = os.path.realpath(path) if os.path.exists(path) else path
    for sp in SYSTEM_INCLUDE_PATHS:
        if real.startswith(sp):
            return True
    return False


def extract_non_system_content(preprocessed_text):
    """Extract only the lines that belong to non-system header files."""
    lines = preprocessed_text.splitlines(keepends=True)
    in_target = False
    result = []
    line_re = re.compile(r'#\s*\d+\s+"([^"]+)"')
    for line in lines:
        m = line_re.match(line)
        if m:
            fname = m.group(1)
            in_target = not is_system_header(fname)
            continue
        if in_target:
            result.append(line)
    return "".join(result)


def clean_preprocessor_directives(text):
    """Remove stray preprocessor directives left in the output."""
    text = re.sub(r'#\s*(pragma|if|ifdef|ifndef|else|elif|endif|define|undef|include|error|warning)\b.*?\n', '\n', text, flags=re.S)
    text = re.sub(r'#\s*\n', '\n', text)
    return text


def strip_initializer(stmt):
    """
    Remove `=` or `{}` in-class initializers from struct field statements
    while safely ignoring template brackets and parenthesized expressions.
    """
    paren_depth = 0
    bracket_depth = 0
    for idx, char in enumerate(stmt):
        if char == '(':
            paren_depth += 1
        elif char == ')':
            paren_depth -= 1
        elif char == '<':
            bracket_depth += 1
        elif char == '>':
            bracket_depth -= 1
        elif char in ('=', '{'):
            if paren_depth == 0 and bracket_depth == 0:
                return stmt[:idx].strip()
    return stmt


def split_body_into_statements(body):
    """Split a C++ struct/class body into top-level statements by semicolons,
    taking into account brace, parenthesis, bracket, and string literal contexts."""
    stmts = []
    current = []
    brace_depth = 0
    paren_depth = 0
    bracket_depth = 0
    in_string = False
    in_char = False
    escape = False
    
    for char in body:
        if escape:
            escape = False
            current.append(char)
            continue
            
        if char == '\\' and (in_string or in_char):
            escape = True
            current.append(char)
            continue
            
        if char == '"' and not in_char:
            in_string = not in_string
            current.append(char)
            continue
            
        if char == "'" and not in_string:
            in_char = not in_char
            current.append(char)
            continue
            
        if not in_string and not in_char:
            if char == '{':
                brace_depth += 1
            elif char == '}':
                brace_depth -= 1
            elif char == '(':
                paren_depth += 1
            elif char == ')':
                paren_depth -= 1
            elif char == '[':
                bracket_depth += 1
            elif char == ']':
                bracket_depth -= 1
                
        if char == ';' and brace_depth == 0 and paren_depth == 0 and bracket_depth == 0 and not in_string and not in_char:
            stmts.append("".join(current).strip())
            current = []
        else:
            current.append(char)
            
    if current:
        last = "".join(current).strip()
        if last:
            stmts.append(last)
            
    return stmts


def parse_fields_from_body(body):
    """Parse fields from a struct/class body, handling nested anonymous structs/unions,
    and returning a list of field names (with prefix if they are nested in an anonymous struct)."""
    fields = []
    statements = split_body_into_statements(body)
    
    for stmt in statements:
        stmt = stmt.strip()
        if not stmt:
            continue
        
        # Skip function declarations
        if re.search(r'\)\s*$', stmt):
            continue
        # Skip typedefs, using, static_assert, friend
        if re.match(r'\b(typedef|using|static_assert|friend)\b', stmt):
            continue
        # Skip access specifiers
        if re.match(r'\b(public|private|protected)\s*:', stmt):
            continue
        # Skip static members
        if re.match(r'\bstatic\b', stmt):
            continue
            
        # Parse nested brace-block structures
        brace_start = stmt.find('{')
        if brace_start != -1:
            depth = 1
            j = brace_start + 1
            n = len(stmt)
            while j < n and depth > 0:
                if stmt[j] == '{':
                    depth += 1
                elif stmt[j] == '}':
                    depth -= 1
                j += 1
            
            if depth == 0:
                inner_body = stmt[brace_start + 1:j - 1]
                prefix_part = stmt[:brace_start].strip()
                suffix_part = stmt[j:].strip()
                
                m_keyword = re.match(r'^\s*(struct|class|union)\b', prefix_part)
                if m_keyword:
                    type_name_part = prefix_part[m_keyword.end():].strip()
                    m_type_name = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)', type_name_part)
                    type_name = m_type_name.group(1) if m_type_name else None
                    
                    cleaned_suffix = strip_initializer(suffix_part)
                    
                    if type_name:
                        # Named type - check if it's an array or simple variable
                        m_var = re.search(
                            r'([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[.*?\])*\s*(?::\s*\d+)?\s*$',
                            cleaned_suffix
                        )
                        var_name = m_var.group(1) if m_var else None
                        if var_name:
                            fields.append(var_name)
                    else:
                        # Anonymous type - could be a struct or union
                        # Check if it's an array, e.g. items[MAX_ITEM_NUM]
                        m_array = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*\[', cleaned_suffix)
                        if m_array:
                            var_name = m_array.group(1)
                            inner_fields = parse_fields_from_body(inner_body)
                            fields.append(('array', var_name, 'size', inner_fields))
                        else:
                            # Not an array
                            m_var = re.search(
                                r'([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*\d+)?\s*$',
                                cleaned_suffix
                            )
                            var_name = m_var.group(1) if m_var else None
                            
                            nested_fields = parse_fields_from_body(inner_body)
                            if var_name:
                                for nf in nested_fields:
                                    if isinstance(nf, tuple) and len(nf) == 4 and nf[0] == 'array':
                                        fields.append(('array', var_name + "." + nf[1], nf[2], nf[3]))
                                    else:
                                        fields.append(var_name + "." + nf)
                            else:
                                fields.extend(nested_fields)
                    continue
        
        cleaned_stmt = strip_initializer(stmt)
        fm = re.search(
            r'([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[.*?\])*\s*(?::\s*\d+)?\s*$',
            cleaned_stmt
        )
        if fm:
            fields.append(fm.group(1))
            
    return fields


def _extract_nested_types(text, ns_stack, parent_name):
    """
    Find struct/class/enum definitions nested inside a parent type body.
    Returns tuples in the same format as parse_preprocessed.
    """
    structs = []
    enums = []
    i = 0
    n = len(text)
    keyword_re = re.compile(r'\b(struct|class|enum\s+class|enum)\b')

    while i < n:
        m = keyword_re.search(text, i)
        if not m:
            break
        keyword = m.group(1)
        is_enum_class = (keyword == 'enum class')
        is_enum = keyword.startswith('enum')
        is_struct = (keyword in ('struct', 'class'))
        i = m.end()

        while i < n and text[i].isspace():
            i += 1

        m2 = re.match(r'([A-Za-z_][A-Za-z0-9_]*)', text[i:])
        name = m2.group(1) if m2 else None
        if m2:
            i += m2.end()
        if not name:
            continue

        brace_start = text.find('{', i)
        semi_start = text.find(';', i)

        if semi_start != -1 and (brace_start == -1 or semi_start < brace_start):
            i = semi_start + 1
            continue

        if brace_start == -1:
            break

        depth = 1
        j = brace_start + 1
        while j < n and depth > 0:
            if text[j] == '{':
                depth += 1
            elif text[j] == '}':
                depth -= 1
            j += 1

        body = text[brace_start + 1:j - 1]
        i = j

        if is_struct and name:
            qname = '::'.join(ns_stack + [parent_name, name])
            fields = parse_fields_from_body(body)
            if fields:
                structs.append((qname, fields, list(ns_stack)))
                nested_s, nested_e = _extract_nested_types(body, ns_stack, parent_name + '::' + name)
                structs.extend(nested_s)
                enums.extend(nested_e)

        elif is_enum and name:
            qname = '::'.join(ns_stack + [parent_name, name])
            values = []
            body_clean = re.sub(r':\s*\w+\s*', '', body)
            for part in body_clean.split(','):
                part = part.strip()
                if not part:
                    continue
                vm = re.match(r'([A-Za-z_][A-Za-z0-9_]*)', part)
                if vm:
                    values.append(vm.group(1))
            if values:
                enums.append((qname, values, is_enum_class, list(ns_stack)))

    return structs, enums


def parse_preprocessed(text):
    """
    Parse preprocessed C++ text and return:
      structs: list of (qualified_name, field_names, namespace_stack)
      enums:   list of (qualified_name, value_names, is_scoped, namespace_stack)
    """
    structs = []
    enums = []
    i = 0
    n = len(text)
    ns_stack = []
    brace_depth = 0
    ns_pushed_at_depth = {}

    # Pattern to detect struct/class/enum keywords
    keyword_re = re.compile(r'\b(namespace|struct|class|enum\s+class|enum)\b')

    while i < n:
        # Skip whitespace
        while i < n and text[i].isspace():
            i += 1
        if i >= n:
            break

        # Track braces for namespace management
        if text[i] == '{':
            brace_depth += 1
            i += 1
            continue
        if text[i] == '}':
            brace_depth -= 1
            i += 1
            while i < n and text[i].isspace():
                i += 1
            if i < n and text[i] == ';':
                i += 1
            num_to_pop = ns_pushed_at_depth.pop(brace_depth + 1, 0)
            for _ in range(num_to_pop):
                if ns_stack:
                    ns_stack.pop()
            continue

        m = keyword_re.match(text[i:])
        if not m:
            i += 1
            continue

        keyword = m.group(1)
        is_enum_class = (keyword == 'enum class')
        is_enum = keyword.startswith('enum')
        is_struct = (keyword in ('struct', 'class'))
        keyword_start = i
        i += m.end()

        while i < n and text[i].isspace():
            i += 1

        # Handle namespace
        if keyword == 'namespace':
            # Check if this is a using namespace directive or a namespace alias
            # (which contains a semicolon ';' before an opening brace '{')
            brace_start = text.find('{', i)
            semi_start = text.find(';', i)
            if semi_start != -1 and (brace_start == -1 or semi_start < brace_start):
                i = semi_start + 1
                continue

            # Match qualified C++17 nested namespace name (e.g. app::files)
            m2 = re.match(r'([A-Za-z_][A-Za-z0-9_]*(?:\s*::\s*[A-Za-z_][A-Za-z0-9_]*)*)', text[i:])
            pushed_count = 0
            if m2:
                ns_parts = [part.strip() for part in m2.group(1).split('::')]
                for part in ns_parts:
                    if part:
                        ns_stack.append(part)
                        pushed_count += 1
                i += m2.end()
            while i < n and text[i].isspace():
                i += 1
            if i < n and text[i] == '{':
                brace_depth += 1
                ns_pushed_at_depth[brace_depth] = pushed_count
                i += 1
            continue

        # Read type name
        m2 = re.match(r'([A-Za-z_][A-Za-z0-9_]*)', text[i:])
        name = m2.group(1) if m2 else None
        if m2:
            i += m2.end()

        # Skip anonymous structs/unions unless they are typedef-ed
        if is_struct and not name:
            prev_boundary = max(
                text.rfind(';', 0, keyword_start),
                text.rfind('}', 0, keyword_start)
            )
            prev_chunk = text[prev_boundary + 1:keyword_start]
            is_typedef = bool(re.search(r'\btypedef\b', prev_chunk))
            
            if not is_typedef:
                brace_start = text.find('{', i)
                if brace_start == -1:
                    break
                depth = 1
                j = brace_start + 1
                while j < n and depth > 0:
                    if text[j] == '{':
                        depth += 1
                    elif text[j] == '}':
                        depth -= 1
                    j += 1
                i = j
                continue

        # Find opening brace
        brace_start = text.find('{', i)
        semi_start = text.find(';', i)

        if semi_start != -1 and (brace_start == -1 or semi_start < brace_start):
            i = semi_start + 1
            continue

        if brace_start == -1:
            break

        # Find matching closing brace
        depth = 1
        j = brace_start + 1
        while j < n and depth > 0:
            if text[j] == '{':
                depth += 1
            elif text[j] == '}':
                depth -= 1
            j += 1

        body = text[brace_start + 1:j - 1]
        i = j
        while i < n and text[i].isspace():
            i += 1

        if is_struct and not name:
            # We must have is_typedef since we didn't skip it.
            # Let's read the typedef name after the closing brace.
            m_typedef = re.match(r'([A-Za-z_][A-Za-z0-9_]*)', text[i:])
            if m_typedef:
                name = m_typedef.group(1)
                i += m_typedef.end()

        if is_struct and name:
            # Skip template definitions: search backward from the keyword to
            # the previous semicolon or closing brace.
            prev_boundary = max(
                text.rfind(';', 0, keyword_start),
                text.rfind('}', 0, keyword_start)
            )
            prev_chunk = text[prev_boundary + 1:keyword_start]
            if re.search(r'\btemplate\s*<', prev_chunk):
                if i < n and text[i] == ';':
                    i += 1
                continue

            qname = '::'.join(ns_stack + [name])
            fields = parse_fields_from_body(body)
            if fields:
                structs.append((qname, fields, list(ns_stack)))
                nested_s, nested_e = _extract_nested_types(body, ns_stack, name)
                structs.extend(nested_s)
                enums.extend(nested_e)

        elif is_enum and name:
            qname = '::'.join(ns_stack + [name])
            values = []
            # Remove enum base like ": uint8_t"
            body = re.sub(r':\s*\w+\s*', '', body)
            for part in body.split(','):
                part = part.strip()
                if not part:
                    continue
                vm = re.match(r'([A-Za-z_][A-Za-z0-9_]*)', part)
                if vm:
                    values.append(vm.group(1))
            if values:
                enums.append((qname, values, is_enum_class, list(ns_stack)))

    return structs, enums


def generate_field_emissions(fields, val_expr, indent_str, lines):
    for f in fields:
        if isinstance(f, tuple) and len(f) == 4 and f[0] == 'array':
            _, array_name, _, inner_fields = f
            if "filler" in array_name.lower():
                continue
            
            # Check if there are any non-filler fields inside this array.
            has_non_filler = False
            for inf in inner_fields:
                if isinstance(inf, tuple):
                    has_non_filler = True
                    break
                elif not "filler" in inf.lower():
                    has_non_filler = True
                    break
            if not has_non_filler:
                continue

            size_expr = "sizeof({}.{}) / sizeof({}.{}[0])".format(val_expr, array_name, val_expr, array_name)
            lines.append("{}dd.begin_array(\"{}\", {});".format(indent_str, array_name, size_expr))
            lines.append("{}for (size_t i = 0; i < {}; ++i) {{".format(indent_str, size_expr))
            lines.append("{}  dd.begin_array_element(i);".format(indent_str))
            
            generate_field_emissions(inner_fields, "{}.{}[i]".format(val_expr, array_name), indent_str + "  ", lines)
            
            lines.append("{}  dd.end_array_element();".format(indent_str))
            lines.append("{}}}".format(indent_str))
            lines.append("{}dd.end_array();".format(indent_str))
        else:
            # f is a simple string field
            if "filler" in f.lower():
                continue
            lines.append("{}dd.field(\"{}\", {}.{});".format(indent_str, f, val_expr, f))


def generate_header(input_headers, structs, enums):
    lines = []
    lines.append("// Auto-generated by generate_dd_dump.py")
    lines.append("// Do not edit manually.")
    lines.append("")
    lines.append('#include "data_dumper.h"')
    for hdr in input_headers:
        lines.append('#include "{}"'.format(hdr))
    lines.append("")

    # Group struct free functions by namespace
    namespace_groups = {}
    for qname, fields, ns_stack in structs:
        key = tuple(ns_stack)
        body = []
        fqname = qname if qname.startswith("::") else "::" + qname
        body.append("inline void dd_dump(const {}& val, DataDumper& dd) {{".format(fqname))
        generate_field_emissions(fields, "val", "  ", body)
        body.append("}")
        namespace_groups.setdefault(key, []).append("\n".join(body))

    for key in sorted(namespace_groups.keys()):
        bodies = namespace_groups[key]
        if key:
            for ns in key:
                lines.append("namespace " + ns + " {")
            lines.append("")
            for b in bodies:
                lines.append(b)
                lines.append("")
            for _ in key:
                lines.append("}")
            lines.append("")
        else:
            for b in bodies:
                lines.append(b)
                lines.append("")

    # Emit enum specializations at global scope
    for qname, values, is_scoped, ns_stack in sorted(enums, key=lambda x: x[0]):
        fqname = qname if qname.startswith("::") else "::" + qname
        lines.append("template <>")
        lines.append("struct EnumTraits<{}> {{".format(fqname))
        lines.append("  static std::string to_string({} val) {{".format(fqname))
        lines.append("    switch (val) {")
        for v in values:
            if is_scoped:
                lines.append("    case {}::{}: return \"{}::{}\";".format(fqname, v, qname, v))
            else:
                lines.append("    case {}::{}: return \"{}\";".format(fqname, v, v))
        lines.append("    default:")
        lines.append("      break;")
        lines.append("    }")
        lines.append("    std::ostringstream oss;")
        lines.append('    oss << "{}" << "(" <<'.format(qname))
        lines.append("        static_cast<typename std::underlying_type<{}>::type>(val) << \")\";".format(fqname))
        lines.append("    return oss.str();")
        lines.append("  }")
        lines.append("};")
        lines.append("")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Generate DataDumper registration for upstream C++ headers."
    )
    parser.add_argument("headers", nargs="+", help="Input header files")
    parser.add_argument("-I", dest="includes", action="append", default=[],
                        help="Additional include directories")
    parser.add_argument("-o", dest="output", required=True,
                        help="Output generated header file")
    args = parser.parse_args()

    preproc = run_cpp_preprocessor(args.headers, args.includes)
    target_text = extract_non_system_content(preproc)
    target_text = clean_preprocessor_directives(target_text)
    structs, enums = parse_preprocessed(target_text)

    # Deduplicate by qualified name (last one wins)
    seen_structs = {}
    for qname, fields, ns in structs:
        seen_structs[qname] = (fields, ns)
    deduped_structs = [(q, f, n) for q, (f, n) in seen_structs.items()]

    seen_enums = {}
    for qname, values, scoped, ns in enums:
        seen_enums[qname] = (values, scoped, ns)
    deduped_enums = [(q, v, s, n) for q, (v, s, n) in seen_enums.items()]

    output = generate_header(args.headers, deduped_structs, deduped_enums)

    out_dir = os.path.dirname(args.output)
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    with open(args.output, "w") as f:
        f.write(output)

    print("Generated:", args.output)
    print("  Structs:", len(deduped_structs))
    print("  Enums:  ", len(deduped_enums))


if __name__ == "__main__":
    main()
