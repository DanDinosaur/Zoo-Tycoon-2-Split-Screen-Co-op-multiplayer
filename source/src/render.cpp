#include "render.h"
#include "split.h"
namespace render {
void Install() { split::Install(); }
void* View() { return split::View(); }
}
