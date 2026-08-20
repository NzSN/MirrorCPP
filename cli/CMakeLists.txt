# mirrorcpp-validate CLI (design §5.7).
add_executable(mirrorcpp-validate validate.cpp)
target_link_libraries(mirrorcpp-validate PRIVATE mirrorcpp::mirrorcpp)
# The CLI uses connect_tls only when the library was built with TLS; the
# notls build must not reference it (tls.cpp isn't compiled there).
target_compile_definitions(mirrorcpp-validate PRIVATE
  MIRRORCPP_WITH_TLS=$<BOOL:${MIRRORCPP_WITH_TLS}>)
