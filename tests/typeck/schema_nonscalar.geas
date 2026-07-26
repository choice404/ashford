// A schema column is one of the seven scalars a flat row can hold; a composite
// column type is a type error, the store never a place a nested value hides.
MyErr is either A or B

contract Bad {
    vow dsn: String = "file:x.db"

    schema Rows {
        id: Int
        tags: List<Int>
    }

    pledge get(id: Int) -> Result<Int, MyErr> {
        return match Store.find(Rows, id) {
            Ok(Some(r)) -> Ok(r.id)
            Ok(None) -> Err(A)
            _ -> Err(B)
        }
    }
}
