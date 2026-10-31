-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 9: calendars (roadmap §6; milestone 5.7). Events stay with Phase 6.
--
-- The clock counts game seconds (standard seconds) from 0: year 1, month 1, day 1, 00:00:00 in
-- every calendar. A minute is always 60 seconds; a calendar sets how many minutes make its hour
-- and how many hours its day. Body calendars take their day length, year length and tilt from
-- the ranges, rolled from the world's seed. Names are string keys (player-facing text).

CREATE TABLE calendar (
    id               TEXT PRIMARY KEY,
    name_key         TEXT NOT NULL REFERENCES string(key),
    standard         INTEGER NOT NULL DEFAULT 0 CHECK (standard IN (0, 1)),  -- one is the standard calendar
    hours_per_day    INTEGER NOT NULL DEFAULT 24,
    minutes_per_hour INTEGER NOT NULL DEFAULT 60,
    leap_every       INTEGER NOT NULL DEFAULT 0,   -- years; 0: no leap years
    leap_month       INTEGER NOT NULL DEFAULT 1,   -- 1-based: the month that gains a day
    era_key          TEXT REFERENCES string(key),  -- "Year {n} of the Ash"
    start_year       INTEGER NOT NULL DEFAULT 1,   -- where a new game begins
    start_month      INTEGER NOT NULL DEFAULT 1,
    start_day        INTEGER NOT NULL DEFAULT 1,
    start_minute     INTEGER NOT NULL DEFAULT 360, -- minutes into the day (06:00)
    real_minutes_per_day REAL NOT NULL DEFAULT 48, -- how long a day lasts for the player
    daylight         REAL NOT NULL DEFAULT 0.6,    -- the day's share that is daylight, without seasons
    solar_noon       REAL NOT NULL DEFAULT 0.5,    -- the middle of daylight, as a share of the day
    twilight         REAL NOT NULL DEFAULT 0.04,   -- dawn and dusk, as a share of the day
    day_hours_min    REAL NOT NULL DEFAULT 24,     -- ranges for body calendars
    day_hours_max    REAL NOT NULL DEFAULT 24,
    year_days_min    INTEGER NOT NULL DEFAULT 0,   -- 0: the months as they are
    year_days_max    INTEGER NOT NULL DEFAULT 0,
    tilt_min         REAL NOT NULL DEFAULT 0,      -- degrees
    tilt_max         REAL NOT NULL DEFAULT 0
);
CREATE UNIQUE INDEX calendar_one_standard ON calendar(standard) WHERE standard = 1;

CREATE TABLE calendar_month (
    calendar TEXT NOT NULL REFERENCES calendar(id) ON DELETE CASCADE,
    idx      INTEGER NOT NULL,                     -- 0-based order in the year
    name_key TEXT NOT NULL REFERENCES string(key),
    days     INTEGER NOT NULL,
    PRIMARY KEY (calendar, idx)
);

CREATE TABLE calendar_weekday (
    calendar TEXT NOT NULL REFERENCES calendar(id) ON DELETE CASCADE,
    idx      INTEGER NOT NULL,                     -- day 1 of year 1 is weekday 0
    name_key TEXT NOT NULL REFERENCES string(key),
    PRIMARY KEY (calendar, idx)
);

CREATE TABLE calendar_season (
    calendar TEXT NOT NULL REFERENCES calendar(id) ON DELETE CASCADE,
    idx      INTEGER NOT NULL,                     -- 0-based; the first starts with the year
    name_key TEXT NOT NULL REFERENCES string(key),
    share    REAL NOT NULL,                        -- of the year; they add up to 1
    daylight REAL NOT NULL,                        -- the day's daylight share at its middle
    temp_min REAL NOT NULL DEFAULT 0,              -- degrees Celsius
    temp_max REAL NOT NULL DEFAULT 20,
    PRIMARY KEY (calendar, idx)
);

CREATE TABLE calendar_moon (
    calendar TEXT NOT NULL REFERENCES calendar(id) ON DELETE CASCADE,
    idx      INTEGER NOT NULL,
    name_key TEXT NOT NULL REFERENCES string(key),
    period   REAL NOT NULL,                        -- days from full moon to full moon
    offset   REAL NOT NULL DEFAULT 0,              -- its phase at the clock's 0 (0: new, 0.5: full)
    PRIMARY KEY (calendar, idx)
);

