# acdat

`acdat` is an experimental PostgreSQL extension for compiling a literal
dictionary and reusing it for multi-pattern detection and replacement.  The
matcher is based on the original ACDAT experiment: an Aho-Corasick automaton
with a future path toward compact Double-Array Trie storage.

Version 0.0.2 targets PostgreSQL 14 through 16.

## Build and upgrade

PostgreSQL server headers and PGXS are required:

```bash
make
sudo make install
make installcheck
```

New installations use 0.0.2.  Upgrade an existing prototype with:

```sql
ALTER EXTENSION acdat UPDATE TO '0.0.2';
```

## Detection

```sql
WITH dictionary AS (
  SELECT acdat_compile(ARRAY['he', 'she', 'his', 'hers']) AS value
)
SELECT acdat_contains('ushers', value)
FROM dictionary;
```

`acdat_contains(text, bytea)` uses the byte-oriented Aho-Corasick machine and
stops after the first match.

## Replacement

```sql
WITH dictionary AS (
  SELECT acdat_compile(
    ARRAY['virus', 'trojan'],
    ARRAY['[risk]', '[risk]']
  ) AS value
)
SELECT acdat_replace('possible virus signature', value)
FROM dictionary;
```

Replacement is left-to-right, longest-first, literal, and non-recursive.
Equal-length ties follow dictionary order.  This implementation scans stored
pattern records while replacing, so it is intentionally simpler and slower
than detection.

## Current limitations

- no match enumeration, pattern identifiers, positions, or priorities;
- replacement does not yet consume automaton match output;
- the transition table uses 256 entries per node;
- the `bytea` layout contains native C structs and is not portable between
  platforms or extension versions;
- dictionaries are managed by application SQL rather than extension catalogs.

The next milestone is a real PostgreSQL machine type with a stable serialized
format, typed match output, and explicit dictionary lifecycle management.

