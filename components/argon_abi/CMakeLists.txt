# Header-only component exposing the public ABI to both the kernel and the
# application SDK.  There is exactly one copy of these headers, in sdk/include.
idf_component_register(
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../sdk/include")
