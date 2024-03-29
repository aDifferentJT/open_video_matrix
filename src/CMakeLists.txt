
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "-Wall -Wextra -Wpedantic -Wconversion")
#set(CMAKE_CXX_FLAGS "-Wall -Wextra -Wpedantic -Wconversion --stdlib=libstdc++")
set(CMAKE_CXX_FLAGS_DEBUG "-g -fsanitize=address")
#TODO openmp
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")

if (CMAKE_SYSTEM_NAME MATCHES Linux)
  find_library(DL dl)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)
find_package(Boost 1.71.0 COMPONENTS asio beast interprocess process)
include_directories(${Boost_INCLUDE_DIRS})

add_definitions(-DMAGICKCORE_QUANTUM_DEPTH=8)
add_definitions(-DMAGICKCORE_HDRI_ENABLE=0)
find_package(ImageMagick COMPONENTS Magick++ MagickCore)
include_directories(${ImageMagick_INCLUDE_DIRS})
message("magick libraries ${ImageMagick_LIBRARIES}")

find_package(PkgConfig REQUIRED)
pkg_search_module(Poppler REQUIRED poppler-cpp)
#pkg_search_module(VLC REQUIRED libvlc)
#pkg_search_module(VLCpp REQUIRED libvlcpp)
#pkg_search_module(avcpp REQUIRED libavcpp)
#pkg_search_module(rtaudio REQUIRED rtaudio)

find_package(PNG REQUIRED)
add_definitions(${PNG_DEFINITIONS})

include_directories(out_ptr/include)

if (CMAKE_SYSTEM_NAME MATCHES Darwin)
  set(PLATFORM Mac)
elseif (CMAKE_SYSTEM_NAME MATCHES Linux)
  set(PLATFORM Linux)
elseif (CMAKE_SYSTEM_NAME MATCHES Windows)
  set(PLATFORM Win)
endif()
include_directories(decklink/${PLATFORM}/include)

if (CMAKE_SYSTEM_NAME MATCHES Windows)
  # Use FindIDL
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../FindIDL/cmake")
  find_package(IDL REQUIRED)
endif()

find_package(fmt)

add_executable(router router.cpp)
if (CMAKE_SYSTEM_NAME MATCHES Linux)
  target_link_libraries(router rt)
endif()
target_link_libraries(router Threads::Threads)
target_link_libraries(router ${Boost_LIBRARIES})
target_link_libraries(router fmt::fmt)

add_executable(presentation_input presentation_input.cpp)
if (CMAKE_SYSTEM_NAME MATCHES Linux)
  target_link_libraries(presentation_input rt)
endif()
target_link_libraries(presentation_input Threads::Threads)
target_link_libraries(presentation_input ${ImageMagick_LIBRARIES})
target_link_libraries(presentation_input fmt::fmt)

add_executable(pdf_input pdf_input.cpp)
if (CMAKE_SYSTEM_NAME MATCHES Linux)
  target_link_libraries(pdf_input rt)
endif()
target_link_libraries(pdf_input Threads::Threads)
target_link_libraries(pdf_input fmt::fmt)
target_link_libraries(pdf_input ${Poppler_LIBRARIES})
target_include_directories(pdf_input PUBLIC ${Poppler_INCLUDE_DIRS})
target_compile_options(pdf_input PUBLIC ${Poppler_CFLAGS})
target_compile_options(pdf_input PUBLIC ${Poppler_CFLAGS_OTHER})
target_link_options(pdf_input PUBLIC ${Poppler_LDFLAGS})
target_link_options(pdf_input PUBLIC ${Poppler_LDFLAGS_OTHER})
target_link_libraries(pdf_input ${PNG_LIBRARIES})
target_include_directories(pdf_input PUBLIC ${PNG_INCLUDE_DIRS})

