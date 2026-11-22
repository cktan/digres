# g04 -- the row reaper passes over a store being dropped.
#
# T2 updates R1 of the second store, and T3's lookup queues it: a
# pass, run by T2 with no transaction, reaps it. T2 updates R1 again,
# and T3's lookup queues it again. T1 then drops the store and holds
# X on it: the pass cannot have its IX, and reaps nothing, so no reap
# writes into a store its drop takes away. T3's lookup queues R1 once
# more; once T1 commits, the store is gone at the newest state, and a
# pass reaps nothing again. The first store still takes a write
# (../../LOCKMGR.md §2.3, ../../GC.md §13.5).

stores chained
pairings rr
session T1
session T2
session T3 rc
seed R1 0

@1 T2 begin
@1 T2 write2 R1 a
@1 T2 commit
@2 T3 begin
@2 T3 lookup R1
@2 T3 commit
@3 T2 reap
@4 check last T2 1
@5 T2 begin
@5 T2 write2 R1 b
@5 T2 commit
@6 T3 begin
@6 T3 lookup R1
@6 T3 commit
@7 T1 begin
@7 T1 rmstore
@8 T2 reap
@9 check last T2 0
@10 T3 begin
@10 T3 lookup R1
@10 T3 commit
@11 T1 commit
@12 T2 reap
@13 check last T2 0
@14 T2 begin
@14 T2 write R1 c
@14 T2 commit
@15 check row R1 c
