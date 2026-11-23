cmake_add_test(
	test_flowie_cluster_topology
	SOURCES
	${PROJECT_SOURCE_DIR}/flowie/cluster/tests/test_flowie_cluster_topology.c
	LIBS
	flowie_cluster_topology
	Salts::TinyTest
	FOLDER
	"flowie/cluster/tests"
)
set_tests_properties(test_flowie_cluster_topology
	PROPERTIES LABELS "flowie;server/cluster;topology"
)

cmake_add_test(
	test_flowie_cluster_publish_event
	SOURCES
	${PROJECT_SOURCE_DIR}/flowie/cluster/tests/test_flowie_cluster_publish_event.c
	LIBS
	flowie_cluster_publish_event
	Salts::TinyTest
	FOLDER
	"flowie/cluster/tests"
)
set_tests_properties(test_flowie_cluster_publish_event
	PROPERTIES LABELS "flowie;server/cluster;publish;event"
)

cmake_add_test(
	test_flowie_cluster_peer_protocol
	SOURCES
	${PROJECT_SOURCE_DIR}/flowie/cluster/tests/test_flowie_cluster_peer_protocol.c
	LIBS
	flowie_cluster_peer
	Salts::TinyTest
	FOLDER
	"flowie/cluster/tests"
)
set_tests_properties(test_flowie_cluster_peer_protocol
	PROPERTIES LABELS "flowie;server/cluster;peer;flowie/protocol"
)

foreach(peer_test
	IN
	ITEMS
	registry
	connect_bind
	connection_lost
	mqtt_command
	publish_settle
	mqtt_reply
	edge_action
	takeover_close
)
	cmake_add_test(
		test_flowie_cluster_peer_${peer_test}
		SOURCES
		test_flowie_cluster_peer_${peer_test}.c
		LIBS
		flowie_cluster_peer
		Salts::TinyTest
		FOLDER
		"flowie/cluster/tests"
	)
	set_tests_properties(test_flowie_cluster_peer_${peer_test}
		PROPERTIES LABELS "flowie;server/cluster;peer;flowie/protocol"
	)
endforeach()
cmake_add_test(
	test_flowie_cluster_publish_stream
	SOURCES
	test_flowie_cluster_publish_stream.c
	LIBS
	flowie_cluster_publish_stream
	Salts::TinyTest
	INCLUDES
	${PROJECT_SOURCE_DIR}/flowie/cluster/src
	${PROJECT_SOURCE_DIR}/flowie/core/src
	${PROJECT_SOURCE_DIR}/flowie/core/src/cluster
	${FLOWIE_PRIVATE_INCLUDE_DIR}
	FOLDER
	"flowie/cluster/tests"
)
set_tests_properties(test_flowie_cluster_publish_stream
	PROPERTIES LABELS "flowie;server/cluster;turboraft;cnet;data-stream;publish"
)

cmake_add_test(
	test_flowie_cluster_publish_ingress
	SOURCES
	test_flowie_cluster_publish_ingress.c
	LIBS
	flowie_cluster_publish_stream
	Salts::TinyTest
	INCLUDES
	${PROJECT_SOURCE_DIR}/flowie/cluster/src
	${PROJECT_SOURCE_DIR}/flowie/core/src
	${PROJECT_SOURCE_DIR}/flowie/core/src/cluster
	${FLOWIE_PRIVATE_INCLUDE_DIR}
	FOLDER
	"flowie/cluster/tests"
)
set_tests_properties(test_flowie_cluster_publish_ingress
	PROPERTIES LABELS "flowie;server/cluster;turboraft;cnet;data-stream;publish;backpressure"
)

cmake_add_test(
	test_flowie_cluster_publish_egress
	SOURCES
	test_flowie_cluster_publish_egress.c
	LIBS
	flowie_cluster_publish_stream
	Salts::TinyTest
	INCLUDES
	${PROJECT_SOURCE_DIR}/flowie/cluster/src
	${PROJECT_SOURCE_DIR}/flowie/core/src
	${PROJECT_SOURCE_DIR}/flowie/core/src/cluster
	${FLOWIE_PRIVATE_INCLUDE_DIR}
	FOLDER
	"flowie/cluster/tests"
)
set_tests_properties(test_flowie_cluster_publish_egress
	PROPERTIES LABELS "flowie;server/cluster;turboraft;cnet;data-stream;publish;quorum"
)

cmake_add_test(
	test_flowie_cluster_publish_router
	SOURCES
	test_flowie_cluster_publish_router.c
	LIBS
	flowie_cluster_publish_stream
	Salts::TinyTest
	INCLUDES
	${PROJECT_SOURCE_DIR}/flowie/cluster/src
	${PROJECT_SOURCE_DIR}/flowie/core/src
	${PROJECT_SOURCE_DIR}/flowie/core/src/cluster
	${FLOWIE_PRIVATE_INCLUDE_DIR}
	FOLDER
	"flowie/cluster/tests"
)
set_tests_properties(test_flowie_cluster_publish_router
	PROPERTIES LABELS "flowie;server/cluster;turboraft;cnet;data-stream;publish;router"
)

cmake_add_test(
	test_flowie_cluster_raft_store
	SOURCES
	test_flowie_cluster_raft_store.c
	LIBS
	flowie_cluster_runtime
	Salts::TinyTest
	INCLUDES
	${PROJECT_SOURCE_DIR}/flowie/cluster/src
	${PROJECT_SOURCE_DIR}/flowie/core/src
	${PROJECT_SOURCE_DIR}/flowie/core/src/cluster
	${FLOWIE_PRIVATE_INCLUDE_DIR}
	FOLDER
	"flowie/cluster/tests"
)
set_tests_properties(test_flowie_cluster_raft_store
	PROPERTIES LABELS "flowie;flowie-release;server/cluster;turboraft;storage;recovery"
)

