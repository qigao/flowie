if(NOT DEFINED FLOWIE_DOCKERFILE OR FLOWIE_DOCKERFILE STREQUAL "")
  message(FATAL_ERROR "FLOWIE_DOCKERFILE is required")
endif()
if(NOT EXISTS "${FLOWIE_DOCKERFILE}")
  message(FATAL_ERROR "Flowie Dockerfile does not exist: ${FLOWIE_DOCKERFILE}")
endif()

file(READ "${FLOWIE_DOCKERFILE}" _dockerfile)

if(NOT _dockerfile MATCHES "-DFLOWIE_BUILD_CLUSTER=OFF")
  message(FATAL_ERROR
          "Standalone Docker build must explicitly set FLOWIE_BUILD_CLUSTER=OFF")
endif()

if(NOT _dockerfile MATCHES "-DTURBO_ENABLE_CAPTURE=OFF")
  message(FATAL_ERROR
          "Standalone Docker build must explicitly disable TurboParser capture")
endif()

foreach(_cluster_reference IN ITEMS flow_mq turbo_raft flowmq turboraft FlowMQ TurboRaft)
  if(_dockerfile MATCHES "${_cluster_reference}")
    message(FATAL_ERROR
            "Standalone Dockerfile must not reference Cluster dependency: "
            "${_cluster_reference}")
  endif()
endforeach()
