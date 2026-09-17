cmake_minimum_required(VERSION 3.16)

# The repository is an ESP-IDF project when IDF_PATH is available. Keeping a
# native host build alongside it is intentional: protocol, timing, parsing,
# storage, simulation, FIT, and retry contracts can be tested without a board
# or an attached trainer.
if(DEFINED ENV{IDF_PATH} AND EXISTS "$ENV{IDF_PATH}/tools/cmake/project.cmake")
  include($ENV{IDF_PATH}/tools/cmake/project.cmake)
  project(trainer_loop)
else()
  project(trainer_loop LANGUAGES C CXX)
  enable_testing()
  add_subdirectory(host)
endif()
