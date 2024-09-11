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

SELECT acdat_replace(
    'ushers',
    acdat_compile(
        ARRAY['she', 'hers'],
        ARRAY['[SHE]', '[HERS]']
    )
);

SELECT acdat_replace(
    'aaa',
    acdat_compile(ARRAY['a', 'aa'], ARRAY['X', 'Y'])
);

SELECT acdat_replace(
    '发现病毒和木马',
    acdat_compile(ARRAY['病毒', '木马'], ARRAY['风险', '风险'])
);

DROP EXTENSION acdat;
