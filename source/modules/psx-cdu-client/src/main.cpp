#include <cdu_client/ui.hpp>
#include <shellapi.h>
namespace
{
void startup_error(const wchar_t *text)
{
    try
    {
        ass::graphics::Context c;
        ass::graphics::check(ag_message_box(c.handle(), nullptr, L"CDU client startup", text, 1));
    }
    catch (...)
    {
        MessageBoxW(nullptr, text, L"CDU client startup", MB_OK | MB_ICONERROR);
    }
}
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        int n{};
        auto args = CommandLineToArgvW(GetCommandLineW(), &n);
        std::string profile = "1";
        bool warp = false;
        for (int i = 1; i < n; ++i)
        {
            std::wstring_view a = args[i];
            if (a == L"--profile" && i + 1 < n)
                profile = cdu_client::narrow(args[++i]);
            else if (a == L"--warp")
                warp = true;
            else
                throw std::runtime_error("Usage: psx-cdu-client [--profile 1..10|legacy-name] [--warp]");
        }
        LocalFree(args);
        cdu_client::Ui ui(cdu_client::profile_name(profile), warp);
        return ui.run();
    }
    catch (const ass::graphics::Error &e)
    {
        startup_error(e.detail.c_str());
    }
    catch (const std::exception &e)
    {
        startup_error(cdu_client::widen(e.what()).c_str());
    }
    return 1;
}
