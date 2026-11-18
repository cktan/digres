# Running digres

How to make a data directory, start the server and connect
to it. [`BUILDING.md`](BUILDING.md) says how to build
`dg/build/digres`.

## Make a data directory

```
dg/build/digres --init /path/to/data
```

`digres --init` makes a data directory, as PostgreSQL's
`initdb` does; digres has no `initdb` (`--init --help` lists
its options). It makes the database `postgres`, and
`pg_global`, which holds the roles and which no client
connects to, with one superuser:

- **`-U NAME`**: the superuser, by default your OS user name.
- **`-W`** (prompt) or **`--pwfile=FILE`**: its password.
  Without one, only local socket connections work.
- The encoding is UTF8 and the collation C. Authentication is
  not configurable: a local socket connection is admitted
  when the OS user is the role (peer), a TCP connection by the
  role's password (SCRAM-SHA-256). There is no `pg_hba.conf`.

## Start the server

```
dg/build/digres -D /path/to/data -p 5432 -k /tmp
```

- It runs in the foreground and logs to stderr; Ctrl-C, or
  `kill -INT $(head -1 /path/to/data/postmaster.pid)`, shuts
  it down.
- **`-p`**: the TCP port, 5432 by default. **`-k`**: the
  socket's directory, `/tmp` by default; keep its path short,
  as a socket's may not pass 107 bytes. It listens on
  `localhost`.
- **Settings**: `-c NAME=VALUE`, or lines in
  `/path/to/data/postgresql.conf`, which `--init` does not
  write. `shared_buffers` (128MB by default) is the size of
  RocksDB's block cache: the benchmark uses 2GB.
- The release build, `dg/build/digres-release`, runs the same
  way.

## Connect

```
psql -h /tmp -p 5432 -d postgres                 # the socket, as your OS user
psql -h localhost -p 5432 -U NAME -d postgres    # TCP, with the password
```

- **Use PostgreSQL 18's `psql`** (`ext/pg18.6/install/bin`,
  [`BUILDING.md`](BUILDING.md)). Ubuntu 24.04's `psql` 16
  connects and runs SQL, and `\d`, `\dt` and `\di` work, but
  `\l` fails, as it does
  against PostgreSQL 18 itself: it reads
  `pg_database.daticulocale`, which PostgreSQL 17 renamed
  `datlocale`. Any PostgreSQL driver connects.
- `CREATE DATABASE app` makes another database; each is a
  directory, `base/app`, of its own.
