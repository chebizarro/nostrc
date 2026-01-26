# Blueprint.cmake - CMake module for compiling Blueprint (.blp) files to GtkBuilder UI files
#
# This module provides functions to compile Blueprint files to XML UI files
# that can be used with GtkBuilder.
#
# Usage:
#   include(Blueprint)
#   blueprint_compile(
#     OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/ui
#     SOURCES file1.blp file2.blp ...
#     OUTPUT_FILES_VAR COMPILED_UI_FILES
#   )

find_program(BLUEPRINT_COMPILER blueprint-compiler)

if(NOT BLUEPRINT_COMPILER)
    message(WARNING "blueprint-compiler not found. Blueprint files will not be compiled.")
endif()

# Function to compile Blueprint files to UI XML
# Arguments:
#   OUTPUT_DIR - Directory where compiled .ui files will be placed
#   SOURCES - List of .blp source files
#   OUTPUT_FILES_VAR - Variable name to store list of output .ui files
function(blueprint_compile)
    cmake_parse_arguments(BP "" "OUTPUT_DIR;OUTPUT_FILES_VAR" "SOURCES" ${ARGN})
    
    if(NOT BLUEPRINT_COMPILER)
        message(WARNING "blueprint-compiler not found, skipping Blueprint compilation")
        return()
    endif()
    
    if(NOT BP_OUTPUT_DIR)
        set(BP_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
    endif()
    
    file(MAKE_DIRECTORY ${BP_OUTPUT_DIR})
    
    set(_output_files)
    
    foreach(_blp_file ${BP_SOURCES})
        # Get the filename without extension
        get_filename_component(_blp_name ${_blp_file} NAME_WE)
        get_filename_component(_blp_dir ${_blp_file} DIRECTORY)
        
        # Determine output path - preserve subdirectory structure
        if(_blp_dir)
            file(RELATIVE_PATH _rel_dir ${CMAKE_CURRENT_SOURCE_DIR} ${_blp_dir})
            set(_out_dir ${BP_OUTPUT_DIR}/${_rel_dir})
        else()
            set(_out_dir ${BP_OUTPUT_DIR})
        endif()
        
        file(MAKE_DIRECTORY ${_out_dir})
        
        set(_ui_file ${_out_dir}/${_blp_name}.ui)
        
        add_custom_command(
            OUTPUT ${_ui_file}
            COMMAND ${BLUEPRINT_COMPILER} compile
                    --output ${_ui_file}
                    ${_blp_file}
            DEPENDS ${_blp_file}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Compiling Blueprint: ${_blp_file}"
            VERBATIM
        )
        
        list(APPEND _output_files ${_ui_file})
    endforeach()
    
    if(BP_OUTPUT_FILES_VAR)
        set(${BP_OUTPUT_FILES_VAR} ${_output_files} PARENT_SCOPE)
    endif()
endfunction()

# Function to batch compile all Blueprint files in a directory
# Arguments:
#   SOURCE_DIR - Directory containing .blp files (searched recursively)
#   OUTPUT_DIR - Directory where compiled .ui files will be placed
#   OUTPUT_FILES_VAR - Variable name to store list of output .ui files
function(blueprint_compile_directory)
    cmake_parse_arguments(BP "" "SOURCE_DIR;OUTPUT_DIR;OUTPUT_FILES_VAR" "" ${ARGN})
    
    if(NOT BP_SOURCE_DIR)
        message(FATAL_ERROR "blueprint_compile_directory: SOURCE_DIR is required")
    endif()
    
    file(GLOB_RECURSE _blp_files "${BP_SOURCE_DIR}/*.blp")
    
    if(_blp_files)
        blueprint_compile(
            OUTPUT_DIR ${BP_OUTPUT_DIR}
            SOURCES ${_blp_files}
            OUTPUT_FILES_VAR _output_files
        )
        
        if(BP_OUTPUT_FILES_VAR)
            set(${BP_OUTPUT_FILES_VAR} ${_output_files} PARENT_SCOPE)
        endif()
    endif()
endfunction()
