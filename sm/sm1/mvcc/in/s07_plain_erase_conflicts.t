# s07 -- REPEATABLE READ: a plain erase in a seal's place is a
# conflict.
#
# The schedule of s01, with S's erase a plain one: T1's update
# meets a tombstone committed after its snapshot, and
# first-committer-wins aborts T1. Only the seal's flag spares it.

pairings rr
session T1 rr
session S rc
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 S begin
@4 S delete R1
@5 S commit
@6 T1 write R1 1
@6 check last T1 conflict
@7 check committed S
@7 check row R1 -
