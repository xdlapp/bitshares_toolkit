file(GLOB HEADERS "include/bts/blockchain/*.hpp")

add_library( bts_blockchain
             time.cpp
             extended_address.cpp
             address.cpp
             pts_address.cpp
             asset.cpp
             types.cpp
             operations.cpp
             withdraw_types.cpp
             pending_chain_state.cpp
             transaction.cpp
             chain_interface.cpp
             block.cpp
             chain_database.cpp
             fire_operation.cpp
             account_record.cpp
             ${HEADERS}
           )

target_link_libraries( bts_blockchain fc bts_db leveldb )
target_include_directories( bts_blockchain 
                            PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )
