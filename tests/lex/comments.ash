// Leading line comment before the first token.
let a = 1 // trailing comment ends at the line
/* one line block */ let b = 2
let c = 3 /* a block comment
that spans lines carries the newline it swallowed */ let d = 4
let e = 5 /* twin */ /* blocks */ + 6
// the divide operator must survive next to comments
let f = a / b