#add_executable(vlc_input vlc_input.cpp)
#if (CMAKE_SYSTEM_NAME MATCHES Linux)
#  target_link_libraries(vlc_input rt)
#endif()
#target_link_libraries(vlc_input Threads::Threads)
#target_link_libraries(vlc_input fmt::fmt)
#target_link_libraries(vlc_input ${VLC_LIBRARIES})
#target_include_directories(vlc_input PUBLIC ${VLC_INCLUDE_DIRS})
#target_compile_options(vlc_input PUBLIC ${VLC_CFLAGS_OTHER})
#target_link_libraries(vlc_input ${VLCpp_LIBRARIES})
#target_include_directories(vlc_input PUBLIC ${VLCpp_INCLUDE_DIRS})
#target_compile_options(vlc_input PUBLIC ${VLCpp_CFLAGS_OTHER})

#add_executable(media_input media_input.cpp)
#if (CMAKE_SYSTEM_NAME MATCHES Linux)
#  target_link_libraries(media_input rt)
#endif()
#target_link_libraries(media_input atomic)
#target_link_libraries(media_input Threads::Threads)
#target_link_libraries(media_input fmt::fmt)
#target_link_libraries(media_input ${Poppler_LIBRARIES})
#target_include_directories(media_input PUBLIC ${Poppler_INCLUDE_DIRS})
#target_compile_options(media_input PUBLIC ${Poppler_CFLAGS_OTHER})
#target_link_libraries(media_input ${PNG_LIBRARIES})
#target_include_directories(media_input PUBLIC ${PNG_INCLUDE_DIRS})
#target_link_directories(media_input PUBLIC ${avcpp_LIBRARY_DIRS})
#target_link_directories(media_input PUBLIC /usr/local/lib)
#target_link_libraries(media_input ${avcpp_LIBRARIES})
#target_include_directories(media_input PUBLIC ${avcpp_INCLUDE_DIRS})
#target_compile_options(media_input PUBLIC ${avcpp_CFLAGS_OTHER})
#target_link_directories(media_input PUBLIC ${rtaudio_LIBRARY_DIRS})
#target_link_libraries(media_input ${rtaudio_LIBRARIES})
#target_include_directories(media_input PUBLIC ${rtaudio_INCLUDE_DIRS})
#target_compile_options(media_input PUBLIC ${rtaudio_CFLAGS_OTHER})

add_executable(colour_input colour_input.cpp)
if (CMAKE_SYSTEM_NAME MATCHES Linux)
  target_link_libraries(colour_input rt)
endif()
target_link_libraries(colour_input Threads::Threads)
target_link_libraries(colour_input fmt::fmt)

add_executable(decklink_input decklink_input.cpp)
if (CMAKE_SYSTEM_NAME MATCHES Linux)
  target_link_libraries(decklink_input rt)
  target_link_libraries(decklink_input dl)
endif()
target_link_libraries(decklink_input Threads::Threads)
target_link_libraries(decklink_input fmt::fmt)
if (APPLE)
  find_library(FOUNDATION Foundation)
  if (NOT FOUNDATION)
    message(FATAL_ERROR "Foundation not found")
  endif()
  
  target_link_libraries(decklink_input ${FOUNDATION})
endif()
if (CMAKE_SYSTEM_NAME MATCHES Windows)
  add_idl(DeckLinkAPI decklink/Win/include/DeckLinkAPI.idl)
  target_link_libraries(decklink_input DeckLinkAPI)
endif()

add_executable(ndi_output ndi_output.cpp)
if (CMAKE_SYSTEM_NAME MATCHES Linux)
  target_link_libraries(ndi_output rt)
  target_link_libraries(ndi_output dl)
endif()
target_link_libraries(ndi_output Threads::Threads)
target_link_libraries(ndi_output fmt::fmt)

add_executable(decklink_output decklink_output.cpp)
if (CMAKE_SYSTEM_NAME MATCHES Linux)
  target_link_libraries(decklink_output rt)
  target_link_libraries(decklink_output dl)
endif()
target_link_libraries(decklink_output Threads::Threads)
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

add_subdirectory(web_source_cef)

