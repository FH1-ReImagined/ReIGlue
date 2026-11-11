# The interrupts-off lwarx/stwcx. and ldarx/stdcx. sections Forza Horizon 1 uses, in the
# shapes its code has them (FH1 #506, ReIGlue 0470): every one but the SList push is an
# atomic read-modify-write and runs without the global lock. mfmsr after a section reads
# 0x8000 (interrupts on: the section left what it entered); inside a section that keeps
# the lock, 0.

test_atomic_increment:
  #_ MEMORY_IN 10001000 00000005
  #_ REGISTER_IN r11 0x10001000
  li r8, 0
increment_retry:
  mfmsr r9
  mtmsrd r13, 1
  lwarx r10, 0, r11
  addi r10, r10, 1
  stwcx. r10, 0, r11
  mtmsrd r9, 1
  bne increment_retry
  lwsync
  mfmsr r8
  blr
  #_ REGISTER_OUT r9 0x8000
  #_ REGISTER_OUT r10 6
  #_ REGISTER_OUT r8 0x8000
  #_ MEMORY_OUT 10001000 00000006

test_atomic_decrement:
  #_ MEMORY_IN 10001000 00000001
  #_ REGISTER_IN r11 0x10001000
  li r8, 0
decrement_retry:
  mfmsr r9
  mtmsrd r13, 1
  lwarx r10, 0, r11
  addi r10, r10, -1
  stwcx. r10, 0, r11
  mtmsrd r9, 1
  bne decrement_retry
  mfmsr r8
  blr
  #_ REGISTER_OUT r10 0
  #_ REGISTER_OUT r8 0x8000
  #_ MEMORY_OUT 10001000 00000000

test_atomic_exchange:
  #_ MEMORY_IN 10001000 12345678
  #_ REGISTER_IN r3 0x10001000
  #_ REGISTER_IN r4 0x9ABCDEF0
  li r8, 0
exchange_retry:
  mfmsr r9
  mtmsrd r13, 1
  lwarx r11, 0, r3
  stwcx. r4, 0, r3
  mtmsrd r9, 1
  bne exchange_retry
  mfmsr r8
  blr
  #_ REGISTER_OUT r11 0x12345678
  #_ REGISTER_OUT r8 0x8000
  #_ MEMORY_OUT 10001000 9ABCDEF0

test_atomic_add:
  #_ MEMORY_IN 10001000 00000010
  #_ REGISTER_IN r3 0x10001000
  #_ REGISTER_IN r4 0x00000022
  li r8, 0
add_retry:
  mfmsr r9
  mtmsrd r13, 1
  lwarx r11, 0, r3
  add r10, r11, r4
  stwcx. r10, 0, r3
  mtmsrd r9, 1
  bne add_retry
  mfmsr r8
  blr
  #_ REGISTER_OUT r11 0x10
  #_ REGISTER_OUT r10 0x32
  #_ REGISTER_OUT r8 0x8000
  #_ MEMORY_OUT 10001000 00000032

test_atomic_and:
  #_ MEMORY_IN 10001000 F0F0FFFF
  #_ REGISTER_IN r3 0x10001000
  #_ REGISTER_IN r4 0x0FF0F00F
  li r8, 0
and_retry:
  mfmsr r9
  mtmsrd r13, 1
  lwarx r11, 0, r3
  and r10, r11, r4
  stwcx. r10, 0, r3
  mtmsrd r9, 1
  bne and_retry
  mfmsr r8
  blr
  #_ REGISTER_OUT r11 0xF0F0FFFF
  #_ REGISTER_OUT r10 0x00F0F00F
  #_ REGISTER_OUT r8 0x8000
  #_ MEMORY_OUT 10001000 00F0F00F

test_atomic_or:
  #_ MEMORY_IN 10001000 00000101
  #_ REGISTER_IN r3 0x10001000
  #_ REGISTER_IN r4 0x80000010
  li r8, 0
or_retry:
  mfmsr r9
  mtmsrd r13, 1
  lwarx r11, 0, r3
  or r10, r11, r4
  stwcx. r10, 0, r3
  mtmsrd r9, 1
  bne or_retry
  mfmsr r8
  blr
  #_ REGISTER_OUT r11 0x101
  #_ REGISTER_OUT r10 0x80000111
  #_ REGISTER_OUT r8 0x8000
  #_ MEMORY_OUT 10001000 80000111

# InterlockedCompareExchange: on a failed compare the word is stored back (the
# second stwcx.), then the second mtmsrd closes the section.
test_compare_exchange_hit:
  #_ MEMORY_IN 10001000 0000002A
  #_ REGISTER_IN r7 0x10001000
  #_ REGISTER_IN r8 0x2A
  #_ REGISTER_IN r31 0x63
  li r6, 0
cas_hit_retry:
  mfmsr r9
  mtmsrd r13, 1
  lwarx r10, 0, r7
  cmpw cr6, r10, r8
  bne cr6, cas_hit_differs
  stwcx. r31, 0, r7
  mtmsrd r9, 1
  bne cas_hit_retry
  b cas_hit_done
cas_hit_differs:
  stwcx. r10, 0, r7
  mtmsrd r9, 1
cas_hit_done:
  mr r11, r10
  mfmsr r6
  blr
  #_ REGISTER_OUT r11 0x2A
  #_ REGISTER_OUT r6 0x8000
  #_ MEMORY_OUT 10001000 00000063

test_compare_exchange_miss:
  #_ MEMORY_IN 10001000 0000002B
  #_ REGISTER_IN r7 0x10001000
  #_ REGISTER_IN r8 0x2A
  #_ REGISTER_IN r31 0x63
  li r6, 0
