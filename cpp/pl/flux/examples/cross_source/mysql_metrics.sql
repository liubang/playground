DROP TABLE IF EXISTS cpu;

CREATE TABLE cpu (
    _time DATETIME(6) NOT NULL,
    host VARCHAR(64) NOT NULL,
    region VARCHAR(32) NOT NULL,
    `usage` DOUBLE NOT NULL,
    cores INT UNSIGNED NOT NULL,
    active BOOLEAN NOT NULL,
    note VARCHAR(128) NULL,
    PRIMARY KEY (_time, host)
);

INSERT INTO cpu (_time, host, region, `usage`, cores, active, note) VALUES
    ('2024-07-01 10:00:00.000000', 'edge-1', 'west', 71.5, 8, TRUE, 'warmup'),
    ('2024-07-01 10:01:00.000000', 'edge-2', 'west', 64.0, 16, TRUE, 'steady'),
    ('2024-07-01 10:02:00.000000', 'edge-1', 'west', 93.25, 8, TRUE, NULL),
    ('2024-07-01 10:03:00.000000', 'edge-3', 'east', 42.75, 4, FALSE, 'idle'),
    ('2024-07-01 10:04:00.000000', 'edge-2', 'west', 82.0, 16, TRUE, 'burst'),
    ('2024-07-01 10:05:00.000000', 'edge-4', 'east', 55.5, 4, FALSE, 'recover');
