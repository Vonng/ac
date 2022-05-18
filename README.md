# acdat

`acdat` is an early PostgreSQL port of the original Go ACDAT benchmark.
Version 0.0.1 targets PostgreSQL 14 and uses PGXS.

`acdat_compile(text[])` now builds a byte-oriented Aho-Corasick trie, including
failure links and terminal propagation, and returns the machine as a `bytea`.

```sql
CREATE EXTENSION acdat;

SELECT octet_length(
  acdat_compile(ARRAY['he', 'she', 'his', 'hers'])
);
```

The implementation deliberately uses a dense 256-entry transition table per
node.  It is simple to inspect but uses much more memory than a Double-Array
Trie.  The SQL scan function is not exposed yet.

```bash
make
sudo make install
```

The machine layout contains native C structs and must not be persisted across
extension upgrades or copied between platforms.

