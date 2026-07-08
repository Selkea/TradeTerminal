// Stand-in for the Intel Decimal Floating-Point library (libbid) that the TWS
// API's Decimal.cpp links against. The API only touches decimals through
// these eight functions, so the token representation is ours to choose: a
// double's bit pattern stored in the 64-bit Decimal. That trades exact
// decimal semantics for IEEE double precision — inconsequential for share
// quantities/volumes, and it removes the awkward Intel library dependency
// entirely (the encode/decode paths convert through strings either way).
//
// UNSET_DECIMAL (ULLONG_MAX) is the API's "no value" sentinel and round-trips
// through every function here.

#include <bit>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using Decimal = unsigned long long;
constexpr Decimal kUnset = ULLONG_MAX;

double dec_to_double(Decimal d) {
    if (d == kUnset) return std::nan("");
    return std::bit_cast<double>(d);
}

Decimal double_to_dec(double v) {
    if (std::isnan(v)) return kUnset;
    return std::bit_cast<Decimal>(v);
}

void clear_flags(unsigned int* flags) {
    if (flags) *flags = 0;
}

} // namespace

extern "C" Decimal __bid64_add(Decimal a, Decimal b, unsigned int, unsigned int* flags) {
    clear_flags(flags);
    if (a == kUnset || b == kUnset) return kUnset;
    return double_to_dec(dec_to_double(a) + dec_to_double(b));
}

extern "C" Decimal __bid64_sub(Decimal a, Decimal b, unsigned int, unsigned int* flags) {
    clear_flags(flags);
    if (a == kUnset || b == kUnset) return kUnset;
    return double_to_dec(dec_to_double(a) - dec_to_double(b));
}

extern "C" Decimal __bid64_mul(Decimal a, Decimal b, unsigned int, unsigned int* flags) {
    clear_flags(flags);
    if (a == kUnset || b == kUnset) return kUnset;
    return double_to_dec(dec_to_double(a) * dec_to_double(b));
}

extern "C" Decimal __bid64_div(Decimal a, Decimal b, unsigned int, unsigned int* flags) {
    clear_flags(flags);
    if (a == kUnset || b == kUnset) return kUnset;
    return double_to_dec(dec_to_double(a) / dec_to_double(b));
}

extern "C" Decimal __bid64_from_string(char* s, unsigned int, unsigned int* flags) {
    clear_flags(flags);
    if (!s || !*s) return kUnset;
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s) return kUnset;
    return double_to_dec(v);
}

extern "C" void __bid64_to_string(char* out, Decimal d, unsigned int* flags) {
    clear_flags(flags);
    if (!out) return;
    if (d == kUnset) {
        out[0] = '\0';
        return;
    }
    // Shortest exact round-trip; plain decimal for the wire ("100", "0.5").
    std::snprintf(out, 64, "%.17g", dec_to_double(d));
}

extern "C" double __bid64_to_binary64(Decimal d, unsigned int, unsigned int* flags) {
    clear_flags(flags);
    return dec_to_double(d);
}

extern "C" Decimal __binary64_to_bid64(double v, unsigned int, unsigned int* flags) {
    clear_flags(flags);
    return double_to_dec(v);
}
