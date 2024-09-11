# acdat

`acdat` 0.0.2 provides exact multi-pattern detection and simple literal
replacement on PostgreSQL 14 through 16.

```sql
CREATE EXTENSION acdat;

WITH dictionary AS (
  SELECT acdat_compile(
    ARRAY['virus', 'trojan'],
    ARRAY['[risk]', '[risk]']
  ) AS value
)
SELECT acdat_replace('possible virus signature', value)
FROM dictionary;
```

`acdat_replace(text, bytea)` walks from left to right and chooses the longest
pattern beginning at the current position.  Equal-length ties follow input
dictionary order.  Replacement is literal and non-recursive.

Detection continues to use the Aho-Corasick machine.  The first replacement
implementation intentionally scans stored pattern records at each position;
it is simple and deterministic, but not yet suitable for very large
dictionaries.

```bash
make
sudo make install
make installcheck
```

The compiled `bytea` remains a private, version-specific format.

