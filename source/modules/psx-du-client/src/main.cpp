#include <du_client/ui.hpp>
#include <shellapi.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        int n{};
        auto argv = CommandLineToArgvW(GetCommandLineW(), &n);
        std::wstring profile = L"1";
        std::filesystem::path output;
        unsigned seconds = 60;
        for (int i = 1; i < n; i++)
        {
            if (std::wstring_view(argv[i]) == L"--profile" && i + 1 < n)
                profile = argv[++i];
            else if (std::wstring_view(argv[i]) == L"--qualify-output" && i + 1 < n)
                output = argv[++i];
            else if (std::wstring_view(argv[i]) == L"--qualify-seconds" && i + 1 < n)
            {
                seconds = unsigned(std::stoul(argv[++i]));
                if (!seconds || seconds > 3600)
                    throw std::runtime_error("Qualification duration must be 1..3600 seconds");
            }
            else
            {
                LocalFree(argv);
                throw std::runtime_error("Usage: psx-du-client [--profile 1..10]");
            }
        }
        LocalFree(argv);
        du_client::Ui app(profile);
        if (!output.empty() && app.primary())
            app.qualify(output, seconds);
        return app.run();
    }
    catch (const ass::graphics::Error &e)
    {
        MessageBoxW(nullptr, e.detail.c_str(), L"PSX DU client", MB_OK | MB_ICONERROR);
        return 1;
    }
    catch (const std::exception &e)
    {
        std::wstring text;
        for (auto p = e.what(); *p; p++)
            text.push_back(wchar_t(uint8_t(*p)));
        MessageBoxW(nullptr, text.c_str(), L"PSX DU client", MB_OK | MB_ICONERROR);
        return 1;
    }
}
