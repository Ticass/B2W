// The script front end: T4's grammar and the VM rules (docs/T4_SCRIPT_LANGUAGE.md).
#include "gsc/ast.h"
#include "gsc/port/loops.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace w2b::gsc;

namespace
{
    std::string Dump(const std::string& source)
    {
        const auto tokens = Tokenize(source);
        return DumpAst(ParseAst(tokens), tokens);
    }

    // the statements of "main() { <body> }"
    std::string Body(const std::string& body)
    {
        auto text = Dump("main() { " + body + " }");
        const std::string head = "(func main (block ";
        REQUIRE(text.starts_with(head));
        return text.substr(head.size(), text.size() - head.size() - 3);
    }

    bool Accepts(const std::string& source)
    {
        try
        {
            Dump(source);
            return true;
        }
        catch (const GscSyntaxError&)
        {
            return false;
        }
    }

    int Watched(const std::string& body)
    {
        auto tokens = Tokenize("f( p ) { " + body + " }");
        return port::WatchLoops(tokens, "kill").watched;
    }
} // namespace

TEST_CASE("expressions have T4's shape", "[gsc]")
{
    CHECK(Body("a = x || y && z;") == "(set = (var a) (bin || (var x) (bin && (var y) (var z))))");
    CHECK(Body("a = x - y - z;") == "(set = (var a) (bin - (bin - (var x) (var y)) (var z)))");
    CHECK(Body("a = !x.y == z;") == "(set = (var a) (bin == (not (field y (var x))) (var z)))");
    CHECK(Body("a = !y foo();") == "(set = (var a) (not (call method (callee foo) (var y))))");
    CHECK(Body("a = x -5;") == "(set = (var a) (bin - (var x) (int 5)))");
    CHECK(Body("a = - 1.5;") == "(set = (var a) (float - 1.5))");
    CHECK(Body("a = b[c[0]].size;") == "(set = (var a) (size (index (var b) (index (var c) (int 0)))))");
    CHECK(Body("self thread [[ a.b ]]( 1 );") == "(do (call thread method (callee (field b (var a))) (self) (int 1)))");
    CHECK(Body("a() maps\\x::b();") == "(do (call method (callee maps\\x b) (call (callee a))))");
    CHECK(Body("a = ( 1, 2, 3 )[0];") == "(set = (var a) (index (list (int 1) (int 2) (int 3)) (int 0)))");
    CHECK(Body("level waittill( \"a\", b, c );") == "(waittill (level) (str \"a\") (var b) (var c))");
    CHECK(Body("for ( ;; ) { }") == "(for (empty) (empty) (empty) (block))");
    CHECK(Body("if ( a ) b(); else if ( c ) d();") == "(if (var a) (do (call (callee b))) (if (var c) (do (call (callee d)))))");
}

TEST_CASE("what T4's compiler rejects is rejected", "[gsc]")
{
    CHECK(Accepts("main() { ; ;; { } }"));
    CHECK(Accepts("/# main() { } #/ b() { /# c(); #/ }"));
    CHECK(Accepts("main() { } /* runs to the end"));
    for (const auto* body : {"if ( x );", "while ( 1 );", "a = b = c;", "x.y();", "++x;", "a = -x;", "a = [ 1 ];", "a = b ? c : d;", "do { } while ( 1 );",
                             "Return;", "f( a, );", "a.size = 1;", "level waittill( \"a\", b.c );", "self a b();"})
        CHECK_FALSE(Accepts("main() { " + std::string(body) + " }"));
    CHECK_FALSE(Accepts("main() { "));
}

TEST_CASE("loops that end or wait get no watchdog", "[gsc]")
{
    CHECK(Watched("for ( i = 0; i < 5; i++ ) a();") == 0);
    CHECK(Watched("for ( i = 10; i >= 0; i -= 2 ) a();") == 0);
    CHECK(Watched("x = []; for ( i = 0; i < x.size; i++ ) a( x[i] );") == 0);
    CHECK(Watched("x = getentarray( \"a\", \"b\" ); for ( i = x.size - 1; i >= 0; i-- ) a( x[i] );") == 0);
    CHECK(Watched("while ( 1 ) { a(); wait 0.05; }") == 0);
    CHECK(Watched("for ( ;; ) { level waittill( \"a\" ); if ( b ) continue; c(); }") == 0);
    CHECK(Watched("while ( a ) { if ( b ) wait( 1 ); else wait 2; }") == 0);
}

TEST_CASE("loops that may never end get T4's watchdog", "[gsc]")
{
    CHECK(Watched("for ( i = 0; i < p.size; i++ ) a();") == 1);
    CHECK(Watched("for ( i = 0; i < level.x.size; i++ ) a();") == 1);
    CHECK(Watched("if ( p ) x = []; for ( i = 0; i < x.size; i++ ) a();") == 1);
    CHECK(Watched("x = []; for ( i = 0; i < x.size; i++ ) x = b( x );") == 1);
    CHECK(Watched("x = []; for ( i = 0; i < x.size; i++ ) x[x.size] = 1;") == 1);
    CHECK(Watched("for ( i = 0; i < 5; i++ ) i = 0;") == 1);
    CHECK(Watched("for ( i = 0; i < 5; i-- ) a();") == 1);
    CHECK(Watched("while ( 1 ) { if ( b ) continue; wait 1; }") == 1);
    CHECK(Watched("while ( 1 ) { wait p; }") == 1); // an undefined wait does not wait
    CHECK(Watched("while ( p[0] != a ) p = b( p );") == 1);
    CHECK(Watched("while ( a ) while ( b ) c();") == 2);

    auto tokens = Tokenize("f( x )\n{\n\twhile ( x ) // forever\n\t\tg();\n\th();\n}\n");
    const auto ast = ParseAst(tokens);
    CHECK_FALSE(port::LoopIsBounded(ast, tokens, ast.Functions()[0], ast[ast[ast.Functions()[0]].kids[1]].kids[0]));
    port::WatchLoops(tokens, "kill");
    const auto text = Emit(tokens);
    CHECK(text.starts_with("f( x )\n{ waw2bo2_turns = 0;\n\twhile ( x ) { waw2bo2_turns++;"));
    CHECK(text.ends_with("kill( \"f\" ); waw2bo2_frame = gettime(); waw2bo2_turns = 0; } // forever\n\t\tg(); }\n\th();\n}\n"));
    CHECK(Accepts(text));
}

TEST_CASE("a switch reads its value as T4 does", "[gsc]")
{
    const auto lower = [](const std::string& body) {
        auto tokens = Tokenize("f() { " + body + " }");
        const auto changed = port::LowerSwitches(tokens, "reader");
        return std::to_string(changed) + Emit(tokens);
    };
    CHECK(lower("switch ( a.b ) { case 1: c(); break; default: d(); }") == "1f() { switch ( reader( a.b ) ) { case 1: c(); break; default: d(); } }");
    CHECK(lower("switch ( a ) { case 1: c(); }") == "0f() { switch ( a ) { case 1: c(); } }");
    CHECK(lower("switch ( 2 ) { default: c(); }") == "0f() { switch ( 2 ) { default: c(); } }");
}
