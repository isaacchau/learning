// ============================================================================
// tests/test_ini_parser.cpp — Unit tests for the INI parser
// ============================================================================
// These tests verify the IniFile parser in isolation before it is integrated
// into config_parser.cpp.  Run with: g++ -std=c++14 -I. tests/test_ini_parser.cpp ini_parser.cpp -o test_ini && ./test_ini
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <functional>

// Simple test framework (same pattern as test_main.cpp)
struct Test {
    const char* name;
    std::function<bool()> func;
};

static std::vector<Test> g_tests;
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) \
    static bool test_##name(); \
    static struct test_##name##_registrar { \
        test_##name##_registrar() { \
            g_tests.push_back({#name, test_##name}); \
        } \
    } test_##name##_instance; \
    static bool test_##name()

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) { \
        printf("    ASSERT_TRUE(%s) FAILED at line %d\n", #expr, __LINE__); \
        return false; \
    } } while(0)

#define ASSERT_FALSE(expr) \
    do { if (expr) { \
        printf("    ASSERT_FALSE(%s) FAILED at line %d\n", #expr, __LINE__); \
        return false; \
    } } while(0)

#define ASSERT_EQ(expected, actual) \
    do { if ((expected) != (actual)) { \
        printf("    ASSERT_EQ(%s, %s) FAILED at line %d: expected %lld, got %lld\n", \
               #expected, #actual, __LINE__, (long long)(expected), (long long)(actual)); \
        return false; \
    } } while(0)

#define ASSERT_STREQ(expected, actual) \
    do { if (std::string(expected) != std::string(actual)) { \
        printf("    ASSERT_STREQ(%s, %s) FAILED at line %d: expected '%s', got '%s'\n", \
               #expected, #actual, __LINE__, expected, actual); \
        return false; \
    } } while(0)

#include "../ini_parser.h"

// ============================================================================
// Basic parsing tests
// ============================================================================

TEST(ini_parse_empty) {
    IniFile ini;
    std::string error;
    bool result = ini.parseString("", error);
    ASSERT_TRUE(result);
    ASSERT_EQ(0, ini.sectionCount("global"));
    return true;
}

TEST(ini_parse_simple_section) {
    IniFile ini;
    std::string error;
    bool result = ini.parseString(
        "[global]\n"
        "workers = 4\n"
        "host = 127.0.0.1\n",
        error);
    ASSERT_TRUE(result);
    ASSERT_EQ(1, ini.sectionCount("global"));
    ASSERT_EQ(4, ini.getInt("global", 0, "workers"));
    ASSERT_STREQ("127.0.0.1", ini.getString("global", 0, "host").c_str());
    return true;
}

TEST(ini_parse_comments) {
    IniFile ini;
    std::string error;
    bool result = ini.parseString(
        "; this is a comment\n"
        "[global]\n"
        "# another comment\n"
        "workers = 4\n",
        error);
    ASSERT_TRUE(result);
    ASSERT_EQ(4, ini.getInt("global", 0, "workers"));
    return true;
}

TEST(ini_parse_whitespace_variations) {
    IniFile ini;
    std::string error;
    bool result = ini.parseString(
        "  [global]  \n"
        "  workers   =   8  \n"
        "port=8888\n",
        error);
    ASSERT_TRUE(result);
    ASSERT_EQ(8, ini.getInt("global", 0, "workers"));
    ASSERT_EQ(8888, ini.getInt("global", 0, "port"));
    return true;
}

TEST(ini_parse_quoted_values) {
    IniFile ini;
    std::string error;
    bool result = ini.parseString(
        "[connection]\n"
        "host = \"127.0.0.1\"\n"
        "item = 'AAPL'\n",
        error);
    ASSERT_TRUE(result);
    ASSERT_STREQ("127.0.0.1", ini.getString("connection", 0, "host"));
    ASSERT_STREQ("AAPL", ini.getString("connection", 0, "item"));
    return true;
}

TEST(ini_parse_repeated_sections) {
    IniFile ini;
    std::string error;
    bool result = ini.parseString(
        "[connection]\n"
        "host = primary.example.com\n"
        "port = 8888\n"
        "[connection]\n"
        "host = backup.example.com\n"
        "port = 8889\n",
        error);
    ASSERT_TRUE(result);
    ASSERT_EQ(2, ini.sectionCount("connection"));
    ASSERT_STREQ("primary.example.com", ini.getString("connection", 0, "host"));
    ASSERT_EQ(8888, ini.getInt("connection", 0, "port"));
    ASSERT_STREQ("backup.example.com", ini.getString("connection", 1, "host"));
    ASSERT_EQ(8889, ini.getInt("connection", 1, "port"));
    return true;
}

TEST(ini_parse_dotted_section_names) {
    IniFile ini;
    std::string error;
    bool result = ini.parseString(
        "[memory_pool.class_5]\n"
        "initial = 512\n"
        "max_free = 1024\n"
        "[memory_pool.class_0]\n"
        "initial = 10\n",
        error);
    ASSERT_TRUE(result);
    ASSERT_EQ(1, ini.sectionCount("memory_pool.class_5"));
    ASSERT_EQ(512, ini.getInt("memory_pool.class_5", 0, "initial"));
    ASSERT_EQ(1, ini.sectionCount("memory_pool.class_0"));
    ASSERT_EQ(10, ini.getInt("memory_pool.class_0", 0, "initial"));
    return true;
}

// ============================================================================
// Type conversion tests
// ============================================================================

TEST(ini_getInt_variations) {
    IniFile ini;
    std::string error;
    ini.parseString(
        "[test]\n"
        "positive = 42\n"
        "negative = -5\n"
        "zero = 0\n"
        "large = 999999\n",
        error);
    ASSERT_EQ(42, ini.getInt("test", 0, "positive"));
    ASSERT_EQ(-5, ini.getInt("test", 0, "negative"));
    ASSERT_EQ(0, ini.getInt("test", 0, "zero"));
    ASSERT_EQ(999999, ini.getInt("test", 0, "large"));
    return true;
}

TEST(ini_getUint64) {
    IniFile ini;
    std::string error;
    ini.parseString(
        "[test]\n"
        "seq = 18446744073709551615\n"
        "window = 5000\n",
        error);
    ASSERT_EQ(5000ULL, ini.getUint64("test", 0, "window"));
    // Note: stoull max is typically ULLONG_MAX, test with a reasonable large value
    ASSERT_EQ(18446744073709551615ULL, ini.getUint64("test", 0, "seq"));
    return true;
}

TEST(ini_getSizeT) {
    IniFile ini;
    std::string error;
    ini.parseString(
        "[test]\n"
        "queue_size = 16384\n",
        error);
    ASSERT_EQ(16384, ini.getSizeT("test", 0, "queue_size"));
    return true;
}

TEST(ini_getBool_variations) {
    IniFile ini;
    std::string error;
    ini.parseString(
        "[test]\n"
        "t1 = true\n"
        "t2 = yes\n"
        "t3 = 1\n"
        "t4 = on\n"
        "f1 = false\n"
        "f2 = no\n"
        "f3 = 0\n"
        "f4 = off\n",
        error);
    ASSERT_TRUE(ini.getBool("test", 0, "t1"));
    ASSERT_TRUE(ini.getBool("test", 0, "t2"));
    ASSERT_TRUE(ini.getBool("test", 0, "t3"));
    ASSERT_TRUE(ini.getBool("test", 0, "t4"));
    ASSERT_FALSE(ini.getBool("test", 0, "f1"));
    ASSERT_FALSE(ini.getBool("test", 0, "f2"));
    ASSERT_FALSE(ini.getBool("test", 0, "f3"));
    ASSERT_FALSE(ini.getBool("test", 0, "f4"));
    return true;
}

TEST(ini_defaults) {
    IniFile ini;
    std::string error;
    ini.parseString("[test]\n", error);
    ASSERT_EQ(99, ini.getInt("test", 0, "missing", 99));
    ASSERT_STREQ("default", ini.getString("test", 0, "missing", "default"));
    ASSERT_TRUE(ini.getBool("test", 0, "missing", true));
    ASSERT_EQ(0, ini.getInt("nonexistent", 0, "key", 0));
    return true;
}

// ============================================================================
// Error handling tests
// ============================================================================

TEST(ini_error_empty_section) {
    IniFile ini;
    std::string error;
    bool result = ini.parseString("[]\n", error);
    ASSERT_FALSE(result);
    ASSERT_TRUE(!error.empty());
    return true;
}

TEST(ini_error_no_equals) {
    IniFile ini;
    std::string error;
    bool result = ini.parseString(
        "[test]\n"
        "badline\n",
        error);
    ASSERT_FALSE(result);
    ASSERT_TRUE(!error.empty());
    return true;
}

TEST(ini_error_key_outside_section) {
    IniFile ini;
    std::string error;
    bool result = ini.parseString("key = value\n", error);
    ASSERT_FALSE(result);
    ASSERT_TRUE(!error.empty());
    return true;
}

TEST(ini_error_empty_key) {
    IniFile ini;
    std::string error;
    bool result = ini.parseString(
        "[test]\n"
        "= value\n",
        error);
    ASSERT_FALSE(result);
    ASSERT_TRUE(!error.empty());
    return true;
}

// ============================================================================
// Full config mapping tests (mimic tcpclient config structure)
// ============================================================================

TEST(ini_full_tcpclient_config) {
    IniFile ini;
    std::string error;
    bool result = ini.parseString(
        "; tcpclient example config\n"
        "[global]\n"
        "workers = 4\n"
        "raw_queue_size = 16384\n"
        "decoded_queue_size = 16384\n"
        "reconnect_interval_ms = 3000\n"
        "queue_push_timeout_ms = 5\n"
        "\n"
        "[connection]\n"
        "host = primary.example.com\n"
        "port = 8888\n"
        "failover_retries = 2\n"
        "item = AAPL\n"
        "client_id = Client1\n"
        "starting_seq = 0\n"
        "\n"
        "[connection]\n"
        "host = backup.example.com\n"
        "port = 8889\n"
        "item = AAPL\n"
        "client_id = Client1\n"
        "starting_seq = 0\n"
        "\n"
        "[aggregation]\n"
        "enabled = true\n"
        "window_ms = 1000\n"
        "output_format = csv\n"
        "output_dir = /tmp/metrics\n"
        "filename_prefix = agg\n"
        "\n"
        "[memory_pool.class_5]\n"
        "initial = 512\n"
        "max_free = 1024\n"
        "max_total = 8192\n",
        error);

    ASSERT_TRUE(result);

    // Global
    ASSERT_EQ(4, ini.getInt("global", 0, "workers"));
    ASSERT_EQ(16384, ini.getSizeT("global", 0, "raw_queue_size"));
    ASSERT_EQ(16384, ini.getSizeT("global", 0, "decoded_queue_size"));
    ASSERT_EQ(3000, ini.getInt("global", 0, "reconnect_interval_ms"));
    ASSERT_EQ(5, ini.getInt("global", 0, "queue_push_timeout_ms"));

    // Connections
    ASSERT_EQ(2, ini.sectionCount("connection"));
    ASSERT_STREQ("primary.example.com", ini.getString("connection", 0, "host"));
    ASSERT_EQ(8888, ini.getInt("connection", 0, "port"));
    ASSERT_EQ(2, ini.getInt("connection", 0, "failover_retries"));
    ASSERT_STREQ("AAPL", ini.getString("connection", 0, "item"));
    ASSERT_STREQ("Client1", ini.getString("connection", 0, "client_id"));
    ASSERT_EQ(0, ini.getUint64("connection", 0, "starting_seq"));

    ASSERT_STREQ("backup.example.com", ini.getString("connection", 1, "host"));
    ASSERT_EQ(8889, ini.getInt("connection", 1, "port"));

    // Aggregation
    ASSERT_TRUE(ini.getBool("aggregation", 0, "enabled"));
    ASSERT_EQ(1000, ini.getUint64("aggregation", 0, "window_ms"));
    ASSERT_STREQ("csv", ini.getString("aggregation", 0, "output_format"));
    ASSERT_STREQ("/tmp/metrics", ini.getString("aggregation", 0, "output_dir"));
    ASSERT_STREQ("agg", ini.getString("aggregation", 0, "filename_prefix"));

    // Memory pool
    ASSERT_EQ(1, ini.sectionCount("memory_pool.class_5"));
    ASSERT_EQ(512, ini.getInt("memory_pool.class_5", 0, "initial"));
    ASSERT_EQ(1024, ini.getInt("memory_pool.class_5", 0, "max_free"));
    ASSERT_EQ(8192, ini.getInt("memory_pool.class_5", 0, "max_total"));

    return true;
}

TEST(ini_parse_from_file) {
    const char* tmpfile = "/tmp/test_ini_parser_file.ini";
    FILE* f = std::fopen(tmpfile, "w");
    ASSERT_TRUE(f != nullptr);
    std::fprintf(f, "[global]\n");
    std::fprintf(f, "workers = 8\n");
    std::fprintf(f, "[connection]\n");
    std::fprintf(f, "host = 127.0.0.1\n");
    std::fclose(f);

    IniFile ini;
    std::string error;
    bool result = ini.parseFile(tmpfile, error);

    ASSERT_TRUE(result);
    ASSERT_EQ(8, ini.getInt("global", 0, "workers"));
    ASSERT_STREQ("127.0.0.1", ini.getString("connection", 0, "host"));

    std::remove(tmpfile);
    return true;
}

// ============================================================================
// Main test runner
// ============================================================================

int main() {
    printf("Running INI parser tests...\n");
    printf("===========================\n\n");

    for (const auto& test : g_tests) {
        printf("TEST: %s ... ", test.name);
        bool passed = test.func();
        if (passed) {
            printf("PASSED\n");
            ++g_passed;
        } else {
            printf("FAILED\n");
            ++g_failed;
        }
    }

    printf("\n===========================\n");
    printf("Results: %d passed, %d failed out of %zu tests\n",
           g_passed, g_failed, g_tests.size());

    return g_failed > 0 ? 1 : 0;
}
