# CMake4GDAL project is distributed under MIT license. See accompanying file LICENSE.txt.

add_gdal_driver(
  TARGET gdal_AIGrid
  SOURCES aigrid.h aigccitt.c aigdataset.cpp aigopen.c gridlib.c
  BUILTIN
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)

gdal_standard_includes(gdal_AIGrid)
target_include_directories(gdal_AIGrid PRIVATE $<TARGET_PROPERTY:ogr_AVC,SOURCE_DIR>
                                               $<TARGET_PROPERTY:ogr_Shape,SOURCE_DIR>)


add_executable(aitest EXCLUDE_FROM_ALL aitest.c ../../ogr/ogrpgeogeometry.cpp)
target_include_directories(aitest PRIVATE $<TARGET_PROPERTY:gdal_AIGrid,SOURCE_DIR>)
target_link_libraries(aitest PRIVATE $<TARGET_NAME:${GDAL_LIB_TARGET_NAME}> gdal_AIGrid ogr_AVC ogr_Shape
                                   $<TARGET_OBJECTS:shapelib>)
