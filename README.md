# acdat

`acdat` is an experiment in moving the original Go ACDAT benchmark into
PostgreSQL.  The long-term idea is to compile a dictionary once and scan text
with an Aho-Corasick automaton.

Version 0.0.1 targets PostgreSQL 14.  The repository currently contains only a
small PGXS extension skeleton and a version function.

```bash
make
sudo make install
```

```sql
CREATE EXTENSION acdat;
SELECT acdat_version();
```

The old benchmark used Aho-Corasick with a Double-Array Trie for large
multi-pattern replacement jobs.  This PostgreSQL port starts with a simpler C
implementation so that the SQL interface can be explored first.

