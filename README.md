# acdat

`acdat` 0.0.2 is an experimental Aho-Corasick matcher for PostgreSQL 14
through 16.  This update keeps the original detection-only SQL interface while
refreshing the extension metadata and upgrade path for PostgreSQL 16.

```bash
make
sudo make install
make installcheck
```

New installations use version 0.0.2.  An existing 0.0.1 installation can be
updated in place:

```sql
ALTER EXTENSION acdat UPDATE TO '0.0.2';
```

```sql
WITH dictionary AS (
  SELECT acdat_compile(ARRAY['he', 'she', 'his', 'hers']) AS value
)
SELECT acdat_contains('ushers', value)
FROM dictionary;
```

The machine is still a byte-oriented dense trie stored in a private `bytea`
format.  Version 0.0.2 does not yet expose match positions or replacement.

