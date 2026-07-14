# The Ashford Grammar

This is the normative grammar for Ashford. It covers the lexical structure, the type grammar, every declaration form, and the statement and expression core that clause and pledge bodies are written in. Where the grammar alone cannot pin a rule down, a semantic note next to the production carries the rest. The parser and the checker are held to this document, and a change to the surface lands here first.

## Notation

Productions are written in EBNF.

```text
::=     is defined as
|       alternation
{ x }   zero or more
[ x ]   optional
( x )   grouping
"x"     literal terminal
NL      statement terminator, see below
```

Nonterminals are CamelCase. Token classes are UPPERCASE.

## Source files

A source file is UTF-8 and ends in `.ash`. A string literal that is not valid UTF-8 is a compile error, not a silent replacement.

Statements and declarations are terminated by a newline. There are no semicolons. A newline inside parentheses, brackets, or an argument list does not terminate anything, so a long signature can wrap freely. A `{` at the end of a line opens a block and the matching `}` closes it. `NL` in the productions below means one or more newlines.

Nesting depth is capped. A parser that hits the ceiling reports a diagnostic and stops rather than hanging or overflowing its stack.

## Comments

```text
// line comment, to end of line
/* block comment, no nesting */
```

## Identifiers and keywords

```text
IDENT ::= (letter | "_") { letter | digit | "_" }
```

Hard keywords, reserved everywhere:

```text
contract    vow         pledge      clause      provisional
subcontract requirements incorporate internal   record
import      let         mut         return      if
else        match       while       for         in
break       continue    is          either      or
true        false       schema      transactional
```

Contextual keywords, ordinary identifiers except in the positions named:

- `fulfill` and `partial` are labels inside a `requirements` block.
- `abi`, `symbol`, and `version` are attribute keys.
- `sign`, `status`, and `partial` are builtin contract methods.

One carve out: after `.` any keyword is accepted as a member name. That is what lets `payment.break()` parse while `break` stays a hard keyword for loop control.

## Literals

```text
INT    ::= decimal | "0x" hex | "0b" binary        // underscores allowed as separators
FLOAT  ::= digits "." digits [ ("e"|"E") ["+"|"-"] digits ]
STRING ::= '"' { char | escape } '"'
CHAR   ::= "'" ( char | escape ) "'"
BOOL   ::= "true" | "false"
escape ::= "\n" | "\t" | "\r" | "\0" | "\\" | "\"" | "\'" | "\u{" hex "}"
```

An `INT` literal types as `Int` unless the expected type at its position is `UInt` or `Byte` and the value fits. A `FLOAT` literal types as `Float`. `CHAR` holds one Unicode scalar value. There is no implicit numeric conversion anywhere, widening included.

A raw newline inside a string literal is an error; a string that needs one writes `\n`. An integer literal above the `Int` maximum is a lex error in the first version, so the largest `UInt` values are not writable as literals yet; the guard moves to the checker, where the expected type is known, when that changes. A `\u{...}` escape naming a surrogate or a value above `0x10FFFF` is an error.

The fixed representations, identical on every target:

| Type   | Representation              |
| ------ | --------------------------- |
| Int    | 64 bit signed               |
| UInt   | 64 bit unsigned             |
| Float  | 64 bit IEEE 754             |
| Bool   | 1 byte, 0 or 1              |
| Byte   | 8 bit unsigned              |
| Char   | 32 bit Unicode scalar value |
| String | UTF-8, pointer plus byte length, no terminator |

## Types

```text
Type       ::= NamedType | TupleType | PledgeType
NamedType  ::= IDENT [ "<" Type { "," Type } ">" ]
TupleType  ::= "Tuple" "<" Type { "," Type } ">"
PledgeType ::= "pledge" "(" [ Type { "," Type } ] ")" "->" Type
```

The builtin named types are `Int`, `UInt`, `Float`, `Bool`, `Byte`, `Char`, `String`, `Unit`, `List<T>`, `Map<K, V>`, `Option<T>`, and `Result<T, E>`. Type arguments belong to the builtin composites only. A user declaration does not take type parameters. Every instantiation is monomorphized, and only instantiations reachable from a pledge signature get an ABI descriptor.

`Map<K, V>` keys are restricted to `Int`, `UInt`, `Bool`, `Byte`, `Char`, and `String`.

A `PledgeType` is the type of a first class pledge value. The value is always bound to a signed contract instance, so calling one fulfills that pledge on that instance.

A contract name used as a type denotes a signed instance, and an instance never crosses the boundary. A contract type is therefore rejected anywhere a value would cross it: a pledge parameter or return, a vow, a record or sum field, and a provisional clause signature, at any nesting depth. Clause parameters, clause returns, and local bindings may carry contract types freely, since they never leave the module.

