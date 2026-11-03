ForgeC

Ein winziger Compiler, geschrieben in C. Er zeigt die gesamte Pipeline in einer gut lesbaren Datei: Er parst eine kleine Sprache für Ganzzahl-Arithmetik, prüft sie, erzeugt C-Code und übergibt diesen an den C-Compiler des Systems, der daraus eine native ausführbare Datei baut.

quelle.fc  ->  Parser  ->  AST  ->  semantische Prüfung  ->  C-Code  ->  gcc/clang  ->  ausführbare Datei
Die Sprache
program   := statement+
statement := "print" expr ";"
expr      := term   (("+" | "-") term)*
term      := unary  (("*" | "/" | "%") unary)*
unary     := ("-" | "+") unary | primary
primary   := integer | "(" expr ")"
integer   := digit+
Zahlen sind 64-Bit-Ganzzahlen mit Vorzeichen.
/ und % runden zur Null hin, wie in C (-7 / 2 ergibt -3, -7 % 3 ergibt -1).
*, /, % binden stärker als + und -. Alle binären Operatoren sind linksassoziativ.
Leerraum, auch Zeilenumbrüche, ist bedeutungslos. // beginnt einen Kommentar bis zum Zeilenende.
Ganzzahl-Überlauf und Division durch null sind Fehler zur Übersetzungszeit.
Beispiel
// examples/arithmetic.fc
print 1 + 2 * 3;        // 7
print (1 + 2) * 3;      // 9
print 10 - 3 - 2;       // 5   (linksassoziativ)
print 100 / 7;          // 14  (rundet zur Null hin)
print 100 % 7;          // 2
print -7 / 2;           // -3
print 3 - -2;           // 5
print -9223372036854775807 - 1;  // kleinste 64-Bit-Ganzzahl
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
Bauen und benutzen

Du brauchst ein POSIX-System (Linux, macOS oder WSL) und GCC oder Clang. Der Compiler verwendet __builtin_*_overflow und muss deshalb mit einem dieser beiden gebaut werden.

bash
gcc -std=c11 -O2 -Wall -Wextra forgec.c -o forgec

./forgec program.fc program          # program.fc zur ausführbaren Datei ./program übersetzen
./forgec --emit-c program.fc out.c   # nur den erzeugten C-Code schreiben
./forgec --help

Standardmäßig ruft ForgeC gcc auf, um den erzeugten C-Code zu übersetzen. Mit der Umgebungsvariable CC kannst du einen anderen Compiler wählen:

bash
CC=clang ./forgec program.fc program
Exit-Codes
Code	Bedeutung
0	Erfolg
1	Übersetzungsfehler (Syntax, Überlauf, Division durch null, nicht lesbare Datei oder der C-Compiler ist fehlgeschlagen)
2	Falsche Verwendung der Kommandozeile
Fehlermeldungen

Fehler werden als datei:zeile:spalte: error: meldung ausgegeben und zeigen auf die genaue Stelle. Die Meldungen selbst sind auf Englisch:

$ cat bad.fc
print 1;
print 9223372036854775807 + 1;
$ ./forgec bad.fc bad
bad.fc:2:27: error: integer overflow in '+'

$ ./forgec worse.fc worse
worse.fc:3:3: error: expected an expression but found ';'

Bei Überlauf und Division durch null zeigt die Meldung auf den Operator. Fehlgeschlagene Übersetzungen hinterlassen keine Ausgabedatei.

So funktioniert es
Stufe	Aufgabe
Parser	Handgeschriebener Recursive-Descent-Parser, eine Funktion pro Grammatikregel. Er verfolgt Zeile und Spalte für die Fehlermeldungen.
AST	Zahlen, unäres Minus und binäre Operatoren. Die Knoten werden nach jeder Anweisung freigegeben.
Semantische Prüfung	Wertet jeden Ausdruck mit überlaufgeprüfter 64-Bit-Arithmetik aus (__builtin_add_overflow und Verwandte). Jeder Überlauf und jede Division durch null beendet die Übersetzung mit einer Fehlermeldung samt Position.
Codegenerierung	Erzeugt pro Anweisung ein printf("%lld\n", <Ausdruck>);, wobei jeder Teilausdruck vollständig geklammert ist.
C-Compiler	Der erzeugte C-Code wird an die Standardeingabe des Compilers übergeben.

