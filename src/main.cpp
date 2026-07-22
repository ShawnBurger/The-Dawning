#include "app.h"

#include <windows.h>

int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE,
                   _In_ LPSTR commandLine, _In_ int)
{
    dawning::App app;
    return app.Run(commandLine);
}
