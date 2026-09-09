# cmake/FlatBuffersCompile.cmake
# Helper function to compile .fbs schema files into C++ headers.
#
# Usage:
#   flatbuffers_generate_headers(
#       TARGET my_schemas
#       SCHEMAS schema1.fbs schema2.fbs
#       OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
#   )
#
# This creates a target `my_schemas` that generates C++ headers from the
# given .fbs files. Link against this target to get the include path.

function(flatbuffers_generate_headers)
    cmake_parse_arguments(
        FBS                          # prefix
        ""                           # options (booleans)
        "TARGET;OUTPUT_DIR"          # one-value keywords
        "SCHEMAS"                    # multi-value keywords
        ${ARGN}
    )

    if(NOT FBS_TARGET)
        message(FATAL_ERROR "flatbuffers_generate_headers: TARGET is required")
    endif()
    if(NOT FBS_SCHEMAS)
        message(FATAL_ERROR "flatbuffers_generate_headers: SCHEMAS is required")
    endif()
    if(NOT FBS_OUTPUT_DIR)
        set(FBS_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    endif()

    file(MAKE_DIRECTORY "${FBS_OUTPUT_DIR}")

    set(generated_headers "")

    foreach(schema_file ${FBS_SCHEMAS})
        get_filename_component(schema_name "${schema_file}" NAME_WE)
        get_filename_component(schema_abs "${schema_file}" ABSOLUTE)

        set(output_header "${FBS_OUTPUT_DIR}/${schema_name}_generated.h")

        add_custom_command(
            OUTPUT "${output_header}"
            COMMAND flatc --cpp --gen-mutable --gen-object-api
                    -o "${FBS_OUTPUT_DIR}"
                    "${schema_abs}"
            DEPENDS "${schema_abs}" flatc
            COMMENT "Compiling FlatBuffers schema: ${schema_file}"
            VERBATIM
        )

        list(APPEND generated_headers "${output_header}")
    endforeach()

    add_custom_target(${FBS_TARGET}_gen DEPENDS ${generated_headers})

    # Create an interface library so consumers can link against it
    add_library(${FBS_TARGET} INTERFACE)
    add_dependencies(${FBS_TARGET} ${FBS_TARGET}_gen)
    target_include_directories(${FBS_TARGET} INTERFACE "${FBS_OUTPUT_DIR}")
    # FlatBuffers headers need the flatbuffers include path
    target_link_libraries(${FBS_TARGET} INTERFACE flatbuffers)
endfunction()
