#include "x11_test_harness.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace lwm::test;

TEST_CASE("Test command capture drains stderr while stdout remains open", "[harness]")
{
    auto result = run_command("/bin/sh", { "-c", "head -c 131072 /dev/zero >&2; printf done" });
    REQUIRE(result);
    CHECK(result->exit_code == 0);
    CHECK(result->stdout_text == "done");
    CHECK(result->stderr_text.size() == 131072);
}

TEST_CASE("Test processes distinguish a dead child from a running WM", "[harness]")
{
    LwmProcess process("", {}, { "--definitely-invalid-option" });
    REQUIRE(wait_for_condition([&] { return !process.running(); }, std::chrono::seconds(2)));
    CHECK_FALSE(process.diagnostics().empty());
}
