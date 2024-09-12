# acdat

`acdat` is an experimental PostgreSQL extension for compiling a literal
dictionary and reusing it for multi-pattern detection and replacement.  The
matcher is based on the original ACDAT experiment: an Aho-Corasick automaton
with a future path toward compact Double-Array Trie storage.

Version 0.0.2 targets PostgreSQL 14 through 16.

## Build and upgrade

```bash
make
sudo make install
make installcheck
```

```sql
ALTER EXTENSION acdat UPDATE TO '0.0.2';
```

## Detection and replacement

```sql
WITH dictionary AS (
  SELECT acdat_compile(
    ARRAY['virus', 'trojan'],
    ARRAY['[risk]', '[risk]']
  ) AS value
)
SELECT
  acdat_contains('possible virus signature', value),
  acdat_replace('possible virus signature', value)
FROM dictionary;
```

Detection uses the byte-oriented Aho-Corasick machine.  Replacement is
left-to-right, longest-first, literal, and non-recursive.  Equal-length ties
follow dictionary order.

Compiled values are checked before use: the header and record section must be
consistent, failure links and transitions must stay inside the node array, and
every pattern payload must fit inside the value.

## Current limitations

- no match enumeration, pattern identifiers, positions, or priorities;
- replacement still scans stored pattern records rather than automaton output;
- the transition table uses 256 entries per node;
- the `bytea` layout contains native C structs and remains specific to a
  platform and extension version;
- dictionaries are managed by application SQL.

The next milestone is a real PostgreSQL machine type with a stable serialized
format, typed match output, and explicit dictionary lifecycle management.

