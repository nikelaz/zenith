#ifndef USAGE_TIME_H
#define USAGE_TIME_H

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

namespace usage_time {
inline std::string format_utc(std::time_t value) {
    std::tm time{};
#ifdef _WIN32
    if (gmtime_s(&time, &value) != 0)
        return {};
#else
    if (gmtime_r(&value, &time) == nullptr)
        return {};
#endif
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (time.tm_mon < 0 || time.tm_mon >= 12)
        return {};
    const int hour = time.tm_hour % 12 == 0 ? 12 : time.tm_hour % 12;
    char formatted[64];
    std::snprintf(formatted, sizeof(formatted), "%s %d, %d at %d:%02d %s UTC",
                  months[time.tm_mon], time.tm_mday, time.tm_year + 1900, hour,
                  time.tm_min, time.tm_hour < 12 ? "AM" : "PM");
    return formatted;
}

inline bool parse_digits(std::string_view value, std::size_t position, std::size_t count,
                         int* result) {
    if (position + count > value.size())
        return false;
    int parsed = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char character = value[position + i];
        if (character < '0' || character > '9')
            return false;
        parsed = parsed * 10 + character - '0';
    }
    *result = parsed;
    return true;
}

inline std::string format_iso8601_utc(std::string_view value) {
    if (value.size() < 19 || value[4] != '-' || value[7] != '-' ||
        (value[10] != 'T' && value[10] != 't' && value[10] != ' ') ||
        value[13] != ':' || value[16] != ':')
        return {};

    int year_value = 0;
    int month_value = 0;
    int day_value = 0;
    int hour_value = 0;
    int minute_value = 0;
    int second_value = 0;
    if (!parse_digits(value, 0, 4, &year_value) ||
        !parse_digits(value, 5, 2, &month_value) ||
        !parse_digits(value, 8, 2, &day_value) ||
        !parse_digits(value, 11, 2, &hour_value) ||
        !parse_digits(value, 14, 2, &minute_value) ||
        !parse_digits(value, 17, 2, &second_value) || hour_value > 23 ||
        minute_value > 59 || second_value > 59)
        return {};

    std::size_t suffix = 19;
    if (suffix < value.size() && value[suffix] == '.') {
        ++suffix;
        const std::size_t fraction_start = suffix;
        while (suffix < value.size() && value[suffix] >= '0' && value[suffix] <= '9')
            ++suffix;
        if (suffix == fraction_start)
            return {};
    }

    int offset_minutes = 0;
    if (suffix < value.size() && value[suffix] != 'Z' && value[suffix] != 'z') {
        const char sign = value[suffix];
        if ((sign != '+' && sign != '-') || value.size() - suffix != 6 ||
            value[suffix + 3] != ':')
            return {};
        int offset_hours = 0;
        int offset_minute_part = 0;
        if (!parse_digits(value, suffix + 1, 2, &offset_hours) ||
            !parse_digits(value, suffix + 4, 2, &offset_minute_part) ||
            offset_hours > 23 || offset_minute_part > 59)
            return {};
        offset_minutes = offset_hours * 60 + offset_minute_part;
        if (sign == '-')
            offset_minutes = -offset_minutes;
        suffix = value.size();
    } else if (suffix < value.size()) {
        if (suffix + 1 != value.size())
            return {};
        ++suffix;
    }

    if (suffix != value.size())
        return {};

    using namespace std::chrono;
    const year_month_day date{year{year_value}, month{static_cast<unsigned>(month_value)},
                              day{static_cast<unsigned>(day_value)}};
    if (!date.ok())
        return {};
    auto utc = sys_days{date} + hours{hour_value} + minutes{minute_value} +
               seconds{second_value} - minutes{offset_minutes};
    const auto timestamp = time_point_cast<system_clock::duration>(utc);
    return format_utc(system_clock::to_time_t(timestamp));
}
}  // namespace usage_time

#endif
