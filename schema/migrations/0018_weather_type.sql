-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 18: weather types (milestone 5.15).
--
-- A weather type is a kind of weather, and what it does: how it looks (cloud cover, how much
-- daylight it takes, the light's tint, how far you can see, what falls and how much, lightning
-- flashes a minute), how it feels (wind in metres a second and its gusts, how wet uncovered
-- things get from 0 to 1, degrees Celsius it adds to the temperature) and how it sounds (sound
-- events that loop while it lasts, and the thunder after each flash). midgard/weather.h reads
-- it; later steps of 5.15 choose the weather from it by season and biome.
--
-- A project starts with the ten below, which the designer can change, delete or add to.

CREATE TABLE weather_type (
    id      TEXT PRIMARY KEY,
    name    TEXT NOT NULL,
    weather TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(weather))
                          -- {"clouds": 0, "darkness": 0, "tint": [1, 1, 1],
                          --  "visibility": 0,              (metres; 0: no fog of its own)
                          --  "falls": "none", "amount": 0, (none, rain, snow, ash, sand; 0-1)
                          --  "lightning": 0,               (flashes a minute)
                          --  "wind": 0, "gusts": 0, "wetness": 0, "temperature": 0,
                          --  "sounds": [{"event": "<id>", "volume": 1}], "thunder": "<id>"}
);

INSERT INTO weather_type (id, name, weather) VALUES
    ('clear', 'Clear', '{"clouds": 0.05, "wind": 2, "gusts": 0.1}'),
    ('cloudy', 'Cloudy',
     '{"clouds": 0.75, "darkness": 0.3, "tint": [0.92, 0.94, 0.98], "wind": 4, "gusts": 0.2,
       "temperature": -1}'),
    ('rain', 'Rain',
     '{"clouds": 0.95, "darkness": 0.5, "tint": [0.85, 0.88, 0.95], "visibility": 3000,
       "falls": "rain", "amount": 0.6, "wind": 5, "gusts": 0.3, "wetness": 0.8,
       "temperature": -3}'),
    ('thunderstorm', 'Thunderstorm',
     '{"clouds": 1, "darkness": 0.75, "tint": [0.78, 0.8, 0.9], "visibility": 1500,
       "falls": "rain", "amount": 1, "lightning": 4, "wind": 12, "gusts": 0.7, "wetness": 1,
       "temperature": -5}'),
    ('snow', 'Snow',
     '{"clouds": 0.9, "darkness": 0.35, "tint": [0.95, 0.97, 1.0], "visibility": 2000,
       "falls": "snow", "amount": 0.5, "wind": 3, "gusts": 0.2, "wetness": 0.2,
       "temperature": -4}'),
    ('blizzard', 'Blizzard',
     '{"clouds": 1, "darkness": 0.6, "tint": [0.9, 0.93, 1.0], "visibility": 120,
       "falls": "snow", "amount": 1, "wind": 18, "gusts": 0.6, "wetness": 0.4,
       "temperature": -10}'),
    ('fog', 'Fog',
     '{"clouds": 0.6, "darkness": 0.25, "tint": [0.93, 0.94, 0.95], "visibility": 200,
       "wind": 0.5, "wetness": 0.1, "temperature": -1}'),
    ('wind', 'Windy',
     '{"clouds": 0.4, "darkness": 0.1, "wind": 14, "gusts": 0.8, "temperature": -2}'),
    ('sandstorm', 'Sandstorm',
     '{"clouds": 0.3, "darkness": 0.5, "tint": [1.0, 0.82, 0.6], "visibility": 150,
       "falls": "sand", "amount": 0.8, "wind": 20, "gusts": 0.7}'),
    ('ash_fall', 'Ash fall',
     '{"clouds": 0.9, "darkness": 0.6, "tint": [0.85, 0.78, 0.72], "visibility": 800,
       "falls": "ash", "amount": 0.5, "wind": 3, "gusts": 0.2, "temperature": 1}');
