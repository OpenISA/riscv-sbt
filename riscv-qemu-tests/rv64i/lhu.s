.include "test.s"

START

  # Basic tests

  TEST_LD_OP 2, lhu, 0x00000000000000ff, 0,  tdat
  TEST_LD_OP 3, lhu, 0x000000000000ff00, 2,  tdat
  TEST_LD_OP 4, lhu, 0x0000000000000ff0, 4,  tdat
  TEST_LD_OP 5, lhu, 0x000000000000f00f, 6, tdat

  # Test with negative offset

  TEST_LD_OP 6, lhu, 0x00000000000000ff, -6,  tdat4
  TEST_LD_OP 7, lhu, 0x000000000000ff00, -4,  tdat4
  TEST_LD_OP 8, lhu, 0x0000000000000ff0, -2,  tdat4
  TEST_LD_OP 9, lhu, 0x000000000000f00f,  0, tdat4

  # Test with a negative base

    la  x1, tdat
    addi x1, x1, -32
    lhu x3, 32(x1)
  TEST_CASE 10, x3, 0x00000000000000ff

  # Test with unaligned base

    la  x1, tdat
    addi x1, x1, -5
    lhu x3, 7(x1)
  TEST_CASE 11, x3, 0x000000000000ff00

  # Bypassing tests

  TEST_LD_DEST_BYPASS 12, 0, lhu, 0x0000000000000ff0, 2, tdat2
  TEST_LD_DEST_BYPASS 13, 1, lhu, 0x000000000000f00f, 2, tdat3
  TEST_LD_DEST_BYPASS 14, 2, lhu, 0x000000000000ff00, 2, tdat1

  TEST_LD_SRC1_BYPASS 15, 0, lhu, 0x0000000000000ff0, 2, tdat2
  TEST_LD_SRC1_BYPASS 16, 1, lhu, 0x000000000000f00f, 2, tdat3
  TEST_LD_SRC1_BYPASS 17, 2, lhu, 0x000000000000ff00, 2, tdat1

  # Test write-after-write hazard

    la  x3, tdat
    lhu  x2, 0(x3)
    li  x2, 2
  TEST_CASE 18, x2, 2

    la  x3, tdat
    lhu  x2, 0(x3)
    nop
    li  x2, 2
  TEST_CASE 19, x2, 2

EXIT

.data

tdat:
tdat1:  .half 0x00ff
tdat2:  .half 0xff00
tdat3:  .half 0x0ff0
tdat4:  .half 0xf00f