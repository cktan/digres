# s02 -- REPEATABLE READ: a row changed after the writer's
# snapshot, then sealed, is a conflict.
#
# T2 updates R1 and commits after T1 began; S then seals it. The
# seal is transparent, so first-committer-wins judges T1's write
# by T2's commit under the sealed tombstone: `conflict`, and T1
# is aborted, as if no seal had come between.

pairings rr
session T1 rr
session T2 rc
session S rc
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@4 T2 write R1 1
@5 T2 commit
@6 S begin
@7 S seal R1
@8 S commit
@9 T1 write R1 2
@9 check last T1 conflict
@10 check committed T2 S
@10 check row R1 -
