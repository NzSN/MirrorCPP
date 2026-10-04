# HourClock example (design §10) — skeleton target; real flow lands with the client API.
add_executable(hourclock hourclock.cpp)
target_link_libraries(hourclock PRIVATE mirrorcpp::mirrorcpp)

add_executable(scheduled_counter scheduled_counter.cpp)
target_link_libraries(scheduled_counter PRIVATE mirrorcpp::mirrorcpp)

if(MIRRORCPP_BUILD_TESTS)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME schedule_fixture_replay
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test/schedule/replay_check.py
      --binary $<TARGET_FILE:scheduled_counter>)
  set_tests_properties(schedule_fixture_replay PROPERTIES TIMEOUT 60)
endif()
