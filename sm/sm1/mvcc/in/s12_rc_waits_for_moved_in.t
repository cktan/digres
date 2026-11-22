# s12 -- READ COMMITTED: a writer waits for a moved-in version in
# flight, then writes over it.
#
# E's moved-in version, in flight, is its lock: T1 blocks on it.
# E commits; T1's write then judges first-committer-wins by the
# seed, under the moved-in version and the sealed tombstone,
# which committed before T1's statement: no `updated`, and the
# write goes on over the moved-in version.

pairings rr
session T1 rc
session S rc
session E rc
seed R1 0

@1 S begin
@2 S seal R1
@3 S commit
@4 E begin
@5 E movein R1 0
@6 T1 begin
@7 T1 write R1 1 blocks
@8 E commit
@9 check last T1 ok
@10 T1 readlatest R1
@10 check last T1 1
@11 T1 commit
@12 check committed S E T1
@12 check row R1 1
