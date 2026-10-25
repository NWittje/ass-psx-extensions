#include <du_host/ui.hpp>
#include <shellapi.h>
namespace
{
void error(const wchar_t *text)
{
    try
    {
        ass::graphics::Context context;
        ass::graphics::check(
            ag_message_box(context.handle(), nullptr, L"DU host startup failed", text, 1));
    }
    catch (...)
    {
        MessageBoxW(nullptr, text, L"DU host startup failed", MB_OK | MB_ICONERROR);
    }
}
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        du_host::UiOptions options;int argc{};auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        if(!argv)throw std::runtime_error("Host command line");
        for(int i=1;i<argc;++i){if(std::wstring_view(argv[i])==L"--qualify-output"&&i+1<argc)options.du_trace_output=argv[++i];else{LocalFree(argv);throw std::runtime_error("Usage: psx-du-host [--qualify-output FRESH_DIRECTORY]");}}LocalFree(argv);
        du_host::Ui app(std::move(options));
        return app.run();
    }
    catch (const ass::graphics::Error &e)
    {
        error(e.detail.c_str());
        return 1;
    }
    catch (const std::exception &e)
    {
        auto text = du_host::widen(e.what());
        error(text.c_str());
        return 1;
    }
}
