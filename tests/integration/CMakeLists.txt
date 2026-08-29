target_sources(vectordb_tests PRIVATE
    cli_test.cpp
    database_test.cpp
)

# The CLI tests drive the real executable, so they need its path. A generator
# expression rather than a guessed path, because the output directory differs
# between generators and configurations.
target_compile_definitions(vectordb_tests PRIVATE
    VECTORDB_CLI_PATH="$<TARGET_FILE:vectordb_cli>"
)
add_dependencies(vectordb_tests vectordb_cli)
