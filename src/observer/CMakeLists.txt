MESSAGE(STATUS "This is CMAKE_CURRENT_SOURCE_DIR dir " ${CMAKE_CURRENT_SOURCE_DIR})

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR})

find_package(FLEX)
find_package(BISON)
BISON_TARGET(parser_bioson ${CMAKE_CURRENT_SOURCE_DIR}/sql/parser/yacc_sql.y ${CMAKE_CURRENT_SOURCE_DIR}/sql/parser/yacc_sql.cpp)
FLEX_TARGET(parser_flex ${CMAKE_CURRENT_SOURCE_DIR}/sql/parser/lex_sql.l ${CMAKE_CURRENT_SOURCE_DIR}/sql/parser/lex_sql.cpp DEFINES_FILE ${CMAKE_CURRENT_SOURCE_DIR}/sql/parser/lex_sql.h)
ADD_FLEX_BISON_DEPENDENCY(parser_flex parser_bioson)

FILE(GLOB_RECURSE ALL_SRC *.cpp *.c)
SET(MAIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
MESSAGE("MAIN SRC: " ${MAIN_SRC})
FOREACH (F ${ALL_SRC})

    IF (NOT ${F} STREQUAL ${MAIN_SRC})
        SET(LIB_SRC ${LIB_SRC} ${F})
        MESSAGE("Use " ${F})
    ENDIF()

ENDFOREACH (F)
SET(LIB_SRC ${LIB_SRC} ${BISON_parser_bioson_OUTPUTS} ${FLEX_parser_flex_OUTPUTS})

find_library(LIBEVENT_CORE_LIBRARY NAMES event_core libevent_core
    PATHS /usr/local/lib /usr/lib/x86_64-linux-gnu
    REQUIRED)
find_library(LIBEVENT_PTHREADS_LIBRARY NAMES event_pthreads libevent_pthreads
    PATHS /usr/local/lib /usr/lib/x86_64-linux-gnu
    REQUIRED)
find_library(JSONCPP_LIBRARY NAMES jsoncpp libjsoncpp
    PATHS /usr/local/lib /usr/lib/x86_64-linux-gnu
    REQUIRED)

if (NOT TARGET libevent::core)
    add_library(libevent::core UNKNOWN IMPORTED)
    set_target_properties(libevent::core PROPERTIES IMPORTED_LOCATION "${LIBEVENT_CORE_LIBRARY}")
endif ()

if (NOT TARGET libevent::pthreads)
    add_library(libevent::pthreads UNKNOWN IMPORTED)
    set_target_properties(libevent::pthreads PROPERTIES IMPORTED_LOCATION "${LIBEVENT_PTHREADS_LIBRARY}")
endif ()

# JsonCpp cannot work correctly with FIND_PACKAGE

SET(LIBRARIES common pthread dl libevent::core libevent::pthreads ${JSONCPP_LIBRARY})

# 指定目标文件位置
SET(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
MESSAGE("Binary directory:" ${EXECUTABLE_OUTPUT_PATH})
SET(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)
MESSAGE("Archive directory:" ${LIBRARY_OUTPUT_PATH})

ADD_EXECUTABLE(observer ${MAIN_SRC})
TARGET_LINK_LIBRARIES(observer observer_static)

ADD_LIBRARY(observer_static STATIC ${LIB_SRC})
INCLUDE (readline)
MINIOB_FIND_READLINE()
IF (HAVE_READLINE)
    TARGET_LINK_LIBRARIES(observer_static ${READLINE_LIBRARY})
    TARGET_INCLUDE_DIRECTORIES(observer_static PRIVATE ${READLINE_INCLUDE_DIR})
    ADD_DEFINITIONS(-DUSE_READLINE)
    MESSAGE ("observer_static use readline")
ELSE ()
    MESSAGE ("readline is not found")
ENDIF()

SET_TARGET_PROPERTIES(observer_static PROPERTIES OUTPUT_NAME observer)
TARGET_LINK_LIBRARIES(observer_static ${LIBRARIES})

# Target 必须在定义 ADD_EXECUTABLE 之后， programs 不受这个限制
# TARGETS和PROGRAMS 的默认权限是OWNER_EXECUTE, GROUP_EXECUTE, 和WORLD_EXECUTE，即755权限， programs 都是处理脚本类
# 类型分为RUNTIME／LIBRARY／ARCHIVE, prog
INSTALL(TARGETS observer observer_static 
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib)
