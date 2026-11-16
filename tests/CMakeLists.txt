# Unit and integration tests.  None of them needs a sound server or Plasma:
# the routing tests run against a fake backend, the network tests talk over
# the loopback interface.

function(soundroot_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE soundroot_core Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

soundroot_test(tst_audiogroup)
soundroot_test(tst_listdiff)
soundroot_test(tst_streambinder)
soundroot_test(tst_routingengine fakebackend.h)
soundroot_test(tst_netprotocol)
soundroot_test(tst_jitterbuffer)
soundroot_test(tst_netengine)
