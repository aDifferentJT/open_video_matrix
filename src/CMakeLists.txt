
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "-Wall -Wextra -Wpedantic -Wconversion")
set(CMAKE_CXX_FLAGS_DEBUG "-g -fsanitize=address")
#TODO openmp
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")

set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)
find_package(Boost 1.78.0 COMPONENTS ALL)
include_directories(${Boost_INCLUDE_DIRS})

add_definitions(-DMAGICKCORE_QUANTUM_DEPTH=8)
add_definitions(-DMAGICKCORE_HDRI_ENABLE=0)
find_package(ImageMagick COMPONENTS Magick++ MagickCore)
include_directories(${ImageMagick_INCLUDE_DIRS})
message("magick libraries ${ImageMagick_LIBRARIES}")

include_directories(out_ptr/include)

if (CMAKE_SYSTEM_NAME MATCHES Darwin)
  set(PLATFORM Mac)
elseif (CMAKE_SYSTEM_NAME MATCHES Linux)
  set(PLATFORM Linux)
elseif (CMAKE_SYSTEM_NAME MATCHES Windows)
  set(PLATFORM Win)
endif()
include_directories(decklink/${PLATFORM}/include)

find_package(fmt)

add_executable(router router.cpp)
target_link_libraries(router ${Boost_LIBRARIES})
target_link_libraries(router fmt::fmt)

add_executable(presentation_source presentation_source.cpp)
target_link_libraries(presentation_source ${ImageMagick_LIBRARIES})
target_link_libraries(presentation_source fmt::fmt)

add_executable(colour_source colour_source.cpp)
target_link_libraries(colour_source fmt::fmt)

add_executable(ndi_output ndi_output.cpp)
target_link_libraries(ndi_output fmt::fmt)

add_executable(decklink_output decklink_output.cpp)
target_link_libraries(decklink_output fmt::fmt)
if (APPLE)
  find_library(FOUNDATION Foundation)
  if (NOT FOUNDATION)
    message(FATAL_ERROR "Foundation not found")
  endif()
  
  target_link_libraries(decklink_output ${FOUNDATION})
endif()
if (CMAKE_SYSTEM_NAME MATCHES Windows)
  add_idl(DeckLinkAPI decklink/Win/include/DeckLinkAPI.idl)
  target_link_libraries(decklink_output DeckLinkAPI)
endif()

add_subdirectory(web_source)

add_subdirectory(controller)

add_executable(test_open_rpc test_open_rpc.cpp)

