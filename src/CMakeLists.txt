if(RUNTIME_HARDWARE_SELF_TEST)
    target_sources(${PROJECT_NAME}
        PRIVATE
            ${PROJECT_SOURCE_DIR}/platform/runtime/tests/hardware_self_test.cpp
    )
else()
    target_sources(${PROJECT_NAME}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    )
endif()

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        platform::hal
)