Einige Entwurfsentscheidungen:

Keine Shell und keine temporären Dateien. Der Compiler wird mit fork und execlp gestartet, der C-Code läuft über eine Pipe. Der Name der Ausgabedatei wird als normales Argument übergeben. Namen mit Leerzeichen, Anführungszeichen, $(...) oder einem führenden - sind deshalb harmlos. Eine frühere Version baute einen Shell-Befehl als Zeichenkette zusammen, was über den Ausgabedateinamen Command Injection erlaubte.
-9223372036854775808 funktioniert. Die Zahl wird als Betrag geparst, und ein führendes - wird in das Literal eingerechnet, denn 9223372036854775808 allein passt nicht in eine 64-Bit-Ganzzahl.
Ressourcengrenzen. Die Eingabe ist auf 1 MB begrenzt, die Verschachtelung von Klammern und unären Operatoren auf 200 Ebenen und die Ausdruckstiefe auf 5.000. Böswillige Eingaben führen so zu einem Fehler statt zu einem Stack-Überlauf.
Tests
bash
./test.sh

Die Testsuite baut den Compiler mit -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror und führt 73 Prüfungen aus:

gültige Programme mit exakter Ausgabe (Rangfolge, Assoziativität, abschneidende Division, LLONG_MIN/LLONG_MAX, Kommentare, CRLF-Zeilenenden)
Übersetzungsfehler, darunter Überlauf bei jedem Operator, Division durch null, fehlerhafte Syntax, NUL-Bytes und zu tiefe Verschachtelung
Zeilen- und Spaltenangaben in Fehlermeldungen
Verhalten der Kommandozeile: Usage, --help, --emit-c, fehlender oder fehlschlagender C-Compiler, die Variable CC
Shell-Injection-Versuche über den Ausgabedateinamen
die Beispielprogramme

Optionen:

bash
CC=clang ./test.sh                                        # anderen C-Compiler verwenden
FORGEC_CFLAGS="-g -fsanitize=address,undefined" ./test.sh # forgec mit Sanitizern bauen

Die Suite besteht unter Linux mit GCC 13.3, sowohl im normalen Build als auch mit aktivierten AddressSanitizer und UBSan. Sie erkennt außerdem Regressionen: Wenn man absichtlich die Überlaufprüfung, die Operatorrangfolge oder den shell-sicheren Prozessstart kaputt macht, meldet sie jeweils Fehler.

Nicht getestet: macOS, Clang und Windows (nicht unterstützt).

Einschränkungen
Nur Konstanten. Es gibt keine Variablen, keine Eingabe und keine Kontrollstrukturen. Die Ausgabe jedes Programms steht daher schon zur Übersetzungszeit fest. Der erzeugte C-Code ist echter Code, aber der C-Compiler kann ihn vollständig auswerten. Das ist ein Lehr-Compiler, keine nützliche Sprache.
Nur POSIX. Er benötigt fork, pipe und execlp. Unter Windows nutze WSL.
Nur ein Ausgabetyp. Jedes print schreibt eine vorzeichenbehaftete 64-Bit-Ganzzahl.
Ideen für Erweiterungen
Variablen (let x = 5;) und eine Symboltabelle hinzufügen. Damit wird aus der semantischen Prüfung eine echte Namensauflösung.
Den Auswerter zur Übersetzungszeit durch erzeugte Laufzeitprüfungen ersetzen, was nötig wird, sobald Werte von Eingaben abhängen.
if/while und danach Funktionen hinzufügen.
Statt C Assembler oder LLVM-IR erzeugen.