contract Bad {
    subcontract Outer {
        subcontract Inner {
            pledge a() -> Unit
        }
    }
}
