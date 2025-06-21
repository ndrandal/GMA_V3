# tests/nodes/CMakeLists.txt
add_executable(tests_nodes
    AggregateTest.cpp
    AtomicAccessorTest.cpp
    IntervalTest.cpp
    ListenerTest.cpp
    ResponderTest.cpp
    SymbolSplitTest.cpp
    WorkerTest.cpp
)

target_link_libraries(tests_nodes
    gma
    GTest::gtest_main
)
enable_testing()               # once per CMakeLists (can be in tests/CMakeLists.txt)
add_test(NAME tests_nodes COMMAND tests_nodes)
