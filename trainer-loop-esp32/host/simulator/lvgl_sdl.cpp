#include "lvgl_sdl.h"

#include "lvgl_presenter.h"

#include <SDL.h>

#include <algorithm>
#include <cstdint>

namespace trainer_loop {

int run_lvgl_sdl(LvglPresenter& presenter,
                 const std::function<void(std::uint32_t)>& tick,
                 const std::function<void(bool)>& boot_button, int scale,
                 unsigned frame_limit) {
  scale = std::clamp(scale, 1, 4);
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) return 1;
  SDL_Window* window = SDL_CreateWindow("TrainerLoop", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, 368 * scale, 448 * scale,
                                        SDL_WINDOW_RESIZABLE);
  if (!window) {
    SDL_Quit();
    return 1;
  }
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  SDL_Texture* texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                                       SDL_TEXTUREACCESS_STREAMING, 368, 448)
                                  : nullptr;
  if (!renderer || !texture) {
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  bool running = true;
  unsigned frames = 0;
  bool pressed = false;
  std::uint32_t previous = SDL_GetTicks();
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = false;
      if (event.type == SDL_KEYDOWN && !event.key.repeat && event.key.keysym.sym == SDLK_b && boot_button)
        boot_button(true);
      if (event.type == SDL_KEYUP && event.key.keysym.sym == SDLK_b && boot_button)
        boot_button(false);
      if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) pressed = true;
      if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) pressed = false;
      if (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN ||
          event.type == SDL_MOUSEBUTTONUP) {
        int width = 368;
        int height = 448;
        SDL_GetWindowSize(window, &width, &height);
        const int x = event.type == SDL_MOUSEMOTION ? event.motion.x : event.button.x;
        const int y = event.type == SDL_MOUSEMOTION ? event.motion.y : event.button.y;
        presenter.host_pointer(x * 368 / std::max(width, 1),
                               y * 448 / std::max(height, 1), pressed);
      }
    }
    const std::uint32_t now = SDL_GetTicks();
    const std::uint32_t elapsed = std::min<std::uint32_t>(now - previous, 100U);
    if (tick) tick(elapsed);
    else presenter.host_pump(elapsed);
    previous = now;
    SDL_UpdateTexture(texture, nullptr, presenter.host_framebuffer(), 368 * 2);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
    if(frame_limit && ++frames >= frame_limit) running = false;
    SDL_Delay(8U);
  }
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

}  // namespace trainer_loop
