DROP TABLE IF EXISTS cpu;
DROP TABLE IF EXISTS mysql_types;

SET time_zone = '+00:00';

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

CREATE TABLE mysql_types (
    id INT NOT NULL PRIMARY KEY,
    tiny_bool BOOLEAN NOT NULL,
    tiny_signed TINYINT NOT NULL,
    huge_unsigned BIGINT UNSIGNED NOT NULL,
    exact_decimal DECIMAL(20, 6) NOT NULL,
    event_date DATE NOT NULL,
    event_time TIME(6) NOT NULL,
    event_datetime DATETIME(6) NOT NULL,
    event_timestamp TIMESTAMP(6) NOT NULL,
    payload VARBINARY(16) NOT NULL,
    nullable_text VARCHAR(64) NULL
);

INSERT INTO mysql_types (
    id,
    tiny_bool,
    tiny_signed,
    huge_unsigned,
    exact_decimal,
    event_date,
    event_time,
    event_datetime,
    event_timestamp,
    payload,
    nullable_text
) VALUES
    (
        1,
        TRUE,
        -7,
        18446744073709551615,
        12345678901234.123456,
        '2024-07-02',
        '12:34:56.123456',
        '2024-07-02 12:34:56.123456',
        '2024-07-02 12:34:56.123456',
        X'666C7578',
        NULL
    );
