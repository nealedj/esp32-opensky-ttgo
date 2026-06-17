#include <flight_info.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp32-hal-log.h>
#include <http_status.h>

static const char *TOKEN_URL =
    "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";
static const char *STATES_HOST = "https://opensky-network.org/api/states/all";

static String        s_client_id;
static String        s_client_secret;
static String        s_access_token;
static unsigned long s_token_expires_at_ms = 0;
static WiFiClientSecure s_tls;

void flights_configure(const char *client_id, const char *client_secret)
{
    s_client_id     = client_id;
    s_client_secret = client_secret;
    s_tls.setInsecure();
    // Force re-auth on next call so a credential change takes effect immediately.
    s_access_token  = "";
    s_token_expires_at_ms = 0;
    log_i("[opensky] credentials updated");
}

static bool refresh_token()
{
    HTTPClient http;
    if (!http.begin(s_tls, TOKEN_URL))
    {
        log_e("[token] begin failed");
        return false;
    }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body = "grant_type=client_credentials";
    body += "&client_id=";
    body += s_client_id;
    body += "&client_secret=";
    body += s_client_secret;

    int code = http.POST(body);
    if (code != HTTP_CODE_OK)
    {
        log_e("[token] HTTP %d", code);
        http.end();
        return false;
    }

    // Filter to only materialise the two fields we need — the JWT alone can be 1 KB+.
    DynamicJsonDocument filter_doc(64);
    filter_doc["access_token"] = true;
    filter_doc["expires_in"]   = true;

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter_doc));
    http.end();
    if (err)
    {
        log_e("[token] json: %s", err.c_str());
        return false;
    }

    s_access_token = doc["access_token"].as<String>();
    long expires_in = doc["expires_in"] | 1800;
    // Refresh 60 s early so we never present an expired token.
    s_token_expires_at_ms = millis() + (unsigned long)(expires_in - 60) * 1000UL;
    log_i("[token] refreshed, valid for %ld s", expires_in);
    return true;
}

static bool ensure_token()
{
    if (s_access_token.isEmpty() || (long)(millis() - s_token_expires_at_ms) >= 0)
        return refresh_token();
    return true;
}

// GET with a single 401 retry after a forced token refresh.
static int do_states_get(HTTPClient &http, const String &url)
{
    http.begin(s_tls, url);
    http.addHeader("Authorization", "Bearer " + s_access_token);
    int code = http.GET();

    if (code == HTTP_CODE_UNAUTHORIZED)
    {
        http.end();
        if (!refresh_token())
            return code;
        http.begin(s_tls, url);
        http.addHeader("Authorization", "Bearer " + s_access_token);
        code = http.GET();
    }
    return code;
}

