SQLHeavy: SQLite on LevelDB, LMDB and everything else you can imagine
=====================================================================

SQLHeavy is a project to replace SQLite3's built-in b-tree with arbitrary storage engines from [libkvstore](https://github.com/btrask/libkvstore). libkvstore already supports several back-ends, including LevelDB, LMDB, and RocksDB, and more can be added by implementing a simple API. This lets you choose performance, scalability, and even distribution characteristics with minimal changes to applications already using SQLite.

The code for SQLHeavy and libkvstore is currently in the proof-of-concept stage. One goal is to get SQLHeavy working under SQLite's extensive test suite, and to add comprehensive tests to libkvstore.

This project is unrelated to a discontinued project by the same name. The other SQLHeavy was a wrapper around SQLite.

FAQ
---

**How is SQLHeavy different from [rqlite](https://github.com/rqlite/rqlite)?**  
Architecturally, rqlite builds on top of SQLite, whereas SQLHeavy builds underneath it. That means that rqlite provides an API over HTTP, but cannot support SQLite's native C interface. Similarly, because rqlite replicates raw SQL statements before they reach SQLite, non-deterministic queries like `INSERT ... random()` are incorrect.

SQLHeavy supports SQLite's native API without changes, so it is literally a drop-in replacement for existing programs that use SQLite. It also uses physical replication, with the possibility of mixed replication being added in the future.

**How is SQLHeavy different from [BedrockDB](http://bedrockdb.com/)?**  
Like rqlite, BedrockDB also builds on top of SQLite. It supports an HTTP API and a MySQL server interface.

SQLHeavy maintains the property of being a database library, not a server.

**What are the advantages of using libkvstore?**  
- Pluggable back-ends (LMDB, LevelDB, RocksDB, and it's easy to add more)
- Pluggable consensus algorithms

**Why not use SQLite [Virtual Tables](https://sqlite.org/vtab.html)?**  
There are pros and cons to each approach. By modifying the SQLite source directly, we can avoid requiring the `CREATE VIRTUAL TABLE` syntax and possibly support mixed replication in the future.

At some point, I would like to see pluggable storage engines supported natively by SQLite, like what was planned for SQLite4.

Building
--------

First, install [libkvstore](https://github.com/btrask/libkvstore). If you wish to use a back-end that isn't bundled (like RocksDB), you will need to install that.

Then:

```sh
./configure
make
sudo make install
```

To test SQLHeavy, run `make test`. SQLite's test suite requires `tcl-dev`.

Usage
-----

SQLHeavy is intended as a drop-in replacement for SQLite. However, you will need to ensure that applications link against SQLHeavy instead of SQLite, and also link to libkvstore, libcrypto, libstdc++, and other back-end libraries like librocksdb as appropriate.

License: SQLHeavy is released under the same terms as SQLite.

