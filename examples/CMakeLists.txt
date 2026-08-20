# HourClock example (design §10) — skeleton target; real flow lands with the client API.
add_executable(hourclock hourclock.cpp)
target_link_libraries(hourclock PRIVATE mirrorcpp::mirrorcpp)