bool get_flights(float latitude, float longitude, float range_latitude, float range_longitude,
                 bool air, bool ground, bool /* gliders */, bool /* vehicles */,
                 std::list<flight_info> &flights, String &error_message)
{
    // Note: gliders and vehicles flags have no equivalent in the OpenSky state vector;
    // they are accepted to keep the call-site unchanged but are currently ignored.

    flights.clear();
    error_message = "";

    if (s_client_id.isEmpty() || s_client_secret.isEmpty())
    {
        error_message = "OpenSky credentials not configured";
        log_e("%s", error_message.c_str());
        return false;
    }

    if (!ensure_token())
    {
        error_message = "Failed to obtain OpenSky token";
        log_e("%s", error_message.c_str());
        return false;
    }

    // Bounding box: range_latitude/longitude are treated as the half-extent,
    // so the watched area is ±range degrees around the configured centre.
    String url = String(STATES_HOST);
    url += "?lamin="; url += String(latitude  - range_latitude,  4);
    url += "&lomin="; url += String(longitude - range_longitude, 4);
    url += "&lamax="; url += String(latitude  + range_latitude,  4);
    url += "&lomax="; url += String(longitude + range_longitude, 4);

    log_i("Request states: %s", url.c_str());

    HTTPClient http;
    int code = do_states_get(http, url);

    if (code == 429)
    {
        http.end();
        error_message = "429 Rate limited";
        log_w("%s", error_message.c_str());
        return false;
    }
    if (code != HTTP_CODE_OK)
    {
        http.end();
        error_message = String(code) + " " + (code < 0 ? http.errorToString(code) : String(http_status_reason(code)));
        log_e("HTTP error: %s", error_message.c_str());
        return false;
    }

    // OpenSky sends the body with "Transfer-Encoding: chunked". HTTPClient::getStream()
    // hands back the RAW socket, which still contains the chunk-size framing and is NOT
    // valid JSON. getString() decodes the chunks for us, so we parse from that instead.
    String response = http.getString();
    http.end();
    log_d("Response %d bytes", response.length());

    // Only materialise the states array to save RAM (filter discards the "time" key).
    DynamicJsonDocument filter_doc(64);
    filter_doc["states"] = true;

    DynamicJsonDocument doc(48 * 1024);
    DeserializationError err = deserializeJson(doc, response, DeserializationOption::Filter(filter_doc));
    if (err)
    {
        log_e("Deserialize error: %s", err.c_str());
        error_message = err.c_str();
        return false;
    }

    JsonArray states = doc["states"].as<JsonArray>();
    if (states.isNull())
    {
        log_i("No states in response");
        return true;
    }

    // OpenSky state-vector positional indices:
    //  0 icao24        1 callsign       2 origin_country  3 time_position
    //  4 last_contact  5 longitude      6 latitude        7 baro_altitude (m)
    //  8 on_ground     9 velocity (m/s) 10 true_track     11 vertical_rate (m/s)
    // 12 sensors       13 geo_altitude  14 squawk         15 spi
    // 16 position_source
    for (JsonArray s : states)
    {
        // Skip state vectors without a valid position fix.
        if (s[5].isNull() || s[6].isNull())
            continue;

        const bool on_ground = s[8] | false;
        if (on_ground && !ground)
            continue;
        if (!on_ground && !air)
            continue;

        String callsign = s[1].isNull() ? String("") : s[1].as<String>();
        callsign.trim(); // OpenSky space-pads callsigns to 8 chars

        // ICAO airline designator = first 3 chars of callsign (works for commercial ops).
        String icao_airline;
        if (callsign.length() >= 3)
            icao_airline = callsign.substring(0, 3);

        const float baro_alt_m = s[7]  | 0.0f;
        const float vel_ms     = s[9]  | 0.0f;
        const float track_deg  = s[10] | 0.0f;
        const float vrate_ms   = s[11] | 0.0f;
        const time_t ts        = s[4]  | (time_t)0;
        String squawk          = s[14].isNull() ? String("") : s[14].as<String>();

        struct flight_info fi
        {
            .icao_address             = s[0].as<const char *>(),
            .latitude                 = s[6].as<float>(),
            .longitude                = s[5].as<float>(),
            .heading                  = (int)track_deg,
            .altitude                 = (int)(baro_alt_m * 3.28084f),  // m  → ft
            .ground_speed             = (int)(vel_ms     * 1.94384f),  // m/s → kts
            .squawk                   = squawk,
            .radar                    = "",
            .aircraft_code            = "",   // filled in Phase 3 (AeroDataBox)
            .registration             = "",   // filled in Phase 3 (AeroDataBox)
            .timestamp                = ts,
            .iata_origin_airport      = "",   // filled in Phase 3 (AeroDataBox)
            .iata_destination_airport = "",   // filled in Phase 3 (AeroDataBox)
            .flight                   = callsign,
            .on_ground                = on_ground,
            .vertical_speed           = (int)(vrate_ms   * 196.85f),   // m/s → ft/min
            .call_sign                = callsign,
            .icao_airline             = icao_airline,
        };
        log_d("%s", fi.toString().c_str());
        flights.push_back(fi);
    }

    log_i("Parsed %d flights", flights.size());
    return true;
}

String flight_info::toString() const
{
    return "ICAO " + icao_address + ": Flight " + flight + " from " + iata_origin_airport + " to " + iata_destination_airport + ", Squawk: " + String(squawk) + ", Radar: " + radar + ", Registration: " + registration + ", Lat: " + String(latitude) + ", Lon: " + String(longitude) + ", Altitude: " + String(altitude) + " ft, Ground speed: " + String(ground_speed) + " kts, Heading: " + String(heading) + " degrees, Vertical speed: " + String(vertical_speed) + ", Aircraft code: " + aircraft_code + ", Airline: " + icao_airline;
}
