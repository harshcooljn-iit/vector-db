# Sources are attached to the `vectordb` library from each subdirectory so that
# adding a file touches only the module it belongs to.
add_subdirectory(core)
add_subdirectory(db)
add_subdirectory(distance)
add_subdirectory(index)
add_subdirectory(persistence)
add_subdirectory(storage)
add_subdirectory(util)
