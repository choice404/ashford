// The sibling module main.ash imports by bare name: a record the importer
// constructs and a provisional clause it incorporates.

record Pair {
    a: Int
    b: Int
}

provisional clause Shared {
    clause describe(n: Int) -> Int
}
