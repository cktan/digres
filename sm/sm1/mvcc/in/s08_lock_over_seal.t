# s08 -- REPEATABLE READ: a row lock over a sealed tombstone is
# judged by the version under it.
#
# S seals R1, unchanged since T1's snapshot: T1's exclusive lock on
# it is granted, as FOR UPDATE of a row the seal moved must be
# (COLSTORE.md §9.1). T2 updates R2 after T1 began, then S seals
# it: T1's lock on R2 is a conflict, as over the update itself.

pairings rr
session T1 rr
session T2 rc
session S rc
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 read R1
@3 S begin
@4 S seal R1
@5 S commit
@6 T1 lock R1 exclusive
@6 check last T1 ok
@7 T2 begin
@8 T2 write R2 1
@9 T2 commit
@10 S begin
@11 S seal R2
@12 S commit
@13 T1 lock R2 exclusive
@13 check last T1 conflict
@14 check committed T2 S
