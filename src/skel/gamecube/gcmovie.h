#pragma once

#ifdef GTA_OGC

// Plays one Ogg Theora + Vorbis GameCube boot movie straight from dvd:/.
// The framebuffer belongs to the boot console and is temporarily reused as
// one half of the movie's double buffer; all transient memory is released
// before normal game loading begins.
bool PlayGameCubeMovie(const char *path, void *bootFramebuffer,
    unsigned width, unsigned height, unsigned framebufferBytes);

#endif
