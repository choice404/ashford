// The expression ladder end to end: every binary level and its
// associativity, unary operators, the postfix set with propagation, tuple
// and list literals, constructors, record literals, mut(), labeled call
// arguments, keyword member names, and an argument list wrapped across
// lines, which the grouping rule must keep as one statement.

contract Calc {
    pledge run(args: List<String>) -> Result<Int, MathError> {
        let a = 1 + 2 * 3 - 4 / 5 % 6
        let b = -a + 2
        let c = !true == false
        let d = a < 3 && b >= 1 || !(c != true)
        let e = (1 + 2) * 3
        let t = (1, "two", 3.5)
        let l = [1, 2, 3]
        let empty = []
        let m = mut(a)
        let q = Some(a + 1)
        let r = None
        let s = Ok("fine")
        let u = Err(overflow)
        let pt = Point { x: 1, y: 2 }
        let box = Rect { corner: Point { x: 0, y: 0 }, wide: true }
        let inst = Calc.sign(precision: 2, mode: "fast")
        let handle = inst.run
        let deep = grid[1][2].cell(3)?.value
        let torn = payment.break()
        let wrapped = combine(
            1,
            2 +
                3
        )
        return Ok(a)
    }
}
