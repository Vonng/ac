# acdat

`acdat` is an early PostgreSQL port of the original Go ACDAT benchmark.
Version 0.0.1 targets PostgreSQL 14 and uses PGXS.

The first usable piece is dictionary serialization:

```sql
CREATE EXTENSION acdat;

SELECT octet_length(
  acdat_compile(ARRAY['he', 'she', 'his', 'hers'])
);
```

`acdat_compile(text[])` validates a one-dimensional pattern array and stores
the strings in a `bytea`.  Empty patterns and null elements are rejected.  The
compiled value is still only a private prototype format; matching is not
exposed yet.

```bash
make
sudo make install
```

The intended matcher is Aho-Corasick, following the same basic idea as the old
Go implementation.  Compact Double-Array Trie storage can be considered after
the PostgreSQL API is working.

