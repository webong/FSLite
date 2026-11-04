# Bundled SQLite

Official SQLite amalgamation, version **3.54.1**. `sqlite3.c` and `sqlite3.h`
are unmodified files from:

https://sqlite.org/2026/sqlite-amalgamation-3540100.zip

Archive SHA3-256:
`92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`

SQLite is public domain: https://sqlite.org/copyright.html

The build disables extension loading and double-quoted string literals, enables
SQLite mutexes, and hides SQLite symbols in the shared library. Static consumers
must avoid linking a second SQLite implementation with conflicting symbols.
