CREATE EXTENSION acdat;

SELECT acdat_version();

SELECT acdat_contains(
    'ushers',
    acdat_compile(ARRAY['he', 'she', 'his', 'hers'])
);

SELECT acdat_contains(
    'the quick brown fox',
    acdat_compile(ARRAY['slow', 'turtle'])
);

SELECT acdat_contains(
    '发现病毒特征码',
    acdat_compile(ARRAY['病毒', '木马'])
);

DROP EXTENSION acdat;

