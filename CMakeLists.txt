cmake_minimum_required(VERSION 3.26)
project(usdBlenderRig VERSION 0.1.0 LANGUAGES CXX)

# Consume the sibling's installed package; never compile a private rig engine.
if(NOT rigExec_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../usdRig/build/install/lib/cmake/rigExec")
    set(rigExec_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../usdRig/build/install/lib/cmake/rigExec")
endif()
find_package(rigExec REQUIRED CONFIG)
get_filename_component(_rigexec_prefix "${rigExec_LIBRARY_DIR}/.." ABSOLUTE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rigexec-prefix" "${_rigexec_prefix}\n")
include(CTest)
find_program(USDBLENDERRIG_BLENDER NAMES blender Blender
    DOC "Blender executable for reading .blend files and integration tests")
if(NOT USDBLENDERRIG_BLENDER)
    set(USDBLENDERRIG_BLENDER "blender")
endif()
configure_file(config.h.in config.h @ONLY)
add_library(usdBlenderRig SHARED fileFormat.cpp blenderReader.cpp translator.cpp)
target_compile_features(usdBlenderRig PRIVATE cxx_std_17)
target_include_directories(usdBlenderRig PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(usdBlenderRig PRIVATE rigExec::rigExecRigging rigExec::rigExec usdGeom usdSkel usdShade ar plug js)
set_target_properties(usdBlenderRig PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
if(MSVC)
    target_compile_definitions(usdBlenderRig PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(usdBlenderRig PRIVATE /bigobj /Zc:__cplusplus /permissive-)
endif()
set(_resources "${CMAKE_CURRENT_BINARY_DIR}/usd/usdBlenderRig/resources")
file(MAKE_DIRECTORY "${_resources}")
# Incremental builds must not retain the former runtime schema resource.
file(REMOVE "${_resources}/generatedSchema.usda")
configure_file(blender/surface_bind.py "${_resources}/surface_bind.py" COPYONLY)
configure_file(blender/extract.py "${_resources}/extract.py" COPYONLY)
configure_file(blender/materials.py "${_resources}/materials.py" COPYONLY)
configure_file(blender/picker.py "${_resources}/picker.py" COPYONLY)
configure_file(resources/biped_picker.usda "${_resources}/biped_picker.usda" COPYONLY)
set(_types "\"UsdBlenderRigFileFormat\":{\"bases\":[\"SdfFileFormat\"],\"displayName\":\"Blender Rig\",\"extensions\":[\"blend\",\"blendrig\"],\"formatId\":\"blenderRig\",\"primary\":true,\"target\":\"usd\",\"supportsReading\":true,\"supportsWriting\":false,\"supportsEditing\":false}")
set(_info "\"Types\":{${_types}}")
file(GENERATE OUTPUT "${_resources}/plugInfo.json" CONTENT
    "{\"Plugins\":[{\"Type\":\"library\",\"Name\":\"usdBlenderRig\",\"Root\":\"..\",\"LibraryPath\":\"$<TARGET_FILE:usdBlenderRig>\",\"ResourcePath\":\"resources\",\"Info\":{${_info}}}]}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/install/plugInfo.json" CONTENT
    "{\"Plugins\":[{\"Type\":\"library\",\"Name\":\"usdBlenderRig\",\"Root\":\"..\",\"LibraryPath\":\"../../$<TARGET_FILE_NAME:usdBlenderRig>\",\"ResourcePath\":\"resources\",\"Info\":{${_info}}}]}")
install(TARGETS usdBlenderRig LIBRARY DESTINATION lib RUNTIME DESTINATION lib ARCHIVE DESTINATION lib)
install(CODE [[file(REMOVE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/usd/usdBlenderRig/resources/generatedSchema.usda")]])
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/install/plugInfo.json" blender/extract.py blender/materials.py blender/surface_bind.py blender/picker.py resources/biped_picker.usda
    DESTINATION lib/usd/usdBlenderRig/resources)
if(APPLE)
    set_target_properties(usdBlenderRig PROPERTIES INSTALL_RPATH "@loader_path;${rigExec_LIBRARY_DIR}")
elseif(UNIX)
    set_target_properties(usdBlenderRig PROPERTIES INSTALL_RPATH "$ORIGIN;${rigExec_LIBRARY_DIR}")
endif()
if(BUILD_TESTING)
    find_program(USDBLENDERRIG_PYTHON3 NAMES python3)
    if(USDBLENDERRIG_PYTHON3)
        add_test(NAME pickerInventory COMMAND "${USDBLENDERRIG_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/testPickerInventory.py")
        if(UNIX)
            add_test(NAME launcherFileFormatOnly COMMAND "${USDBLENDERRIG_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/testLauncherEnvironment.py")
        endif()
    endif()
    # Read the immutable generation usdview/Hydra consumes, without creating a
    # second evaluator. This inspection module is only built on request.
    add_library(usdBlenderRigViewSnapshot MODULE EXCLUDE_FROM_ALL tests/viewSnapshot.cpp)
    target_compile_features(usdBlenderRigViewSnapshot PRIVATE cxx_std_17)
    target_link_libraries(usdBlenderRigViewSnapshot PRIVATE rigExec::rigExecImaging)
    set_target_properties(usdBlenderRigViewSnapshot PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    add_executable(testUsdBlenderRig tests/testFileFormat.cpp)
    target_compile_features(testUsdBlenderRig PRIVATE cxx_std_17)
    target_link_libraries(testUsdBlenderRig PRIVATE usd usdGeom usdShade plug js)
    add_dependencies(testUsdBlenderRig usdBlenderRig)
    add_test(NAME fileFormat COMMAND testUsdBlenderRig "${_resources}"
        "${rigExec_PLUGIN_DIR}/rigExecSchema/resources" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_executable(testUsdBlenderRigEvaluation tests/testEvaluation.cpp)
    target_compile_features(testUsdBlenderRigEvaluation PRIVATE cxx_std_17)
    target_link_libraries(testUsdBlenderRigEvaluation PRIVATE rigExec::rigExec js)
    add_dependencies(testUsdBlenderRigEvaluation usdBlenderRig)
    add_test(NAME evaluation COMMAND testUsdBlenderRigEvaluation "${_resources}"
        "${rigExec_PLUGIN_DIR}/rigExecSchema/resources" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rig.blendrig")
    option(USDBLENDERRIG_TEST_BLENDER "Run real .blend extraction and numerical Blender comparison" ON)
    if(USDBLENDERRIG_TEST_BLENDER)
        add_executable(testUsdBlenderRead tests/testBlenderRead.cpp)
        target_compile_features(testUsdBlenderRead PRIVATE cxx_std_17)
        target_link_libraries(testUsdBlenderRead PRIVATE usd plug)
        add_dependencies(testUsdBlenderRead usdBlenderRig)
        add_test(NAME blenderMaterials COMMAND "${USDBLENDERRIG_BLENDER}" --background --factory-startup
            --disable-autoexec --python-exit-code 1 --python "${CMAKE_CURRENT_SOURCE_DIR}/tests/testMaterials.py"
            -- "${CMAKE_CURRENT_BINARY_DIR}/material-proof")
        set_tests_properties(blenderMaterials PROPERTIES TIMEOUT 90)
        add_test(NAME blenderFixture COMMAND "${USDBLENDERRIG_BLENDER}" --background --factory-startup
            --disable-autoexec --python-exit-code 1 --python "${CMAKE_CURRENT_SOURCE_DIR}/tests/makeFixture.py"
            -- "${CMAKE_CURRENT_BINARY_DIR}/blender-proof")
        set_tests_properties(blenderFixture PROPERTIES FIXTURES_SETUP blenderFiles TIMEOUT 90)
        add_test(NAME blenderRead COMMAND testUsdBlenderRead "${_resources}"
            "${rigExec_PLUGIN_DIR}/rigExecSchema/resources" "${CMAKE_CURRENT_BINARY_DIR}/blender-proof")
        set_tests_properties(blenderRead PROPERTIES FIXTURES_REQUIRED blenderFiles TIMEOUT 180
            ENVIRONMENT "USDBLENDERRIG_BLENDER=${USDBLENDERRIG_BLENDER}")
        add_test(NAME blenderParity COMMAND testUsdBlenderRigEvaluation "${_resources}"
            "${rigExec_PLUGIN_DIR}/rigExecSchema/resources" "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/rig.blend"
            "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/reference.json")
        set_tests_properties(blenderParity PROPERTIES FIXTURES_REQUIRED blenderFiles TIMEOUT 180
            FIXTURES_SETUP nativeUsd ENVIRONMENT "USDBLENDERRIG_BLENDER=${USDBLENDERRIG_BLENDER}")
        add_test(NAME snapshotParity COMMAND testUsdBlenderRigEvaluation "${_resources}"
            "${rigExec_PLUGIN_DIR}/rigExecSchema/resources" "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/rig.blendrig"
            "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/reference.json")
        set_tests_properties(snapshotParity PROPERTIES FIXTURES_REQUIRED blenderFiles
            ENVIRONMENT "USDBLENDERRIG_BLENDER=/blender-must-not-be-launched" RUN_SERIAL TRUE)
        add_test(NAME incrementalPoseParity COMMAND testUsdBlenderRigEvaluation "${_resources}"
            "${rigExec_PLUGIN_DIR}/rigExecSchema/resources" "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/rig.blendrig"
            "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/reference.json")
        set_tests_properties(incrementalPoseParity PROPERTIES FIXTURES_REQUIRED blenderFiles
            ENVIRONMENT "USDBLENDERRIG_BLENDER=/blender-must-not-be-launched;USDBLENDERRIG_INCREMENTAL_POSES=1" RUN_SERIAL TRUE)
        add_test(NAME nativeUsdParity COMMAND testUsdBlenderRigEvaluation "${_resources}"
            "${rigExec_PLUGIN_DIR}/rigExecSchema/resources" "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/converted.usda"
            "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/reference.json")
        set_tests_properties(nativeUsdParity PROPERTIES FIXTURES_REQUIRED nativeUsd
            ENVIRONMENT "USDBLENDERRIG_BLENDER=/blender-must-not-be-launched" RUN_SERIAL TRUE)
        add_test(NAME dualQuaternionParity COMMAND testUsdBlenderRigEvaluation "${_resources}"
            "${rigExec_PLUGIN_DIR}/rigExecSchema/resources" "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/dual.blend"
            "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/dual-reference.json")
        set_tests_properties(dualQuaternionParity PROPERTIES FIXTURES_REQUIRED blenderFiles FIXTURES_SETUP dualNativeUsd
            TIMEOUT 180 ENVIRONMENT "USDBLENDERRIG_BLENDER=${USDBLENDERRIG_BLENDER}")
        add_test(NAME nativeDualQuaternionParity COMMAND testUsdBlenderRigEvaluation "${_resources}"
            "${rigExec_PLUGIN_DIR}/rigExecSchema/resources" "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/dual-reference-converted.usda"
            "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/dual-reference.json")
        set_tests_properties(nativeDualQuaternionParity PROPERTIES FIXTURES_REQUIRED dualNativeUsd
            ENVIRONMENT "USDBLENDERRIG_BLENDER=/blender-must-not-be-launched" RUN_SERIAL TRUE)
        # Reload exported native USD in a distinct process whose registry has
        # only core schemas; the converter library must not be present.
        add_test(NAME coreOnlyNativeUsdParity COMMAND testUsdBlenderRigEvaluation "-no-converter-plugin"
            "${rigExec_PLUGIN_DIR}/rigExecSchema/resources" "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/converted.usda"
            "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/reference.json")
        set_tests_properties(coreOnlyNativeUsdParity PROPERTIES FIXTURES_REQUIRED nativeUsd RUN_SERIAL TRUE
            ENVIRONMENT "PXR_PLUGINPATH_NAME=${rigExec_PLUGIN_DIR}/rigExecSchema/resources;USDBLENDERRIG_REQUIRE_CORE_ONLY=1;USDBLENDERRIG_BLENDER=/blender-must-not-be-launched")
        add_test(NAME coreOnlyDualQuaternionParity COMMAND testUsdBlenderRigEvaluation "-no-converter-plugin"
            "${rigExec_PLUGIN_DIR}/rigExecSchema/resources" "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/dual-reference-converted.usda"
            "${CMAKE_CURRENT_BINARY_DIR}/blender-proof/dual-reference.json")
        set_tests_properties(coreOnlyDualQuaternionParity PROPERTIES FIXTURES_REQUIRED dualNativeUsd RUN_SERIAL TRUE
            ENVIRONMENT "PXR_PLUGINPATH_NAME=${rigExec_PLUGIN_DIR}/rigExecSchema/resources;USDBLENDERRIG_REQUIRE_CORE_ONLY=1;USDBLENDERRIG_BLENDER=/blender-must-not-be-launched")
    endif()
endif()
