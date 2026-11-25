# The T4 script language

What World at War does with a script, read from `t4sp.exe` (the unpacked
Plutonium copy) and measured in game. The converter's front end
(`native/src/gsc/ast.*`) and VM rules (`native/src/gsc/port/vm_rules.*`) follow it.
T6 means BO2 zombies under Plutonium.

## Where it was read

| What | Address (t4sp.exe) |
|---|---|
| Parser (bison), 133 rules, 262 states | `0x69AEC0`; tables `0x82ED08`-`0x830A28` |
| Lexer (flex) | `0x69C0D0`; tables `0x830A28`-`0x831D10` |
| Escapes in strings | `0x69BFF0` |
| Source reader: a C string, read to the first NUL | `0x689900` |
| VM | `0x693E80`; its error recovery from `0x696FCD` |
| Error raise / report | `0x693CF0` / `0x68B790` |
| Level script load (the "loading" flag) | `0x503300` |
| T6's VM | `0x8F4380` in `t6zm.exe` (the database in `work/glow_ida`) |

The grammar was proven, not transcribed by eye: running T4's own tables over
1172 unique scripts of 28 custom maps accepts 1169, and the converter's parser
builds the same tree for each (2,578,642 nodes, no difference). Two of the
other three only start with a byte order mark, which T4 rejects and the
converter skips; the third is a syntax error in both.

## Lexical rules

- Keywords are lower case: `return wait thread undefined self level game anim
  if else while for waittill waittillmatch waittillframeend notify endon switch
  case default break continue true false breakpoint prof_begin prof_end
  breakon`. `Return` or `IF` is an identifier. There is no `foreach`, `do`,
  `const` or ternary.
- Identifiers are ASCII `[A-Za-z_][A-Za-z0-9_]*`; a path is identifiers joined
  by `\`.
- Numbers are digits, or digits with a `.` and digits. No exponent, no hex, no
  trailing dot.
- Strings stay on one line. `\n`, `\r`, `\t` are escapes; any other `\c` is
  `c`. `&"..."` is one token.
- `.size` is one token when written together; `x. size` is a field named size.
- `//` ends at the line. `/*` ends at `*/`, or silently at the end of the file.
- White space is space, tab, CR and LF. Any other byte outside strings and
  comments (form feed, a byte order mark) is "bad token", a compile error.

## Grammar

    script     = { "#include" path ";" } { declaration }
    declaration = name "(" [ name { "," name } ] ")" block
                | "#using_animtree" "(" string ")" ";" | "/#" | "#/"
    block      = "{" { item } "}"
    item       = ";" | "case" primary ":" | "default" ":" | statement
    statement  = block | simple ";" | "/#" { item } "#/"
               | "if" "(" expr ")" statement [ "else" statement ]
               | "while" "(" expr ")" statement
               | "for" "(" item [ expr ] ";" [ simple ] ")" statement
               | "switch" "(" expr ")" block
    simple     = call | target "=" expr | target op"=" expr | target "++" | target "--"
               | "return" [ expr ] | "wait" expr | "waittillframeend" | "break" | "continue"
               | primary "waittill" "(" expr { "," name } ")"
               | primary "waittillmatch" "(" expr { "," expr } ")"
               | primary "notify" "(" expr { "," expr } ")"
               | primary "endon" "(" expr ")"
    target     = name | primary "." name | primary "[" expr "]"
    primary    = number | "-" number | string | "&"string | "undefined" | "self" | "level"
               | "game" | "anim" | "true" | "false" | "[" "]" | "%" name | "#animtree"
               | "(" [ expr { "," expr } ] ")" | target | primary ".size"
               | [ path ] "::" name | call
    call       = [ primary ] [ "thread" ] callee "(" [ expr { "," expr } ] ")"
    callee     = [ path "::" ] name | "[[" expr "]]"

Operators, loosest first, all left associative: `||`, `&&`, `|`, `^`, `&`,
`== !=`, `< > <= >=`, `<< >>`, `+ -`, `* / %`; `!` and `~` bind tighter. So:

- `if ( x );` and `while ( x );` do not compile; a statement cannot be empty
  there.
- `-x` does not compile; only a number has a sign.
- A waittill's results are plain local variables.
- Arguments are evaluated last to first (`f( a(), b() )` calls `b` first); the
  operands of an operator first to last; an index after the assigned value.
- Constant expressions are folded by the compiler, and a type mismatch between
  literals is a compile error.

## Faults and the loop watchdog

A bad operation is a fault. Neither game ends the thread: the VM raises it with
`longjmp`, and the recovery cleans the stack and goes on.

- The failed expression is `undefined`. A failed builtin call returns
  `undefined`.
