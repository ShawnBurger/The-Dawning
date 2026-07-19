#include "app.h"

#include <windows.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR commandLine, int)
{
    dawning::App app;
    return app.Run(commandLine);
}
