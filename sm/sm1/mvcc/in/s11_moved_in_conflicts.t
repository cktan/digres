# s11 -- REPEATABLE READ: a moved-in version over a change that
# committed after the writer's snapshot is a conflict.
#
# T2 updates R1 after T1 began. S seals R1, and E moves it back
# in, unchanged. First-committer-wins sees through the moved-in
# version and the sealed tombstone to T2's update: T1's write
# fails with `conflict`, and a lock of R1 by T3, also older than
# T2's commit, does too.

pairings rr
session T1 rr
session T2 rc
session T3 rr
session S rc
session E rc
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 T3 begin
@4 T3 read R1
@5 T2 begin
@6 T2 write R1 1
@7 T2 commit
@8 S begin
@9 S seal R1
@10 S commit
@11 E begin
@12 E movein R1 1
@13 E commit
@14 T1 write R1 2
@14 check last T1 conflict
@15 T3 lock R1 exclusive
@15 check last T3 conflict
@16 check committed T2 S E
@16 check row R1 1
