// The script front end: T4's grammar and the VM rules (docs/T4_SCRIPT_LANGUAGE.md).
#include "gsc/ast.h"
#include "core/files.h"
#include "gsc/port/link_check.h"
#include "gsc/port/types.h"
#include "gsc/port/vm_rules.h"

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
        return port::ApplyVmRules(tokens, {"run"}).watched;
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
    port::ApplyVmRules(tokens, {"run"});
    const auto text = Emit(tokens);
    CHECK(text.starts_with("f( x )\n{ waw2bo2_turns = 0;\n\twhile ( x ) { waw2bo2_turns++;"));
    CHECK(text.ends_with("if ( waw2bo2_turns > 100 ) { waw2bo2_turns = 0; run::waw_watch( \"f\" ); } // forever\n\t\tg(); }\n\th();\n}\n"));
    CHECK(Accepts(text));
}

TEST_CASE("T4's results where T6's differ", "[gsc]")
{
    const auto lower = [](const std::string& body) {
        auto tokens = Tokenize("f() { " + body + " }");
        port::ApplyVmRules(tokens, {"run"});
        const auto text = Emit(tokens);
        CHECK(Accepts(text));
        return text.substr(6, text.size() - 8);
    };
    // a value that cannot match runs the default label
    CHECK(lower("switch ( a.b ) { case 1: c(); break; default: d(); }") == "switch ( run::waw_switch( a.b ) ) { case 1: c(); break; default: d(); }");
    CHECK(lower("switch ( a ) { case 1: c(); }") == "switch ( a ) { case 1: c(); }");
    CHECK(lower("switch ( 2 ) { default: c(); }") == "switch ( 2 ) { default: c(); }");
    // vectors of parts that may not be numbers
    CHECK(lower("a = ( 1, -2, 3 * 4 );") == "a = ( 1, -2, 3 * 4 );");
    CHECK(lower("a = ( 0, randomint( 5 ), 0 );") == "a = ( 0, randomint( 5 ), 0 );");
    CHECK(lower("a = b + ( c[0], d.e, 0 );") == "a = b + run::waw_vector ( c[0], d.e, 0 );");
    CHECK(lower("a = ( b, 0, 0 )[0];") == "a = ( b, 0, 0 )[0];");
    CHECK(lower("a = ( b );") == "a = ( b );");
    // && and || give 0 or 1
    CHECK(lower("a = b && c;") == "a = b && !( !( c ) );");
    CHECK(lower("a = b || c == 1;") == "a = b || c == 1;");
    CHECK(lower("a = b && ( c || d.e );") == "a = b && ( c || !( !( d.e ) ) );");
    CHECK(lower("if ( b && c ) d();") == "if ( b && c ) d();");
    CHECK(lower("if ( e( b && c ) ) d();") == "if ( e( b && !( !( c ) ) ) ) d();");
    CHECK(lower("return !( b && c );") == "return !( b && c );");
    CHECK(lower("a = b[c && d[0]];") == "a = b[c && !( !( d[0] ) )];");
    // comparisons
    CHECK(lower("a = []; if ( a == b.d ) c();") == "a = []; if ( run::waw_equal( a , b.d ) ) c();");
    CHECK(lower("if ( [] != b.c ) d();") == "if ( run::waw_not_equal( [] , b.c ) ) d();");
    CHECK(lower("if ( a == b ) c();") == "if ( a == b ) c();"); // two locals never set
    CHECK(lower("if ( self.a == self.b ) c();") == "if ( run::waw_equal( self.a , self.b ) ) c();");
    CHECK(lower("if ( self.a == 1 || self.b == undefined ) c();") == "if ( self.a == 1 || self.b == undefined ) c();");
    CHECK(lower("if ( self.a < distance( b, c ) ) d();") == "if ( self.a < distance( b, c ) ) d();");
    CHECK(lower("if ( a() == b() ) c();") == "if ( a() == b() ) c();");
    CHECK(lower("if ( b.c < \"d\" ) e();") == "if ( run::waw_less( b.c , \"d\" ) ) e();");
    CHECK(lower("a = getdvar( \"b\" ); if ( a >= c.d ) e();") == "a = getdvar( \"b\" ); if ( run::waw_greater_equal( a , c.d ) ) e();");
    CHECK(lower("a = \"b\"; if ( a < 5 ) c();") == "a = \"b\"; if ( a < 5 ) c();"); // a fault in both
    CHECK(lower("a = 1; if ( a < b.c ) d();") == "a = 1; if ( a < b.c ) d();");
    CHECK(lower("a = getentarray( \"b\" ); if ( a == 1 ) c();") == "a = getentarray( \"b\" ); if ( a == 1 ) c();");
    CHECK(lower("level.a[0] = 1; if ( level.a == b.c ) d();") == "level.a[0] = 1; if ( run::waw_equal( level.a , b.c ) ) d();");
    CHECK(lower("if ( a == \"b\" ) c();") == "if ( a == \"b\" ) c();");
    // brackets keep their text
    CHECK(lower("[[ a ]](); self [[ b.c ]]( d[e[0]] );") == "[[ a ]](); self [[ b.c ]]( d[e[0]] );");
}

TEST_CASE("types are inferred over a script", "[gsc]")
{
    const auto tokens = Tokenize("f( p ) { a = 1; a = \"x\" + p; b = g(); c = self.arr; d = p; e = a.size; }\ng() { return []; }\nh() { self.arr[0] = 1; }");
    const auto ast = ParseAst(tokens);
    const port::TypeInference types(ast, tokens);
    const auto function = ast.Functions()[0];
    const auto block = ast[function].kids[1];
    const auto of = [&](const size_t statement) { return types.Of(function, ast[ast[block].kids[statement]].kids[0]); };
    CHECK(of(0).Known(port::TYPE_STRING));
    CHECK(of(0).Known(port::TYPE_NUMBER));
    CHECK(of(2).Known(port::TYPE_ARRAY)); // through g's return
    CHECK_FALSE(of(2).May(port::TYPE_STRING));
    CHECK(of(3).Known(port::TYPE_ARRAY)); // stored in h
    CHECK(of(3).May(port::TYPE_STRING));  // and by other scripts
    CHECK(of(4).open);
    CHECK_FALSE(of(5).open);
}

TEST_CASE("what T6 would not resolve is reported", "[gsc]")
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "w2b_link_check_test";
    fs::remove_all(root);
    fs::create_directories(root / "maps");
    const auto write = [&](const char* name, const std::string& text) { w2b::files::WriteText(root / "maps" / name, text); };
    write("a.gsc", "#include maps\\b;\nmain()\n{\n\tin_b( 1 );\n\tmaps\\b::in_b();\n\tx = ::in_b;\n\tnative( 1 );\n\tself method();\n\t/# nothing(); #/\n}\n");
    write("b.gsc", "in_b( x )\n{\n}\n");
    port::T6Api api;
    api.builtins["native"] = {1, 2};
    api.methods["method"] = {0, 0};
    CHECK(port::LinkCheck(root, api).empty());
    write("a.gsc", "main()\n{\n\tin_b();\n\tmaps\\b::gone();\n\tmaps\\c::gone();\n\tmaps\\b::in_b( 1, 2 );\n\tnative();\n\tmethod();\n\tx = ::gone;\n}\nmain()\n{\n}\n");
    CHECK(port::LinkCheck(root, api).size() == 8);
    CHECK(port::LinkCheck(root, api, {"maps\\b"}).empty());
    fs::remove_all(root);
}