## Program structure

```text
File        ::= { Import NL } { TopDecl NL }
Import      ::= "import" IDENT { "." IDENT }
TopDecl     ::= RecordDecl | SumDecl | ProvClause | ContractDecl
```

A file is a module. A directory is a package. `import ashstd.math` brings a module into scope by its dotted path. `internal` on a contract or provisional clause keeps it inside its package and out of the descriptor table.

## Attributes

```text
Attrs ::= "[" IDENT ":" Literal { "," IDENT ":" Literal } "]"
```

An attribute list trails a declaration header. Unknown keys are compile errors. The known keys:

- `version: INT` on a contract. Defaults to 1. Baked into every mangled name the contract produces.
- `abi: STRING` and `symbol: STRING` on a pledge. Binds the pledge to a foreign symbol. `"c"` is the only ABI in the first version.

## Data declarations

```text
RecordDecl ::= "record" IDENT "{" NL { FieldDecl NL } "}"
FieldDecl  ::= IDENT ":" Type

SumDecl    ::= IDENT "is" "either" Variant { "or" Variant }
Variant    ::= IDENT [ "(" FieldDecl { "," FieldDecl } ")" ]
```

A `record` is plain data with no lifecycle, copyable across the ABI. A sum declaration reads the way the builtin ones do:

```text
record Card {
    number: String
    expiry: String
}

LogLevel is either Debug or Info or Error
MathError is either DivByZero or Overflow(detail: String)
```

Both get a deterministic C compatible layout, a tag plus a union for sums. Error types are ordinary sums used in the `E` slot of `Result`.

A sum declaration is one statement, so its `or` chain stays on one line; a newline ends the declaration and a leading `or` on the next line is an error.

## Provisional clauses

```text
ProvClause ::= [ "internal" ] "provisional" "clause" IDENT "{" NL { ClauseSig NL } "}"
ClauseSig  ::= "clause" IDENT "(" [ Params ] ")" "->" Type
```

A provisional clause is a clause template. A contract that incorporates one must implement every signature exactly. Two incorporated provisional clauses contributing the same clause name is a compile error at the `incorporate` site.

## Contracts

```text
ContractDecl ::= [ "internal" ] "contract" IDENT [ Attrs ] "{" NL { Member NL } "}"
Member       ::= Incorporate | VowDecl | SchemaDecl | PledgeDecl | ClauseDecl
               | SubcontractDecl | RequirementsBlock

Incorporate  ::= "incorporate" IDENT

VowDecl      ::= "vow" IDENT ":" Type [ "=" Expr ]

SchemaDecl   ::= "schema" IDENT "{" NL { ColumnDecl NL } "}"
ColumnDecl   ::= IDENT ":" Type

PledgeDecl   ::= "pledge" IDENT "(" [ Params ] ")" "->" Type [ Attrs ] [ Block ]

ClauseDecl   ::= "clause" IDENT "(" [ Params ] ")" "->" Type Block

Params       ::= Param { "," Param }
Param        ::= IDENT ":" Type

SubcontractDecl ::= "subcontract" [ IDENT ] [ "transactional" ] "{" NL { PledgeDecl NL } "}"
```

Semantic notes.

- A pledge return type must be `Result<T, E>`, `Option<T>`, or `Unit`. Nothing else.
- A pledge with a `Block` is implemented in Ashford. A pledge without one is abstract: it must be bound to a foreign symbol through `[abi: ..., symbol: ...]` or bound by the host through the runtime before the contract signs. Signing with an unbound pledge is an error.
- A vow initializer must be a constant expression of the vow type. A vow without an initializer must be supplied at sign time.
- Clauses are not first class. A clause is callable by bare name only inside its own contract.
- Subcontracts do not nest. An anonymous subcontract cannot be named in a `requirements` block.
- A `schema` names a table and its columns for a store-backed contract. A column type is one of the seven scalars a flat row holds, `Int`, `UInt`, `Float`, `Bool`, `Byte`, `Char`, or `String`; a composite column type is a type error. The first column is the schema's primary key. A contract may declare more than one schema, one per table it touches, and the schema is vow shaped: it joins the shape hash and locks at sign. A contract that declares at least one schema is store-backed and must carry a `dsn` vow, or the sign fails unbound.
- The `transactional` modifier runs a subcontract's pledges as one all-or-nothing episode and is allowed only on a subcontract in a store-backed contract. Its pledges commit together when the subcontract completes and roll back on the first `Err` or at a break before the commit; a subcontract without the modifier groups for the policy alone, its pledges autocommit.

## Requirements

