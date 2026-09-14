#ifndef ARG_COERCE_H
#define ARG_COERCE_H

// The type inference `call` applies to one positional argument.
//
// Split out of call_command.cpp because it is the part that was WRONG and the
// part worth pinning. A command-line argument carries no type, so `call` guesses
// one; the guess used to be "whatever std::stoll / std::stoull / std::stod will
// swallow whole", and those three are C library parsers with grammars far wider
// than the one this CLI documents. std::stod alone accepts HEXADECIMAL FLOATS
// (C99 `0x` + hex digits, optional `p` exponent) and the words `inf` / `nan`, so
//
//   logoscore call keystore_module has_address 0x8ad0Fcf71D6FBD060BAfd45f5155b1e52d3591C5
//
// inferred a DOUBLE (7.9250088148318908e+47) and the module's `tstr` parameter
// never saw the address the user typed. Every EVM address, transaction hash,
// private key and signature is `0x` + hex, so that was every wallet and keystore
// method that names an account.
//
// The rule is now stated here rather than implied by three library calls: a
// numeric argument is a DECIMAL literal and nothing else. `0x1F`, `inf` and
// `nan` are strings, exactly as `0xzz` always was. Nothing that was a number
// before stops being one -- the decimal grammar is a strict subset of what the
// three parsers accepted.
//
// Pure, header-light and free of Qt, so the inference is exercised directly by
// the unit suite (tests/test_arg_coerce.cpp) instead of only through a command.

#include <logos_json.h>

#include <string>

namespace argcoerce {

// True when `text` is a DECIMAL number literal, the whole string and nothing
// but: an optional sign, digits with an optional fractional part (either side
// of the point may be the empty one, but not both), and an optional `e`/`E`
// exponent. No `0x`, no `inf`, no `nan`, no leading or trailing spaces --
// callers trim first.
bool isDecimalNumber(const std::string& text);

// `text` as the JSON value `call` will send: a bool for `true`/`false`, an
// int64 / uint64 / double for a decimal literal, and the string itself for
// everything else.
//
// Surrounding whitespace is ignored for the numeric decision only, so a numeric
// `@file` argument ending in a newline still coerces ("123\n" -> 123) while a
// string argument keeps every byte the caller passed ("hi\n" -> "hi\n").
nlohmann::json scalar(const std::string& text);

} // namespace argcoerce

#endif // ARG_COERCE_H
