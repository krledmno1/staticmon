# MonPoly formula & signature grammar (requirements baseline)

Extracted from the newest MonPoly (`jshs/monpoly` a.k.a. `monpoly-develop`,
commit `fe89b7da43c1c15edd615a749c56e643cb2e10b8`), files
`src/formula_lexer.mll`, `src/formula_parser.mly` (ocamlyacc),
`src/log_parser.ml` + `src/log_lexer.mll` (signatures), `src/MFOTL.mli`,
`src/predicate.mli`. This document is the requirements baseline for
staticmon's standalone C++ parser; functional equivalence is defined against
these sources and verified by differential testing.

## 1. Formula lexer

Whitespace: space, tab, CR, LF. Comments: `(* ... *)` (non-nesting,
unterminated comment = lexer failure) and `#` to end of line.

Fixed tokens, longest-match, in ocamllex rule order (earlier rule wins ties):

| lexeme(s) | token |
|---|---|
| `+` `-` `.` `*` `/` `(` `)` `[` `]` | PLUS MINUS DOT STAR SLASH LPA RPA LSB RSB |
| `\|>` `▷` `FORWARD` `MATCHF` | FREX |
| `<\|` `◁` `BACKWARD` `MATCHP` | PREX |
| `\|` | BAR (declared but unused by the grammar → always a parse error) |
| `,` `;` `?` `_` | COM SC QM LD |
| `<-` `<=` `<` `>=` `>` `=` | LARROW LESSEQ LESS GTREQ GTR EQ |
| `SUBSTRING` `MATCHES` | SUBSTRING MATCHES |
| `r2s` `s2r` `f2i` `i2f` `i2s` `s2i` `f2s` `s2f` (lowercase) | conversion functions |
| `MOD` `DAY_OF_MONTH` `MONTH` `YEAR` `FORMAT_DATE` | arithmetic/date functions |
| `FALSE` `TRUE` | FALSE TRUE |
| `LET` `LETPAST` `FRZ` `IN` | LET LETPAST FRZ IN |
| `NOT` `AND` `OR` `IMPLIES` `EQUIV` | NOT AND OR IMPL EQUIV |
| `EXISTS` `FORALL` | EX FA |
| `PREV` `PREVIOUS` | PREV |
| `NEXT` | NEXT |
| `EVENTUALLY` `SOMETIMES` | EVENTUALLY |
| `ONCE` | ONCE |
| `ALWAYS` | ALWAYS |
| `PAST_ALWAYS` `HISTORICALLY` | PAST_ALWAYS |
| `SINCE` `TRIGGER` `UNTIL` `RELEASE` | SINCE TRIGGER UNTIL RELEASE |
| `CNT` `MIN` `MAX` `SUM` `AVG` `MED` | aggregation operators |

Valued lexemes (order matters — these come *after* all fixed tokens):

- `TU`: `digit+ letter` — time unit, e.g. `5d`. Scanned as `(n, c)`;
  `c ∈ {d,h,m,s}` multiplies to seconds (`d`=86400, `h`=3600, `m`=60, `s`=1);
  any other letter fails at parse time ("unrecognized time unit").
- `INT`: `-? digit+` — **arbitrary precision** (zarith `Z.t`).
- `RAT`: `-? digit+ '.' digit*` — OCaml `float_of_string` (so `1.` is valid;
  `.5` is not — it lexes as DOT then INT).
- `STR_CST`: `" ( [^"\\] | \\ any )* "` — quoted string; quotes stripped at
  parse, escape sequences kept verbatim (no unescaping).
- `REGEXP_CST`: `r` immediately followed by a quoted string; pattern compiled
  with OCaml `Str.regexp` at parse time (invalid pattern = failure).
- `STR` (identifier): `(letter|digit|_) (letter|digit|_|-|/|:|'|")*`.

Longest-match consequences the C++ lexer must replicate:

- Identifiers may start with a digit and may **contain** `-` `/` `:` `'` `"`:
  `x-5` is ONE identifier; `x -5` is `x` INT(-5); `x - 5` is `x` MINUS INT(5).
