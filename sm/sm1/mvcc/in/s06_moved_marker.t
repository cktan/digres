# s06 -- a moved marker, a live sealed version: every write over
# it gets `sealed`, and a reader sees it as any version.
#
# S puts a moved marker at R1, a key with no version, as RG
# compaction marks a row it moves to a new row group
# (COLSTORE.md §11). T1's snapshot is older than the marker, and
# nothing under it committed: no conflict, and every write, an
# insert too, gets `sealed`. T2, after it, reads the marker.

pairings rr
session T1 rr
session T2 rr
session S rc

@1 T1 begin
@2 T1 read R1
@3 S begin
@4 S sealput R1 m
@5 S commit
@6 T1 insert R1 1
@6 check last T1 sealed
@7 T1 write R1 1
@7 check last T1 sealed
@8 T1 delete R1
@8 check last T1 sealed
@9 T1 commit
@10 T2 begin
@11 T2 read R1
@11 check last T2 m
@12 T2 commit
@13 check committed S T1 T2
@13 check row R1 m
