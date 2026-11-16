/* Raiden V: Director's Cut uses its native frame clock and aspect ratio. The inherited
 * Tempest address hooks must not inspect or patch this game's image. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/runtime_game.c"

int runtime_file_mount(const char *guest,const char *host) { (void)guest; (void)host; return 0; }

int main(void) {
    unsigned char image[256], before[256];
    memset(image, 0xa5, sizeof image);
    memcpy(before, image, sizeof image);
    runtime_game_set_image(image);
    runtime_game_frame_rate();
    const uint32_t widths[] = {2880, 3456, 3840, 5120, 7680};
    for (size_t i=0; i<sizeof widths/sizeof widths[0]; ++i) {
        assert(runtime_game_widen(image, widths[i]) == (widths[i] == 3840));
        assert(runtime_game_wide_width("mrt_albedo", 1920, 1080, widths[i]) == 1920);
        assert(runtime_game_wide_width(NULL, 960, 540, widths[i]) == 960);
        assert(memcmp(image, before, sizeof image) == 0);
    }
    puts("Raiden V: Director's Cut aspect/frame hooks: game image unchanged");
    return 0;
}
