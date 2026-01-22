cmake_minimum_required(VERSION 3.10)
project(NetNexus C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O2")

# Find pthread library
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Find libxml2
find_package(LibXml2 REQUIRED)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/src)
include_directories(${LIBXML2_INCLUDE_DIR})

# Source files
set(nn_cli_SOURCES
        src/cli/nn_cli_handler.c
        src/cli/nn_cli_tree.c
        src/cli/nn_cli_xml_parser.c
        src/cli/nn_cli_element.c
        src/cli/nn_cli_view.c
)

set(MAIN_SOURCES
    src/main.c
)

# Create executable
add_executable(netnexus 
    ${MAIN_SOURCES}
    ${nn_cli_SOURCES}
)

# Link libraries
target_link_libraries(netnexus PRIVATE 
    Threads::Threads
    ${LIBXML2_LIBRARIES}
)

# Installation
install(TARGETS netnexus
    RUNTIME DESTINATION bin
)

# Install config files
install(FILES config/commands.xml
    DESTINATION etc/netnexus
)

# Custom target for running the server
add_custom_target(run
    COMMAND netnexus
    DEPENDS netnexus
    WORKING_DIRECTORY ${CMAKE_PROJECT_DIR}
    COMMENT "Running NetNexus Telnet CLI Server"
)
