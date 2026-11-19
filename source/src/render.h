#pragma once
namespace render {
    void Install();          // after the game code is decrypted (first frame)
    void* View();            // last render view passed to the scene draw (0x724994 arg)
}
