# s10 -- a moved-in version, a live sealed version: a reader sees
# it as any version, and a write goes on over it.
#
# S seals R1, as the Column Store's seal moves a row into a row
# group; E then moves it back in, unchanged, as elimination puts
# a row of an RG into the delta store (COLSTORE2.md §6.8). E
# moves R2 in too, a key with no version, as for a row of a bulk
# load. T1's snapshot is older than both, but the rows' own
# commits are older than T1: no conflict. T1 updates R1 and
# deletes R2 over the moved-in versions, and an insert over one
# is a duplicate. T2, after E, reads both.

pairings rr
session T1 rr
session T2 rr
session S rc
session E rc
seed R1 0

@1 T1 begin
@2 T1 read R1
@2 check last T1 0
@3 S begin
@4 S seal R1
@5 S commit
@6 E begin
@7 E movein R1 0
@8 E movein R2 7
@9 E commit
@10 T2 begin
@11 T2 read R1
@11 check last T2 0
@12 T2 read R2
@12 check last T2 7
@13 T2 commit
@14 T1 read R1
@14 check last T1 0
@15 T1 insert R1 9
@15 check last T1 dupkey
@16 T1 write R1 1
@16 check last T1 ok
@17 T1 delete R2
@17 check last T1 ok
@18 T1 commit
@19 check committed S E T2 T1
@19 check row R1 1
@19 check row R2 -
