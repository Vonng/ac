# acdat

`acdat` 0.0.2 is an experimental Aho-Corasick matcher for PostgreSQL 14
through 16.

The compiler can now store an optional replacement beside each pattern:

```sql
SELECT acdat_compile(
  ARRAY['virus', 'trojan'],
  ARRAY['[risk]', '[risk]']
);
```

The public scan API remains detection-only at this point:

```sql
SELECT acdat_contains(
  'possible virus signature',
  acdat_compile(ARRAY['virus', 'trojan'])
);
```

Replacement records are part of a revised private machine layout.  This is a
preparatory format change; no SQL replacement function is exposed yet.  Null
replacement elements mean that a pattern has no replacement value.

```bash
make
sudo make install
make installcheck
```

The machine still uses native C structs and is not portable across extension
versions or platforms.

