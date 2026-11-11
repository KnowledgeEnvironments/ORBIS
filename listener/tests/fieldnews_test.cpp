#include "model/Activity.hpp"
#include "model/FieldNews.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

using ke::model::aboutThePrincipal;

TEST_CASE("a Principal hears about what it is, not its running figures") {
    CHECK(aboutThePrincipal("instructions/charter"));
    CHECK(aboutThePrincipal("memory/notes"));
    CHECK_FALSE(aboutThePrincipal("serialize/summary-tokens"));
    CHECK_FALSE(aboutThePrincipal("loop/reasoning"));
    CHECK_FALSE(aboutThePrincipal("principal/models"));
    CHECK_FALSE(aboutThePrincipal("cost/tokens-in"));
    CHECK_FALSE(aboutThePrincipal("loop/iteration"));
    CHECK_FALSE(aboutThePrincipal("loop/seen-seq"));
    CHECK_FALSE(aboutThePrincipal("principal/routes"));
    CHECK_FALSE(aboutThePrincipal("policy/unsafe"));
    CHECK_FALSE(aboutThePrincipal("notes/x/hint"));
}

TEST_CASE("the roll-up says what wants a person, and nothing when nothing does") {
    ke::model::ActivityBoard b;
    CHECK(ke::model::rollup(b).empty());
    b.done = 1;
    CHECK(ke::model::rollup(b) == "1 done");
    b.needYou = 2;
    b.failed = 1;
    CHECK(ke::model::rollup(b) == "2 need you \xc2\xb7 1 failed \xc2\xb7 1 done");
    b.needYou = 1;
    CHECK(ke::model::rollup(b).rfind("1 needs you", 0) == 0);
    CHECK(std::string(ke::model::word(ke::model::Activity::Finished)) == "finished, not seen");
}
