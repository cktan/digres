# s01 -- REPEATABLE READ: a seal of a row unchanged since the
# writer's snapshot is no conflict.
#
# S erases R1 with a sealed tombstone and commits, as the Column
# Store's seal moves a row into a row group (COLSTORE.md §10.2).
# T1's snapshot is older than the seal, but the row's own commit,
# the seed, is older than T1: an update and a delete get `sealed`,
# not `conflict`, and T1 is not aborted. An insert goes in over
# the sealed tombstone, as an update's reinsert does.

pairings rr
session T1 rr
session S rc
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 S begin
@4 S seal R1
@5 S commit
@6 T1 write R1 1
@6 check last T1 sealed
@7 T1 delete R1
@7 check last T1 sealed
@8 T1 insert R1 5
@8 check last T1 ok
@9 T1 commit
@10 check committed S T1
@10 check row R1 5
