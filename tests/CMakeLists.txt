# Per-library unit tests live next to their library at
# src/<lib>/tests/ and are registered from those CMakeLists. This
# directory only holds tests whose scope is the whole project — today
# that means the example-corpus tolerance tests in examples/.

add_subdirectory(examples)
