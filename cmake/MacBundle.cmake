# pop_mac_bundle(<target>): make <target> a .app in build/, named by
# POP_RECOMP_APP_NAME, with Info.plist copied verbatim and the resources,
# core mods, texture pack and ad-hoc signature applied after the link.
function(pop_mac_bundle target)
  set_target_properties(${target} PROPERTIES
    MACOSX_BUNDLE ON
    OUTPUT_NAME ${POP_RECOMP_APP_NAME}
    RUNTIME_OUTPUT_DIRECTORY ${POP_ROOT}/build
    MACOSX_BUNDLE_INFO_PLIST ${POP_ROOT}/src/recomp/host/Info.plist)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/recomp/finish_bundle.py
            --bundle ${POP_ROOT}/build/${POP_RECOMP_APP_NAME}.app
            --name ${POP_RECOMP_APP_NAME} --cc ${CMAKE_C_COMPILER}
    WORKING_DIRECTORY ${POP_ROOT}
    COMMENT "Finishing ${POP_RECOMP_APP_NAME}.app"
    VERBATIM)
endfunction()
