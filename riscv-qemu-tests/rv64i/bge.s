.include "test.s"

START

  # Each test checks both forward and backward branches

  TEST_BR2_OP_TAKEN 2, bge,  0,  0
  TEST_BR2_OP_TAKEN 3, bge,  1,  1
  TEST_BR2_OP_TAKEN 4, bge, -1, -1
  TEST_BR2_OP_TAKEN 5, bge,  1,  0
  TEST_BR2_OP_TAKEN 6, bge,  1, -1
  TEST_BR2_OP_TAKEN 7, bge, -1, -2

  TEST_BR2_OP_NOTTAKEN  8, bge,  0,  1
  TEST_BR2_OP_NOTTAKEN  9, bge, -1,  1
  TEST_BR2_OP_NOTTAKEN 10, bge, -2, -1
  TEST_BR2_OP_NOTTAKEN 11, bge, -2,  1

  # Bypassing tests

  TEST_BR2_SRC12_BYPASS 12, 0, 0, bge, -1, 0
  TEST_BR2_SRC12_BYPASS 13, 0, 1, bge, -1, 0
  TEST_BR2_SRC12_BYPASS 14, 0, 2, bge, -1, 0
  TEST_BR2_SRC12_BYPASS 15, 1, 0, bge, -1, 0
  TEST_BR2_SRC12_BYPASS 16, 1, 1, bge, -1, 0
  TEST_BR2_SRC12_BYPASS 17, 2, 0, bge, -1, 0

  TEST_BR2_SRC12_BYPASS 18, 0, 0, bge, -1, 0
  TEST_BR2_SRC12_BYPASS 19, 0, 1, bge, -1, 0
  TEST_BR2_SRC12_BYPASS 20, 0, 2, bge, -1, 0
  TEST_BR2_SRC12_BYPASS 21, 1, 0, bge, -1, 0
  TEST_BR2_SRC12_BYPASS 22, 1, 1, bge, -1, 0
  TEST_BR2_SRC12_BYPASS 23, 2, 0, bge, -1, 0

  # Test delay slot instructions not executed nor bypassed

    li  x1, 1
    bge x1, x0, 1f
    addi x1, x1, 1
    addi x1, x1, 1
    addi x1, x1, 1
    addi x1, x1, 1
1:  addi x1, x1, 1
    addi x1, x1, 1
  TEST_CASE 24, x1, 3

EXIT