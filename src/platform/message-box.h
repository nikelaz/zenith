#ifndef MESSAGE_BOX_H
#define MESSAGE_BOX_H

struct SDL_Window;

void show_error_message(const char* message, SDL_Window* window = nullptr);

#endif
