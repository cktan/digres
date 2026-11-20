-- Verify a column store table, made WITH (storage = columnar): its keys
-- and indexes taken with a warning and not made, its rows loaded,
-- inserted, read, updated and deleted.
-- 1. Make one, with a primary key, a unique key and a foreign key:
--    each is a warning, and the primary key's NOT NULL holds.
-- 2. CREATE INDEX is a warning too.
-- 3. INSERT ... SELECT loads 25,000 rows into row groups; one INSERT
--    puts a row into the delta store; scans read every row.
-- 4. UPDATE, DELETE ... RETURNING and SELECT ... FOR UPDATE of rows of
--    a row group, by the scan's name of the row; a DELETE by _id, a
--    TID scan's, which sm finds the row group of; an UPDATE in a
--    REPEATABLE READ transaction.
-- 5. CREATE TABLE AS and COPY load one too.
-- 6. A SERIALIZABLE transaction cannot read it.
-- 7. storage = 'row' is a row store table ('row' quoted, as ROW is a
--    keyword); another value is refused.
\set ON_ERROR_STOP off
create temp table columnar_ref (id int primary key);
create temp table columnar_t (
  id int primary key,
  code text unique,
  ref int references columnar_ref (id),
  amount int)
  with (storage = columnar);
insert into columnar_t values (null, 'x', null, 1);
create index columnar_t_amount on columnar_t (amount);
insert into columnar_t
  select g, 'c' || g, null, g % 10 from generate_series(1, 25000) g;
insert into columnar_t values (0, 'zero', 7, 0);
select count(*), sum(amount), min(id), max(id) from columnar_t;
select amount, count(*) from columnar_t group by amount order by amount;
select * from columnar_t where id = 0;
select code from columnar_t where id between 12000 and 12002 order by id;
update columnar_t set amount = 100 where id = 1;
update columnar_t set amount = 200 where id = 0;
select id, amount from columnar_t where id in (0, 1) order by id;
delete from columnar_t where id between 2 and 4 returning id, code;
select id, code from columnar_t where id = 5 for update;
select _id as columnar_victim from columnar_t where id = 6 \gset
delete from columnar_t where _id = :columnar_victim returning id;
begin isolation level repeatable read;
update columnar_t set amount = amount + 1 where id = 7;
commit;
select count(*), sum(amount) from columnar_t;
select id, amount from columnar_t where id between 1 and 7 order by id;
create temp table columnar_ctas with (storage = columnar) as
  select g as id, g % 3 as k from generate_series(1, 12000) g;
copy columnar_ctas from stdin;
12001	5
12002	5
\.
select count(*), sum(k) from columnar_ctas;
select * from columnar_ctas where k = 5 order by id;
drop table columnar_ctas;
begin isolation level serializable;
select count(*) from columnar_t;
rollback;
create temp table columnar_row (a int) with (storage = 'row');
insert into columnar_row values (1);
select * from columnar_row;
create temp table columnar_bad (a int) with (storage = banana);
drop table columnar_row;
drop table columnar_t;
drop table columnar_ref;
