// SCNativeFileDialog: IFileOpenDialog on Windows (COM is initialised on the game thread by the engine).
#include "SCNativeFileDialog.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <shobjidl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

bool SCNativeFileDialog::IsAvailable()
{
    return PLATFORM_WINDOWS != 0;
}

bool SCNativeFileDialog::OpenFile(const void* ParentWindowHandle, const FString& Title, const FString& FilterName, const FString& Pattern, FString& OutPath)
{
#if PLATFORM_WINDOWS
    IFileOpenDialog* Dialog = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Dialog))) || !Dialog) return false;
    const COMDLG_FILTERSPEC Filters[] = {{*FilterName, *Pattern}};
    Dialog->SetFileTypes(1, Filters);
    Dialog->SetTitle(*Title);
    DWORD Options = 0;
    Dialog->GetOptions(&Options);
    Dialog->SetOptions(Options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    bool bChosen = false;
    if (SUCCEEDED(Dialog->Show(HWND(const_cast<void*>(ParentWindowHandle)))))
    {
        IShellItem* Item = nullptr;
        if (SUCCEEDED(Dialog->GetResult(&Item)) && Item)
        {
            PWSTR Path = nullptr;
            if (SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH, &Path)) && Path)
            {
                OutPath = FString(Path);
                ::CoTaskMemFree(Path);
                bChosen = !OutPath.IsEmpty();
            }
            Item->Release();
        }
    }
    Dialog->Release();
    return bChosen;
#else
    return false;
#endif
}
