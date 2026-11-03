ForgeC

A tiny compiler written in C, built to show the whole pipeline in one readable file. It parses a small language of integer arithmetic, checks it, generates C, and hands that to the system C compiler to produce a native executable.

source.fc  ->  parser  ->  AST  ->  semantic check  ->  C code  ->  gcc/clang  ->  executable
The language
program   := statement+
statement := "print" expr ";"
expr      := term   (("+" | "-") term)*
term      := unary  (("*" | "/" | "%") unary)*
unary     := ("-" | "+") unary | primary
primary   := integer | "(" expr ")"
integer   := digit+
Numbers are 64-bit signed integers.
/ and % truncate toward zero, like C (-7 / 2 is -3, -7 % 3 is -1).
*, /, % bind tighter than + and -. All binary operators are left-associative.
Whitespace, including newlines, is insignificant. // starts a comment that runs to the end of the line.
Integer overflow and division by zero are compile-time errors.
Example
// examples/arithmetic.fc
print 1 + 2 * 3;        // 7
print (1 + 2) * 3;      // 9
print 10 - 3 - 2;       // 5   (left-associative)
print 100 / 7;          // 14  (truncates toward zero)
print 100 % 7;          // 2
print -7 / 2;           // -3
print 3 - -2;           // 5
print -9223372036854775807 - 1;  // smallest 64-bit integer
$ ./forgec examples/arithmetic.fc arith
Compilation successful.
$ ./arith
7
9
5
14
2
-3
5
-9223372036854775808
Build and use

You need a POSIX system (Linux, macOS, or WSL) and GCC or Clang. The compiler uses __builtin_*_overflow, so it must be built with one of those two.

bash
gcc -std=c11 -O2 -Wall -Wextra forgec.c -o forgec

./forgec program.fc program     # compile program.fc to the executable ./program
./forgec --emit-c program.fc out.c   # just write the generated C
./forgec --help

By default ForgeC runs gcc to build the generated C. Set CC to use a different compiler:

bash
CC=clang ./forgec program.fc program
Exit codes
Code	Meaning
0	Success
1	Compile error (syntax, overflow, division by zero, unreadable file, or the C compiler failed)
2	Wrong command-line usage
Error messages

Errors are reported as file:line:column: error: message, pointing at the exact spot:

$ cat bad.fc
print 1;
print 9223372036854775807 + 1;
$ ./forgec bad.fc bad
bad.fc:2:27: error: integer overflow in '+'

$ ./forgec worse.fc worse
worse.fc:3:3: error: expected an expression but found ';'

Overflow and division-by-zero errors point at the operator. Failed compiles leave no output file behind.

How it works
Stage	What it does
Parser	A hand-written recursive-descent parser, one function per grammar rule. It tracks line and column for error messages.
AST	Numbers, unary minus, and binary operators. Nodes are freed after each statement.
Semantic check	Evaluates each expression with checked 64-bit arithmetic (__builtin_add_overflow and friends). Any overflow or division by zero stops compilation with a located error.
Code generation	Emits one printf("%lld\n", <expression>); per statement, with every sub-expression fully parenthesized.
C compiler	The generated C is piped to the compiler's standard input.

Some design decisions worth knowing about:

No shell and no temp files. The compiler is started with fork and execlp, and the C source goes over a pipe. The output filename is passed as a plain argument, so names with spaces, quotes, $(...), or a leading - are all harmless. An earlier version built a shell command string, which allowed command injection through the output filename.
-9223372036854775808 works. The number is parsed as a magnitude, and a leading - is folded into the literal, because 9223372036854775808 on its own does not fit in a 64-bit integer.
Resource limits. Input is capped at 1 MB, parenthesis and unary-operator nesting at 200 levels, and expression depth at 5,000, so hostile input produces an error rather than a stack overflow.
Tests
bash
./test.sh

The suite builds the compiler with -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror and runs 73 checks:

valid programs and their exact output (precedence, associativity, truncating division, LLONG_MIN/LLONG_MAX, comments, CRLF line endings)
compile-time errors, including overflow in every operator, division by zero, bad syntax, NUL bytes, and over-deep nesting
error line and column positions
command-line behavior: usage, --help, --emit-c, a missing or failing C compiler, the CC variable
shell-injection attempts through the output filename
the example programs

Options:

bash
CC=clang ./test.sh                                        # use a different C compiler
FORGEC_CFLAGS="-g -fsanitize=address,undefined" ./test.sh # build forgec with sanitizers

The suite passes on Linux with GCC 13.3, both in a normal build and with AddressSanitizer and UBSan enabled. I also checked that it can fail: deliberately breaking the overflow check, the operator precedence, or the shell-safe process launch each makes it report failures.

Not tested: macOS, Clang, and Windows (not supported).

Limitations
Only constants. There are no variables, input, or control flow, so every program's output is fixed at compile time. The generated C is real code, but the C compiler can fold all of it. This is a teaching compiler, not a useful language.
POSIX only. It relies on fork, pipe, and execlp. On Windows, use WSL.
One output type. Every print writes a signed 64-bit integer.
Ideas for extending it
Add variables (let x = 5;) and a symbol table. This turns the semantic check into a real name-resolution pass.
Replace the compile-time evaluator with generated runtime checks, which you need once values depend on input.
Add if/while, and then functions.
Generate assembly or LLVM IR instead of C.