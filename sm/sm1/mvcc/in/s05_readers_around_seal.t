# s05 -- a reader sees a sealed tombstone as any tombstone.
#
# T1 began before S's seal, T2 after it. T1 still reads the row,
# as the seal committed after its snapshot; T2 sees no row in this
# store, as the row lives on elsewhere. A scan agrees.

pairings rr
session T1 rr
session T2 rr
session S rc
seed R1 0
seed R2 0

@1 T1 begin
@2 S begin
@3 S seal R1
@4 S commit
@5 T2 begin
@6 T1 read R1
@6 check last T1 0
@7 T2 read R1
@7 check last T2 -
@8 T1 scan R1 R2
@9 T2 scan R1 R2
@10 T1 commit
@11 T2 commit
@12 check row R1 -
@12 check row R2 0