```text
RequirementsBlock ::= "requirements" "{" NL
                        [ "fulfill" ":" ReqExpr NL ]
                        [ "partial" ":" ReqExpr NL ]
                        [ "break"   ":" ReqExpr NL ]
                      "}"

ReqExpr  ::= ReqOr
ReqOr    ::= ReqAnd { "||" ReqAnd }
ReqAnd   ::= ReqNot { "&&" ReqNot }
ReqNot   ::= [ "!" ] ReqAtom
ReqAtom  ::= IDENT | "(" ReqExpr ")"
```

A `ReqAtom` names a subcontract or a pledge declared outside any subcontract. A bare name means that item is fulfilled, `!` negates it. At most 16 atoms per contract.

A pledge latches. It becomes fulfilled on its first `Ok` and broken on an `Err` that lands before any `Ok`. Later calls still run and still return their results to the caller, but the latched state never changes, so contract state never regresses. A subcontract is fulfilled when every pledge inside it is, and broken when every pledge inside it is.

The policy is evaluated after every fulfillment, in priority order `break`, then `fulfill`, then `partial`, and the first line that matches sets the contract state. The `break` line arms only once at least one pledge has latched broken; before that it never matches, whatever its expression says. Without this rule a policy like `break: !Validation && !Processing` would tear the contract down after its very first successful fulfillment, when nothing has broken and nothing is yet fulfilled. A state that satisfies both `fulfill` and `partial` is therefore fine, `fulfill` wins. The one static rejection is a `fulfill` and a `break` that can be true at the same time, checked by enumerating the atom assignments.

When the block is absent the defaults apply: `fulfill` when every subcontract and every loose pledge is fulfilled, `partial` when at least one subcontract is, `break` when everything is broken.

## Statements

```text
Block     ::= "{" NL { Stmt NL } "}"
Stmt      ::= LetStmt | AssignStmt | ReturnStmt | IfStmt | MatchExpr
            | WhileStmt | ForStmt | "break" | "continue" | Expr

LetStmt   ::= "let" [ "mut" ] IDENT [ ":" Type ] "=" Expr
AssignStmt::= Lvalue "=" Expr
Lvalue    ::= IDENT { "." IDENT | "[" Expr "]" }
ReturnStmt::= "return" [ Expr ]
IfStmt    ::= "if" Expr Block { "else" "if" Expr Block } [ "else" Block ]
WhileStmt ::= "while" Expr Block
ForStmt   ::= "for" IDENT "in" Expr Block
```

Bindings are immutable unless declared `let mut`. Assignment targets a `let mut` binding, a field of one, or an element of one. There are no compound assignment operators in the first version. `break` and `continue` are loop control only, so they appear inside a `while` or `for` body.

A block used as a value takes the type of its final expression statement, and `Unit` when it ends with anything else. Match arms must agree on one type wherever the match sits, so a statement position match with mixed arms ends each arm in a statement or returns.

`+` on two `String` values concatenates into a new `String`. That is the one operator with a non numeric arm; a `String` never meets a numeric operand.

`&&` and `||` short circuit: the right operand is not evaluated when the left already decides the answer, so a bounds guard like `i < n && xs[i] == 0` never touches `xs[i]` out of range.

Every value has value semantics. A `let`, an assignment, an argument, and a return each carry an independent copy, composites included; mutating a record or list through one binding is never visible through another. The copy is cheap by assumption, the same assumption the boundary makes.

The one exception is a signed instance, which is a reference handle: `let b = a` binds both names to the same instance, equality on instances is identity, and there is no way to copy a contract. A pledge called on another contract inside a body runs a real fulfillment on that instance, and its result is copied home before it is used, so the value survives the callee breaking.

Vows are never assignable. `mut(expr)` is the builtin that produces a mutable copy of a vow value; the original never changes.

## Expressions

Precedence, tightest first.

| Level | Operators                            | Associativity |
| ----- | ------------------------------------ | ------------- |
| 1     | call `()`, index `[]`, member `.`, propagate `?` | left |
| 2     | unary `!`, unary `-`                 | right         |
| 3     | `*` `/` `%`                          | left          |
| 4     | `+` `-`                              | left          |
| 5     | `<` `<=` `>` `>=`                    | left          |
| 6     | `==` `!=`                            | left          |
| 7     | `&&`                                 | left          |
| 8     | `\|\|`                               | left          |

```text
Expr      ::= see the ladder above
Primary   ::= Literal | IDENT | "(" Expr ")" | TupleLit | ListLit
            | MatchExpr | CtorExpr | "mut" "(" Expr ")"

TupleLit  ::= "(" Expr "," Expr { "," Expr } ")"
ListLit   ::= "[" [ Expr { "," Expr } ] "]"

CtorExpr  ::= "Some" "(" Expr ")" | "None"
            | "Ok" "(" Expr ")" | "Err" "(" Expr ")"
            | IDENT "{" [ IDENT ":" Expr { "," IDENT ":" Expr } ] "}"   // record
            | IDENT [ "(" Expr { "," Expr } ")" ]                        // sum variant
```

