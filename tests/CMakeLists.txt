# Download and configure GTest
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(threadpool_tests test_threadpool.cpp)
target_link_libraries(threadpool_tests PRIVATE ThreadPool GTest::gtest_main)

add_executable(spsc_queue_tests test_spsc_queue.cpp)
target_link_libraries(spsc_queue_tests PRIVATE ThreadPool GTest::gtest_main)

add_executable(spmc_queue_tests test_spmc_queue.cpp)
target_link_libraries(spmc_queue_tests PRIVATE ThreadPool GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(threadpool_tests)
gtest_discover_tests(spsc_queue_tests)
gtest_discover_tests(spmc_queue_tests)
