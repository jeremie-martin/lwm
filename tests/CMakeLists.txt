include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.0
)
FetchContent_MakeAvailable(Catch2)

add_executable(lwm_tests
    test_ewmh_classification.cpp
    test_ewmh_policy.cpp
    test_focus_cycling_policy.cpp
    test_focus_policy.cpp
    test_focus_restoration_policy.cpp
    test_focus_visibility_policy.cpp
    test_integration_focus.cpp
    test_floating_policy.cpp
    test_layout_policy.cpp
    test_workspace.cpp
    test_workspace_policy.cpp
)

target_link_libraries(lwm_tests PRIVATE liblwm Catch2::Catch2WithMain)
target_include_directories(lwm_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(Catch)
catch_discover_tests(lwm_tests)
