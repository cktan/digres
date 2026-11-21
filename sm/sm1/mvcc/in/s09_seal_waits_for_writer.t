# s09 -- READ COMMITTED: a seal that meets a writer in flight waits
# for it; the writer's commit gives the seal `updated`, its abort lets
# the seal go on.
#
# The Column Store's seal tries each row's lock first and passes over
# a busy row (COLSTORE.md §10.1); this is sm1's rule under that, for
# a seal that does not try first. T1 updates R1 and holds it; S's
# sealed erase of R1 blocks on T1. T1 commits: S's erase gets
# `updated`, as any READ COMMITTED write over a newer commit does, and
# writes nothing: the seal passes the row over, and R1 keeps T1's
# value. R2 is the same with T1's abort: S seals the seeded version.

pairings rr
session T1 rc
session S rc
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 write R1 1
@3 S begin
@4 S seal R1 blocks
@5 T1 commit
@6 check last S updated
@7 T1 begin
@8 T1 write R2 2
@9 S seal R2 blocks
@10 T1 abort
@11 check last S ok
@12 S commit
@13 check committed S
@13 check row R1 1
@13 check row R2 -
