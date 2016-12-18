SQLHeavy: SQLite on LevelDB, LMDB and everything else you can imagine
=====================================================================

SQLHeavy is a project to replace SQLite3's built-in b-tree with arbitrary storage engines from [libkvstore](https://github.com/btrask/libkvstore). libkvstore already supports several back-ends, including LevelDB, LMDB, and RocksDB, and more can be added by implementing a simple API. This lets you choose performance, scalability, and even distribution characteristics with minimal changes to applications already using SQLite.

The code for SQLHeavy and libkvstore is currently in the proof-of-concept stage. One goal is to get SQLHeavy working under SQLite's extensive test suite, and to add comprehensive tests to libkvstore.

This project is unrelated to a discontinued project by the same name. The other SQLHeavy was a wrapper around SQLite.

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

