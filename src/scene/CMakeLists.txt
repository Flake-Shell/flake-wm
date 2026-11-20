target_sources(flakewm PRIVATE
  animation.c
  box.c
  buffer.c
  commit_timing.c
  decoration.c
  fifo.c
  hyprland_surface.c
  output.c
  rect.c
  render.c
  scene.c
  surface.c
  subsurface.c
  thumbnail.c
  xdg_shell.c
)

add_subdirectory(shaders)
