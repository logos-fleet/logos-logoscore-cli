#include "arg_coerce.h"
#include "../../string_utils.h"

#include <cstddef>
#include <string>

namespace argcoerce {

bool isDecimalNumber(const std::string& text)
{
    size_t i = 0;
    const size_t n = text.size();

    if (i < n && (text[i] == '+' || text[i] == '-')) ++i;

    size_t intDigits = 0;
    while (i < n && text[i] >= '0' && text[i] <= '9') { ++i; ++intDigits; }

    size_t fracDigits = 0;
    if (i < n && text[i] == '.') {
        ++i;
        while (i < n && text[i] >= '0' && text[i] <= '9') { ++i; ++fracDigits; }
    }

    // "5", "5.", ".5" and "5.5" are all numbers; "." and "+" are not.
    if (intDigits == 0 && fracDigits == 0) return false;

    if (i < n && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < n && (text[i] == '+' || text[i] == '-')) ++i;
        size_t expDigits = 0;
        while (i < n && text[i] >= '0' && text[i] <= '9') { ++i; ++expDigits; }
        if (expDigits == 0) return false;   // "1e" / "1e+" are not numbers
    }

    return i == n;   // the WHOLE string, or it is a string
}

nlohmann::json scalar(const std::string& text)
{
    if (text == "true")  return true;
    if (text == "false") return false;

    // Trim for the numeric decision only -- see the header. The value pushed on
    // the string path below is the caller's bytes, untouched.
    const std::string num = strutil::trim(text);
    if (!isDecimalNumber(num))
        return text;

    // std::stoll/std::stod parse a leading prefix and ignore the rest, so
    // "1.25" would parse as int 1. Require the whole string to be consumed
    // (matching the old QString::toInt(&ok) semantics) before accepting it as
    // that type.
    //
    // 64-BIT, in both signednesses. LIDL `int`/`uint` are int64_t / uint64_t
    // everywhere, so an argument must survive the full range. This used
    // std::stoi -- 32 bits -- and every integer outside int32 threw
    // out_of_range, got swallowed, and was re-parsed by std::stod as a DOUBLE.
    // Under 2^53 that is exact and looks fine; above it the value is silently
    // rounded (echoUint 9007199254740993 came back 9007199254740992). stoull
    // covers the band above int64max; it is only tried for a non-negative
    // literal because stoull("-1") happily wraps to 18446744073709551615.
    // (`num` has at least one digit, or isDecimalNumber would have said no.)
    try {
        size_t pos = 0;
        const long long intVal = std::stoll(num, &pos);
        if (pos == num.size()) return intVal;
    } catch (...) {}

    if (num.front() != '-') {
        try {
            size_t pos = 0;
            const unsigned long long uintVal = std::stoull(num, &pos);
            if (pos == num.size()) return uintVal;
        } catch (...) {}
    }

    // Past both integer types: a literal too long for uint64, or one with a
    // fraction or an exponent. isDecimalNumber already vouched for the grammar,
    // so a throw here can only be out_of_range -- an overflowing literal, which
    // keeps the old fall-through to the string rather than inventing an inf.
    try {
        size_t pos = 0;
        const double dblVal = std::stod(num, &pos);
        if (pos == num.size()) return dblVal;
    } catch (...) {}

    return text;
}

} // namespace argcoerce
