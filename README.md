# acdat

`acdat` 0.0.1 is a small PostgreSQL 14 extension for exact multi-pattern
detection with a byte-oriented Aho-Corasick automaton.

```bash
make
sudo make install
make installcheck
```

```sql
CREATE EXTENSION acdat;

WITH dictionary AS (
  SELECT acdat_compile(ARRAY['he', 'she', 'his', 'hers']) AS value
)
SELECT acdat_contains('ushers', value)
FROM dictionary;
```

`acdat_compile(text[])` builds the trie and failure links.
`acdat_contains(text, bytea)` scans once and stops at the first match.  The
initial regression suite covers positive and negative matches, failure-link
fallback, suffix matches, and complete UTF-8 patterns.

Current limitations:

- detection only;
- no match identifiers, positions, priorities, or replacement;
- a dense 256-entry transition table per node;
- a native, version-specific `bytea` layout;
- dictionaries are managed by application SQL.

