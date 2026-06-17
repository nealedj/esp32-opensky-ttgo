#include <aerodatabox.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <esp32-hal-log.h>
#include <map>

static const char *AERODATABOX_HOST = "aerodatabox.p.rapidapi.com";

// Persistent cache file and an upper bound on stored callsigns (protects flash/RAM).
static const char *CACHE_PATH = "/adb_cache.json";
static const size_t CACHE_MAX = 200;
// JSON buffer for loading/saving the whole cache (~88 bytes/entry, plus headroom).
static const size_t CACHE_DOC_BYTES = 32 * 1024;

static String           s_api_key;
static WiFiClientSecure s_tls;
static bool             s_fs_ready = false;

// One cached enrichment per callsign. `valid` is set even for empty results
// (e.g. private flights with no schedule) so we never re-query the same callsign.
struct enrichment_t
{
    String origin;       // IATA airport code
    String destination;  // IATA airport code
    String aircraft;     // aircraft model string (e.g. "Boeing 737-800")
    String registration; // tail number
    bool   valid;
};

// In-memory cache, persisted to LittleFS so it survives reboots (warm cache keeps us
// well under AeroDataBox's 600-call/month free tier).
static std::map<String, enrichment_t> s_cache;

// Load the persisted cache from LittleFS into the in-memory map (called once at boot).
static void cache_load()
{
    if (!s_fs_ready)
        return;

    File f = LittleFS.open(CACHE_PATH, "r");
    if (!f)
    {
        log_i("[aerodatabox] no cache file yet");
        return;
    }

    DynamicJsonDocument doc(CACHE_DOC_BYTES);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err)
    {
        log_w("[aerodatabox] cache parse failed: %s", err.c_str());
        return;
    }

    for (JsonPair kv : doc.as<JsonObject>())
    {
        JsonObject e = kv.value().as<JsonObject>();
        s_cache[String(kv.key().c_str())] = enrichment_t{
            e["o"] | "", e["d"] | "", e["a"] | "", e["r"] | "", true};
    }
    log_i("[aerodatabox] loaded %d cached entries", s_cache.size());
}

// Rewrite the whole cache file. Called only when a new entry is added (rare after
// warm-up), so the full-rewrite cost and flash wear are negligible.
static void cache_save()
{
    if (!s_fs_ready)
        return;

    DynamicJsonDocument doc(CACHE_DOC_BYTES);
    JsonObject root = doc.to<JsonObject>();
    for (auto &kv : s_cache)
    {
        JsonObject e      = root.createNestedObject(kv.first);
        e["o"]            = kv.second.origin;
        e["d"]            = kv.second.destination;
        e["a"]            = kv.second.aircraft;
        e["r"]            = kv.second.registration;
    }

    File f = LittleFS.open(CACHE_PATH, "w");
    if (!f)
    {
        log_e("[aerodatabox] cannot open cache for write");
        return;
    }
    serializeJson(doc, f);
    f.close();
    log_d("[aerodatabox] saved %d entries", s_cache.size());
}

// Insert/update an entry (evicting one if at capacity) and persist.
static void cache_put(const String &callsign, const enrichment_t &e)
{
    if (s_cache.size() >= CACHE_MAX && s_cache.find(callsign) == s_cache.end())
        s_cache.erase(s_cache.begin());
    s_cache[callsign] = e;
    cache_save();
}

void aerodatabox_configure(const char *rapidapi_key)
{
    s_api_key = rapidapi_key;
    s_tls.setInsecure();

    if (!s_fs_ready)
    {
        s_fs_ready = LittleFS.begin(true); // format on first use
        if (s_fs_ready)
        {
            log_i("[aerodatabox] LittleFS mounted");
            cache_load();
        }
        else
            log_e("[aerodatabox] LittleFS mount failed - cache not persistent");
    }
    log_i("[aerodatabox] key %s", s_api_key.isEmpty() ? "not set" : "configured");
}

static void apply_enrichment(flight_info &flight, const enrichment_t &e)
{
    flight.iata_origin_airport      = e.origin;
    flight.iata_destination_airport = e.destination;
    flight.aircraft_code            = e.aircraft;
    flight.registration             = e.registration;
}

bool enrich_flight(flight_info &flight, String &error_message)
{
    error_message = "";

    String callsign = flight.call_sign;
    callsign.trim();
    if (callsign.isEmpty())
        return false;

    // Cache check first — keeps us well under the AeroDataBox free-tier quota.
    auto cached = s_cache.find(callsign);
    if (cached != s_cache.end())
    {
        log_i("[aerodatabox] %s: cache hit", callsign.c_str());
        apply_enrichment(flight, cached->second);
        return true;
    }

    if (s_api_key.isEmpty())
    {
        error_message = "AeroDataBox key not configured";
        return false;
    }

    String url = String("https://") + AERODATABOX_HOST + "/flights/callsign/" + callsign;
    log_i("[aerodatabox] query %s", url.c_str());

    HTTPClient http;
    if (!http.begin(s_tls, url))
    {
        error_message = "Failed to start client";
        return false;
    }
    http.addHeader("X-RapidAPI-Key", s_api_key);
    http.addHeader("X-RapidAPI-Host", AERODATABOX_HOST);

    int code = http.GET();

    // 204/400/404 = no flight matched. Cache an empty entry so we don't ask again.
    if (code == 204 || code == 400 || code == 404)
    {
        http.end();
        cache_put(callsign, enrichment_t{"", "", "", "", true});
        log_i("[aerodatabox] %s: no data (HTTP %d), cached as miss", callsign.c_str(), code);
        return true;
    }
    if (code != HTTP_CODE_OK)
    {
        http.end();
        error_message = String(code);
        log_e("[aerodatabox] %s: HTTP %d", callsign.c_str(), code);
        return false;
    }

    String response = http.getString();
    http.end();

    // Response is an array of flight objects. Filter so only the few fields we need
    // are materialised; in an ArduinoJson array filter, element [0] is the template
    // applied to every element.
    DynamicJsonDocument filter(512);
    filter[0]["departure"]["airport"]["iata"] = true;
    filter[0]["arrival"]["airport"]["iata"]   = true;
    filter[0]["aircraft"]["model"]            = true;
    filter[0]["aircraft"]["reg"]              = true;

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, response, DeserializationOption::Filter(filter));
    if (err)
    {
        error_message = err.c_str();
        log_e("[aerodatabox] %s: json %s", callsign.c_str(), err.c_str());
        return false;
    }

    enrichment_t e{"", "", "", "", true};
    JsonArray arr = doc.as<JsonArray>();
    if (!arr.isNull() && arr.size() > 0)
    {
        JsonObject f      = arr[0];
        e.origin          = f["departure"]["airport"]["iata"] | "";
        e.destination     = f["arrival"]["airport"]["iata"]   | "";
        e.aircraft        = f["aircraft"]["model"]            | "";
        e.registration    = f["aircraft"]["reg"]              | "";
    }

    cache_put(callsign, e);
    apply_enrichment(flight, e);
    log_i("[aerodatabox] %s: %s-%s %s %s", callsign.c_str(),
          e.origin.c_str(), e.destination.c_str(), e.aircraft.c_str(), e.registration.c_str());
    return true;
}
