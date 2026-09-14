// The one-argument type guess `call` applies to a positional argument.
//
// Pinned here rather than only through CallCommand because the inference is the
// part that was wrong, and because the defect it carries is invisible end to
// end: a `0x`-hex address inferred as a DOUBLE reached the module as a number,
// the module's `tstr` parameter never matched, and the call came back
// {"result": null, "status": "ok"} with nothing logged anywhere.
//
// The rule under test: a numeric argument is a DECIMAL literal and nothing
// else. std::stod -- which the inference used to lean on -- also accepts C99
// hexadecimal floats and the words `inf` / `nan`, and all three of those are
// strings here.

#include <gtest/gtest.h>

#include "client/commands/arg_coerce.h"

using argcoerce::isDecimalNumber;
using argcoerce::scalar;

// ── The reported defect ──────────────────────────────────────────────────────

// Every EVM address, transaction hash, private key and signature is `0x` + hex,
// so this one cell is every wallet and keystore method that names an account.
TEST(ArgCoerce, HexAddressStaysTheStringTheUserTyped)
{
    const std::string addr = "0x8ad0Fcf71D6FBD060BAfd45f5155b1e52d3591C5";
    const nlohmann::json v = scalar(addr);
    ASSERT_TRUE(v.is_string()) << "reached the module as " << v.dump();
    EXPECT_EQ(v.get<std::string>(), addr);
}

// The same address in lower case: all-hex either way, and the C library's
// hex-float grammar is case-insensitive, so both forms were numbers.
TEST(ArgCoerce, LowerCaseHexAddressStaysAString)
{
    const std::string addr = "0x8ad0fcf71d6fbd060bafd45f5155b1e52d3591c5";
    EXPECT_EQ(scalar(addr), nlohmann::json(addr));
}

// The short hex forms std::stod swallowed whole: a plain hex integer, an upper
// case `0X`, a signed one, and a genuine C99 hex FLOAT with a `p` exponent.
TEST(ArgCoerce, EveryHexSpellingIsAString)
{
    for (const char* s : {"0x1F", "0Xff", "-0x10", "0x1p4", "0x0", "0xdeadbeef"}) {
        const nlohmann::json v = scalar(s);
        EXPECT_TRUE(v.is_string()) << s << " inferred as " << v.dump();
        EXPECT_EQ(v, nlohmann::json(std::string(s)));
    }
}

// `inf` and `nan` are the other two words std::stod accepts. A NaN is the worse
// of the pair: nlohmann serialises it as `null`, so the argument did not merely
// arrive with the wrong type, it arrived as no value at all.
TEST(ArgCoerce, InfinityAndNanAreStrings)
{
    for (const char* s : {"inf", "INF", "infinity", "nan", "NaN", "-inf"}) {
        const nlohmann::json v = scalar(s);
        EXPECT_TRUE(v.is_string()) << s << " inferred as " << v.dump();
    }
}

// The control from the issue: not valid hex, and always a string. It is what
// made the defect legible -- `0xzz` answered while `0x8ad0...` did not.
TEST(ArgCoerce, NonHexIsStillAString)
{
    EXPECT_EQ(scalar("0xzz"), nlohmann::json("0xzz"));
    EXPECT_EQ(scalar("zzz"), nlohmann::json("zzz"));
}

// ── Nothing that was a number stops being one ───────────────────────────────

TEST(ArgCoerce, DecimalIntegersAreIntegers)
{
    EXPECT_EQ(scalar("3"), nlohmann::json(3));
    EXPECT_EQ(scalar("-4"), nlohmann::json(-4));
    EXPECT_EQ(scalar("+5"), nlohmann::json(5));
    EXPECT_EQ(scalar("0"), nlohmann::json(0));
    EXPECT_TRUE(scalar("3").is_number_integer());
}

TEST(ArgCoerce, SixtyFourBitIntegersStayExact)
{
    EXPECT_EQ(scalar("9007199254740993").get<int64_t>(), 9007199254740993LL);
    EXPECT_EQ(scalar("9223372036854775807").get<int64_t>(), INT64_MAX);
    EXPECT_EQ(scalar("-9223372036854775808").get<int64_t>(), INT64_MIN);
    EXPECT_TRUE(scalar("18446744073709551615").is_number_unsigned());
    EXPECT_EQ(scalar("18446744073709551615").get<uint64_t>(), 18446744073709551615ULL);
}

TEST(ArgCoerce, DecimalFloatsAreFloats)
{
    EXPECT_TRUE(scalar("1.25").is_number_float());
    EXPECT_DOUBLE_EQ(scalar("1.25").get<double>(), 1.25);
    EXPECT_DOUBLE_EQ(scalar("-0.5").get<double>(), -0.5);
    EXPECT_DOUBLE_EQ(scalar(".5").get<double>(), 0.5);
    EXPECT_DOUBLE_EQ(scalar("5.").get<double>(), 5.0);
    // A DECIMAL exponent is still a number -- only the hex `p` exponent went.
    EXPECT_DOUBLE_EQ(scalar("1e5").get<double>(), 100000.0);
    EXPECT_DOUBLE_EQ(scalar("1E-3").get<double>(), 0.001);
    EXPECT_DOUBLE_EQ(scalar("-2.5e2").get<double>(), -250.0);
    // Beyond uint64 there is no integer type left; still a number.
    EXPECT_TRUE(scalar("99999999999999999999999").is_number_float());
}

TEST(ArgCoerce, BooleansAreBooleans)
{
    EXPECT_EQ(scalar("true"), nlohmann::json(true));
    EXPECT_EQ(scalar("false"), nlohmann::json(false));
    // Only the exact words; `True` is a string, as it always was.
    EXPECT_TRUE(scalar("True").is_string());
}

// A numeric @file argument commonly arrives with a trailing newline and must
// still coerce, while a STRING argument keeps every byte the caller passed.
TEST(ArgCoerce, TrimsForTheNumericDecisionOnly)
{
    EXPECT_EQ(scalar("123\n"), nlohmann::json(123));
    EXPECT_DOUBLE_EQ(scalar(" 1.5 ").get<double>(), 1.5);
    EXPECT_EQ(scalar("hi\n"), nlohmann::json("hi\n"));
    EXPECT_EQ(scalar(""), nlohmann::json(""));
    EXPECT_EQ(scalar("   "), nlohmann::json("   "));
}

// ── The grammar itself ──────────────────────────────────────────────────────

TEST(ArgCoerce, DecimalGrammarAcceptsOnlyWholeDecimalLiterals)
{
    for (const char* s : {"0", "5", "-5", "+5", "5.", ".5", "5.5", "1e5",
                          "1E5", "1e+5", "1e-5", "-2.5e2"})
        EXPECT_TRUE(isDecimalNumber(s)) << s;

    for (const char* s : {"", ".", "+", "-", "e5", "1e", "1e+", "5x", "x5",
                          "0x1", "1.2.3", "1 2", "inf", "nan", "--5", "1,5"})
        EXPECT_FALSE(isDecimalNumber(s)) << s;
}
