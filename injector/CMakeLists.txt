add_executable(BlacklistInjector main.cpp)

target_compile_definitions(BlacklistInjector PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(BlacklistInjector PRIVATE
    psapi
)

set_target_properties(BlacklistInjector PROPERTIES
    OUTPUT_NAME "BlacklistInjector"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
