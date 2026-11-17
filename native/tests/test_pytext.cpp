// Expectations printed by Python itself: tests/make_expectations.py
#include "core/pytext.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

using namespace w2b;

TEST_CASE("Python string repr preserves quote ordering and escapes non-printable code points")
{
    CHECK(py::StringRepr("test's") == "\"test's\"");
    CHECK(py::StringRepr("\"test's\"") == "'\"test\\'s\"'");
    CHECK(py::StringRepr("line\n\t\\\xC2\xA0é") == "'line\\n\\t\\\\\\xa0é'");
}

TEST_CASE("Compact JSON keeps Python float text and escaping")
{
    const py::Json value{{"a", {-0.0, 1.0, 1e-7}}, {"b", "é"}};
    CHECK(py::JsonDumps(value, -1) == "{\"a\":[-0.0,1.0,1e-07],\"b\":\"\\u00e9\"}");
}

TEST_CASE("FloatRepr matches Python repr(float)")
{
    const std::pair<const char*, const char*> cases[] = {
        {"0x0.0p+0", "0.0"},
        {"-0x0.0p+0", "-0.0"},
        {"0x1.0000000000000p+0", "1.0"},
        {"0x1.0000000000000p-1", "0.5"},
        {"-0x1.0000000000000p+1", "-2.0"},
        {"0x1.4f8b588e368f1p-17", "1e-05"},
        {"0x1.a36e2eb1c432dp-14", "0.0001"},
        {"0x1.d6f3454000000p+26", "123456789.0"},
        {"0x1.1c37937e08000p+53", "1e+16"},
        {"0x1.c6bf526340000p+49", "1000000000000000.0"},
        {"0x1.043561a882930p+67", "1.5e+20"},
        {"0x1.999999999999ap-4", "0.1"},
        {"0x1.5555555555555p-2", "0.3333333333333333"},
        {"0x1.ac9a7b3b7302fp-996", "2.5e-300"},
        {"0x1.ffc0000000000p+15", "65504.0"},
        {"0x1.0f0cf064dd592p+73", "1e+22"},
        {"0x1.5ee2a2eb5a5c4p+53", "1.2345678901234568e+16"},
        {"-0x1.ad7f29abcaf48p-24", "-1e-07"},
        {"0x1.f75104d551d69p-14", "0.00012"},
        {"0x1.921f9f01b866ep+1", "3.14159"},
    };
    for (const auto& [hex, expected] : cases)
    {
        INFO(hex);
        CHECK(py::FloatRepr(std::strtod(hex, nullptr)) == expected);
    }
}

TEST_CASE("JsonDumps matches json.dumps(indent=2)")
{
    py::Json doc = py::Json::object();
    doc["a"] = py::Json::array({1, py::Json::object({{"b", "x\xc3\xa9\x7f\"\\"}})});
    doc["c"] = py::Json::object();
    doc["d"] = py::Json::array();
    doc["e"] = 0.5;
    doc["f"] = true;
    doc["g"] = nullptr;
    doc["h"] = "\xf0\x9f\x98\x80";
    doc["i"] = -3;
    CHECK(py::JsonDumps(doc, 2)
          == "{\n  \"a\": [\n    1,\n    {\n      \"b\": \"x\\u00e9\\u007f\\\"\\\\\"\n    }\n  ],\n  \"c\": {},\n  \"d\": [],\n  \"e\": 0.5,\n  \"f\": "
             "true,\n  \"g\": null,\n  \"h\": \"\\ud83d\\ude00\",\n  \"i\": -3\n}");
}

TEST_CASE("str helpers follow Python")
{
    CHECK(py::SplitLines("a\r\nb\rc\n\nd") == std::vector<std::string_view>{"a", "b", "c", "", "d"});
    CHECK(py::SplitLines("a\n") == std::vector<std::string_view>{"a"});
    CHECK(py::Strip("  x y \t") == "x y");
    CHECK(py::Split("a,,b", ",") == std::vector<std::string_view>{"a", "", "b"});
    CHECK(py::Split("", ",") == std::vector<std::string_view>{""});
    double v = 0;
    CHECK(py::ParseFloat(" -1.5e-3 ", v));
    CHECK(v == -1.5e-3);
    CHECK(py::ParseFloat("1_0", v));
    CHECK(v == 10.0);
    CHECK_FALSE(py::ParseFloat("0x10", v));
    CHECK_FALSE(py::ParseFloat("1.#INF", v));
    CHECK(py::ParseFloat("inf", v));
    long long i = 0;
    bool overflow = false;
    CHECK(py::ParseInt("+42", i, overflow));
    CHECK(i == 42);
    CHECK_FALSE(py::ParseInt("4.2", i, overflow));
    CHECK(py::ParseInt("99999999999999999999999", i, overflow));
    CHECK(overflow);
}
