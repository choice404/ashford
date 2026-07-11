// Seventeen distinct atoms in one contract's requirements, one past the
// grammar's limit of 16.

contract Wide {
    pledge p01() -> Unit
    pledge p02() -> Unit
    pledge p03() -> Unit
    pledge p04() -> Unit
    pledge p05() -> Unit
    pledge p06() -> Unit
    pledge p07() -> Unit
    pledge p08() -> Unit
    pledge p09() -> Unit
    pledge p10() -> Unit
    pledge p11() -> Unit
    pledge p12() -> Unit
    pledge p13() -> Unit
    pledge p14() -> Unit
    pledge p15() -> Unit
    pledge p16() -> Unit
    pledge p17() -> Unit

    requirements {
        fulfill: p01 && p02 && p03 && p04 && p05 && p06 && p07 && p08 && p09 && p10 && p11 && p12 && p13 && p14 && p15 && p16 && p17
    }
}
