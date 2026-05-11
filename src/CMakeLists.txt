# Subprojects. bson/ is the vendored libbson 1.28.0; umongoc/ holds the
# small MongoDB-wire-protocol client we're writing fresh; logging/ is April's
# standard queue-based logger ported in from creature-controller.
add_subdirectory(logging)
add_subdirectory(bson)
add_subdirectory(umongoc)
add_subdirectory(app)
