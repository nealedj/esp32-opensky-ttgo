#pragma once
#include <Arduino.h>
#include <math.h>

#define KM_TO_MI 0.621371

// Great-circle distance between two lat/lon points, in kilometres (haversine).
inline double haversine_km(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double R = 6371.0;
    const double dlat = radians(lat2 - lat1);
    const double dlon = radians(lon2 - lon1);
    const double a = sin(dlat / 2) * sin(dlat / 2) +
                     cos(radians(lat1)) * cos(radians(lat2)) * sin(dlon / 2) * sin(dlon / 2);
    return 2 * R * asin(sqrt(a));
}

// Initial great-circle bearing from point 1 to point 2, in degrees (0 = North, clockwise, 0..360).
inline double initial_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    const double phi1 = radians(lat1), phi2 = radians(lat2);
    const double dlon = radians(lon2 - lon1);
    const double y = sin(dlon) * cos(phi2);
    const double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dlon);
    return fmod(degrees(atan2(y, x)) + 360.0, 360.0);
}
