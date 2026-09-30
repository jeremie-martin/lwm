#include "ipc_subscription.hpp"
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
    LwmProcess process("", { }, { "--definitely-invalid-option" });
    REQUIRE(wait_for_condition([&] { return !process.running(); }, std::chrono::seconds(2)));
    CHECK_FALSE(process.diagnostics().empty());
}

TEST_CASE("Test line reader retains batched events and incomplete records", "[harness]")
{
    int pipe[2];
    REQUIRE(pipe2(pipe, O_CLOEXEC) == 0);
    TestFd reader_fd{ pipe[0] }, writer_fd{ pipe[1] };
    LineReader reader;
    REQUIRE(write(writer_fd.fd, "one\ntwo\npar", 11) == 11);
    CHECK(reader.read(reader_fd.fd, std::chrono::milliseconds(20)) == "one");
    CHECK(reader.read(reader_fd.fd, std::chrono::milliseconds(20)) == "two");
    CHECK_FALSE(reader.read(reader_fd.fd, std::chrono::milliseconds(20)));
    REQUIRE(write(writer_fd.fd, "tial\ntruncated", 14) == 14);
    writer_fd.reset();
    CHECK(reader.read(reader_fd.fd, std::chrono::milliseconds(20)) == "partial");
    CHECK_FALSE(reader.read(reader_fd.fd, std::chrono::milliseconds(20)));
}
