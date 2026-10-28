#pragma once

namespace os::features {

// Resolves every hook target and attaches the catalog callbacks. Call from the worker thread.
void Install();

// Render thread, once per frame. Keeps globals edits applied after the game reloads them.
void Tick();

}