cas_miss_retry:
  mfmsr r9
  mtmsrd r13, 1
  lwarx r10, 0, r7
  cmpw cr6, r10, r8
  bne cr6, cas_miss_differs
  stwcx. r31, 0, r7
  mtmsrd r9, 1
  bne cas_miss_retry
  b cas_miss_done
cas_miss_differs:
  stwcx. r10, 0, r7
  mtmsrd r9, 1
cas_miss_done:
  mr r11, r10
  mfmsr r6
  blr
  #_ REGISTER_OUT r11 0x2B
  #_ REGISTER_OUT r6 0x8000
  #_ MEMORY_OUT 10001000 0000002B

test_atomic_increment64:
  #_ MEMORY_IN 10001000 00000001 FFFFFFFF
  #_ REGISTER_IN r11 0x10001000
  li r8, 0
increment64_retry:
  mfmsr r9
  mtmsrd r13, 1
  ldarx r10, 0, r11
  addi r10, r10, 1
  stdcx. r10, 0, r11
  mtmsrd r9, 1
  bne increment64_retry
  mfmsr r8
  blr
  #_ REGISTER_OUT r10 0x0000000200000000
  #_ REGISTER_OUT r8 0x8000
  #_ MEMORY_OUT 10001000 00000002 00000000

test_compare_exchange64_hit:
  #_ MEMORY_IN 10001000 11112222 33334444
  #_ REGISTER_IN r3 0x10001000
  #_ REGISTER_IN r5 0x1111222233334444
  #_ REGISTER_IN r4 0x5555666677778888
  li r6, 0
cas64_hit_retry:
  mfmsr r9
  mtmsrd r13, 1
  ldarx r10, 0, r3
  cmpd cr6, r10, r5
  bne cr6, cas64_hit_differs
  stdcx. r4, 0, r3
  mtmsrd r9, 1
  bne cas64_hit_retry
  b cas64_hit_done
cas64_hit_differs:
  stdcx. r10, 0, r3
  mtmsrd r9, 1
cas64_hit_done:
  mr r11, r10
  mfmsr r6
  blr
  #_ REGISTER_OUT r11 0x1111222233334444
  #_ REGISTER_OUT r6 0x8000
  #_ MEMORY_OUT 10001000 55556666 77778888

test_compare_exchange64_miss:
  #_ MEMORY_IN 10001000 11112222 33334445
  #_ REGISTER_IN r3 0x10001000
  #_ REGISTER_IN r5 0x1111222233334444
  #_ REGISTER_IN r4 0x5555666677778888
  li r6, 0
cas64_miss_retry:
  mfmsr r9
  mtmsrd r13, 1
  ldarx r10, 0, r3
  cmpd cr6, r10, r5
  bne cr6, cas64_miss_differs
  stdcx. r4, 0, r3
  mtmsrd r9, 1
  bne cas64_miss_retry
  b cas64_miss_done
cas64_miss_differs:
  stdcx. r10, 0, r3
  mtmsrd r9, 1
cas64_miss_done:
  mr r11, r10
  mfmsr r6
  blr
  #_ REGISTER_OUT r11 0x1111222233334445
  #_ REGISTER_OUT r6 0x8000
  #_ MEMORY_OUT 10001000 11112222 33334445

test_atomic_or64:
  #_ MEMORY_IN 10001000 00000001 00000002
  #_ REGISTER_IN r3 0x10001000
  #_ REGISTER_IN r4 0x8000000000000100
  li r8, 0
or64_retry:
  mfmsr r9
  mtmsrd r13, 1
  ldarx r11, 0, r3
  or r10, r11, r4
  stdcx. r10, 0, r3
  mtmsrd r9, 1
  bne or64_retry
  mfmsr r8
  blr
  #_ REGISTER_OUT r10 0x8000000100000102
  #_ REGISTER_OUT r8 0x8000
  #_ MEMORY_OUT 10001000 80000001 00000102

# InterlockedPushEntrySList (TU4 sub_8314D1F0's shape): the stw of the entry's next
# inside the section keeps the global lock. Header: next 10002000, depth 1, sequence 7.
test_slist_push:
  #_ MEMORY_IN 10001000 10002000 00010007
  #_ MEMORY_IN 10003000 00000000
  #_ REGISTER_IN r5 0x10001000
  #_ REGISTER_IN r4 0x10003000
  li r9, 0
push_retry:
  mfmsr r8
  mtmsrd r13, 1
  ldarx r6, 0, r5
  rldicl r3, r6, 32, 32
  stw r3, 0(r4)
  lwsync
  addis r7, r6, 1
  addi r6, r6, 1
  rlwimi r6, r7, 0, 0, 15
  rldimi r6, r4, 32, 0
  stdcx. r6, 0, r5
  mtmsrd r8, 1
  bne- push_retry
  mfmsr r9
  blr
  #_ REGISTER_OUT r3 0x10002000
  #_ REGISTER_OUT r8 0x8000
  #_ REGISTER_OUT r9 0x8000
  #_ MEMORY_OUT 10001000 10003000 00020008
  #_ MEMORY_OUT 10003000 10002000

# A section with anything else inside keeps the lock: mfmsr reads interrupts off
# inside it, on after it.
test_locked_section_mfmsr:
  #_ MEMORY_IN 10001000 00000007
  #_ REGISTER_IN r11 0x10001000
  li r7, 1
  li r8, 0
locked_retry:
  mfmsr r9
  mtmsrd r13, 1
  lwarx r10, 0, r11
  mfmsr r7
  addi r10, r10, 1
  stwcx. r10, 0, r11
  mtmsrd r9, 1
  bne locked_retry
  mfmsr r8
  blr
  #_ REGISTER_OUT r7 0
  #_ REGISTER_OUT r8 0x8000
  #_ REGISTER_OUT r10 8
  #_ MEMORY_OUT 10001000 00000008