There is no map literal. A map starts empty from its constructor form, `Map<K, V>()`, with the type arguments spelled out, and grows through index assignment:

```text
let mut ages = Map<String, Int>()
ages["ada"] = 36
let hit = ages["ada"]      // Option<Int>, Some(36)
let miss = ages["bob"]     // None
```

Indexing a map reads an `Option<V>`, `None` on a missing key, so a miss is a value and never a fault; `m[k] = v` inserts or updates. Entries keep insertion order, which is the order any serialization sees. Removal and iteration over a map are not in the first version.

An assignment target is a place, not a value, so the lvalue walk differs from reading in two ways. A map index in an assignment target names the value slot directly and types as `V`, not `Option<V>`: in final position `m[k] = v` inserts or updates and never faults, while a missing key in the middle of a chain, `m[k].field = v`, is a fault, the same status an out of range list index reports, because a place that does not exist cannot hang a `None` anywhere. And writing through a chain, `b.xs[0] = 99`, mutates the named binding in place; the copies value semantics make on reads never apply to the target being assigned.

A record literal is ambiguous where a block can follow, so two rules keep the parse deterministic. A bare record literal is not allowed in the head expression of `if`, `while`, `for`, or `match`; wrap it in parentheses to opt back in. And the `{` of a record literal must sit on the same line as the name, since a line ending before `{` reads as the name alone with the brace opening a block.

### Calls and contract operations

```text
CallExpr   ::= Expr "(" [ Args ] ")"
MemberExpr ::= Expr "." IDENT
Args       ::= Arg { "," Arg }
Arg        ::= [ IDENT ":" ] Expr
```

Fulfilling a pledge is a method call on a signed instance, and it type checks against the pledge signature directly:

```text
let payment = PaymentService.sign(currency: "EUR")
let result = payment.validate_card(my_card)
```

- `Contract.sign(...)` takes named vow overrides. Every vow without a default must appear. It returns the signed instance.
- `instance.pledge_name(args)` fulfills that pledge and returns exactly the pledge's declared type.
- `instance.pledge_name` with no call is a first class pledge value of the matching `PledgeType`, bound to that instance.
- `instance.break()` tears the contract down and returns `Unit`. Every later fulfillment on it returns the broken contract error.
- `instance.status()` returns the contract state. `instance.partial()` returns the `PartialResult`.
- A clause is called by bare name, `format_message(msg)`, and only from inside its own contract.

There is no dynamic `fulfill(name, args)` form in the language. Dispatch by name lives in the runtime C API for foreign hosts, where nothing is statically checked anyway.

### Propagation

`expr?` unwraps an `Option` or a `Result`. On `Some` or `Ok` it yields the inner value. On `None` or `Err` it returns from the enclosing pledge or clause immediately with that `None` or `Err`. The enclosing return type must match, `Option` propagates only inside an `Option` returning body, `Err` only where the error types agree.

### Match

```text
MatchExpr ::= "match" Expr "{" NL { MatchArm NL } "}"
MatchArm  ::= Pattern "->" ( Expr | Block )
Pattern   ::= Literal
            | "_"
            | IDENT                                   // binding
            | IDENT "(" Pattern { "," Pattern } ")"   // variant with payload
            | "Some" "(" Pattern ")" | "None"
            | "Ok" "(" Pattern ")" | "Err" "(" Pattern ")"
```

`match` is an expression. Arms are checked for exhaustiveness over sums, `Option`, `Result`, and `Bool`. A `_` or binding arm covers the rest. Non exhaustive matching over an open type like `Int` requires a final `_` arm.

## The entry point

A standalone program declares exactly one `Main`:

```text
contract Main {
    pledge run(args: List<String>) -> Result<Int, E>
}
```

`run` takes exactly one `List<String>` parameter, returns `Result<Int, E>` for any error type `E` the program declares, and must carry a body. The emitted executable signs `Main`, fulfills `run`, maps `Ok(n)` to exit code n truncated to its low eight bits, and maps `Err` to a rendered diagnostic on stderr and exit code 1.

## What the grammar deliberately leaves out

Kept out of the first version so the surface stays small. Each one has a reserved spelling or a stated path so adding it later breaks nothing.

- Generic user declarations. Type parameters stay on the builtin composites.
- Map literals, compound assignment, ranges, and a `for` over numeric ranges.
- Async fulfillment surface. The runtime API is shaped for it, the language keeps the synchronous call form for now.
- Provisional clauses requiring pledges. Today they require clauses only, which makes them internal code sharing, not a public interface.
