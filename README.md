# acdat

`acdat` 0.0.2 provides exact multi-pattern detection and simple literal
replacement on PostgreSQL 14 through 16.

```bash
make
sudo make install
make installcheck
```

```sql
WITH dictionary AS (
  SELECT acdat_compile(
    ARRAY['a', 'aa'],
    ARRAY['X', 'Y']
  ) AS value
)
SELECT acdat_replace('aaa', value)
FROM dictionary;
```

The result is `YX`: replacement walks from left to right and chooses the
longest pattern at each position.  Equal-length ties preserve dictionary
order, and generated text is never scanned again.

Regression coverage now includes replacement order, overlapping patterns,
empty replacement values, multibyte text, and ordinary contains queries.
Detection uses the Aho-Corasick machine; replacement still uses a simple scan
of the stored pattern records.

The compiled `bytea` remains a private, version-specific format.

