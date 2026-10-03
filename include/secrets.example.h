// Copy this file to secrets.h and fill in your details.
#pragma once

#define WIFI_SSID "your-wifi-name"
#define WIFI_PASS "your-wifi-password"

// Locations to cycle through by tapping the screen. Anything wttr.in accepts:
//   ""              -> auto-detect from your public IP
//   "London"        -> city name (spaces are fine: "Salt Lake City")
//   "~Eiffel Tower" -> landmark search
//   "JFK"           -> airport code
//   "48.85,2.35"    -> coordinates
static const char *const WTTR_LOCATIONS[] = {""};

// Units: "u" = USCS (°F, mph), "m" = metric (°C, km/h), "M" = metric with m/s,
// "" = let wttr.in decide from your location.
#define WTTR_UNITS ""

// wttr.in data only updates every ~30-60 min; please don't hammer the service.
#define REFRESH_MINUTES 15
