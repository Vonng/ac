# acdat

`acdat` 0.0.1 is a small PostgreSQL 14 extension for exact multi-pattern
detection with a byte-oriented Aho-Corasick automaton.

```bash
make
sudo make install
```

Compile a dictionary and test an input:

```sql
CREATE EXTENSION acdat;

WITH dictionary AS (
  SELECT acdat_compile(ARRAY['he', 'she', 'his', 'hers']) AS value
)
SELECT acdat_contains('ushers', value)
FROM dictionary;
```

`acdat_compile(text[])` builds the trie and failure links.  PostgreSQL text is
already valid in the database encoding, so matching full UTF-8 patterns as
bytes is sufficient for exact literal detection.  `acdat_contains(text,
bytea)` scans once and stops at the first match.

Current limitations:

- detection only;
- no match identifiers, positions, priorities, or replacement;
- a dense transition table with high memory use;
- a native, version-specific `bytea` layout;
- dictionaries are managed by application SQL.

