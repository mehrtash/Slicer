
#-----------------------------------------------------------------------------
set(MODULE_NAME DataProbe)

#-----------------------------------------------------------------------------
if(${VTK_VERSION_MAJOR} GREATER 5)
  add_subdirectory(Logic)
endif()

#-----------------------------------------------------------------------------
set(MODULE_PYTHON_SCRIPTS
  ${MODULE_NAME}.py
  ${MODULE_NAME}Lib/__init__
  ${MODULE_NAME}Lib/SliceViewAnnotations
  ${MODULE_NAME}Lib/DataProbeUtil
  )

set(MODULE_PYTHON_RESOURCES
  Resources/Icons/SlicerAdvancedGear-Small.png
  ${MODULE_NAME}Lib/Resources/UI/settings.ui
  ${MODULE_NAME}Lib/Resources/Models/human.stl
  ${MODULE_NAME}Lib/Resources/Models/rightShoe.stl
  ${MODULE_NAME}Lib/Resources/Models/leftShoe.stl
  ${MODULE_NAME}Lib/Resources/Models/shorts.stl
  )

#-----------------------------------------------------------------------------
slicerMacroBuildScriptedModule(
  NAME ${MODULE_NAME}
  SCRIPTS ${MODULE_PYTHON_SCRIPTS}
  RESOURCES ${MODULE_PYTHON_RESOURCES}
  WITH_GENERIC_TESTS
  )
