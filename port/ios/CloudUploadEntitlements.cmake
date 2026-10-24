# Shared generation lets policy probes inspect the exact plist the app signs with.
function(halo_cloud_upload_entitlements output base_file)
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/VISR-cloud.entitlements" cloud_entitlements)
    if(base_file)
        file(READ "${base_file}" base_entitlements)
        string(REGEX REPLACE "^.*<dict>(.*)</dict>.*$" "\\1" base_keys "${base_entitlements}")
        string(REPLACE "<dict>" "<dict>${base_keys}" cloud_entitlements "${cloud_entitlements}")
    endif()
    # App Store Connect requires a push environment for CloudKit on tvOS, including saves-only builds; a Catalyst
    # test host with saves alone signs without one, as before. The native Mac app's team-signed entitlements
    # replace this file (CMakeLists.txt)
    if(HALO_BENCHMARK_UPLOAD OR HALO_SHADER_UPLOAD OR (HALO_CLOUD_SAVES AND NOT HALO_MAC))
        if(HALO_NATIVE_MAC)
            set(push_key "com.apple.developer.aps-environment")
        else()
            set(push_key "aps-environment")
        endif()
        string(REPLACE "<dict>" "<dict>\n\t<key>${push_key}</key>\n\t<string>${HALO_BENCHMARK_APS_ENVIRONMENT}</string>"
            cloud_entitlements "${cloud_entitlements}")
    endif()
    file(WRITE "${output}" "${cloud_entitlements}")
endfunction()
