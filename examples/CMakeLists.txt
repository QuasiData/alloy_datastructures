# One explicit registration per example; additional sources may be passed.
function(alloy_datastructures_example name)
    add_executable(alloy_datastructures_example_${name} ${ARGN})
    target_link_libraries(alloy_datastructures_example_${name} PRIVATE alloy::datastructures alloy_datastructures_compile_options)
    set_target_properties(alloy_datastructures_example_${name} PROPERTIES CXX_EXTENSIONS OFF CXX_SCAN_FOR_MODULES OFF)
endfunction()

alloy_datastructures_example(hello hello.cpp)
