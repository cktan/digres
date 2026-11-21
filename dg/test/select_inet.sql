-- Verify inet and cidr, which ty does (ty/DESIGN.md section 14), and what a
-- client reads from them.
-- 1. Text in and out, the short forms, IPv4 and IPv6, and the casts.
-- 2. The order, the compares and the sort.
-- 3. Containment: <<, <<=, >>, >>= and &&.
-- 4. host, network, broadcast, netmask, hostmask, masklen, family, abbrev.
-- 5. merge, set_masklen, and ~, & and | of two values.
-- 6. + and - with integers, and - of two values.
-- 7. The hash and a hash join on inet.
-- (Errors are server.sh's: a run stops at its first.)
SELECT '1.2.3.4'::inet AS a, '1.2.3.4/24'::inet AS b, '10/8'::inet AS c, '10.1.2/24'::inet AS d, '::1'::inet AS e, '2001:db8::1/64'::inet AS f, '::ffff:1.2.3.4'::inet AS g, '1:0:0:2:0:0:0:3'::inet AS h, 'FE80::ABCD'::inet AS i, '0.0.0.0/0'::inet AS j, '::/0'::inet AS k;
SELECT '192.168.1.0/24'::cidr AS a, '10/8'::cidr AS b, '10.1'::cidr AS c, '2001:db8::/32'::cidr AS d, '::/0'::cidr AS e, '1.2.3.4'::cidr AS f, '::1'::cidr AS g, '192.168/16'::cidr AS h, '128.0.0.0/1'::cidr AS i;
SELECT '192.168.1.5/24'::inet::cidr AS a, '1.2.3.4/32'::cidr::inet AS b, '1.2.3.4/24'::inet::text AS c, '1.2.3.4'::inet::text AS d, '10.0.0.0/8'::cidr::text AS e, text('::1'::inet) AS f;
SELECT a, b, a < b AS lt, a = b AS eq, a > b AS gt, a <> b AS ne, network_cmp(a, b) AS cmp FROM (VALUES ('10.1.1.1/8'::inet, '10.1.1.1/16'::inet), ('10.1.1.1/8', '10.1.1.2/8'), ('10.1.1.1', '::1'), ('::1/128', '::1/64'), ('1.2.3.4', '1.2.3.4'), ('192.168.1.0/24', '192.168.0.0/24')) v(a, b);
SELECT i FROM (VALUES ('10.0.0.1'::inet), ('10.0.0.0/8'), ('::1'), ('9.9.9.9/32'), ('10.0.0.1/24'), ('2001:db8::/32'), ('0.0.0.0/0'), ('::/0'), ('255.255.255.255'), ('10.0.0.1/8')) v(i) ORDER BY i;
SELECT a, b, a << b AS sub, a <<= b AS subeq, a >> b AS sup, a >>= b AS supeq, a && b AS ovl FROM (VALUES ('192.168.1.5/32'::inet, '192.168.1.0/24'::inet), ('192.168.1.0/24', '192.168.1.0/24'), ('192.168.0.0/16', '192.168.1.0/24'), ('10.0.0.0/8', '192.168.1.0/24'), ('::1/128', '::/0'), ('::1', '10.0.0.0/8')) v(a, b);
SELECT host(i) AS host, network(i) AS net, broadcast(i) AS bc, netmask(i) AS nm, hostmask(i) AS hm, masklen(i) AS ml, family(i) AS fam, abbrev(i) AS ab FROM (VALUES ('192.168.1.5/24'::inet), ('10.1.2.3/8'), ('1.2.3.4'), ('2001:db8::5/48'), ('::1'), ('0.0.0.0/0'), ('255.255.255.255/31'), ('1.2.3.4/25')) v(i);
SELECT abbrev(c) AS ab, text(c) AS t, host(c) AS h, broadcast(c) AS bc FROM (VALUES ('10.0.0.0/8'::cidr), ('192.168.1.0/24'), ('10.1.0.0/16'), ('2001:db8::/32'), ('::/0'), ('1.2.3.4/32')) v(c);
SELECT inet_merge('192.168.1.5/24', '192.168.2.5/24') AS a, inet_merge('10.0.0.1', '10.0.0.2') AS b, inet_merge('::1', '::2') AS c, inet_same_family('1.2.3.4', '::1') AS d, inet_same_family('1.2.3.4', '5.6.7.8') AS e, set_masklen('1.2.3.4/24'::inet, 8) AS f, set_masklen('1.2.3.4/24'::inet, -1) AS g, set_masklen('1.2.3.0/24'::cidr::inet, 16) AS h, set_masklen('10.1.2.0/24'::cidr, 16) AS i;
SELECT ~'1.2.3.4'::inet AS a, '1.2.3.4' & '255.255.0.0'::inet AS b, '1.2.3.4/8' | '0.0.0.255'::inet AS c, ~'::1'::inet AS d, '::ffff' & 'ffff::ffff'::inet AS e;
SELECT '1.2.3.4'::inet + 5 AS a, '1.2.3.4'::inet - 5 AS b, '1.2.3.255'::inet + 1 AS c, '0.0.0.0'::inet + 0 AS d, '::1'::inet + 1000000000000 AS e, '::ffff:ffff:ffff:ffff'::inet - 9223372036854775807 AS f, '1.2.3.4'::inet - '1.2.3.1'::inet AS g, '1.2.3.1'::inet - '1.2.3.4'::inet AS h, '::10:0:0:0'::inet - '::'::inet AS i, 1000 + '10.0.0.0'::inet AS j;
SELECT hashinet('1.2.3.4'::inet) = hashinet('1.2.3.4/32'::inet) AS same, hashinet('1.2.3.4'::inet) = hashinet('1.2.3.4/24'::inet) AS diff, hashinetextended('1.2.3.4'::inet, 0) AS h0, hashinetextended('::1'::inet, 7) AS h7, hashinet('10.0.0.0/8'::cidr) AS hc, hashinet('::/0'::inet) AS h6;
SELECT count(*) FROM (SELECT ('10.0.0.0'::inet + i) a FROM generate_series(1, 500) i) x JOIN (SELECT ('10.0.0.0'::inet + i * 2) b FROM generate_series(1, 500) i) y ON a = b;
SELECT a, count(*) FROM (SELECT ('10.0.0.0/24'::inet + (i % 7)) a FROM generate_series(1, 100) i) x GROUP BY a ORDER BY a;
SELECT max(i) AS mx, min(i) AS mn FROM (VALUES ('10.0.0.1'::inet), ('10.0.0.0/8'), ('::1'), ('9.9.9.9/32')) v(i);