cmake_add_test(
	test_flowie_cluster_raft_runtime
	SOURCES
	test_flowie_cluster_raft_runtime.c
	LIBS
	flowie_cluster_runtime
	Salts::TinyTest
	INCLUDES
	${PROJECT_SOURCE_DIR}/flowie/cluster/src
	${PROJECT_SOURCE_DIR}/flowie/core/src
	${PROJECT_SOURCE_DIR}/flowie/core/src/cluster
	${FLOWIE_PRIVATE_INCLUDE_DIR}
	FOLDER
	"flowie/cluster/tests"
)
set_tests_properties(test_flowie_cluster_raft_runtime
	PROPERTIES LABELS "flowie;server/cluster;turboraft;cnet;runtime"
)

cmake_add_test(
	test_flowie_cluster_raft_generation
	SOURCES
	test_flowie_cluster_raft_generation.c
	LIBS
	flowie_cluster_raft_generation
	Salts::TinyTest
	INCLUDES
	${PROJECT_SOURCE_DIR}/flowie/cluster/src
	${PROJECT_SOURCE_DIR}/flowie/core/src
	${PROJECT_SOURCE_DIR}/flowie/core/src/cluster
	${FLOWIE_PRIVATE_INCLUDE_DIR}
	FOLDER
	"flowie/cluster/tests"
)
set_tests_properties(test_flowie_cluster_raft_generation
	PROPERTIES LABELS "flowie;server/cluster;turboraft;cnet;composition;runtime"
)

if(WIN32)
	get_filename_component(_flowie_compiler_runtime_dir
		"${CMAKE_C_COMPILER}"
		DIRECTORY
	)
	set_tests_properties(
		test_flowie_cluster_publish_stream
		test_flowie_cluster_publish_ingress
		test_flowie_cluster_publish_egress
		test_flowie_cluster_publish_router
		test_flowie_cluster_raft_runtime
		test_flowie_cluster_raft_generation
		PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::CNet>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:${_flowie_compiler_runtime_dir}"
	)
	unset(_flowie_compiler_runtime_dir)
endif()

cmake_add_test(
	test_flowie_cluster_peer_authority
	SOURCES
	test_flowie_cluster_peer_authority.c
	LIBS
	flowie_cluster_runtime
	Salts::TinyTest
	INCLUDES
	${PROJECT_SOURCE_DIR}/flowie/cluster/src
	${PROJECT_SOURCE_DIR}/flowie/core/src
	${PROJECT_SOURCE_DIR}/flowie/core/src/cluster
	${FLOWIE_PRIVATE_INCLUDE_DIR}
	FOLDER
	"flowie/cluster/tests"
)
if(WIN32)
	target_link_libraries(test_flowie_cluster_peer_authority PRIVATE ws2_32)
endif()
set_tests_properties(test_flowie_cluster_peer_authority
	PROPERTIES LABELS "flowie;flowie-release;server/cluster;membership;security-contract"
)

cmake_add_test(
	test_flowie_cluster_owner_directory
	SOURCES
	test_flowie_cluster_owner_directory.c
	LIBS
	flowie_cluster_runtime
	Salts::TinyTest
	INCLUDES
	${PROJECT_SOURCE_DIR}/flowie/cluster/src
	${PROJECT_SOURCE_DIR}/flowie/core/src
	${PROJECT_SOURCE_DIR}/flowie/core/src/cluster
	${FLOWIE_PRIVATE_INCLUDE_DIR}
	FOLDER
	"flowie/cluster/tests"
)
if(WIN32)
	target_link_libraries(test_flowie_cluster_owner_directory PRIVATE ws2_32)
endif()
set_tests_properties(test_flowie_cluster_owner_directory
	PROPERTIES LABELS "flowie;flowie-release;server/cluster;ownership;security-contract"
)

cmake_add_test(
	test_flowie_cluster_owner_projection
	SOURCES
	test_flowie_cluster_owner_projection.c
	LIBS
	flowie_cluster_runtime
	Salts::TinyTest
	INCLUDES
	${PROJECT_SOURCE_DIR}/flowie/cluster/src
	${PROJECT_SOURCE_DIR}/flowie/core/src
	${PROJECT_SOURCE_DIR}/flowie/core/src/cluster
	${FLOWIE_PRIVATE_INCLUDE_DIR}
	FOLDER
	"flowie/cluster/tests"
)
set_tests_properties(test_flowie_cluster_owner_projection
	PROPERTIES LABELS "flowie;server/cluster;turboraft;ownership;recovery"
)

cmake_add_test(
	test_flowie_cluster_state_machine
	SOURCES
	test_flowie_cluster_state_machine.c
	LIBS
	flowie_cluster_runtime
	Salts::TinyTest
	INCLUDES
	${PROJECT_SOURCE_DIR}/flowie/cluster/src
	${PROJECT_SOURCE_DIR}/flowie/core/src
	${PROJECT_SOURCE_DIR}/flowie/core/src/cluster
	${FLOWIE_PRIVATE_INCLUDE_DIR}
	FOLDER
	"flowie/cluster/tests"
)
set_tests_properties(test_flowie_cluster_state_machine
	PROPERTIES LABELS "flowie;flowie-release;server/cluster;turboraft;ownership;publish;recovery"
)
