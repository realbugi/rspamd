SET(LIBKANNSRC	kautodiff.c kann.c)

IF(ENABLE_FULL_DEBUG MATCHES "OFF")
    if ("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
        SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
    endif ()
ENDIF()

ADD_LIBRARY(rspamd-kann SHARED ${LIBKANNSRC})

ProcessPackage(BLAS OPTIONAL LIBRARY openblas blas
        INCLUDE cblas.h INCLUDE_SUFFIXES include/openblas
        include/blas
        ROOT ${BLAS_ROOT_DIR}
        LIB_OUTPUT BLAS_REQUIRED_LIBRARIES)
IF(WITH_BLAS)
    MESSAGE(STATUS "Use openblas to accelerate kann")
    TARGET_LINK_LIBRARIES(rspamd-kann ${BLAS_REQUIRED_LIBRARIES})
ENDIF(WITH_BLAS)

INSTALL(TARGETS rspamd-kann LIBRARY DESTINATION ${RSPAMD_LIBDIR})