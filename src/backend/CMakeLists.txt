target_sources(flakewm PRIVATE
  backend.c
)

if(HAVE_FBDEV)
  add_subdirectory(fbdev)
endif()
