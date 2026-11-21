# s04 -- READ COMMITTED: a writer waits for a seal in flight that
# aborts, and its write goes ahead.
#
# S's sealed tombstone dies with S: T1, which waited on it, then
# writes over the row as if no seal had been tried.

pairings rr
session T1 rc
session S rc
seed R1 0

@1 S begin
@2 S seal R1
@3 T1 begin
@4 T1 write R1 1 blocks
@5 S abort
@6 check last T1 ok
@7 T1 commit
@8 check committed T1
@8 check row R1 1
