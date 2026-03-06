add_executable(lwm main.cpp)
add_executable(lwmctl lwmctl.cpp)

target_link_libraries(lwm PRIVATE liblwm)
target_link_libraries(lwmctl PRIVATE liblwm)
