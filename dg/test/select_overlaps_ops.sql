-- Verify OVERLAPS of timestamp, date, time and timetz, with null ends, which
-- ty does (ty/DESIGN.md section 19).
-- timestamp
SELECT 0 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 1 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 2 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 3 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 4 AS i, (timestamp '2024-01-01 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 5 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 6 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 7 AS i, (NULL::timestamp, timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 8 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 9 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (timestamp 'infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 10 AS i, (timestamp '-infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 11 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', NULL::timestamp) AS r
UNION ALL
SELECT 12 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 13 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 14 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 15 AS i, (NULL::timestamp, timestamp '-infinity') OVERLAPS (timestamp '-infinity', NULL::timestamp) AS r
UNION ALL
SELECT 16 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp 'infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 17 AS i, (timestamp '2024-01-05 12:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 18 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 19 AS i, (timestamp '-infinity', NULL::timestamp) OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 20 AS i, (timestamp 'infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 21 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (timestamp '-infinity', NULL::timestamp) AS r
UNION ALL
SELECT 22 AS i, (timestamp '-infinity', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 23 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 24 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (NULL::timestamp, NULL::timestamp) AS r
UNION ALL
SELECT 25 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 26 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 27 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 28 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 29 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 30 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 31 AS i, (timestamp '-infinity', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 32 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 33 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 34 AS i, (timestamp 'infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 35 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 36 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 37 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp 'infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 38 AS i, (timestamp '-infinity', timestamp 'infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 39 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 40 AS i, (timestamp '-infinity', NULL::timestamp) OVERLAPS (timestamp 'infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 41 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 42 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 43 AS i, (timestamp '-infinity', timestamp 'infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 44 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 45 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 46 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 47 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 48 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 49 AS i, (timestamp '-infinity', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 50 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (timestamp '-infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 51 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 52 AS i, (timestamp 'infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 53 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 54 AS i, (timestamp 'infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 55 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 56 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 57 AS i, (timestamp 'infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 58 AS i, (NULL::timestamp, timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 59 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 60 AS i, (timestamp '2024-01-05 12:00:00', timestamp '-infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 61 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 62 AS i, (timestamp '2024-01-05 12:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 63 AS i, (timestamp '2024-01-01 00:00:00', timestamp '-infinity') OVERLAPS (timestamp 'infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 64 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-01 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 65 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 66 AS i, (timestamp 'infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 67 AS i, (NULL::timestamp, timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 68 AS i, (timestamp 'infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 69 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 70 AS i, (timestamp '2024-01-01 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 71 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 72 AS i, (timestamp '-infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 73 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (NULL::timestamp, timestamp 'infinity') AS r
UNION ALL
SELECT 74 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 75 AS i, (NULL::timestamp, timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 76 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 77 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 78 AS i, (timestamp 'infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 79 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 80 AS i, (timestamp '2024-01-01 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 81 AS i, (timestamp '-infinity', NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 82 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 83 AS i, (NULL::timestamp, timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 84 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 85 AS i, (timestamp 'infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 86 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 87 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 88 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 89 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 90 AS i, (timestamp 'infinity', NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 91 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 92 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 93 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 94 AS i, (timestamp 'infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 95 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 96 AS i, (NULL::timestamp, NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 97 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 98 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 99 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 100 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 101 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 102 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 103 AS i, (timestamp 'infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 104 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 105 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 106 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 107 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', NULL::timestamp) AS r
UNION ALL
SELECT 108 AS i, (timestamp '2024-01-05 12:00:00', timestamp '-infinity') OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 109 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 110 AS i, (NULL::timestamp, NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp 'infinity') AS r
UNION ALL
SELECT 111 AS i, (timestamp '-infinity', timestamp 'infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 112 AS i, (timestamp '2024-01-01 00:00:00', timestamp '-infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 113 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 114 AS i, (timestamp 'infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 115 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (NULL::timestamp, timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 116 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 117 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 118 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 119 AS i, (timestamp 'infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 120 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 121 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 122 AS i, (NULL::timestamp, timestamp '-infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 123 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 124 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', NULL::timestamp) AS r
UNION ALL
SELECT 125 AS i, (timestamp '2024-01-05 12:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 126 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 127 AS i, (timestamp '2024-01-01 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 128 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 129 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 130 AS i, (timestamp '2024-01-05 12:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 131 AS i, (timestamp '2024-01-05 12:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 132 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 133 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 134 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 135 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '-infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 136 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 137 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 138 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 139 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 140 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (NULL::timestamp, timestamp 'infinity') AS r
UNION ALL
SELECT 141 AS i, (NULL::timestamp, timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 142 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 143 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 144 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 145 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 146 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 147 AS i, (NULL::timestamp, NULL::timestamp) OVERLAPS (timestamp 'infinity', NULL::timestamp) AS r
UNION ALL
SELECT 148 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 149 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 150 AS i, (NULL::timestamp, NULL::timestamp) OVERLAPS (timestamp '-infinity', NULL::timestamp) AS r
UNION ALL
SELECT 151 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, NULL::timestamp) AS r
UNION ALL
SELECT 152 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 153 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 154 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 155 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 156 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 157 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 158 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 159 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 160 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 161 AS i, (NULL::timestamp, timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 162 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 163 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 164 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 165 AS i, (timestamp '-infinity', NULL::timestamp) OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 166 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp 'infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 167 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 168 AS i, (NULL::timestamp, timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 169 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 170 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 171 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 172 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 173 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 174 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 175 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 176 AS i, (NULL::timestamp, timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 177 AS i, (timestamp 'infinity', NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 178 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 179 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 180 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (timestamp 'infinity', NULL::timestamp) AS r
UNION ALL
SELECT 181 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', NULL::timestamp) AS r
UNION ALL
SELECT 182 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 183 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 184 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 185 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 186 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 187 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 188 AS i, (NULL::timestamp, timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 189 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 190 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 191 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 192 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 193 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 194 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 195 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 196 AS i, (timestamp '-infinity', timestamp 'infinity') OVERLAPS (timestamp 'infinity', NULL::timestamp) AS r
UNION ALL
SELECT 197 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 198 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 199 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 200 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 201 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 202 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 203 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 204 AS i, (NULL::timestamp, timestamp '2024-01-05 12:00:00') OVERLAPS (NULL::timestamp, NULL::timestamp) AS r
UNION ALL
SELECT 205 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '-infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 206 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 207 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 208 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 209 AS i, (timestamp '-infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 210 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 211 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 212 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 213 AS i, (timestamp '2024-01-05 12:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 214 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 215 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (timestamp '-infinity', NULL::timestamp) AS r
UNION ALL
SELECT 216 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 217 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 218 AS i, (timestamp 'infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 219 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 220 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 221 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 222 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 223 AS i, (timestamp 'infinity', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 224 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, NULL::timestamp) AS r
UNION ALL
SELECT 225 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 226 AS i, (NULL::timestamp, timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 227 AS i, (timestamp 'infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 228 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 229 AS i, (NULL::timestamp, timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 230 AS i, (timestamp '2024-01-01 00:00:00', timestamp '-infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 231 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 232 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 233 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (timestamp '-infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 234 AS i, (timestamp 'infinity', NULL::timestamp) OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 235 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 236 AS i, (NULL::timestamp, timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 237 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 238 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 239 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 240 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 241 AS i, (NULL::timestamp, timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 242 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 243 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 244 AS i, (NULL::timestamp, NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 245 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (NULL::timestamp, NULL::timestamp) AS r
UNION ALL
SELECT 246 AS i, (timestamp 'infinity', NULL::timestamp) OVERLAPS (timestamp '-infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 247 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 248 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 249 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 250 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 251 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 252 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 253 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 254 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 255 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 256 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 257 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 258 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 259 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 260 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 261 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 262 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 263 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 264 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 265 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', NULL::timestamp) AS r
UNION ALL
SELECT 266 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 267 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 268 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 269 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 270 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 271 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 272 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 273 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 274 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 275 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 276 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 277 AS i, (timestamp 'infinity', timestamp '-infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 278 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 279 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 280 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp 'infinity') AS r
UNION ALL
SELECT 281 AS i, (timestamp '-infinity', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 282 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 283 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 284 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 285 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 286 AS i, (timestamp 'infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 287 AS i, (timestamp 'infinity', timestamp '-infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 288 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 289 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (NULL::timestamp, timestamp 'infinity') AS r
UNION ALL
SELECT 290 AS i, (timestamp '-infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp 'infinity') AS r
UNION ALL
SELECT 291 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 292 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 293 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 294 AS i, (timestamp '-infinity', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 295 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 296 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 297 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp 'infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 298 AS i, (timestamp 'infinity', NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 299 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 300 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 301 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 302 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 303 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 304 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 305 AS i, (timestamp '-infinity', timestamp 'infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 306 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 307 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 308 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 309 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 310 AS i, (timestamp '-infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 311 AS i, (timestamp 'infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp 'infinity', NULL::timestamp) AS r
UNION ALL
SELECT 312 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 313 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 314 AS i, (timestamp 'infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 315 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 316 AS i, (timestamp '-infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 317 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 318 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 319 AS i, (timestamp '-infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 320 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 321 AS i, (timestamp '-infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, NULL::timestamp) AS r
UNION ALL
SELECT 322 AS i, (timestamp '-infinity', NULL::timestamp) OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 323 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 324 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 325 AS i, (timestamp 'infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 326 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 327 AS i, (NULL::timestamp, timestamp '-infinity') OVERLAPS (timestamp 'infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 328 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 329 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 330 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', NULL::timestamp) AS r
UNION ALL
SELECT 331 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 332 AS i, (timestamp '2024-01-01 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 333 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 334 AS i, (timestamp 'infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', NULL::timestamp) AS r
UNION ALL
SELECT 335 AS i, (timestamp '-infinity', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 336 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 337 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 338 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 339 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 340 AS i, (NULL::timestamp, timestamp '2024-01-05 12:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 341 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '-infinity', NULL::timestamp) AS r
UNION ALL
SELECT 342 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 343 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 344 AS i, (timestamp 'infinity', NULL::timestamp) OVERLAPS (timestamp '2024-01-01 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 345 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 346 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 347 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 348 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 349 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 350 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 351 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 352 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 353 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 354 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 355 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 356 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 357 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 358 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 359 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 360 AS i, (NULL::timestamp, timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 361 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 362 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', NULL::timestamp) AS r
UNION ALL
SELECT 363 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 364 AS i, (timestamp '-infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 365 AS i, (timestamp '2024-01-05 12:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 366 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 367 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 368 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 369 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 370 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp 'infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 371 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 372 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 373 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 374 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 375 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 376 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (NULL::timestamp, timestamp 'infinity') AS r
UNION ALL
SELECT 377 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (timestamp 'infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 378 AS i, (timestamp 'infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 379 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 380 AS i, (timestamp '-infinity', NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 381 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 382 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 383 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 384 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-05 12:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 385 AS i, (timestamp '-infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 386 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-01 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 387 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 388 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, NULL::timestamp) AS r
UNION ALL
SELECT 389 AS i, (timestamp 'infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 390 AS i, (NULL::timestamp, NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 391 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 392 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 393 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 394 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 395 AS i, (NULL::timestamp, timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 396 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 397 AS i, (timestamp 'infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 398 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 399 AS i, (NULL::timestamp, timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 400 AS i, (NULL::timestamp, timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 401 AS i, (timestamp 'infinity', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 402 AS i, (timestamp '-infinity', NULL::timestamp) OVERLAPS (timestamp 'infinity', NULL::timestamp) AS r
UNION ALL
SELECT 403 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '-infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 404 AS i, (timestamp '2024-01-01 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 405 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 406 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 407 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 408 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp 'infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 409 AS i, (timestamp '2024-01-05 12:00:00', timestamp '-infinity') OVERLAPS (NULL::timestamp, NULL::timestamp) AS r
UNION ALL
SELECT 410 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 411 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (NULL::timestamp, NULL::timestamp) AS r
UNION ALL
SELECT 412 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 413 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (NULL::timestamp, NULL::timestamp) AS r
UNION ALL
SELECT 414 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 415 AS i, (NULL::timestamp, timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 416 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 417 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, NULL::timestamp) AS r
UNION ALL
SELECT 418 AS i, (timestamp '-infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 419 AS i, (timestamp '-infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 420 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 421 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 422 AS i, (timestamp 'infinity', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 423 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 424 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 425 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 426 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 427 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 428 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 429 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 430 AS i, (timestamp 'infinity', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 431 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 432 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 433 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 434 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 435 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 436 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 437 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 438 AS i, (timestamp 'infinity', timestamp '-infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 439 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 440 AS i, (timestamp '-infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 441 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 442 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 443 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 444 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '-infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 445 AS i, (timestamp '2024-01-01 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 446 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', NULL::timestamp) AS r
UNION ALL
SELECT 447 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 448 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, NULL::timestamp) AS r
UNION ALL
SELECT 449 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '-infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 450 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 451 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 452 AS i, (timestamp '-infinity', NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp '-infinity') AS r
UNION ALL
SELECT 453 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 454 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 455 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 456 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 457 AS i, (timestamp 'infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '-infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 458 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 459 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 460 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 461 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 462 AS i, (NULL::timestamp, timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 463 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 464 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 465 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 466 AS i, (timestamp '2024-01-05 12:00:00', timestamp '-infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 467 AS i, (NULL::timestamp, timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 468 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 469 AS i, (timestamp '2024-01-10 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 470 AS i, (timestamp '2024-01-05 12:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 471 AS i, (timestamp '-infinity', timestamp 'infinity') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 472 AS i, (timestamp 'infinity', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 473 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 474 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp 'infinity') AS r
UNION ALL
SELECT 475 AS i, (timestamp '2024-01-01 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 476 AS i, (timestamp 'infinity', NULL::timestamp) OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 477 AS i, (NULL::timestamp, timestamp '2024-01-05 12:00:00') OVERLAPS (NULL::timestamp, timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 478 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '-infinity') AS r
UNION ALL
SELECT 479 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 480 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 481 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 482 AS i, (timestamp '2024-01-05 12:00:00', timestamp 'infinity') OVERLAPS (timestamp 'infinity', timestamp '-infinity') AS r
UNION ALL
SELECT 483 AS i, (timestamp '-infinity', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp '2024-01-05 12:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 484 AS i, (timestamp 'infinity', NULL::timestamp) OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 485 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (NULL::timestamp, timestamp 'infinity') AS r
UNION ALL
SELECT 486 AS i, (timestamp '2024-01-01 00:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 487 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', NULL::timestamp) AS r
UNION ALL
SELECT 488 AS i, (timestamp 'infinity', timestamp 'infinity') OVERLAPS (timestamp 'infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 489 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 490 AS i, (timestamp '2024-01-05 12:00:00', NULL::timestamp) OVERLAPS (timestamp '2024-01-01 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 491 AS i, (timestamp '2024-01-10 00:00:00', timestamp '-infinity') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 492 AS i, (timestamp '2024-01-05 12:00:00', timestamp '-infinity') OVERLAPS (NULL::timestamp, timestamp 'infinity') AS r
UNION ALL
SELECT 493 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') OVERLAPS (timestamp 'infinity', timestamp 'infinity') AS r
UNION ALL
SELECT 494 AS i, (NULL::timestamp, timestamp '-infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 495 AS i, (timestamp 'infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 496 AS i, (timestamp '-infinity', timestamp '2024-01-05 12:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-05 12:00:00') AS r
UNION ALL
SELECT 497 AS i, (timestamp '2024-01-10 00:00:00', timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp '2024-01-10 00:00:00', timestamp '2024-01-10 00:00:00') AS r
UNION ALL
SELECT 498 AS i, (timestamp '2024-01-10 00:00:00', timestamp 'infinity') OVERLAPS (timestamp '-infinity', timestamp '2024-01-01 00:00:00') AS r
UNION ALL
SELECT 499 AS i, (NULL::timestamp, timestamp '2024-01-01 00:00:00') OVERLAPS (timestamp 'infinity', timestamp '2024-01-10 00:00:00') AS r
ORDER BY 1;
-- date
SELECT 0 AS i, (date '2024-01-05', NULL::date) OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 1 AS i, (date '2024-01-10', date '2024-01-10') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 2 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 3 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 4 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 5 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 6 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 7 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 8 AS i, (date 'infinity', NULL::date) OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 9 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 10 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 11 AS i, (date '2024-01-10', NULL::date) OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 12 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 13 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 14 AS i, (NULL::date, date '2024-01-05') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 15 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 16 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 17 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 18 AS i, (date '2024-01-01', NULL::date) OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 19 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 20 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 21 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 22 AS i, (date 'infinity', NULL::date) OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 23 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 24 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 25 AS i, (date 'infinity', date 'infinity') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 26 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 27 AS i, (date 'infinity', NULL::date) OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 28 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 29 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 30 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 31 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 32 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 33 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 34 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 35 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 36 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 37 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 38 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 39 AS i, (date 'infinity', NULL::date) OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 40 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 41 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 42 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 43 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 44 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 45 AS i, (NULL::date, date '2024-01-01') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 46 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 47 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 48 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 49 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 50 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 51 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 52 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 53 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 54 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 55 AS i, (NULL::date, date 'infinity') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 56 AS i, (NULL::date, NULL::date) OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 57 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 58 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 59 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 60 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 61 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 62 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 63 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 64 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 65 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 66 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 67 AS i, (NULL::date, NULL::date) OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 68 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 69 AS i, (date 'infinity', date 'infinity') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 70 AS i, (date 'infinity', NULL::date) OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 71 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 72 AS i, (date 'infinity', date 'infinity') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 73 AS i, (date '2024-01-10', date '2024-01-10') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 74 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 75 AS i, (date '2024-01-10', date '2024-01-10') OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 76 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 77 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 78 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 79 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 80 AS i, (date 'infinity', NULL::date) OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 81 AS i, (date '2024-01-05', NULL::date) OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 82 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 83 AS i, (date 'infinity', NULL::date) OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 84 AS i, (NULL::date, NULL::date) OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 85 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 86 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 87 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 88 AS i, (NULL::date, date 'infinity') OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 89 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 90 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 91 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 92 AS i, (NULL::date, NULL::date) OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 93 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 94 AS i, (NULL::date, date '2024-01-01') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 95 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 96 AS i, (date '2024-01-01', date '2024-01-01') OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 97 AS i, (NULL::date, NULL::date) OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 98 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 99 AS i, (date '2024-01-10', date '2024-01-10') OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 100 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 101 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 102 AS i, (date 'infinity', date 'infinity') OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 103 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 104 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 105 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 106 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 107 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 108 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 109 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 110 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 111 AS i, (date '2024-01-10', date '2024-01-10') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 112 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 113 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 114 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 115 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 116 AS i, (NULL::date, NULL::date) OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 117 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 118 AS i, (NULL::date, date '2024-01-10') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 119 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 120 AS i, (NULL::date, date '2024-01-05') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 121 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 122 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 123 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 124 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 125 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 126 AS i, (date '2024-01-05', NULL::date) OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 127 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 128 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 129 AS i, (NULL::date, date 'infinity') OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 130 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 131 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 132 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 133 AS i, (date '2024-01-01', NULL::date) OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 134 AS i, (NULL::date, date 'infinity') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 135 AS i, (NULL::date, date 'infinity') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 136 AS i, (date '2024-01-05', NULL::date) OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 137 AS i, (date 'infinity', NULL::date) OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 138 AS i, (date '2024-01-01', date '2024-01-01') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 139 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 140 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 141 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 142 AS i, (date '2024-01-01', NULL::date) OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 143 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 144 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 145 AS i, (date '2024-01-01', date '2024-01-01') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 146 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 147 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 148 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 149 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 150 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 151 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 152 AS i, (date '2024-01-10', date '2024-01-10') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 153 AS i, (date 'infinity', NULL::date) OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 154 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 155 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 156 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 157 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 158 AS i, (date '2024-01-05', NULL::date) OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 159 AS i, (date '2024-01-05', NULL::date) OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 160 AS i, (NULL::date, date '2024-01-01') OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 161 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 162 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 163 AS i, (NULL::date, NULL::date) OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 164 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 165 AS i, (NULL::date, date '2024-01-01') OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 166 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 167 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 168 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 169 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 170 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 171 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 172 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 173 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 174 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 175 AS i, (date 'infinity', date 'infinity') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 176 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 177 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 178 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 179 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 180 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 181 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 182 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 183 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 184 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 185 AS i, (date '2024-01-10', date '2024-01-10') OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 186 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 187 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 188 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 189 AS i, (date '2024-01-01', date '2024-01-01') OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 190 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 191 AS i, (NULL::date, date 'infinity') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 192 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 193 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 194 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 195 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 196 AS i, (date '2024-01-01', date '2024-01-01') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 197 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 198 AS i, (NULL::date, date '2024-01-05') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 199 AS i, (date 'infinity', NULL::date) OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 200 AS i, (date 'infinity', NULL::date) OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 201 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 202 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 203 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 204 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 205 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 206 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 207 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 208 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 209 AS i, (date 'infinity', date 'infinity') OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 210 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 211 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 212 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 213 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 214 AS i, (date '2024-01-10', date '2024-01-10') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 215 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 216 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 217 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 218 AS i, (NULL::date, date '2024-01-01') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 219 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 220 AS i, (NULL::date, date '2024-01-01') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 221 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 222 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 223 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 224 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 225 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 226 AS i, (date '2024-01-01', date '2024-01-01') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 227 AS i, (date 'infinity', NULL::date) OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 228 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 229 AS i, (date 'infinity', NULL::date) OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 230 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 231 AS i, (NULL::date, NULL::date) OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 232 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 233 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 234 AS i, (NULL::date, date '2024-01-01') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 235 AS i, (NULL::date, date '2024-01-01') OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 236 AS i, (date '2024-01-10', date '2024-01-10') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 237 AS i, (date '2024-01-05', NULL::date) OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 238 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 239 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 240 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 241 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 242 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 243 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 244 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 245 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 246 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 247 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 248 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 249 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 250 AS i, (date '2024-01-05', NULL::date) OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 251 AS i, (date 'infinity', date 'infinity') OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 252 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 253 AS i, (date 'infinity', NULL::date) OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 254 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 255 AS i, (NULL::date, NULL::date) OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 256 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 257 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 258 AS i, (date 'infinity', NULL::date) OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 259 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 260 AS i, (NULL::date, date '2024-01-01') OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 261 AS i, (date '2024-01-01', date '2024-01-01') OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 262 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 263 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 264 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 265 AS i, (date 'infinity', date 'infinity') OVERLAPS (date '2024-01-05', date 'infinity') AS r
UNION ALL
SELECT 266 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 267 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 268 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 269 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 270 AS i, (date 'infinity', date 'infinity') OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 271 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 272 AS i, (NULL::date, date '2024-01-10') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 273 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 274 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 275 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 276 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 277 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 278 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 279 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 280 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 281 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 282 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 283 AS i, (NULL::date, NULL::date) OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 284 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 285 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 286 AS i, (date '2024-01-05', NULL::date) OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 287 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 288 AS i, (NULL::date, date '2024-01-01') OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 289 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 290 AS i, (date 'infinity', NULL::date) OVERLAPS (date '2024-01-01', date '2024-01-01') AS r
UNION ALL
SELECT 291 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 292 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 293 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 294 AS i, (date '2024-01-01', NULL::date) OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 295 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 296 AS i, (date 'infinity', date 'infinity') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 297 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 298 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 299 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 300 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 301 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 302 AS i, (NULL::date, NULL::date) OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 303 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 304 AS i, (date '2024-01-01', NULL::date) OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 305 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 306 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 307 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 308 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 309 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 310 AS i, (NULL::date, date 'infinity') OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 311 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 312 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 313 AS i, (NULL::date, date '2024-01-10') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 314 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 315 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 316 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 317 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 318 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 319 AS i, (date '2024-01-01', date '2024-01-01') OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 320 AS i, (date '2024-01-01', date '2024-01-01') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 321 AS i, (NULL::date, date 'infinity') OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 322 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (NULL::date, date 'infinity') AS r
UNION ALL
SELECT 323 AS i, (date 'infinity', date 'infinity') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 324 AS i, (date '2024-01-05', NULL::date) OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 325 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 326 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 327 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 328 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date '2024-01-01', NULL::date) AS r
UNION ALL
SELECT 329 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 330 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 331 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 332 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 333 AS i, (date '2024-01-10', date '2024-01-10') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 334 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 335 AS i, (NULL::date, date '2024-01-01') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 336 AS i, (NULL::date, date 'infinity') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 337 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 338 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 339 AS i, (date '2024-01-10', date '2024-01-05') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 340 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 341 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 342 AS i, (date '2024-01-10', NULL::date) OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 343 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 344 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date '2024-01-05', date '2024-01-01') AS r
UNION ALL
SELECT 345 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 346 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 347 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 348 AS i, (date '2024-01-05', date '2024-01-01') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 349 AS i, (date '2024-01-01', date '2024-01-01') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 350 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 351 AS i, (NULL::date, date 'infinity') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 352 AS i, (NULL::date, date 'infinity') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 353 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 354 AS i, (date 'infinity', date 'infinity') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 355 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 356 AS i, (date '2024-01-05', date '2024-01-05') OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 357 AS i, (date '2024-01-05', NULL::date) OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 358 AS i, (NULL::date, NULL::date) OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 359 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 360 AS i, (date '2024-01-05', NULL::date) OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 361 AS i, (date '2024-01-01', date '2024-01-05') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 362 AS i, (date '2024-01-01', date '2024-01-10') OVERLAPS (date '2024-01-01', date 'infinity') AS r
UNION ALL
SELECT 363 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date '2024-01-01', date '2024-01-05') AS r
UNION ALL
SELECT 364 AS i, (NULL::date, NULL::date) OVERLAPS (date '2024-01-05', date '2024-01-10') AS r
UNION ALL
SELECT 365 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 366 AS i, (date '2024-01-01', date '2024-01-01') OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 367 AS i, (date 'infinity', NULL::date) OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 368 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 369 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 370 AS i, (NULL::date, date 'infinity') OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 371 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 372 AS i, (NULL::date, NULL::date) OVERLAPS (date '2024-01-05', date '2024-01-05') AS r
UNION ALL
SELECT 373 AS i, (date '2024-01-01', date '2024-01-01') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 374 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 375 AS i, (NULL::date, date '2024-01-05') OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 376 AS i, (date '2024-01-10', date '2024-01-10') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 377 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date 'infinity', date '2024-01-10') AS r
UNION ALL
SELECT 378 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 379 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 380 AS i, (date 'infinity', date '2024-01-10') OVERLAPS (date 'infinity', date '2024-01-05') AS r
UNION ALL
SELECT 381 AS i, (NULL::date, date '2024-01-10') OVERLAPS (date 'infinity', date '2024-01-01') AS r
UNION ALL
SELECT 382 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 383 AS i, (date '2024-01-01', date 'infinity') OVERLAPS (NULL::date, date '2024-01-01') AS r
UNION ALL
SELECT 384 AS i, (date '2024-01-10', date '2024-01-01') OVERLAPS (date '2024-01-10', date '2024-01-01') AS r
UNION ALL
SELECT 385 AS i, (date 'infinity', NULL::date) OVERLAPS (NULL::date, date '2024-01-10') AS r
UNION ALL
SELECT 386 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date '2024-01-10', date '2024-01-10') AS r
UNION ALL
SELECT 387 AS i, (date 'infinity', date '2024-01-05') OVERLAPS (NULL::date, date '2024-01-05') AS r
UNION ALL
SELECT 388 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (date '2024-01-10', NULL::date) AS r
UNION ALL
SELECT 389 AS i, (date 'infinity', NULL::date) OVERLAPS (date 'infinity', NULL::date) AS r
UNION ALL
SELECT 390 AS i, (NULL::date, date '2024-01-01') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 391 AS i, (NULL::date, date '2024-01-01') OVERLAPS (date '2024-01-10', date 'infinity') AS r
UNION ALL
SELECT 392 AS i, (date '2024-01-10', NULL::date) OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 393 AS i, (date '2024-01-10', date 'infinity') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 394 AS i, (NULL::date, date '2024-01-05') OVERLAPS (date '2024-01-05', NULL::date) AS r
UNION ALL
SELECT 395 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (date '2024-01-01', date '2024-01-10') AS r
UNION ALL
SELECT 396 AS i, (date 'infinity', date '2024-01-01') OVERLAPS (NULL::date, NULL::date) AS r
UNION ALL
SELECT 397 AS i, (date 'infinity', date 'infinity') OVERLAPS (date 'infinity', date 'infinity') AS r
UNION ALL
SELECT 398 AS i, (date '2024-01-05', date '2024-01-10') OVERLAPS (date '2024-01-10', date '2024-01-05') AS r
UNION ALL
SELECT 399 AS i, (date '2024-01-05', date 'infinity') OVERLAPS (date 'infinity', NULL::date) AS r
ORDER BY 1;
-- time
SELECT 0 AS i, (time '05:00', time '01:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 1 AS i, (time '05:00', time '03:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 2 AS i, (time '05:00', NULL::time) OVERLAPS (time '03:00', NULL::time) AS r
UNION ALL
SELECT 3 AS i, (time '01:00', time '03:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 4 AS i, (time '01:00', NULL::time) OVERLAPS (time '01:00', NULL::time) AS r
UNION ALL
SELECT 5 AS i, (time '24:00', time '01:00') OVERLAPS (time '03:00', time '24:00') AS r
UNION ALL
SELECT 6 AS i, (time '01:00', time '24:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 7 AS i, (time '05:00', time '03:00') OVERLAPS (NULL::time, NULL::time) AS r
UNION ALL
SELECT 8 AS i, (time '24:00', time '05:00') OVERLAPS (time '01:00', time '24:00') AS r
UNION ALL
SELECT 9 AS i, (time '01:00', time '05:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 10 AS i, (NULL::time, NULL::time) OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 11 AS i, (time '03:00', time '01:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 12 AS i, (time '01:00', time '03:00') OVERLAPS (time '01:00', NULL::time) AS r
UNION ALL
SELECT 13 AS i, (time '01:00', NULL::time) OVERLAPS (time '24:00', time '03:00') AS r
UNION ALL
SELECT 14 AS i, (NULL::time, time '01:00') OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 15 AS i, (time '05:00', time '24:00') OVERLAPS (NULL::time, NULL::time) AS r
UNION ALL
SELECT 16 AS i, (time '01:00', time '05:00') OVERLAPS (time '24:00', time '24:00') AS r
UNION ALL
SELECT 17 AS i, (time '03:00', time '05:00') OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 18 AS i, (time '01:00', time '24:00') OVERLAPS (time '01:00', time '24:00') AS r
UNION ALL
SELECT 19 AS i, (time '24:00', time '05:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 20 AS i, (NULL::time, time '01:00') OVERLAPS (time '01:00', time '24:00') AS r
UNION ALL
SELECT 21 AS i, (time '01:00', time '05:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 22 AS i, (time '24:00', time '03:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 23 AS i, (time '05:00', time '05:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 24 AS i, (time '03:00', time '01:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 25 AS i, (time '24:00', time '24:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 26 AS i, (time '01:00', time '05:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 27 AS i, (time '24:00', time '03:00') OVERLAPS (NULL::time, time '24:00') AS r
UNION ALL
SELECT 28 AS i, (time '24:00', time '05:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 29 AS i, (time '05:00', NULL::time) OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 30 AS i, (time '01:00', time '03:00') OVERLAPS (NULL::time, time '24:00') AS r
UNION ALL
SELECT 31 AS i, (time '03:00', time '01:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 32 AS i, (time '01:00', time '03:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 33 AS i, (time '24:00', time '05:00') OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 34 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 35 AS i, (time '03:00', NULL::time) OVERLAPS (time '03:00', time '24:00') AS r
UNION ALL
SELECT 36 AS i, (time '05:00', time '24:00') OVERLAPS (time '24:00', time '01:00') AS r
UNION ALL
SELECT 37 AS i, (time '05:00', time '03:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 38 AS i, (time '24:00', time '01:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 39 AS i, (time '05:00', time '01:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 40 AS i, (time '24:00', time '03:00') OVERLAPS (time '03:00', time '24:00') AS r
UNION ALL
SELECT 41 AS i, (time '03:00', time '24:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 42 AS i, (time '24:00', time '05:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 43 AS i, (time '05:00', NULL::time) OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 44 AS i, (time '01:00', time '24:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 45 AS i, (time '24:00', time '05:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 46 AS i, (time '24:00', NULL::time) OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 47 AS i, (time '05:00', NULL::time) OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 48 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 49 AS i, (time '01:00', time '24:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 50 AS i, (time '24:00', time '05:00') OVERLAPS (time '01:00', time '24:00') AS r
UNION ALL
SELECT 51 AS i, (time '01:00', time '05:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 52 AS i, (time '24:00', time '03:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 53 AS i, (time '01:00', time '05:00') OVERLAPS (time '03:00', time '03:00') AS r
UNION ALL
SELECT 54 AS i, (time '05:00', time '24:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 55 AS i, (NULL::time, NULL::time) OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 56 AS i, (time '24:00', time '01:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 57 AS i, (NULL::time, time '01:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 58 AS i, (time '03:00', time '24:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 59 AS i, (NULL::time, time '03:00') OVERLAPS (time '03:00', time '24:00') AS r
UNION ALL
SELECT 60 AS i, (time '24:00', time '24:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 61 AS i, (NULL::time, time '05:00') OVERLAPS (NULL::time, NULL::time) AS r
UNION ALL
SELECT 62 AS i, (time '05:00', time '03:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 63 AS i, (time '03:00', time '24:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 64 AS i, (time '03:00', time '03:00') OVERLAPS (time '01:00', NULL::time) AS r
UNION ALL
SELECT 65 AS i, (time '05:00', NULL::time) OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 66 AS i, (time '03:00', time '05:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 67 AS i, (time '01:00', NULL::time) OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 68 AS i, (time '24:00', time '03:00') OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 69 AS i, (time '03:00', time '24:00') OVERLAPS (time '01:00', time '03:00') AS r
UNION ALL
SELECT 70 AS i, (NULL::time, time '24:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 71 AS i, (NULL::time, NULL::time) OVERLAPS (time '01:00', time '24:00') AS r
UNION ALL
SELECT 72 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 73 AS i, (NULL::time, time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 74 AS i, (time '03:00', NULL::time) OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 75 AS i, (time '01:00', NULL::time) OVERLAPS (time '01:00', time '24:00') AS r
UNION ALL
SELECT 76 AS i, (time '24:00', NULL::time) OVERLAPS (time '24:00', time '03:00') AS r
UNION ALL
SELECT 77 AS i, (NULL::time, time '24:00') OVERLAPS (time '01:00', NULL::time) AS r
UNION ALL
SELECT 78 AS i, (time '24:00', time '24:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 79 AS i, (time '05:00', time '05:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 80 AS i, (time '05:00', time '05:00') OVERLAPS (time '03:00', NULL::time) AS r
UNION ALL
SELECT 81 AS i, (time '05:00', time '03:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 82 AS i, (NULL::time, time '05:00') OVERLAPS (NULL::time, time '24:00') AS r
UNION ALL
SELECT 83 AS i, (time '05:00', time '24:00') OVERLAPS (time '24:00', time '24:00') AS r
UNION ALL
SELECT 84 AS i, (time '01:00', time '03:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 85 AS i, (time '01:00', NULL::time) OVERLAPS (time '03:00', time '03:00') AS r
UNION ALL
SELECT 86 AS i, (time '24:00', time '05:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 87 AS i, (time '24:00', time '03:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 88 AS i, (time '03:00', time '24:00') OVERLAPS (time '24:00', time '01:00') AS r
UNION ALL
SELECT 89 AS i, (time '05:00', time '05:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 90 AS i, (time '05:00', time '05:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 91 AS i, (NULL::time, NULL::time) OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 92 AS i, (time '24:00', time '03:00') OVERLAPS (time '24:00', time '24:00') AS r
UNION ALL
SELECT 93 AS i, (NULL::time, time '05:00') OVERLAPS (time '24:00', NULL::time) AS r
UNION ALL
SELECT 94 AS i, (time '01:00', time '05:00') OVERLAPS (time '24:00', time '03:00') AS r
UNION ALL
SELECT 95 AS i, (time '01:00', time '24:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 96 AS i, (time '03:00', time '05:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 97 AS i, (NULL::time, time '03:00') OVERLAPS (time '24:00', NULL::time) AS r
UNION ALL
SELECT 98 AS i, (time '01:00', time '05:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 99 AS i, (time '01:00', time '05:00') OVERLAPS (time '03:00', NULL::time) AS r
UNION ALL
SELECT 100 AS i, (time '03:00', time '24:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 101 AS i, (time '24:00', time '03:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 102 AS i, (NULL::time, time '05:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 103 AS i, (time '03:00', NULL::time) OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 104 AS i, (time '05:00', time '05:00') OVERLAPS (time '24:00', NULL::time) AS r
UNION ALL
SELECT 105 AS i, (time '05:00', time '05:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 106 AS i, (time '01:00', time '05:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 107 AS i, (NULL::time, time '03:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 108 AS i, (time '05:00', time '03:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 109 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 110 AS i, (time '05:00', time '01:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 111 AS i, (NULL::time, NULL::time) OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 112 AS i, (time '01:00', time '05:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 113 AS i, (time '03:00', time '01:00') OVERLAPS (time '03:00', time '03:00') AS r
UNION ALL
SELECT 114 AS i, (time '03:00', NULL::time) OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 115 AS i, (time '05:00', time '05:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 116 AS i, (time '03:00', time '03:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 117 AS i, (time '05:00', NULL::time) OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 118 AS i, (time '05:00', NULL::time) OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 119 AS i, (time '24:00', NULL::time) OVERLAPS (time '03:00', NULL::time) AS r
UNION ALL
SELECT 120 AS i, (time '01:00', NULL::time) OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 121 AS i, (time '05:00', time '05:00') OVERLAPS (time '03:00', NULL::time) AS r
UNION ALL
SELECT 122 AS i, (time '24:00', time '05:00') OVERLAPS (time '24:00', NULL::time) AS r
UNION ALL
SELECT 123 AS i, (time '05:00', NULL::time) OVERLAPS (time '24:00', time '01:00') AS r
UNION ALL
SELECT 124 AS i, (time '24:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 125 AS i, (time '03:00', time '05:00') OVERLAPS (NULL::time, time '24:00') AS r
UNION ALL
SELECT 126 AS i, (time '05:00', time '05:00') OVERLAPS (NULL::time, NULL::time) AS r
UNION ALL
SELECT 127 AS i, (NULL::time, time '01:00') OVERLAPS (time '03:00', time '24:00') AS r
UNION ALL
SELECT 128 AS i, (time '24:00', time '05:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 129 AS i, (time '03:00', time '05:00') OVERLAPS (time '24:00', time '01:00') AS r
UNION ALL
SELECT 130 AS i, (time '05:00', time '24:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 131 AS i, (time '05:00', time '03:00') OVERLAPS (time '03:00', time '03:00') AS r
UNION ALL
SELECT 132 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 133 AS i, (time '03:00', time '05:00') OVERLAPS (time '01:00', NULL::time) AS r
UNION ALL
SELECT 134 AS i, (time '05:00', time '03:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 135 AS i, (time '01:00', NULL::time) OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 136 AS i, (time '05:00', NULL::time) OVERLAPS (time '01:00', time '01:00') AS r
UNION ALL
SELECT 137 AS i, (time '24:00', time '03:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 138 AS i, (time '03:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 139 AS i, (time '03:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 140 AS i, (time '01:00', time '01:00') OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 141 AS i, (NULL::time, time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 142 AS i, (time '05:00', NULL::time) OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 143 AS i, (time '03:00', time '03:00') OVERLAPS (time '24:00', NULL::time) AS r
UNION ALL
SELECT 144 AS i, (time '03:00', NULL::time) OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 145 AS i, (time '01:00', time '01:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 146 AS i, (time '05:00', time '03:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 147 AS i, (time '24:00', NULL::time) OVERLAPS (time '24:00', time '24:00') AS r
UNION ALL
SELECT 148 AS i, (time '24:00', time '01:00') OVERLAPS (time '03:00', time '03:00') AS r
UNION ALL
SELECT 149 AS i, (time '05:00', time '05:00') OVERLAPS (time '01:00', time '01:00') AS r
UNION ALL
SELECT 150 AS i, (time '05:00', time '01:00') OVERLAPS (time '01:00', NULL::time) AS r
UNION ALL
SELECT 151 AS i, (time '05:00', time '05:00') OVERLAPS (time '01:00', time '24:00') AS r
UNION ALL
SELECT 152 AS i, (NULL::time, time '24:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 153 AS i, (time '01:00', time '05:00') OVERLAPS (time '01:00', time '03:00') AS r
UNION ALL
SELECT 154 AS i, (NULL::time, time '05:00') OVERLAPS (time '24:00', time '24:00') AS r
UNION ALL
SELECT 155 AS i, (time '05:00', NULL::time) OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 156 AS i, (time '05:00', NULL::time) OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 157 AS i, (time '05:00', NULL::time) OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 158 AS i, (time '05:00', NULL::time) OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 159 AS i, (time '01:00', time '24:00') OVERLAPS (time '24:00', time '03:00') AS r
UNION ALL
SELECT 160 AS i, (NULL::time, time '05:00') OVERLAPS (time '03:00', time '03:00') AS r
UNION ALL
SELECT 161 AS i, (time '05:00', NULL::time) OVERLAPS (NULL::time, NULL::time) AS r
UNION ALL
SELECT 162 AS i, (time '01:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 163 AS i, (time '05:00', NULL::time) OVERLAPS (time '24:00', time '01:00') AS r
UNION ALL
SELECT 164 AS i, (time '01:00', time '05:00') OVERLAPS (NULL::time, time '24:00') AS r
UNION ALL
SELECT 165 AS i, (time '05:00', time '24:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 166 AS i, (NULL::time, time '05:00') OVERLAPS (time '01:00', time '03:00') AS r
UNION ALL
SELECT 167 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 168 AS i, (time '05:00', time '01:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 169 AS i, (time '24:00', time '05:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 170 AS i, (time '01:00', time '03:00') OVERLAPS (time '24:00', time '24:00') AS r
UNION ALL
SELECT 171 AS i, (time '01:00', time '24:00') OVERLAPS (NULL::time, time '24:00') AS r
UNION ALL
SELECT 172 AS i, (time '01:00', time '01:00') OVERLAPS (time '01:00', time '01:00') AS r
UNION ALL
SELECT 173 AS i, (time '24:00', time '24:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 174 AS i, (time '24:00', time '01:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 175 AS i, (time '01:00', time '24:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 176 AS i, (time '05:00', time '03:00') OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 177 AS i, (time '01:00', time '05:00') OVERLAPS (time '03:00', NULL::time) AS r
UNION ALL
SELECT 178 AS i, (time '01:00', NULL::time) OVERLAPS (time '01:00', time '01:00') AS r
UNION ALL
SELECT 179 AS i, (time '05:00', time '24:00') OVERLAPS (NULL::time, time '24:00') AS r
UNION ALL
SELECT 180 AS i, (time '05:00', time '05:00') OVERLAPS (time '03:00', time '03:00') AS r
UNION ALL
SELECT 181 AS i, (time '05:00', time '03:00') OVERLAPS (time '03:00', NULL::time) AS r
UNION ALL
SELECT 182 AS i, (time '03:00', time '03:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 183 AS i, (time '05:00', time '05:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 184 AS i, (time '05:00', time '03:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 185 AS i, (NULL::time, time '03:00') OVERLAPS (time '24:00', time '24:00') AS r
UNION ALL
SELECT 186 AS i, (time '05:00', time '01:00') OVERLAPS (time '24:00', time '24:00') AS r
UNION ALL
SELECT 187 AS i, (time '05:00', time '01:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 188 AS i, (NULL::time, time '05:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 189 AS i, (NULL::time, time '03:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 190 AS i, (NULL::time, time '05:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 191 AS i, (NULL::time, time '05:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 192 AS i, (time '03:00', time '24:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 193 AS i, (time '01:00', NULL::time) OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 194 AS i, (time '24:00', NULL::time) OVERLAPS (time '24:00', NULL::time) AS r
UNION ALL
SELECT 195 AS i, (time '24:00', time '05:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 196 AS i, (time '05:00', time '05:00') OVERLAPS (time '03:00', time '24:00') AS r
UNION ALL
SELECT 197 AS i, (time '03:00', time '05:00') OVERLAPS (time '01:00', time '03:00') AS r
UNION ALL
SELECT 198 AS i, (NULL::time, time '05:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 199 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 200 AS i, (NULL::time, time '24:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 201 AS i, (time '01:00', time '05:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 202 AS i, (time '05:00', time '24:00') OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 203 AS i, (time '24:00', time '01:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 204 AS i, (time '24:00', NULL::time) OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 205 AS i, (time '05:00', time '03:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 206 AS i, (time '01:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 207 AS i, (time '24:00', time '01:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 208 AS i, (time '03:00', NULL::time) OVERLAPS (time '24:00', NULL::time) AS r
UNION ALL
SELECT 209 AS i, (time '01:00', time '24:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 210 AS i, (time '03:00', time '03:00') OVERLAPS (time '24:00', time '01:00') AS r
UNION ALL
SELECT 211 AS i, (NULL::time, time '24:00') OVERLAPS (time '03:00', time '24:00') AS r
UNION ALL
SELECT 212 AS i, (time '05:00', time '03:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 213 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 214 AS i, (time '05:00', time '03:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 215 AS i, (time '03:00', time '01:00') OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 216 AS i, (NULL::time, NULL::time) OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 217 AS i, (time '05:00', time '01:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 218 AS i, (time '24:00', time '01:00') OVERLAPS (time '01:00', time '01:00') AS r
UNION ALL
SELECT 219 AS i, (time '05:00', time '24:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 220 AS i, (time '03:00', time '05:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 221 AS i, (time '24:00', time '05:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 222 AS i, (time '03:00', time '01:00') OVERLAPS (time '24:00', time '03:00') AS r
UNION ALL
SELECT 223 AS i, (time '05:00', time '24:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 224 AS i, (NULL::time, time '24:00') OVERLAPS (time '03:00', NULL::time) AS r
UNION ALL
SELECT 225 AS i, (NULL::time, NULL::time) OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 226 AS i, (time '24:00', NULL::time) OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 227 AS i, (time '01:00', time '05:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 228 AS i, (time '01:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 229 AS i, (time '03:00', time '05:00') OVERLAPS (time '24:00', time '03:00') AS r
UNION ALL
SELECT 230 AS i, (NULL::time, time '03:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 231 AS i, (time '03:00', time '03:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 232 AS i, (time '05:00', time '24:00') OVERLAPS (time '01:00', time '01:00') AS r
UNION ALL
SELECT 233 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 234 AS i, (NULL::time, time '05:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 235 AS i, (time '05:00', time '05:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 236 AS i, (time '05:00', time '03:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 237 AS i, (time '01:00', NULL::time) OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 238 AS i, (time '03:00', time '01:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 239 AS i, (time '24:00', time '01:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 240 AS i, (NULL::time, time '24:00') OVERLAPS (NULL::time, time '24:00') AS r
UNION ALL
SELECT 241 AS i, (NULL::time, time '03:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 242 AS i, (time '05:00', time '24:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 243 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 244 AS i, (time '05:00', time '24:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 245 AS i, (NULL::time, time '05:00') OVERLAPS (time '03:00', NULL::time) AS r
UNION ALL
SELECT 246 AS i, (time '01:00', time '01:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 247 AS i, (NULL::time, time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 248 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 249 AS i, (time '01:00', NULL::time) OVERLAPS (NULL::time, time '24:00') AS r
UNION ALL
SELECT 250 AS i, (time '05:00', time '01:00') OVERLAPS (NULL::time, time '24:00') AS r
UNION ALL
SELECT 251 AS i, (time '05:00', NULL::time) OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 252 AS i, (time '05:00', time '24:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 253 AS i, (NULL::time, time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 254 AS i, (time '05:00', NULL::time) OVERLAPS (time '01:00', time '24:00') AS r
UNION ALL
SELECT 255 AS i, (time '24:00', time '24:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 256 AS i, (time '05:00', time '01:00') OVERLAPS (time '24:00', time '03:00') AS r
UNION ALL
SELECT 257 AS i, (time '01:00', NULL::time) OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 258 AS i, (time '05:00', NULL::time) OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 259 AS i, (NULL::time, time '03:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 260 AS i, (time '24:00', time '03:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 261 AS i, (NULL::time, time '24:00') OVERLAPS (time '01:00', time '03:00') AS r
UNION ALL
SELECT 262 AS i, (time '05:00', time '05:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 263 AS i, (time '05:00', time '05:00') OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 264 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 265 AS i, (time '01:00', time '05:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 266 AS i, (time '24:00', time '05:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 267 AS i, (time '24:00', NULL::time) OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 268 AS i, (time '05:00', time '03:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 269 AS i, (time '24:00', time '01:00') OVERLAPS (time '24:00', time '01:00') AS r
UNION ALL
SELECT 270 AS i, (time '24:00', time '05:00') OVERLAPS (time '03:00', time '24:00') AS r
UNION ALL
SELECT 271 AS i, (time '05:00', time '03:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 272 AS i, (time '05:00', time '05:00') OVERLAPS (time '03:00', NULL::time) AS r
UNION ALL
SELECT 273 AS i, (time '01:00', time '01:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 274 AS i, (time '01:00', time '01:00') OVERLAPS (time '03:00', time '03:00') AS r
UNION ALL
SELECT 275 AS i, (time '05:00', NULL::time) OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 276 AS i, (NULL::time, time '05:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 277 AS i, (time '05:00', time '03:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 278 AS i, (time '05:00', time '01:00') OVERLAPS (time '03:00', time '24:00') AS r
UNION ALL
SELECT 279 AS i, (time '01:00', time '03:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 280 AS i, (time '03:00', time '03:00') OVERLAPS (time '24:00', time '24:00') AS r
UNION ALL
SELECT 281 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 282 AS i, (NULL::time, time '03:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 283 AS i, (time '01:00', time '05:00') OVERLAPS (time '24:00', NULL::time) AS r
UNION ALL
SELECT 284 AS i, (time '03:00', time '01:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 285 AS i, (NULL::time, NULL::time) OVERLAPS (time '01:00', time '03:00') AS r
UNION ALL
SELECT 286 AS i, (NULL::time, time '01:00') OVERLAPS (NULL::time, NULL::time) AS r
UNION ALL
SELECT 287 AS i, (time '24:00', NULL::time) OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 288 AS i, (time '05:00', time '24:00') OVERLAPS (time '03:00', time '24:00') AS r
UNION ALL
SELECT 289 AS i, (NULL::time, time '01:00') OVERLAPS (time '24:00', time '01:00') AS r
UNION ALL
SELECT 290 AS i, (time '05:00', time '01:00') OVERLAPS (time '01:00', time '01:00') AS r
UNION ALL
SELECT 291 AS i, (time '01:00', time '01:00') OVERLAPS (NULL::time, NULL::time) AS r
UNION ALL
SELECT 292 AS i, (time '24:00', time '05:00') OVERLAPS (NULL::time, NULL::time) AS r
UNION ALL
SELECT 293 AS i, (time '05:00', time '01:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 294 AS i, (NULL::time, NULL::time) OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 295 AS i, (time '03:00', time '03:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 296 AS i, (time '05:00', time '01:00') OVERLAPS (time '24:00', time '01:00') AS r
UNION ALL
SELECT 297 AS i, (time '05:00', time '05:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 298 AS i, (time '03:00', NULL::time) OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 299 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 300 AS i, (time '05:00', time '05:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 301 AS i, (time '03:00', time '05:00') OVERLAPS (time '03:00', time '24:00') AS r
UNION ALL
SELECT 302 AS i, (time '05:00', time '24:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 303 AS i, (time '05:00', time '24:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 304 AS i, (time '01:00', time '05:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 305 AS i, (time '01:00', time '05:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 306 AS i, (NULL::time, time '05:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 307 AS i, (time '05:00', time '05:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 308 AS i, (time '05:00', NULL::time) OVERLAPS (time '01:00', time '03:00') AS r
UNION ALL
SELECT 309 AS i, (time '24:00', time '05:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 310 AS i, (time '03:00', time '05:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 311 AS i, (time '05:00', time '01:00') OVERLAPS (time '24:00', time '01:00') AS r
UNION ALL
SELECT 312 AS i, (time '03:00', NULL::time) OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 313 AS i, (time '05:00', time '24:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 314 AS i, (NULL::time, time '05:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 315 AS i, (time '03:00', time '03:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 316 AS i, (time '05:00', time '24:00') OVERLAPS (time '24:00', NULL::time) AS r
UNION ALL
SELECT 317 AS i, (time '05:00', time '24:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 318 AS i, (NULL::time, time '03:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 319 AS i, (NULL::time, time '01:00') OVERLAPS (time '24:00', time '24:00') AS r
UNION ALL
SELECT 320 AS i, (NULL::time, time '05:00') OVERLAPS (NULL::time, NULL::time) AS r
UNION ALL
SELECT 321 AS i, (time '24:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 322 AS i, (time '03:00', time '05:00') OVERLAPS (time '01:00', NULL::time) AS r
UNION ALL
SELECT 323 AS i, (time '01:00', NULL::time) OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 324 AS i, (time '03:00', time '03:00') OVERLAPS (time '24:00', time '03:00') AS r
UNION ALL
SELECT 325 AS i, (time '03:00', time '03:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 326 AS i, (time '01:00', time '01:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 327 AS i, (NULL::time, time '01:00') OVERLAPS (NULL::time, time '24:00') AS r
UNION ALL
SELECT 328 AS i, (time '03:00', time '01:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 329 AS i, (NULL::time, time '24:00') OVERLAPS (time '01:00', time '01:00') AS r
UNION ALL
SELECT 330 AS i, (time '01:00', time '24:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 331 AS i, (time '03:00', time '24:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 332 AS i, (time '01:00', time '01:00') OVERLAPS (time '01:00', NULL::time) AS r
UNION ALL
SELECT 333 AS i, (time '05:00', NULL::time) OVERLAPS (time '01:00', time '03:00') AS r
UNION ALL
SELECT 334 AS i, (NULL::time, NULL::time) OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 335 AS i, (time '01:00', NULL::time) OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 336 AS i, (time '24:00', time '24:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 337 AS i, (time '24:00', time '05:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 338 AS i, (time '03:00', time '05:00') OVERLAPS (time '03:00', NULL::time) AS r
UNION ALL
SELECT 339 AS i, (time '03:00', time '24:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 340 AS i, (time '05:00', time '03:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 341 AS i, (time '01:00', time '05:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 342 AS i, (time '24:00', time '03:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 343 AS i, (time '01:00', time '05:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 344 AS i, (NULL::time, time '05:00') OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 345 AS i, (time '05:00', time '05:00') OVERLAPS (time '03:00', time '03:00') AS r
UNION ALL
SELECT 346 AS i, (NULL::time, time '24:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 347 AS i, (time '03:00', time '05:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 348 AS i, (time '03:00', time '05:00') OVERLAPS (NULL::time, NULL::time) AS r
UNION ALL
SELECT 349 AS i, (time '03:00', time '05:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 350 AS i, (time '24:00', time '24:00') OVERLAPS (time '01:00', time '03:00') AS r
UNION ALL
SELECT 351 AS i, (NULL::time, time '03:00') OVERLAPS (time '24:00', time '01:00') AS r
UNION ALL
SELECT 352 AS i, (time '01:00', time '05:00') OVERLAPS (time '01:00', time '01:00') AS r
UNION ALL
SELECT 353 AS i, (NULL::time, time '05:00') OVERLAPS (time '01:00', NULL::time) AS r
UNION ALL
SELECT 354 AS i, (time '03:00', time '05:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 355 AS i, (time '01:00', time '05:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 356 AS i, (time '05:00', time '05:00') OVERLAPS (time '01:00', time '03:00') AS r
UNION ALL
SELECT 357 AS i, (time '01:00', time '24:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 358 AS i, (time '05:00', time '05:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 359 AS i, (time '01:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 360 AS i, (time '24:00', NULL::time) OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 361 AS i, (time '01:00', time '03:00') OVERLAPS (NULL::time, NULL::time) AS r
UNION ALL
SELECT 362 AS i, (time '03:00', time '03:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 363 AS i, (time '03:00', time '01:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 364 AS i, (time '03:00', time '05:00') OVERLAPS (time '24:00', time '24:00') AS r
UNION ALL
SELECT 365 AS i, (NULL::time, time '05:00') OVERLAPS (time '24:00', time '03:00') AS r
UNION ALL
SELECT 366 AS i, (time '05:00', time '05:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 367 AS i, (time '24:00', time '01:00') OVERLAPS (time '01:00', NULL::time) AS r
UNION ALL
SELECT 368 AS i, (time '01:00', time '05:00') OVERLAPS (NULL::time, NULL::time) AS r
UNION ALL
SELECT 369 AS i, (time '03:00', time '01:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 370 AS i, (NULL::time, time '03:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 371 AS i, (time '03:00', time '24:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 372 AS i, (time '03:00', time '03:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 373 AS i, (time '03:00', time '24:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 374 AS i, (time '03:00', time '03:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 375 AS i, (time '01:00', time '24:00') OVERLAPS (NULL::time, time '01:00') AS r
UNION ALL
SELECT 376 AS i, (time '05:00', time '05:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 377 AS i, (NULL::time, time '03:00') OVERLAPS (time '01:00', time '05:00') AS r
UNION ALL
SELECT 378 AS i, (NULL::time, time '05:00') OVERLAPS (time '24:00', time '03:00') AS r
UNION ALL
SELECT 379 AS i, (time '03:00', time '03:00') OVERLAPS (time '03:00', NULL::time) AS r
UNION ALL
SELECT 380 AS i, (time '03:00', time '05:00') OVERLAPS (time '05:00', time '01:00') AS r
UNION ALL
SELECT 381 AS i, (time '05:00', NULL::time) OVERLAPS (time '24:00', NULL::time) AS r
UNION ALL
SELECT 382 AS i, (time '03:00', time '05:00') OVERLAPS (time '05:00', time '05:00') AS r
UNION ALL
SELECT 383 AS i, (NULL::time, time '05:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 384 AS i, (time '24:00', time '24:00') OVERLAPS (time '03:00', time '01:00') AS r
UNION ALL
SELECT 385 AS i, (NULL::time, time '24:00') OVERLAPS (NULL::time, time '05:00') AS r
UNION ALL
SELECT 386 AS i, (time '03:00', time '03:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 387 AS i, (NULL::time, time '01:00') OVERLAPS (time '24:00', time '05:00') AS r
UNION ALL
SELECT 388 AS i, (NULL::time, time '05:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 389 AS i, (NULL::time, time '05:00') OVERLAPS (time '05:00', time '24:00') AS r
UNION ALL
SELECT 390 AS i, (NULL::time, time '24:00') OVERLAPS (time '05:00', NULL::time) AS r
UNION ALL
SELECT 391 AS i, (time '24:00', time '05:00') OVERLAPS (time '01:00', NULL::time) AS r
UNION ALL
SELECT 392 AS i, (time '05:00', time '24:00') OVERLAPS (time '24:00', time '03:00') AS r
UNION ALL
SELECT 393 AS i, (time '03:00', time '01:00') OVERLAPS (NULL::time, time '03:00') AS r
UNION ALL
SELECT 394 AS i, (time '24:00', time '05:00') OVERLAPS (time '03:00', time '05:00') AS r
UNION ALL
SELECT 395 AS i, (time '05:00', time '24:00') OVERLAPS (time '24:00', time '01:00') AS r
UNION ALL
SELECT 396 AS i, (time '24:00', time '05:00') OVERLAPS (time '05:00', time '03:00') AS r
UNION ALL
SELECT 397 AS i, (time '05:00', time '24:00') OVERLAPS (time '01:00', time '24:00') AS r
UNION ALL
SELECT 398 AS i, (time '24:00', time '24:00') OVERLAPS (time '01:00', NULL::time) AS r
UNION ALL
SELECT 399 AS i, (time '05:00', time '03:00') OVERLAPS (time '24:00', time '24:00') AS r
ORDER BY 1;
-- timetz
SELECT 0 AS i, (timetz '04:00-02', timetz '01:00+00') OVERLAPS (timetz '03:00+02', timetz '04:00-02') AS r
UNION ALL
SELECT 1 AS i, (timetz '05:00+00', timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 2 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (timetz '03:00+02', NULL::timetz) AS r
UNION ALL
SELECT 3 AS i, (timetz '01:00+00', timetz '03:00+02') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 4 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (timetz '01:00+00', NULL::timetz) AS r
UNION ALL
SELECT 5 AS i, (timetz '23:00+10', timetz '01:00+00') OVERLAPS (timetz '03:00+02', timetz '23:00+10') AS r
UNION ALL
SELECT 6 AS i, (timetz '01:00+00', timetz '23:00+10') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 7 AS i, (timetz '04:00-02', timetz '03:00+02') OVERLAPS (NULL::timetz, NULL::timetz) AS r
UNION ALL
SELECT 8 AS i, (timetz '23:00+10', timetz '04:00-02') OVERLAPS (timetz '01:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 9 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '05:00+00', NULL::timetz) AS r
UNION ALL
SELECT 10 AS i, (NULL::timetz, NULL::timetz) OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 11 AS i, (timetz '03:00+02', timetz '01:00+00') OVERLAPS (timetz '05:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 12 AS i, (timetz '01:00+00', timetz '03:00+02') OVERLAPS (timetz '01:00+00', NULL::timetz) AS r
UNION ALL
SELECT 13 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (timetz '23:00+10', timetz '03:00+02') AS r
UNION ALL
SELECT 14 AS i, (NULL::timetz, timetz '01:00+00') OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 15 AS i, (timetz '04:00-02', timetz '23:00+10') OVERLAPS (NULL::timetz, NULL::timetz) AS r
UNION ALL
SELECT 16 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '23:00+10', timetz '23:00+10') AS r
UNION ALL
SELECT 17 AS i, (timetz '03:00+02', timetz '05:00+00') OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 18 AS i, (timetz '01:00+00', timetz '23:00+10') OVERLAPS (timetz '01:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 19 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 20 AS i, (NULL::timetz, timetz '01:00+00') OVERLAPS (timetz '01:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 21 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '03:00+02', timetz '05:00+00') AS r
UNION ALL
SELECT 22 AS i, (timetz '23:00+10', timetz '03:00+02') OVERLAPS (timetz '05:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 23 AS i, (timetz '05:00+00', timetz '05:00+00') OVERLAPS (timetz '01:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 24 AS i, (timetz '03:00+02', timetz '01:00+00') OVERLAPS (NULL::timetz, timetz '05:00+00') AS r
UNION ALL
SELECT 25 AS i, (timetz '23:00+10', timetz '23:00+10') OVERLAPS (NULL::timetz, timetz '04:00-02') AS r
UNION ALL
SELECT 26 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '04:00-02', timetz '01:00+00') AS r
UNION ALL
SELECT 27 AS i, (timetz '23:00+10', timetz '03:00+02') OVERLAPS (NULL::timetz, timetz '23:00+10') AS r
UNION ALL
SELECT 28 AS i, (timetz '23:00+10', timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 29 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 30 AS i, (timetz '01:00+00', timetz '03:00+02') OVERLAPS (NULL::timetz, timetz '23:00+10') AS r
UNION ALL
SELECT 31 AS i, (timetz '03:00+02', timetz '01:00+00') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 32 AS i, (timetz '01:00+00', timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '23:00+10') AS r
UNION ALL
SELECT 33 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 34 AS i, (timetz '05:00+00', timetz '05:00+00') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 35 AS i, (timetz '03:00+02', NULL::timetz) OVERLAPS (timetz '03:00+02', timetz '23:00+10') AS r
UNION ALL
SELECT 36 AS i, (timetz '04:00-02', timetz '23:00+10') OVERLAPS (timetz '23:00+10', timetz '01:00+00') AS r
UNION ALL
SELECT 37 AS i, (timetz '05:00+00', timetz '03:00+02') OVERLAPS (timetz '05:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 38 AS i, (timetz '23:00+10', timetz '01:00+00') OVERLAPS (NULL::timetz, timetz '04:00-02') AS r
UNION ALL
SELECT 39 AS i, (timetz '05:00+00', timetz '01:00+00') OVERLAPS (NULL::timetz, timetz '05:00+00') AS r
UNION ALL
SELECT 40 AS i, (timetz '23:00+10', timetz '03:00+02') OVERLAPS (timetz '03:00+02', timetz '23:00+10') AS r
UNION ALL
SELECT 41 AS i, (timetz '03:00+02', timetz '23:00+10') OVERLAPS (timetz '04:00-02', timetz '05:00+00') AS r
UNION ALL
SELECT 42 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 43 AS i, (timetz '05:00+00', NULL::timetz) OVERLAPS (timetz '05:00+00', NULL::timetz) AS r
UNION ALL
SELECT 44 AS i, (timetz '01:00+00', timetz '23:00+10') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 45 AS i, (timetz '23:00+10', timetz '04:00-02') OVERLAPS (timetz '01:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 46 AS i, (timetz '23:00+10', NULL::timetz) OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 47 AS i, (timetz '05:00+00', NULL::timetz) OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 48 AS i, (timetz '04:00-02', timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 49 AS i, (timetz '01:00+00', timetz '23:00+10') OVERLAPS (timetz '04:00-02', timetz '05:00+00') AS r
UNION ALL
SELECT 50 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (timetz '01:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 51 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 52 AS i, (timetz '23:00+10', timetz '03:00+02') OVERLAPS (timetz '01:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 53 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '03:00+02', timetz '03:00+02') AS r
UNION ALL
SELECT 54 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (NULL::timetz, timetz '05:00+00') AS r
UNION ALL
SELECT 55 AS i, (NULL::timetz, NULL::timetz) OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 56 AS i, (timetz '23:00+10', timetz '01:00+00') OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 57 AS i, (NULL::timetz, timetz '01:00+00') OVERLAPS (timetz '05:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 58 AS i, (timetz '03:00+02', timetz '23:00+10') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 59 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (timetz '03:00+02', timetz '23:00+10') AS r
UNION ALL
SELECT 60 AS i, (timetz '23:00+10', timetz '23:00+10') OVERLAPS (timetz '05:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 61 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (NULL::timetz, NULL::timetz) AS r
UNION ALL
SELECT 62 AS i, (timetz '04:00-02', timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 63 AS i, (timetz '03:00+02', timetz '23:00+10') OVERLAPS (timetz '01:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 64 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '01:00+00', NULL::timetz) AS r
UNION ALL
SELECT 65 AS i, (timetz '05:00+00', NULL::timetz) OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 66 AS i, (timetz '03:00+02', timetz '05:00+00') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 67 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (timetz '04:00-02', timetz '23:00+10') AS r
UNION ALL
SELECT 68 AS i, (timetz '23:00+10', timetz '03:00+02') OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 69 AS i, (timetz '03:00+02', timetz '23:00+10') OVERLAPS (timetz '01:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 70 AS i, (NULL::timetz, timetz '23:00+10') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 71 AS i, (NULL::timetz, NULL::timetz) OVERLAPS (timetz '01:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 72 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (timetz '04:00-02', timetz '05:00+00') AS r
UNION ALL
SELECT 73 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (timetz '04:00-02', timetz '05:00+00') AS r
UNION ALL
SELECT 74 AS i, (timetz '03:00+02', NULL::timetz) OVERLAPS (timetz '03:00+02', timetz '05:00+00') AS r
UNION ALL
SELECT 75 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (timetz '01:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 76 AS i, (timetz '23:00+10', NULL::timetz) OVERLAPS (timetz '23:00+10', timetz '03:00+02') AS r
UNION ALL
SELECT 77 AS i, (NULL::timetz, timetz '23:00+10') OVERLAPS (timetz '01:00+00', NULL::timetz) AS r
UNION ALL
SELECT 78 AS i, (timetz '23:00+10', timetz '23:00+10') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 79 AS i, (timetz '05:00+00', timetz '04:00-02') OVERLAPS (timetz '03:00+02', timetz '05:00+00') AS r
UNION ALL
SELECT 80 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (timetz '03:00+02', NULL::timetz) AS r
UNION ALL
SELECT 81 AS i, (timetz '05:00+00', timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '23:00+10') AS r
UNION ALL
SELECT 82 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (NULL::timetz, timetz '23:00+10') AS r
UNION ALL
SELECT 83 AS i, (timetz '04:00-02', timetz '23:00+10') OVERLAPS (timetz '23:00+10', timetz '23:00+10') AS r
UNION ALL
SELECT 84 AS i, (timetz '01:00+00', timetz '03:00+02') OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 85 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (timetz '03:00+02', timetz '03:00+02') AS r
UNION ALL
SELECT 86 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 87 AS i, (timetz '23:00+10', timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 88 AS i, (timetz '03:00+02', timetz '23:00+10') OVERLAPS (timetz '23:00+10', timetz '01:00+00') AS r
UNION ALL
SELECT 89 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 90 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (timetz '23:00+10', timetz '04:00-02') AS r
UNION ALL
SELECT 91 AS i, (NULL::timetz, NULL::timetz) OVERLAPS (timetz '05:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 92 AS i, (timetz '23:00+10', timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '23:00+10') AS r
UNION ALL
SELECT 93 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (timetz '23:00+10', NULL::timetz) AS r
UNION ALL
SELECT 94 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '23:00+10', timetz '03:00+02') AS r
UNION ALL
SELECT 95 AS i, (timetz '01:00+00', timetz '23:00+10') OVERLAPS (timetz '05:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 96 AS i, (timetz '03:00+02', timetz '04:00-02') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 97 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (timetz '23:00+10', NULL::timetz) AS r
UNION ALL
SELECT 98 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (NULL::timetz, timetz '05:00+00') AS r
UNION ALL
SELECT 99 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '03:00+02', NULL::timetz) AS r
UNION ALL
SELECT 100 AS i, (timetz '03:00+02', timetz '23:00+10') OVERLAPS (timetz '04:00-02', NULL::timetz) AS r
UNION ALL
SELECT 101 AS i, (timetz '23:00+10', timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 102 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 103 AS i, (timetz '03:00+02', NULL::timetz) OVERLAPS (timetz '05:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 104 AS i, (timetz '04:00-02', timetz '04:00-02') OVERLAPS (timetz '23:00+10', NULL::timetz) AS r
UNION ALL
SELECT 105 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 106 AS i, (timetz '01:00+00', timetz '05:00+00') OVERLAPS (timetz '05:00+00', NULL::timetz) AS r
UNION ALL
SELECT 107 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (timetz '05:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 108 AS i, (timetz '04:00-02', timetz '03:00+02') OVERLAPS (timetz '05:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 109 AS i, (timetz '05:00+00', timetz '04:00-02') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 110 AS i, (timetz '05:00+00', timetz '01:00+00') OVERLAPS (timetz '04:00-02', timetz '23:00+10') AS r
UNION ALL
SELECT 111 AS i, (NULL::timetz, NULL::timetz) OVERLAPS (timetz '01:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 112 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 113 AS i, (timetz '03:00+02', timetz '01:00+00') OVERLAPS (timetz '03:00+02', timetz '03:00+02') AS r
UNION ALL
SELECT 114 AS i, (timetz '03:00+02', NULL::timetz) OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 115 AS i, (timetz '05:00+00', timetz '05:00+00') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 116 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '01:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 117 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (timetz '04:00-02', NULL::timetz) AS r
UNION ALL
SELECT 118 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 119 AS i, (timetz '23:00+10', NULL::timetz) OVERLAPS (timetz '03:00+02', NULL::timetz) AS r
UNION ALL
SELECT 120 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 121 AS i, (timetz '05:00+00', timetz '04:00-02') OVERLAPS (timetz '03:00+02', NULL::timetz) AS r
UNION ALL
SELECT 122 AS i, (timetz '23:00+10', timetz '04:00-02') OVERLAPS (timetz '23:00+10', NULL::timetz) AS r
UNION ALL
SELECT 123 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (timetz '23:00+10', timetz '01:00+00') AS r
UNION ALL
SELECT 124 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (timetz '05:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 125 AS i, (timetz '03:00+02', timetz '04:00-02') OVERLAPS (NULL::timetz, timetz '23:00+10') AS r
UNION ALL
SELECT 126 AS i, (timetz '05:00+00', timetz '05:00+00') OVERLAPS (NULL::timetz, NULL::timetz) AS r
UNION ALL
SELECT 127 AS i, (NULL::timetz, timetz '01:00+00') OVERLAPS (timetz '03:00+02', timetz '23:00+10') AS r
UNION ALL
SELECT 128 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (timetz '05:00+00', NULL::timetz) AS r
UNION ALL
SELECT 129 AS i, (timetz '03:00+02', timetz '04:00-02') OVERLAPS (timetz '23:00+10', timetz '01:00+00') AS r
UNION ALL
SELECT 130 AS i, (timetz '04:00-02', timetz '23:00+10') OVERLAPS (timetz '04:00-02', NULL::timetz) AS r
UNION ALL
SELECT 131 AS i, (timetz '04:00-02', timetz '03:00+02') OVERLAPS (timetz '03:00+02', timetz '03:00+02') AS r
UNION ALL
SELECT 132 AS i, (timetz '04:00-02', timetz '04:00-02') OVERLAPS (timetz '04:00-02', timetz '23:00+10') AS r
UNION ALL
SELECT 133 AS i, (timetz '03:00+02', timetz '05:00+00') OVERLAPS (timetz '01:00+00', NULL::timetz) AS r
UNION ALL
SELECT 134 AS i, (timetz '05:00+00', timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 135 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (NULL::timetz, timetz '05:00+00') AS r
UNION ALL
SELECT 136 AS i, (timetz '05:00+00', NULL::timetz) OVERLAPS (timetz '01:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 137 AS i, (timetz '23:00+10', timetz '03:00+02') OVERLAPS (timetz '05:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 138 AS i, (timetz '03:00+02', timetz '05:00+00') OVERLAPS (timetz '05:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 139 AS i, (timetz '03:00+02', timetz '05:00+00') OVERLAPS (timetz '05:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 140 AS i, (timetz '01:00+00', timetz '01:00+00') OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 141 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 142 AS i, (timetz '05:00+00', NULL::timetz) OVERLAPS (timetz '03:00+02', timetz '05:00+00') AS r
UNION ALL
SELECT 143 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '23:00+10', NULL::timetz) AS r
UNION ALL
SELECT 144 AS i, (timetz '03:00+02', NULL::timetz) OVERLAPS (timetz '01:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 145 AS i, (timetz '01:00+00', timetz '01:00+00') OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 146 AS i, (timetz '05:00+00', timetz '03:00+02') OVERLAPS (timetz '05:00+00', NULL::timetz) AS r
UNION ALL
SELECT 147 AS i, (timetz '23:00+10', NULL::timetz) OVERLAPS (timetz '23:00+10', timetz '23:00+10') AS r
UNION ALL
SELECT 148 AS i, (timetz '23:00+10', timetz '01:00+00') OVERLAPS (timetz '03:00+02', timetz '03:00+02') AS r
UNION ALL
SELECT 149 AS i, (timetz '04:00-02', timetz '04:00-02') OVERLAPS (timetz '01:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 150 AS i, (timetz '04:00-02', timetz '01:00+00') OVERLAPS (timetz '01:00+00', NULL::timetz) AS r
UNION ALL
SELECT 151 AS i, (timetz '05:00+00', timetz '05:00+00') OVERLAPS (timetz '01:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 152 AS i, (NULL::timetz, timetz '23:00+10') OVERLAPS (timetz '05:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 153 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '01:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 154 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (timetz '23:00+10', timetz '23:00+10') AS r
UNION ALL
SELECT 155 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (timetz '05:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 156 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (timetz '04:00-02', timetz '23:00+10') AS r
UNION ALL
SELECT 157 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (NULL::timetz, timetz '05:00+00') AS r
UNION ALL
SELECT 158 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (timetz '03:00+02', timetz '04:00-02') AS r
UNION ALL
SELECT 159 AS i, (timetz '01:00+00', timetz '23:00+10') OVERLAPS (timetz '23:00+10', timetz '03:00+02') AS r
UNION ALL
SELECT 160 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (timetz '03:00+02', timetz '03:00+02') AS r
UNION ALL
SELECT 161 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (NULL::timetz, NULL::timetz) AS r
UNION ALL
SELECT 162 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '04:00-02', timetz '05:00+00') AS r
UNION ALL
SELECT 163 AS i, (timetz '05:00+00', NULL::timetz) OVERLAPS (timetz '23:00+10', timetz '01:00+00') AS r
UNION ALL
SELECT 164 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (NULL::timetz, timetz '23:00+10') AS r
UNION ALL
SELECT 165 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 166 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (timetz '01:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 167 AS i, (timetz '05:00+00', timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 168 AS i, (timetz '05:00+00', timetz '01:00+00') OVERLAPS (timetz '05:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 169 AS i, (timetz '23:00+10', timetz '04:00-02') OVERLAPS (timetz '04:00-02', timetz '01:00+00') AS r
UNION ALL
SELECT 170 AS i, (timetz '01:00+00', timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '23:00+10') AS r
UNION ALL
SELECT 171 AS i, (timetz '01:00+00', timetz '23:00+10') OVERLAPS (NULL::timetz, timetz '23:00+10') AS r
UNION ALL
SELECT 172 AS i, (timetz '01:00+00', timetz '01:00+00') OVERLAPS (timetz '01:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 173 AS i, (timetz '23:00+10', timetz '23:00+10') OVERLAPS (timetz '03:00+02', timetz '05:00+00') AS r
UNION ALL
SELECT 174 AS i, (timetz '23:00+10', timetz '01:00+00') OVERLAPS (timetz '04:00-02', timetz '01:00+00') AS r
UNION ALL
SELECT 175 AS i, (timetz '01:00+00', timetz '23:00+10') OVERLAPS (NULL::timetz, timetz '04:00-02') AS r
UNION ALL
SELECT 176 AS i, (timetz '04:00-02', timetz '03:00+02') OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 177 AS i, (timetz '01:00+00', timetz '05:00+00') OVERLAPS (timetz '03:00+02', NULL::timetz) AS r
UNION ALL
SELECT 178 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (timetz '01:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 179 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (NULL::timetz, timetz '23:00+10') AS r
UNION ALL
SELECT 180 AS i, (timetz '04:00-02', timetz '04:00-02') OVERLAPS (timetz '03:00+02', timetz '03:00+02') AS r
UNION ALL
SELECT 181 AS i, (timetz '05:00+00', timetz '03:00+02') OVERLAPS (timetz '03:00+02', NULL::timetz) AS r
UNION ALL
SELECT 182 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 183 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (NULL::timetz, timetz '04:00-02') AS r
UNION ALL
SELECT 184 AS i, (timetz '04:00-02', timetz '03:00+02') OVERLAPS (NULL::timetz, timetz '05:00+00') AS r
UNION ALL
SELECT 185 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '23:00+10') AS r
UNION ALL
SELECT 186 AS i, (timetz '05:00+00', timetz '01:00+00') OVERLAPS (timetz '23:00+10', timetz '23:00+10') AS r
UNION ALL
SELECT 187 AS i, (timetz '05:00+00', timetz '01:00+00') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 188 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (NULL::timetz, timetz '04:00-02') AS r
UNION ALL
SELECT 189 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '01:00+00') AS r
UNION ALL
SELECT 190 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 191 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (timetz '04:00-02', timetz '01:00+00') AS r
UNION ALL
SELECT 192 AS i, (timetz '03:00+02', timetz '23:00+10') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 193 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 194 AS i, (timetz '23:00+10', NULL::timetz) OVERLAPS (timetz '23:00+10', NULL::timetz) AS r
UNION ALL
SELECT 195 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 196 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (timetz '03:00+02', timetz '23:00+10') AS r
UNION ALL
SELECT 197 AS i, (timetz '03:00+02', timetz '04:00-02') OVERLAPS (timetz '01:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 198 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 199 AS i, (timetz '05:00+00', timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 200 AS i, (NULL::timetz, timetz '23:00+10') OVERLAPS (timetz '03:00+02', timetz '05:00+00') AS r
UNION ALL
SELECT 201 AS i, (timetz '01:00+00', timetz '05:00+00') OVERLAPS (timetz '05:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 202 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 203 AS i, (timetz '23:00+10', timetz '01:00+00') OVERLAPS (timetz '01:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 204 AS i, (timetz '23:00+10', NULL::timetz) OVERLAPS (timetz '05:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 205 AS i, (timetz '05:00+00', timetz '03:00+02') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 206 AS i, (timetz '01:00+00', timetz '05:00+00') OVERLAPS (timetz '04:00-02', timetz '05:00+00') AS r
UNION ALL
SELECT 207 AS i, (timetz '23:00+10', timetz '01:00+00') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 208 AS i, (timetz '03:00+02', NULL::timetz) OVERLAPS (timetz '23:00+10', NULL::timetz) AS r
UNION ALL
SELECT 209 AS i, (timetz '01:00+00', timetz '23:00+10') OVERLAPS (timetz '05:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 210 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '01:00+00') AS r
UNION ALL
SELECT 211 AS i, (NULL::timetz, timetz '23:00+10') OVERLAPS (timetz '03:00+02', timetz '23:00+10') AS r
UNION ALL
SELECT 212 AS i, (timetz '04:00-02', timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 213 AS i, (timetz '05:00+00', timetz '04:00-02') OVERLAPS (timetz '04:00-02', timetz '01:00+00') AS r
UNION ALL
SELECT 214 AS i, (timetz '04:00-02', timetz '03:00+02') OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 215 AS i, (timetz '03:00+02', timetz '01:00+00') OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 216 AS i, (NULL::timetz, NULL::timetz) OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 217 AS i, (timetz '04:00-02', timetz '01:00+00') OVERLAPS (NULL::timetz, timetz '04:00-02') AS r
UNION ALL
SELECT 218 AS i, (timetz '23:00+10', timetz '01:00+00') OVERLAPS (timetz '01:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 219 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (timetz '01:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 220 AS i, (timetz '03:00+02', timetz '05:00+00') OVERLAPS (timetz '04:00-02', NULL::timetz) AS r
UNION ALL
SELECT 221 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (timetz '04:00-02', timetz '01:00+00') AS r
UNION ALL
SELECT 222 AS i, (timetz '03:00+02', timetz '01:00+00') OVERLAPS (timetz '23:00+10', timetz '03:00+02') AS r
UNION ALL
SELECT 223 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (timetz '03:00+02', timetz '05:00+00') AS r
UNION ALL
SELECT 224 AS i, (NULL::timetz, timetz '23:00+10') OVERLAPS (timetz '03:00+02', NULL::timetz) AS r
UNION ALL
SELECT 225 AS i, (NULL::timetz, NULL::timetz) OVERLAPS (timetz '01:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 226 AS i, (timetz '23:00+10', NULL::timetz) OVERLAPS (timetz '03:00+02', timetz '04:00-02') AS r
UNION ALL
SELECT 227 AS i, (timetz '01:00+00', timetz '05:00+00') OVERLAPS (timetz '04:00-02', timetz '23:00+10') AS r
UNION ALL
SELECT 228 AS i, (timetz '01:00+00', timetz '05:00+00') OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 229 AS i, (timetz '03:00+02', timetz '04:00-02') OVERLAPS (timetz '23:00+10', timetz '03:00+02') AS r
UNION ALL
SELECT 230 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 231 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 232 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (timetz '01:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 233 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 234 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (timetz '03:00+02', timetz '04:00-02') AS r
UNION ALL
SELECT 235 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 236 AS i, (timetz '04:00-02', timetz '03:00+02') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 237 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 238 AS i, (timetz '03:00+02', timetz '01:00+00') OVERLAPS (timetz '04:00-02', timetz '05:00+00') AS r
UNION ALL
SELECT 239 AS i, (timetz '23:00+10', timetz '01:00+00') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 240 AS i, (NULL::timetz, timetz '23:00+10') OVERLAPS (NULL::timetz, timetz '23:00+10') AS r
UNION ALL
SELECT 241 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 242 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (timetz '05:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 243 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (timetz '05:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 244 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (timetz '04:00-02', NULL::timetz) AS r
UNION ALL
SELECT 245 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (timetz '03:00+02', NULL::timetz) AS r
UNION ALL
SELECT 246 AS i, (timetz '01:00+00', timetz '01:00+00') OVERLAPS (timetz '01:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 247 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 248 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 249 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (NULL::timetz, timetz '23:00+10') AS r
UNION ALL
SELECT 250 AS i, (timetz '05:00+00', timetz '01:00+00') OVERLAPS (NULL::timetz, timetz '23:00+10') AS r
UNION ALL
SELECT 251 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (timetz '01:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 252 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 253 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 254 AS i, (timetz '05:00+00', NULL::timetz) OVERLAPS (timetz '01:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 255 AS i, (timetz '23:00+10', timetz '23:00+10') OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 256 AS i, (timetz '04:00-02', timetz '01:00+00') OVERLAPS (timetz '23:00+10', timetz '03:00+02') AS r
UNION ALL
SELECT 257 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (timetz '23:00+10', timetz '04:00-02') AS r
UNION ALL
SELECT 258 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (timetz '04:00-02', timetz '01:00+00') AS r
UNION ALL
SELECT 259 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 260 AS i, (timetz '23:00+10', timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 261 AS i, (NULL::timetz, timetz '23:00+10') OVERLAPS (timetz '01:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 262 AS i, (timetz '05:00+00', timetz '04:00-02') OVERLAPS (timetz '01:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 263 AS i, (timetz '05:00+00', timetz '04:00-02') OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 264 AS i, (timetz '05:00+00', timetz '05:00+00') OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 265 AS i, (timetz '01:00+00', timetz '05:00+00') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 266 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (timetz '05:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 267 AS i, (timetz '23:00+10', NULL::timetz) OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 268 AS i, (timetz '05:00+00', timetz '03:00+02') OVERLAPS (timetz '05:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 269 AS i, (timetz '23:00+10', timetz '01:00+00') OVERLAPS (timetz '23:00+10', timetz '01:00+00') AS r
UNION ALL
SELECT 270 AS i, (timetz '23:00+10', timetz '04:00-02') OVERLAPS (timetz '03:00+02', timetz '23:00+10') AS r
UNION ALL
SELECT 271 AS i, (timetz '05:00+00', timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 272 AS i, (timetz '05:00+00', timetz '05:00+00') OVERLAPS (timetz '03:00+02', NULL::timetz) AS r
UNION ALL
SELECT 273 AS i, (timetz '01:00+00', timetz '01:00+00') OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 274 AS i, (timetz '01:00+00', timetz '01:00+00') OVERLAPS (timetz '03:00+02', timetz '03:00+02') AS r
UNION ALL
SELECT 275 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 276 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (timetz '04:00-02', NULL::timetz) AS r
UNION ALL
SELECT 277 AS i, (timetz '04:00-02', timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '23:00+10') AS r
UNION ALL
SELECT 278 AS i, (timetz '04:00-02', timetz '01:00+00') OVERLAPS (timetz '03:00+02', timetz '23:00+10') AS r
UNION ALL
SELECT 279 AS i, (timetz '01:00+00', timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '04:00-02') AS r
UNION ALL
SELECT 280 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '23:00+10') AS r
UNION ALL
SELECT 281 AS i, (timetz '04:00-02', timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 282 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 283 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '23:00+10', NULL::timetz) AS r
UNION ALL
SELECT 284 AS i, (timetz '03:00+02', timetz '01:00+00') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 285 AS i, (NULL::timetz, NULL::timetz) OVERLAPS (timetz '01:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 286 AS i, (NULL::timetz, timetz '01:00+00') OVERLAPS (NULL::timetz, NULL::timetz) AS r
UNION ALL
SELECT 287 AS i, (timetz '23:00+10', NULL::timetz) OVERLAPS (timetz '04:00-02', NULL::timetz) AS r
UNION ALL
SELECT 288 AS i, (timetz '04:00-02', timetz '23:00+10') OVERLAPS (timetz '03:00+02', timetz '23:00+10') AS r
UNION ALL
SELECT 289 AS i, (NULL::timetz, timetz '01:00+00') OVERLAPS (timetz '23:00+10', timetz '01:00+00') AS r
UNION ALL
SELECT 290 AS i, (timetz '05:00+00', timetz '01:00+00') OVERLAPS (timetz '01:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 291 AS i, (timetz '01:00+00', timetz '01:00+00') OVERLAPS (NULL::timetz, NULL::timetz) AS r
UNION ALL
SELECT 292 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (NULL::timetz, NULL::timetz) AS r
UNION ALL
SELECT 293 AS i, (timetz '05:00+00', timetz '01:00+00') OVERLAPS (timetz '01:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 294 AS i, (NULL::timetz, NULL::timetz) OVERLAPS (timetz '04:00-02', timetz '05:00+00') AS r
UNION ALL
SELECT 295 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '05:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 296 AS i, (timetz '05:00+00', timetz '01:00+00') OVERLAPS (timetz '23:00+10', timetz '01:00+00') AS r
UNION ALL
SELECT 297 AS i, (timetz '04:00-02', timetz '04:00-02') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 298 AS i, (timetz '03:00+02', NULL::timetz) OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 299 AS i, (timetz '05:00+00', timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 300 AS i, (timetz '05:00+00', timetz '05:00+00') OVERLAPS (timetz '03:00+02', timetz '05:00+00') AS r
UNION ALL
SELECT 301 AS i, (timetz '03:00+02', timetz '04:00-02') OVERLAPS (timetz '03:00+02', timetz '23:00+10') AS r
UNION ALL
SELECT 302 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (timetz '23:00+10', timetz '04:00-02') AS r
UNION ALL
SELECT 303 AS i, (timetz '04:00-02', timetz '23:00+10') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 304 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 305 AS i, (timetz '01:00+00', timetz '05:00+00') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 306 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 307 AS i, (timetz '04:00-02', timetz '04:00-02') OVERLAPS (timetz '01:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 308 AS i, (timetz '05:00+00', NULL::timetz) OVERLAPS (timetz '01:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 309 AS i, (timetz '23:00+10', timetz '04:00-02') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 310 AS i, (timetz '03:00+02', timetz '05:00+00') OVERLAPS (timetz '01:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 311 AS i, (timetz '04:00-02', timetz '01:00+00') OVERLAPS (timetz '23:00+10', timetz '01:00+00') AS r
UNION ALL
SELECT 312 AS i, (timetz '03:00+02', NULL::timetz) OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 313 AS i, (timetz '04:00-02', timetz '23:00+10') OVERLAPS (timetz '05:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 314 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (NULL::timetz, timetz '05:00+00') AS r
UNION ALL
SELECT 315 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (NULL::timetz, timetz '05:00+00') AS r
UNION ALL
SELECT 316 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (timetz '23:00+10', NULL::timetz) AS r
UNION ALL
SELECT 317 AS i, (timetz '04:00-02', timetz '23:00+10') OVERLAPS (timetz '04:00-02', timetz '01:00+00') AS r
UNION ALL
SELECT 318 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (timetz '01:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 319 AS i, (NULL::timetz, timetz '01:00+00') OVERLAPS (timetz '23:00+10', timetz '23:00+10') AS r
UNION ALL
SELECT 320 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (NULL::timetz, NULL::timetz) AS r
UNION ALL
SELECT 321 AS i, (timetz '23:00+10', timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 322 AS i, (timetz '03:00+02', timetz '04:00-02') OVERLAPS (timetz '01:00+00', NULL::timetz) AS r
UNION ALL
SELECT 323 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (timetz '05:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 324 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '03:00+02') AS r
UNION ALL
SELECT 325 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '01:00+00') AS r
UNION ALL
SELECT 326 AS i, (timetz '01:00+00', timetz '01:00+00') OVERLAPS (timetz '04:00-02', timetz '05:00+00') AS r
UNION ALL
SELECT 327 AS i, (NULL::timetz, timetz '01:00+00') OVERLAPS (NULL::timetz, timetz '23:00+10') AS r
UNION ALL
SELECT 328 AS i, (timetz '03:00+02', timetz '01:00+00') OVERLAPS (timetz '04:00-02', timetz '01:00+00') AS r
UNION ALL
SELECT 329 AS i, (NULL::timetz, timetz '23:00+10') OVERLAPS (timetz '01:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 330 AS i, (timetz '01:00+00', timetz '23:00+10') OVERLAPS (timetz '05:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 331 AS i, (timetz '03:00+02', timetz '23:00+10') OVERLAPS (timetz '05:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 332 AS i, (timetz '01:00+00', timetz '01:00+00') OVERLAPS (timetz '01:00+00', NULL::timetz) AS r
UNION ALL
SELECT 333 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (timetz '01:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 334 AS i, (NULL::timetz, NULL::timetz) OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 335 AS i, (timetz '01:00+00', NULL::timetz) OVERLAPS (timetz '05:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 336 AS i, (timetz '23:00+10', timetz '23:00+10') OVERLAPS (timetz '04:00-02', NULL::timetz) AS r
UNION ALL
SELECT 337 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (timetz '01:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 338 AS i, (timetz '03:00+02', timetz '05:00+00') OVERLAPS (timetz '03:00+02', NULL::timetz) AS r
UNION ALL
SELECT 339 AS i, (timetz '03:00+02', timetz '23:00+10') OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 340 AS i, (timetz '04:00-02', timetz '03:00+02') OVERLAPS (timetz '04:00-02', NULL::timetz) AS r
UNION ALL
SELECT 341 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '03:00+02', timetz '04:00-02') AS r
UNION ALL
SELECT 342 AS i, (timetz '23:00+10', timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '01:00+00') AS r
UNION ALL
SELECT 343 AS i, (timetz '01:00+00', timetz '05:00+00') OVERLAPS (NULL::timetz, timetz '04:00-02') AS r
UNION ALL
SELECT 344 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 345 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (timetz '03:00+02', timetz '03:00+02') AS r
UNION ALL
SELECT 346 AS i, (NULL::timetz, timetz '23:00+10') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 347 AS i, (timetz '03:00+02', timetz '04:00-02') OVERLAPS (timetz '01:00+00', timetz '04:00-02') AS r
UNION ALL
SELECT 348 AS i, (timetz '03:00+02', timetz '04:00-02') OVERLAPS (NULL::timetz, NULL::timetz) AS r
UNION ALL
SELECT 349 AS i, (timetz '03:00+02', timetz '05:00+00') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 350 AS i, (timetz '23:00+10', timetz '23:00+10') OVERLAPS (timetz '01:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 351 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '01:00+00') AS r
UNION ALL
SELECT 352 AS i, (timetz '01:00+00', timetz '04:00-02') OVERLAPS (timetz '01:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 353 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (timetz '01:00+00', NULL::timetz) AS r
UNION ALL
SELECT 354 AS i, (timetz '03:00+02', timetz '04:00-02') OVERLAPS (timetz '23:00+10', timetz '04:00-02') AS r
UNION ALL
SELECT 355 AS i, (timetz '01:00+00', timetz '05:00+00') OVERLAPS (timetz '04:00-02', NULL::timetz) AS r
UNION ALL
SELECT 356 AS i, (timetz '05:00+00', timetz '05:00+00') OVERLAPS (timetz '01:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 357 AS i, (timetz '01:00+00', timetz '23:00+10') OVERLAPS (timetz '03:00+02', timetz '04:00-02') AS r
UNION ALL
SELECT 358 AS i, (timetz '05:00+00', timetz '05:00+00') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 359 AS i, (timetz '01:00+00', timetz '05:00+00') OVERLAPS (timetz '05:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 360 AS i, (timetz '23:00+10', NULL::timetz) OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 361 AS i, (timetz '01:00+00', timetz '03:00+02') OVERLAPS (NULL::timetz, NULL::timetz) AS r
UNION ALL
SELECT 362 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '03:00+02', timetz '04:00-02') AS r
UNION ALL
SELECT 363 AS i, (timetz '03:00+02', timetz '01:00+00') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 364 AS i, (timetz '03:00+02', timetz '04:00-02') OVERLAPS (timetz '23:00+10', timetz '23:00+10') AS r
UNION ALL
SELECT 365 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (timetz '23:00+10', timetz '03:00+02') AS r
UNION ALL
SELECT 366 AS i, (timetz '04:00-02', timetz '04:00-02') OVERLAPS (timetz '04:00-02', NULL::timetz) AS r
UNION ALL
SELECT 367 AS i, (timetz '23:00+10', timetz '01:00+00') OVERLAPS (timetz '01:00+00', NULL::timetz) AS r
UNION ALL
SELECT 368 AS i, (timetz '01:00+00', timetz '05:00+00') OVERLAPS (NULL::timetz, NULL::timetz) AS r
UNION ALL
SELECT 369 AS i, (timetz '03:00+02', timetz '01:00+00') OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 370 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (timetz '03:00+02', timetz '04:00-02') AS r
UNION ALL
SELECT 371 AS i, (timetz '03:00+02', timetz '23:00+10') OVERLAPS (timetz '03:00+02', timetz '04:00-02') AS r
UNION ALL
SELECT 372 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '03:00+02', timetz '05:00+00') AS r
UNION ALL
SELECT 373 AS i, (timetz '03:00+02', timetz '23:00+10') OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 374 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '04:00-02', timetz '03:00+02') AS r
UNION ALL
SELECT 375 AS i, (timetz '01:00+00', timetz '23:00+10') OVERLAPS (NULL::timetz, timetz '01:00+00') AS r
UNION ALL
SELECT 376 AS i, (timetz '04:00-02', timetz '05:00+00') OVERLAPS (NULL::timetz, timetz '05:00+00') AS r
UNION ALL
SELECT 377 AS i, (NULL::timetz, timetz '03:00+02') OVERLAPS (timetz '01:00+00', timetz '05:00+00') AS r
UNION ALL
SELECT 378 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (timetz '23:00+10', timetz '03:00+02') AS r
UNION ALL
SELECT 379 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '03:00+02', NULL::timetz) AS r
UNION ALL
SELECT 380 AS i, (timetz '03:00+02', timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '01:00+00') AS r
UNION ALL
SELECT 381 AS i, (timetz '04:00-02', NULL::timetz) OVERLAPS (timetz '23:00+10', NULL::timetz) AS r
UNION ALL
SELECT 382 AS i, (timetz '03:00+02', timetz '05:00+00') OVERLAPS (timetz '04:00-02', timetz '04:00-02') AS r
UNION ALL
SELECT 383 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (timetz '23:00+10', timetz '04:00-02') AS r
UNION ALL
SELECT 384 AS i, (timetz '23:00+10', timetz '23:00+10') OVERLAPS (timetz '03:00+02', timetz '01:00+00') AS r
UNION ALL
SELECT 385 AS i, (NULL::timetz, timetz '23:00+10') OVERLAPS (NULL::timetz, timetz '04:00-02') AS r
UNION ALL
SELECT 386 AS i, (timetz '03:00+02', timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '05:00+00') AS r
UNION ALL
SELECT 387 AS i, (NULL::timetz, timetz '01:00+00') OVERLAPS (timetz '23:00+10', timetz '04:00-02') AS r
UNION ALL
SELECT 388 AS i, (NULL::timetz, timetz '05:00+00') OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 389 AS i, (NULL::timetz, timetz '04:00-02') OVERLAPS (timetz '04:00-02', timetz '23:00+10') AS r
UNION ALL
SELECT 390 AS i, (NULL::timetz, timetz '23:00+10') OVERLAPS (timetz '04:00-02', NULL::timetz) AS r
UNION ALL
SELECT 391 AS i, (timetz '23:00+10', timetz '05:00+00') OVERLAPS (timetz '01:00+00', NULL::timetz) AS r
UNION ALL
SELECT 392 AS i, (timetz '04:00-02', timetz '23:00+10') OVERLAPS (timetz '23:00+10', timetz '03:00+02') AS r
UNION ALL
SELECT 393 AS i, (timetz '03:00+02', timetz '01:00+00') OVERLAPS (NULL::timetz, timetz '03:00+02') AS r
UNION ALL
SELECT 394 AS i, (timetz '23:00+10', timetz '04:00-02') OVERLAPS (timetz '03:00+02', timetz '05:00+00') AS r
UNION ALL
SELECT 395 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (timetz '23:00+10', timetz '01:00+00') AS r
UNION ALL
SELECT 396 AS i, (timetz '23:00+10', timetz '04:00-02') OVERLAPS (timetz '05:00+00', timetz '03:00+02') AS r
UNION ALL
SELECT 397 AS i, (timetz '05:00+00', timetz '23:00+10') OVERLAPS (timetz '01:00+00', timetz '23:00+10') AS r
UNION ALL
SELECT 398 AS i, (timetz '23:00+10', timetz '23:00+10') OVERLAPS (timetz '01:00+00', NULL::timetz) AS r
UNION ALL
SELECT 399 AS i, (timetz '05:00+00', timetz '03:00+02') OVERLAPS (timetz '23:00+10', timetz '23:00+10') AS r
ORDER BY 1;
