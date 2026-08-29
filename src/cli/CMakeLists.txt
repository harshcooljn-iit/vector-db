target_sources(vectordb_cli PRIVATE
    args.cpp
    commands.cpp
    main.cpp
    output.cpp
)
target_include_directories(vectordb_cli PRIVATE ${CMAKE_SOURCE_DIR}/src)
