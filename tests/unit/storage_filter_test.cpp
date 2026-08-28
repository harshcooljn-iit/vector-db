// SPDX-License-Identifier: MIT
#include <string>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/storage/filter.hpp>

namespace vectordb {
namespace {

Metadata make_metadata() {
    return Metadata{
        {"category", std::string("finance")},
        {"year", std::int64_t{2026}},
        {"score", 0.75},
        {"note", std::monostate{}},
    };
}

TEST(FilterParse, ParsesASingleEqualityCondition) {
    const Filter filter = Filter::parse("category == \"finance\"");
    ASSERT_EQ(filter.size(), 1U);
    EXPECT_EQ(filter.conditions()[0].key, "category");
    EXPECT_EQ(filter.conditions()[0].op, FilterOp::kEqual);
    EXPECT_TRUE(filter.matches(make_metadata()));
}

TEST(FilterParse, ParsesEveryOperator) {
    const std::pair<std::string, FilterOp> cases[] = {
        {"a == 1", FilterOp::kEqual},
        {"a != 1", FilterOp::kNotEqual},
        {"a < 1", FilterOp::kLess},
        {"a <= 1", FilterOp::kLessEqual},
        {"a > 1", FilterOp::kGreater},
        {"a >= 1", FilterOp::kGreaterEqual},
        {"a = 1", FilterOp::kEqual},
    };
    for (const auto& [text, op] : cases) {
        const Filter filter = Filter::parse(text);
        ASSERT_EQ(filter.size(), 1U) << text;
        EXPECT_EQ(filter.conditions()[0].op, op) << text;
    }
}

TEST(FilterParse, CombinesConditionsWithAnd) {
    const Filter filter =
        Filter::parse("category == \"finance\" and year >= 2020 and score < 0.9");
    ASSERT_EQ(filter.size(), 3U);
    EXPECT_TRUE(filter.matches(make_metadata()));
}

TEST(FilterParse, IsCaseInsensitiveAboutTheAndKeyword) {
    EXPECT_EQ(Filter::parse("a == 1 AND b == 2").size(), 2U);
    EXPECT_EQ(Filter::parse("a == 1 And b == 2").size(), 2U);
}

TEST(FilterParse, ToleratesArbitraryWhitespace) {
    const Filter filter = Filter::parse("   year   >=   2020   and   score<1  ");
    EXPECT_EQ(filter.size(), 2U);
    EXPECT_TRUE(filter.matches(make_metadata()));
}

TEST(FilterParse, AnEmptyFilterMatchesEverything) {
    EXPECT_TRUE(Filter::parse("").empty());
    EXPECT_TRUE(Filter::parse("   ").matches(make_metadata()));
    EXPECT_TRUE(Filter{}.matches(Metadata{}));
}

// Quoting is how a caller says "this is text, not a number", and it is the
// difference between matching the string "2026" and the integer 2026.
TEST(FilterParse, QuotingForcesAValueToBeText) {
    const Filter quoted = Filter::parse("year == \"2026\"");
    const Filter bare = Filter::parse("year == 2026");

    EXPECT_TRUE(std::holds_alternative<std::string>(quoted.conditions()[0].value));
    EXPECT_TRUE(std::holds_alternative<std::int64_t>(bare.conditions()[0].value));

    const Metadata numeric{{"year", std::int64_t{2026}}};
    const Metadata textual{{"year", std::string("2026")}};
    EXPECT_TRUE(bare.matches(numeric));
    EXPECT_FALSE(quoted.matches(numeric)) << "text must not silently match a number";
    EXPECT_TRUE(quoted.matches(textual));
}

TEST(FilterParse, AcceptsSingleQuotesAndEscapes) {
    const Filter filter = Filter::parse("name == 'say \\'hi\\''");
    ASSERT_EQ(filter.size(), 1U);
    EXPECT_EQ(std::get<std::string>(filter.conditions()[0].value), "say 'hi'");
}

TEST(FilterParse, RecognisesNegativeAndFractionalNumbers) {
    const Filter filter = Filter::parse("a >= -3 and b < 0.125 and c == 1e3");
    EXPECT_EQ(std::get<std::int64_t>(filter.conditions()[0].value), -3);
    EXPECT_DOUBLE_EQ(std::get<double>(filter.conditions()[1].value), 0.125);
    EXPECT_DOUBLE_EQ(std::get<double>(filter.conditions()[2].value), 1000.0);
}

TEST(FilterParse, ReportsSyntaxErrorsWithAPosition) {
    for (const std::string_view bad :
         {"category ==", "== 5", "a ~ 5", "a == 1 or b == 2", "a == \"unterminated"}) {
        try {
            static_cast<void>(Filter::parse(bad));
            ADD_FAILURE() << "expected a parse failure for: " << bad;
        } catch (const InvalidArgumentError& error) {
            const std::string message = error.what();
            EXPECT_NE(message.find("position"), std::string::npos) << bad << " -> " << message;
        }
    }
}

TEST(FilterParse, PointsAtTheAbsenceOfOr) {
    try {
        static_cast<void>(Filter::parse("a == 1 or b == 2"));
        FAIL() << "expected a parse failure";
    } catch (const InvalidArgumentError& error) {
        EXPECT_NE(std::string(error.what()).find("or"), std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

TEST(FilterMatch, ComparesNumbersAcrossIntegerAndRealStorage) {
    const Metadata integer{{"n", std::int64_t{5}}};
    const Metadata real{{"n", 5.0}};

    for (const Metadata& metadata : {integer, real}) {
        EXPECT_TRUE(Filter::parse("n == 5").matches(metadata));
        EXPECT_TRUE(Filter::parse("n == 5.0").matches(metadata));
        EXPECT_TRUE(Filter::parse("n >= 5").matches(metadata));
        EXPECT_TRUE(Filter::parse("n < 6").matches(metadata));
        EXPECT_FALSE(Filter::parse("n > 5").matches(metadata));
    }
}

TEST(FilterMatch, ComparesTextLexicographically) {
    const Metadata metadata{{"name", std::string("banana")}};
    EXPECT_TRUE(Filter::parse("name > \"apple\"").matches(metadata));
    EXPECT_TRUE(Filter::parse("name < \"cherry\"").matches(metadata));
    EXPECT_TRUE(Filter::parse("name != \"apple\"").matches(metadata));
}

// The documented rule, and the one most likely to surprise: a missing key never
// matches, including for `!=`.
TEST(FilterMatch, AMissingKeyNeverMatchesIncludingNotEqual) {
    const Metadata metadata{{"present", std::int64_t{1}}};

    EXPECT_FALSE(Filter::parse("absent == 5").matches(metadata));
    EXPECT_FALSE(Filter::parse("absent != 5").matches(metadata))
        << "chosen so a filter's meaning does not depend on which keys exist";
    EXPECT_FALSE(Filter::parse("absent > 0").matches(metadata));
}

TEST(FilterMatch, IncomparableTypesNeverMatch) {
    const Metadata metadata{{"mixed", std::string("hello")}};
    EXPECT_FALSE(Filter::parse("mixed == 5").matches(metadata));
    EXPECT_FALSE(Filter::parse("mixed > 5").matches(metadata));
    EXPECT_FALSE(Filter::parse("mixed != 5").matches(metadata))
        << "a number and a string have no defensible ordering";
}

TEST(FilterMatch, RequiresEveryConditionToHold) {
    const Metadata metadata = make_metadata();
    EXPECT_TRUE(Filter::parse("category == \"finance\" and year == 2026").matches(metadata));
    EXPECT_FALSE(Filter::parse("category == \"finance\" and year == 1999").matches(metadata));
    EXPECT_FALSE(Filter::parse("category == \"tech\" and year == 2026").matches(metadata));
}

TEST(Filter, RoundTripsThroughItsStringForm) {
    for (const std::string_view text :
         {"category == \"finance\"", "year >= 2020 and score < 0.5", "a != \"b\""}) {
        const Filter original = Filter::parse(text);
        const Filter reparsed = Filter::parse(original.to_string());
        EXPECT_EQ(original.to_string(), reparsed.to_string()) << text;
    }
}

}  // namespace
}  // namespace vectordb
