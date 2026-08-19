SELECT acdat.compile(ARRAY[''], ARRAY[1]::bigint[]);

SELECT acdat.compile(ARRAY['a', 'b'], ARRAY[1, 1]::bigint[]);

WITH machine AS (
    SELECT acdat.compile(ARRAY['a'], ARRAY[1]::bigint[]) AS value
)
SELECT *
FROM machine, LATERAL acdat.matches('a', value, 'unknown_policy');

WITH machine AS (
    SELECT acdat.compile(
        ARRAY['a', 'aa', 'aaa'], ARRAY[1, 2, 3]::bigint[]
    ) AS value
)
SELECT *
FROM machine, LATERAL acdat.matches('aaa', value, 'all_overlapping', 2);

WITH machine AS (
    SELECT acdat.compile(ARRAY['a'], ARRAY[1]::bigint[]) AS value
)
SELECT acdat.replace('a', value)
FROM machine;

WITH machine AS (
    SELECT acdat.compile(ARRAY['a'], ARRAY[1]::bigint[]) AS value
)
SELECT acdat.contains(decode('61', 'hex'), value)
FROM machine;

WITH machine AS (
    SELECT acdat.compile(ARRAY['a'], ARRAY[1]::bigint[]) AS value
), damaged AS (
    SELECT set_byte(acdat.export(value), 0, 0) AS value
    FROM machine
)
SELECT acdat.validate(value) AS damaged_is_valid
FROM damaged;

WITH machine AS (
    SELECT acdat.compile(ARRAY['a'], ARRAY[1]::bigint[]) AS value
), damaged AS (
    SELECT set_byte(acdat.export(value), 0, 0) AS value
    FROM machine
)
SELECT acdat.import(value)
FROM damaged;

CREATE TEMP TABLE active_build AS
WITH machine AS (
    SELECT acdat.compile(ARRAY['a'], ARRAY[1]::bigint[]) AS value
), published AS (
    SELECT acdat.publish('errors', 1, value) AS build_id
    FROM machine
)
SELECT build_id FROM published;

SELECT acdat.activate('errors', build_id)
FROM active_build;

SELECT acdat.retire_build('errors', build_id)
FROM active_build;

-- end of acdat_errors