- `123` → INT (rule order beats identifier on equal length); `123a` → TU;
  `123ab` → STR (longer than TU's one letter).
- `-` immediately followed by digits lexes as a negative INT, not MINUS.
- Keywords win only on exact match; `ANDx`, `TRUEfoo` are identifiers
  (longer match). `ONCE(` → keyword + LPA (`(` not in identifier charset).

## 2. Formula grammar (ocamlyacc)

Start symbol `formula`; the start rule does **not** require EOF.

Precedence/associativity, low → high (yacc declaration order):

```
%nonassoc REDUCE_CONCAT           (synthetic: empty varlist/xopttermlist, regex reduce)
%nonassoc AGGREG                  (aggregation rules)
%nonassoc LET FRZ IN
%right    SINCE TRIGGER UNTIL RELEASE
%nonassoc PREV NEXT EVENTUALLY ONCE ALWAYS PAST_ALWAYS
%nonassoc EX FA
%left     EQUIV
%right    IMPL
%left     OR
%left     AND
%nonassoc LPA
%nonassoc NOT
%nonassoc BASE                    (synthetic: bare formula as regex atom)
%nonassoc QM
%nonassoc RPA
%nonassoc EQ LESSEQ LESS GTR GTREQ SUBSTRING MATCHES
%left     PLUS MINUS
%left     STAR SLASH MOD
%nonassoc UMINUS F2I I2F DAY_OF_MONTH MONTH YEAR FORMAT_DATE
%nonassoc FALSE TRUE LD STR STR_CST REGEXP_CST INT RAT DOT FREX PREX
%nonassoc LETPAST R2S S2R I2S S2I F2S S2F
```

So: AND binds tighter than OR, OR tighter than IMPL (right-assoc), IMPL
tighter than EQUIV (left-assoc); quantifiers scope over all of these; unary
temporal operators bind looser than quantifiers; binary temporal operators
(right-assoc) are loosest of all operator productions. NOT is above AND/OR:
`NOT a AND b` = `(NOT a) AND b`.

Productions (P = predicate, t = term, f = formula, I = interval, `[I]` =
optional interval defaulting to `[0,*)`):

```
f ::= ( f )
    | FALSE                      ⇒ Equal(Cst 0, Cst 1)     (desugared!)
    | TRUE                       ⇒ Equal(Cst 0, Cst 0)     (desugared!)
    | P                          (see add_ex below)
    | LET  P = f IN f            (P must be syntactic predicate)
    | LETPAST P = f IN f
    | FRZ  P = f IN f            (new in monpoly-develop)
    | t = t | t <= t | t < t
    | t > t                      ⇒ Less(t2,t1)              (flipped!)
    | t >= t                     ⇒ LessEq(t2,t1)            (flipped!)
    | f EQUIV f | f IMPLIES f | f OR f | f AND f | NOT f
    | EXISTS varlist . f         (empty varlist = parse-time failure)
    | FORALL varlist . f         (ForAll AST node, not desugared)
    | var <- agg var f           (Aggreg, empty group-by)
    | var <- agg var ; varlist f (Aggreg with group-by)
    | t SUBSTRING t
    | t MATCHES t [xopttermlist]
    | PREV [I] f | NEXT [I] f | EVENTUALLY [I] f | ONCE [I] f
    | ALWAYS [I] f | PAST_ALWAYS [I] f
    | f SINCE [I] f | f TRIGGER [I] f | f UNTIL [I] f | f RELEASE [I] f
    | FREX [I] fregex | PREX [I] pregex          (MFODL regexes)

agg ::= CNT | MIN | MAX | SUM | AVG | MED

I  ::= lb , rb
lb ::= ( u | [ u                 (open / closed lower bound)
rb ::= u ) | u ] | * ) | * ]     (open / closed / infinite upper bound)
u  ::= TU | INT

P        ::= ident ( termlist )
termlist ::= ε | termarg (, termarg)*
termarg  ::= t | _                (each `_` becomes a fresh var "_1","_2",…
                                   counter is GLOBAL per process, not reset per
                                   formula)

t ::= t + t | t - t | t * t | t / t | t MOD t
    | - t                        (UMINUS precedence)
    | ( t )
    | f2i(t) | i2f(t) | i2s(t) | s2i(t) | f2s(t) | s2f(t) | r2s(t) | s2r(t)
    | DAY_OF_MONTH(t) | MONTH(t) | YEAR(t) | FORMAT_DATE(t)
    | cst | var

cst ::= INT | RAT | STR_CST (stripped) | REGEXP_CST (compiled)

varlist      ::= ε | var (, var)*         (ε only via REDUCE_CONCAT context)
xopttermlist ::= ε | ( opttermlist )
opttermlist  ::= ε | optterm (, optterm)*
optterm      ::= _ ⇒ None | t ⇒ Some t
```

Regex fragment (MFODL), stratified alternation > concatenation > atom, both
levels right-associative; `a b + c` = `(a b) + c`:

```
fregex  ::= fconcat (+ fregex)?
fconcat ::= fatom fconcat?
fatom   ::= ( fregex ) | .                    (wildcard = Skip 1)
          | f                                  ⇒ Concat(Test f, wild)   (FREX)
          | f ?                                ⇒ Test f
          | fatom *                            ⇒ Star

pregex/pconcat/patom: same, except bare formula ⇒ Concat(wild, Test f)  (PREX)
```

### add_ex (automatic existential quantification)

After parsing `P(args)`: every argument that is a *variable whose name starts
with `_`* (the anonymous `_` becomes `_1`, `_2`, …; but also user-written
`_foo`) is collected in argument order into `Exists([vars], Pred P)`. If none,
plain `Pred P`.

### Other parse-time semantics

- `TRUE`/`FALSE` desugar to integer equalities (see above).
- `>`/`>=` flip into `Less`/`LessEq`.
- Interval bounds: any combination of open/closed/infinite as per grammar;
  no validation at parse time (e.g. `[5,3]` parses; rejected later).
- Predicate name is a plain identifier only (no quoted names).
- `LET`/`LETPAST`/`FRZ` head must be a syntactic predicate (grammar reuses the
  `predicate` production; anything else fails with "expected predicate").
  NOTE: `add_ex` applies inside the `predicate` production, so a head with a
  `_`-argument yields `Exists` and hence the "expected predicate" failure.
- Aggregation: result var, `<-`, operator, aggregated var, optional `;`
  group-by varlist, then the formula. Parse-time check: aggregated var and all
  group-by vars must be free in the sub-formula (else failure). AST carries a
  type placeholder `TSymb(TAny, 0)`.

## 3. AST (target of the parse)

From `MFOTL.mli` / `predicate.mli` (C++ AST must be structurally equivalent):

```
term  ::= Var v | Cst c | F2i t | I2f t | I2s t | S2i t | F2s t | S2f t
        | DayOfMonth t | Month t | Year t | FormatDate t | R2s t | S2r t
        | Plus t t | Minus t t | UMinus t | Mult t t | Div t t | Mod t t
cst   ::= Int Z | Str s | Float f | Regexp (pattern, compiled)
bound ::= OBnd Z | CBnd Z | Inf          interval = bound × bound
agg_op::= Cnt | Min | Max | Sum | Avg | Med

formula ::= Equal t t | Less t t | LessEq t t | Substring t t
          | Matches t t [optterm...]
          | Pred (name, arity, args) | Let | LetPast | Frz
          | Neg | And | Or | Implies | Equiv
          | Exists [v...] f | ForAll [v...] f
          | Aggreg (tsymb, var, agg_op, var, [var...], f)
          | Prev I f | Next I f | Eventually I f | Once I f
          | Always I f | PastAlways I f
          | Since I f f | Trigger I f f | Until I f f | Release I f f
          | Frex I regex | Prex I regex
regex   ::= Skip 1 (wild) | Test f | Concat r r | Plus r r | Star r
```

Integers are arbitrary-precision everywhere (constants, interval bounds).

## 4. Signature grammar (`log_parser.ml`, hand-written over `log_lexer`)

Lexer: whitespace/newlines; `#` line comments; tokens `@ > < ( ) { } , ;`;
identifiers `(letter|digit|_|[|]|/|:|-|.|!)+` (NOTE: different charset than
formula identifiers — includes `[`,`]`,`.`,`!`, excludes `'`); quoted and
`r`-quoted strings yield their (unstripped-content) STR.

```
signature ::= predicate*
predicate ::= ident ( args )
args      ::= ε | arg (, arg)*
arg       ::= type | name : type [: ...ignored]
type      ::= int | string | float | regexp
```

Duplicate predicate names = failure. The base schema pre-defines
`tp(i:int)`, `ts(t:int)`, `tpts(i:int, t:int)` — user redefinition of these
names is therefore a duplicate-definition failure.

## 5. Oracle for differential testing

`Formula_parser.formula Formula_lexer.token lexbuf` on the formula file, i.e.
parsing is signature-independent (the signature only matters for the later
type check, which is NOT part of parser equivalence). The oracle driver links
`libmonpoly` (public dune library) from the pinned commit and prints the AST
as canonical s-expressions; the C++ parser prints the same format.

Known behaviors to test explicitly: the start rule has no EOF anchor
(trailing-input behavior must be probed empirically); `?` `.` `*` `+` are
context-dependent (regex vs term vs interval); `_` handling in MATCHES lists
vs predicate arguments; the global `_n` counter; nonassoc comparisons
(`a = b = c` is an error); `(*` inside a quoted string is not a comment; the
`"`/`'` characters allowed inside identifiers.