- A failed store does nothing.
- A condition that cannot be read as a boolean does not jump: `if ( undefined )`
  runs its body and `while ( undefined )` loops. `!undefined` is undefined, so
  `if ( !undefined )` runs its body too.
- Any comparison with undefined is undefined, except `undefined == undefined`,
  which is true.
- `wait undefined` does not wait.
- A method call on undefined is skipped, as is a thread on undefined.

So `for ( i = 0; i < x.size; i++ )` with an undefined `x` never ends by itself.
T4's backward jump (opcode 99) reads a clock: 2500 ms after the engine last
entered the VM it prints "potential infinite loop in script - killing thread"
and ends the thread with every function it is in, then restarts the clock.
While the level's scripts load it only warns and goes on.

T6's engine has no such clock. Plutonium adds one, off by default
(`scr_kill_infinite_loops 0`, `scr_max_loop_time 2500`); on, it does not
restart the clock after a kill, so it ends every later thread of that frame
that loops (16 unrelated ones in a Der Riese test). The converter leaves it off
and gives the watchdog to the loops that need it: every 100 turns a loop not
proven to end or to wait calls `waw_watch`, which parks the thread when the
time is up. Nothing after the loop runs, in the thread or its callers, as in
T4; other threads go on. The clock is `getutc()` in server scripts (whole
seconds, so 2 to 3 s) and `getrealtime()` in client scripts (2.5 s). Measured
in game, server side: kills after 2.3 to 2.7 s, callers ended, a started
thread's starter and the other threads untouched.

Loops proven to end: a counter that starts at a number, steps one way, is not
assigned in the body, and runs to a number or the size of a variable that only
ever holds an array. Loops proven to wait: every turn passes a `wait` of a
literal above 0 or a `waittill` of a literal on `self` or `level`.

## Measured differences between the two VMs

Every operator on every pair of 17 kinds of value, and every unary operation,
index, store, switch, event and call on each, run by one script in WaW and in
BO2 (2026-11-25): 6528 results, these differences and no others.

| Script | WaW | BO2 | Converter |
|---|---|---|---|
| `a && b`, <code>a &#124;&#124; b</code> as a value | 0, 1, or undefined | `b` itself | the right side becomes `!( !( b ) )` |
| `( x, y, z )` with a part that is no number | undefined | garbage, and the script stack stays wrong: repeated, it stops threads | `waw_vector`, unless the parts are known numbers |
| `switch` on anything but an int or a string | `default:` runs | no label runs | `waw_switch` |
| array `==`/`!=` array | the same array or not | a fault | `waw_equal`, `waw_not_equal` |
| array `==` object | 0 | a fault | `waw_equal` (server) |
| string `<` string (also `>`, `<=`, `>=`) | a fault | compares | `waw_less` and the like |

Comparisons are rewritten where type inference (`types.*`) allows both sides
the type (per map about 180 array and 85 string comparisons of a few thousand)
and at most one side calls functions, since T4 runs an operator's left side
first and a call's last argument first. T6 has no array identity: equal
contents stand for the same array. T6 client scripts cannot tell an int from a
float, so there only undefined takes the `default:` label.

With the rules applied, 6506 of the 6528 results in BO2 are WaW's. The other 22:
an empty array compared with itself, arrays compared with functions or
localized strings, and localized strings put in order.

Everything else matched: arithmetic, string conversion, arrays by value, key
order of `getarraykeys`, `int / int` giving a float, division by zero giving 0,
integer wrap, missing and extra arguments, deleted entities, case fall-through,
evaluation order, notifies with bad events.

## The link check

T6 resolves calls when a map loads and reports the first that fails as
"Unresolved external". `link_check.*` does it when the scripts are staged, for
every server and client script, with the file, line and function: calls and
function pointers that resolve to nothing, arguments past a function's
parameters or outside a builtin's range, missing includes, functions defined
twice. T6's builtins come from its tables (`compat/t6_script_api.json`; client
ones from `0xD3AF00` and `0xC62CA8` of `t6zm.exe`).

## Checking

    tools\build_gsc_corpus.ps1
    native\build\waw2bo2n.exe gsc-check work\gsc_corpus\maps
    native\build\waw2bo2n.exe gsc-check <build>\stage\zone_raw --bo2
    native\build\waw2bo2n.exe gsc-link <build>\stage\zone_raw\<project>
    native\build\waw2bo2n.exe gsc-port work\gsc_corpus\maps <stock WaW scripts> <BO2 folder> work\gsc_corpus\ported

`gsc-port` translates and link-checks every collected map without building
anything (35 maps in 90 s); the stock scripts are the `views` folder of the WaW
asset cache. `gsc-rules` applies the VM rules to one script.
