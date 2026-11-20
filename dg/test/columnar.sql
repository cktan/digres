-- Verify a column store table, made WITH (storage = columnar): its keys
-- and indexes taken with a warning and not made, its rows loaded,
-- inserted and read, and what needs a row's name refused for now.
-- 1. Make one, with a primary key, a unique key and a foreign key:
--    each is a warning, and the primary key's NOT NULL holds.
-- 2. CREATE INDEX is a warning too.
-- 3. INSERT ... SELECT loads 25,000 rows into row groups; one INSERT
--    puts a row into the delta store; scans read every row.
-- 4. UPDATE, DELETE and SELECT ... FOR UPDATE are refused.
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
update columnar_t set amount = 1 where id = 1;
delete from columnar_t where id = 1;
select * from columnar_t where id = 1 for update;
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
