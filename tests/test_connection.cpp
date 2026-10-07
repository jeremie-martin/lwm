#include "x11_test_harness.hpp"
#include "lwm/core/connection.hpp"
#include <limits>

using namespace lwm::test;

TEST_CASE("Property size guard includes padding at the server request limit", "[connection][integration]")
{
    auto& env = X11TestEnvironment::instance();
    if (!env.available())
        SKIP("X11 unavailable");

    lwm::Connection conn;
    auto words = xcb_get_maximum_request_length(conn.get());
    REQUIRE(words > 7);
    size_t max_bytes = (static_cast<size_t>(words) - 7) * 4;

    for (size_t padding = 0; padding < 4; ++padding)
    {
        INFO("bytes below limit: " << padding);
        CHECK(conn.fits_property(max_bytes - padding));
    }
    for (size_t excess = 1; excess <= 4; ++excess)
    {
        INFO("bytes above limit: " << excess);
        CHECK_FALSE(conn.fits_property(max_bytes + excess));
    }
    CHECK(conn.fits_property(0));
    CHECK_FALSE(conn.fits_property(std::numeric_limits<size_t>::max()));
}
