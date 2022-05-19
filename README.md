# acdat

`acdat` is an early PostgreSQL port of the ACDAT experiment: multi-pattern
matching with an Aho-Corasick automaton and, eventually, a Double-Array Trie.
The original Go program was written for a dictionary-based string replacement
benchmark.  Sensitive-word detection and virus-signature lookup are the two
obvious database use cases.

Version 0.0.1 deliberately stays small.  It targets PostgreSQL 14 and exposes
only one operation: determine whether an input contains any pattern in a
compiled dictionary.

## Build

PostgreSQL 14 server headers and PGXS are required:

```bash
make
sudo make install
make installcheck
```

This is an experimental source build, not a packaged extension.

## Example

```sql
CREATE EXTENSION acdat;

WITH dictionary AS (
  SELECT acdat_compile(ARRAY['he', 'she', 'his', 'hers']) AS value
)
SELECT acdat_contains('ushers', value)
FROM dictionary;
```

`acdat_compile(text[])` builds a byte-oriented Aho-Corasick trie and stores it
in a `bytea`.  PostgreSQL text is already valid in the database encoding, so
matching complete UTF-8 patterns as bytes is sufficient for exact literal
detection.  `acdat_contains(text, bytea)` scans the input once and stops after
the first match.

## Current limitations

- detection only; there is no match enumeration or replacement API;
- no pattern identifiers or priorities;
- the transition table uses 256 entries per node and wastes a lot of space;
- the `bytea` layout contains native C structs and is not portable between
  machines or extension versions;
- malformed compiled values receive only basic validation;
- dictionaries must be compiled by the application and kept in a table or a
  query expression.

The next useful step is deterministic replacement.  A compact Double-Array
Trie and a stable serialized format can come later, after the SQL interface is
understood.

## Why Aho-Corasick

The automaton combines a trie with KMP-style failure links.  Building the
dictionary is more expensive than a single search, but an input can then be
checked against every pattern in one pass.  This is the same basic idea as the
old Go benchmark, with the I/O loop replaced by a PostgreSQL function call.

