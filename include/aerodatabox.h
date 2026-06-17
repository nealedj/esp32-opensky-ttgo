#pragma once

#include <Arduino.h>
#include <flight_info.h>

// Call once (and again if the key changes) before the first enrich_flight().
extern void aerodatabox_configure(const char *rapidapi_key);

// Best-effort route + aircraft enrichment from AeroDataBox, keyed by callsign.
// On success fills flight.iata_origin_airport, flight.iata_destination_airport,
// flight.aircraft_code and flight.registration (any of which may be empty if the
// API has no data). Results are cached so each callsign is only queried once.
// Returns true if the flight was enriched (from cache or a fresh query), false on error.
extern bool enrich_flight(flight_info &flight, String &error_message);
