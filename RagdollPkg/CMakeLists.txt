set(RAGDOLLPKG_PATH ${CMAKE_CURRENT_SOURCE_DIR})
set(RAGDOLLPKG_CLASSES_PATH ${RAGDOLLPKG_PATH}/Classes)
set(RAGDOLLPKG_INC_PATH ${RAGDOLLPKG_PATH}/Inc)
set(RAGDOLLPKG_SRC_PATH ${RAGDOLLPKG_PATH}/Src)
set(RAGDOLLPKG_FILES_CLASSES
    ${RAGDOLLPKG_CLASSES_PATH}/RagdollMutator.uc
    ${RAGDOLLPKG_CLASSES_PATH}/RagdollSpawnNotify.uc
    ${RAGDOLLPKG_CLASSES_PATH}/RagdollCarcass.uc)
set(RAGDOLLPKG_FILES_INC
    ${RAGDOLLPKG_INC_PATH}/RagdollPkg.h
    ${RAGDOLLPKG_INC_PATH}/RagdollInternal.h
    ${RAGDOLLPKG_INC_PATH}/RagdollPkgClasses.h)
set(RAGDOLLPKG_FILES_SRC
    ${RAGDOLLPKG_SRC_PATH}/RagdollCarcass.cpp
    ${RAGDOLLPKG_SRC_PATH}/UnRagdollMesh.h
    ${RAGDOLLPKG_SRC_PATH}/UnRagdollMesh.cpp)
set(UNREALSCRIPT_CLASSES
    ${UNREALSCRIPT_CLASSES}
    ${RAGDOLLPKG_FILES_CLASSES}
    PARENT_SCOPE)
set(UNREALSCRIPT_HEADERS
    ${UNREALSCRIPT_HEADERS}
    ${RAGDOLLPKG_INC_PATH}/RagdollPkgClasses.h
    PARENT_SCOPE)
set(UNREALSCRIPT_MODULES
    ${UNREALSCRIPT_MODULES}
    ${SYSTEM_DIR}/RagdollPkg.u
    PARENT_SCOPE)
set_source_files_properties(${RAGDOLLPKG_INC_PATH}/RagdollPkgClasses.h
    PROPERTIES GENERATED TRUE)
add_library(RagdollPkg MODULE
    ${RAGDOLLPKG_FILES_CLASSES}
    ${RAGDOLLPKG_FILES_INC}
    ${RAGDOLLPKG_FILES_SRC})
source_group(Classes FILES ${RAGDOLLPKG_FILES_CLASSES})
source_group(Inc FILES ${RAGDOLLPKG_FILES_INC})
source_group(Src FILES ${RAGDOLLPKG_FILES_SRC})
target_compile_definitions(RagdollPkg
    PRIVATE
        RAGDOLLPKG_EXPORTS
        ThisPackage=RagdollPkg)
target_include_directories(RagdollPkg PUBLIC ${RAGDOLLPKG_INC_PATH})
target_link_libraries(RagdollPkg Core Engine box3d)
add_dependencies(RagdollPkg UnrealScript)