-- The standard calendar every project starts with.
INSERT OR IGNORE INTO string (key, english, context) VALUES
    ('cal_standard.name', 'Standard', 'The name of the standard calendar'),
    ('cal_standard.era', 'Year {n}', 'How a year is shown; {n} is its number'),
    ('cal_standard.month.0', 'January', 'Month'), ('cal_standard.month.1', 'February', 'Month'),
    ('cal_standard.month.2', 'March', 'Month'), ('cal_standard.month.3', 'April', 'Month'),
    ('cal_standard.month.4', 'May', 'Month'), ('cal_standard.month.5', 'June', 'Month'),
    ('cal_standard.month.6', 'July', 'Month'), ('cal_standard.month.7', 'August', 'Month'),
    ('cal_standard.month.8', 'September', 'Month'), ('cal_standard.month.9', 'October', 'Month'),
    ('cal_standard.month.10', 'November', 'Month'), ('cal_standard.month.11', 'December', 'Month'),
    ('cal_standard.weekday.0', 'Sunnudagr', 'Weekday'), ('cal_standard.weekday.1', 'Mánadagr', 'Weekday'),
    ('cal_standard.weekday.2', 'Týsdagr', 'Weekday'), ('cal_standard.weekday.3', 'Óðinsdagr', 'Weekday'),
    ('cal_standard.weekday.4', 'Þórsdagr', 'Weekday'), ('cal_standard.weekday.5', 'Frjádagr', 'Weekday'),
    ('cal_standard.weekday.6', 'Laugardagr', 'Weekday'),
    ('cal_standard.season.0', 'Winter', 'Season'), ('cal_standard.season.1', 'Spring', 'Season'),
    ('cal_standard.season.2', 'Summer', 'Season'), ('cal_standard.season.3', 'Autumn', 'Season'),
    ('cal_standard.moon.0', 'Máni', 'Moon');

INSERT OR IGNORE INTO calendar (id, name_key, standard, leap_every, leap_month, era_key, start_month,
                                day_hours_min, day_hours_max, year_days_min, year_days_max,
                                tilt_min, tilt_max)
VALUES ('cal_standard', 'cal_standard.name', 1, 4, 2, 'cal_standard.era', 4,
        18, 36, 200, 500, 0, 35);

INSERT OR IGNORE INTO calendar_month (calendar, idx, name_key, days) VALUES
    ('cal_standard', 0, 'cal_standard.month.0', 31), ('cal_standard', 1, 'cal_standard.month.1', 28),
    ('cal_standard', 2, 'cal_standard.month.2', 31), ('cal_standard', 3, 'cal_standard.month.3', 30),
    ('cal_standard', 4, 'cal_standard.month.4', 31), ('cal_standard', 5, 'cal_standard.month.5', 30),
    ('cal_standard', 6, 'cal_standard.month.6', 31), ('cal_standard', 7, 'cal_standard.month.7', 31),
    ('cal_standard', 8, 'cal_standard.month.8', 30), ('cal_standard', 9, 'cal_standard.month.9', 31),
    ('cal_standard', 10, 'cal_standard.month.10', 30), ('cal_standard', 11, 'cal_standard.month.11', 31);

INSERT OR IGNORE INTO calendar_weekday (calendar, idx, name_key) VALUES
    ('cal_standard', 0, 'cal_standard.weekday.0'), ('cal_standard', 1, 'cal_standard.weekday.1'),
    ('cal_standard', 2, 'cal_standard.weekday.2'), ('cal_standard', 3, 'cal_standard.weekday.3'),
    ('cal_standard', 4, 'cal_standard.weekday.4'), ('cal_standard', 5, 'cal_standard.weekday.5'),
    ('cal_standard', 6, 'cal_standard.weekday.6');

-- Winter at the year's start and end (with its shortest day in the middle), as in the north.
INSERT OR IGNORE INTO calendar_season (calendar, idx, name_key, share, daylight, temp_min, temp_max) VALUES
    ('cal_standard', 0, 'cal_standard.season.0', 0.25, 0.35, -10, 5),
    ('cal_standard', 1, 'cal_standard.season.1', 0.25, 0.5, 2, 16),
    ('cal_standard', 2, 'cal_standard.season.2', 0.25, 0.65, 12, 28),
    ('cal_standard', 3, 'cal_standard.season.3', 0.25, 0.5, 3, 17);

INSERT OR IGNORE INTO calendar_moon (calendar, idx, name_key, period, offset) VALUES
    ('cal_standard', 0, 'cal_standard.moon.0', 29.5, 0.0);
