cmake_minimum_required(VERSION 3.21)
project(bee_sim C)
set(CMAKE_C_STANDARD 11)

# vcpkg toolchain gets passed on the command line; use CONFIG find
find_package(SDL2 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)

add_executable(bee_sim
  src/main.c
)

target_link_libraries(bee_sim PRIVATE
  glad::glad
  $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
  $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
)

if (MSVC)
  target_link_libraries(bee_sim PRIVATE opengl32)
  target_compile_options(bee_sim PRIVATE /W4 /permissive-)
else()
  target_compile_options(bee_sim PRIVATE -O3 -march=native -Wall -Wextra -Wpedantic)
endif()
