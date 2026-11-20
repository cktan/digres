# s03 -- READ COMMITTED: a writer waits for a seal in flight, and
# is told `sealed` when it commits.
#
# S's sealed tombstone, in flight, is its lock: T1 blocks on it.
# S commits; T1's write then finds the sealed tombstone as the
# newest version, and gets `sealed`: the row lives on elsewhere,
# unchanged. A delete taken again gets `sealed` too.

pairings rr
session T1 rc
session S rc
seed R1 0

@1 S begin
@2 S seal R1
@3 T1 begin
@4 T1 write R1 1 blocks
@5 S commit
@6 check last T1 sealed
@7 T1 delete R1
@7 check last T1 sealed
@8 T1 commit
@9 check committed S T1
@9 check row R1 -
