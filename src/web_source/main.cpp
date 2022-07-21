
#include "include/base/cef_logging.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"

#include "../ipc_shared_object.hpp"
#include "../triple_buffer.hpp"

#include "../server/server.hpp"

#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <fmt/ostream.h>

#ifdef __APPLE__
#include "include/cef_application_mac.h"
#include "include/wrapper/cef_library_loader.h"
#import <Cocoa/Cocoa.h>
#endif

#ifdef WIN32
#include <windows.h>
#endif

using fmt::operator""_a;

using namespace std::literals;

class http_delegate {
public:
  using body_type = beast::http::string_body;

private:
  CefRefPtr<CefBrowser> &_browser;
  CefString &_title;
  CefString &_url;

public:
  http_delegate(CefRefPtr<CefBrowser> &browser, CefString &title,
                CefString &url)
      : _browser{browser}, _title{title}, _url{url} {}

  template <typename Body, typename Allocator>
  void handle_request(
      beast::http::request<Body, beast::http::basic_fields<Allocator>> &&req,
      auto &&send) {
    if (req.target() == "/") {
      auto body = fmt::format(
          R"html(
<html>
  <head>
  </head>
  <body>
    {title}
    <br/>
    <input type="text" id="url" value="{url}"></input>
    <button onclick="fetch('/load', {{method: 'POST', body: document.getElementById('url').value}})">
      Load
    </button>
    <br/>
    <button onclick="fetch('/refresh')">
      Refresh
    </button>
    <script>
      let ws;
      
      function open_ws() {{
        ws = new WebSocket(`ws://${{window.location.host}}`);
        ws.onopen = function(ev) {{}};
        ws.onclose = function(ev) {{
          console.log(`Close: ${{ev}}`);
        }};
        ws.onmessage = function(ev) {{
          window.location.reload();
        }};
        ws.onerror = function(ev) {{
          console.log(`Error: ${{ev}}`);
          open_ws();
        }};
      }}

      open_ws();
    </script>
  </body>
</html>
)html"sv,
          "title"_a = _title, "url"_a = _url);
      auto mime_type = "text/html"sv;

      return http::string_response(req, std::move(body), mime_type, send);
    } else if (req.target() == "/load" &&
               req.method() == beast::http::verb::post) {
      auto const url = req.body();
      _browser->GetMainFrame()->LoadURL({url.data(), url.size()});
      return send(http::empty_response(req));
    } else if (req.target() == "/refresh") {
      _browser->Reload();
      return send(http::empty_response(req));
    } else {
      return send(http::not_found(req));
    }
  }
};

class websocket_delegate {
public:
  void on_read(beast::flat_buffer &buffer) {
    // TODO
  }
};

template <typename F> class StringVisitor : public CefStringVisitor {
  // Include the default reference counting implementation.
  IMPLEMENT_REFCOUNTING(StringVisitor);

private:
  F f;

public:
  StringVisitor(F f) : f{std::move(f)} {}

private:
  void Visit(CefString const &str) override { f(str); }
};

class Client : public CefClient,
               CefDisplayHandler,
               CefLifeSpanHandler,
               CefRenderHandler {
  // Include the default reference counting implementation.
  IMPLEMENT_REFCOUNTING(Client);

private:
  CefRefPtr<CefBrowser> &_browser;
  triple_buffer *output_buffer;
  CefString &_title;
  CefString &_url;
  std::function<void()> &reload_clients;

public:
  Client(CefRefPtr<CefBrowser> &browser, triple_buffer *output_buffer,
         CefString &title, CefString &url,
         std::function<void()> &reload_clients)
      : _browser{browser}, output_buffer{output_buffer}, _title{title},
        _url{url}, reload_clients{reload_clients} {}

  // CefClient methods
  auto GetDisplayHandler() -> CefRefPtr<CefDisplayHandler> override {
    return this;
  }
  auto GetLifeSpanHandler() -> CefRefPtr<CefLifeSpanHandler> override {
    return this;
  }
  auto GetRenderHandler() -> CefRefPtr<CefRenderHandler> override {
    return this;
  }

  // CefDisplayHandler methods
  void OnTitleChange(CefRefPtr<CefBrowser> browser,
                     CefString const &title) override {
    _title = title;
    reload_clients();
  }

  void OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                       CefString const &url) override {
    _url = url;
    reload_clients();
  }

  // CefLifeSpanHandler methods
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    _browser = browser;
  }

  // CefRenderHandler methods
  auto GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo &screen_info)
      -> bool override {
    screen_info = {1.0, 32, 8, false, {0, 0, 1920, 1080}, {0, 0, 1920, 1080}};
    return true;
  }

  void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect) override {
    rect = {0, 0, 1920, 1080};
  }

  void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
               RectList const &dirtyRects, void const *buffer, int width,
               int height) override {
    std::memcpy(output_buffer->write().data(), buffer, triple_buffer::size);
    output_buffer->done_writing();
  }
};

class App : public CefApp, CefBrowserProcessHandler {
  // Include the default reference counting implementation.
  IMPLEMENT_REFCOUNTING(App);

private:
  CefRefPtr<CefBrowser> &_browser;
  triple_buffer *output_buffer;
  CefString &_title;
  CefString &_url;
  std::function<void()> &reload_clients;

public:
  App(CefRefPtr<CefBrowser> &browser, triple_buffer *output_buffer,
      CefString &title, CefString &url, std::function<void()> &reload_clients)
      : _browser{browser}, output_buffer{output_buffer}, _title{title},
        _url{url}, reload_clients{reload_clients} {}

  // CefApp methods
  auto GetBrowserProcessHandler()
      -> CefRefPtr<CefBrowserProcessHandler> override {
    return this;
  }

  // CefBrowserProcessHandler methods
  void OnContextInitialized() override {
    auto info = CefWindowInfo{};

    info.SetAsWindowless(0);

    auto settings = CefBrowserSettings{};

    settings.windowless_frame_rate = 25;

    auto client = CefRefPtr<Client>{
        new Client{_browser, output_buffer, _title, _url, reload_clients}};

#ifdef WIN32
    info.SetAsPopup(nullptr, "Web View");
#endif

    auto url = "http://randomcolour.com"s;

    CefBrowserHost::CreateBrowser(info, client, url, settings, nullptr,
                                  nullptr);
  }
};

#ifdef __APPLE__
// Provide the CefAppProtocol implementation required by CEF.
@interface Application : NSApplication <CefAppProtocol> {
@private
  BOOL handlingSendEvent_;
}
@end

@implementation Application
- (BOOL)isHandlingSendEvent {
  return handlingSendEvent_;
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent {
  handlingSendEvent_ = handlingSendEvent;
}

- (void)sendEvent:(NSEvent *)event {
  CefScopedSendingEvent sendingEventScoper;
  [super sendEvent:event];
}

// |-terminate:| is the entry point for orderly "quit" operations in Cocoa.
// This includes the application menu's quit menu item and keyboard
// equivalent, the application's dock icon menu's quit menu item, "quit" (not
// "force quit") in the Activity Monitor, and quits triggered by user logout
// and system restart and shutdown.
//
// The default |-terminate:| implementation ends the process by calling
// exit(), and thus never leaves the main run loop. This is unsuitable for
// Chromium since Chromium depends on leaving the main run loop to perform an
// orderly shutdown. We support the normal |-terminate:| interface by
// overriding the default implementation. Our implementation, which is very
// specific to the needs of Chromium, works by asking the application delegate
// to terminate using its |-tryToTerminateApplication:| method.
//
// |-tryToTerminateApplication:| differs from the standard
// |-applicationShouldTerminate:| in that no special event loop is run in the
// case that immediate termination is not possible (e.g., if dialog boxes
// allowing the user to cancel have to be shown). Instead, this method tries
// to close all browsers by calling CloseBrowser(false) via
// ClientHandler::CloseAllBrowsers. Calling CloseBrowser will result in a call
// to ClientHandler::DoClose and execution of |-performClose:| on the
// NSWindow. DoClose sets a flag that is used to differentiate between new
// close events (e.g., user clicked the window close button) and in-progress
// close events (e.g., user approved the close window dialog). The
// NSWindowDelegate
// |-windowShouldClose:| method checks this flag and either calls
// CloseBrowser(false) in the case of a new close event or destructs the
// NSWindow in the case of an in-progress close event.
// ClientHandler::OnBeforeClose will be called after the CEF NSView hosted in
// the NSWindow is dealloc'ed.
//
// After the final browser window has closed ClientHandler::OnBeforeClose will
// begin actual tear-down of the application by calling CefQuitMessageLoop.
// This ends the NSApplication event loop and execution then returns to the
// main() function for cleanup before application termination.
//
// The standard |-applicationShouldTerminate:| is not supported, and code
// paths leading to it must be redirected.
- (void)terminate:(id)sender {
  // TODO
  // SimpleHandler* handler = SimpleHandler::GetInstance();
  // if (handler && !handler->IsClosing())
  //  handler->CloseAllBrowsers(false);
  // Return, don't exit. The application is responsible for exiting on its
  // own.
}
@end
#endif

#ifdef WIN32
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                      LPTSTR lpCmdLine, int nCmdShow) {
  UNREFERENCED_PARAMETER(hPrevInstance);
  UNREFERENCED_PARAMETER(lpCmdLine);
  UNREFERENCED_PARAMETER(nCmdShow);

  auto mainArgs = CefMainArgs{hInstance};

  int argc = __argc;
  char **argv = __argv;
#else
auto main(int argc, char **argv) -> int {
  auto mainArgs = CefMainArgs{argc, argv};
#endif

#ifdef __APPLE__
  auto library_loader = CefScopedLibraryLoader{};
  if (!library_loader.LoadInMain()) {
    std::cerr << "Cannot load shared library\n";
    return EXIT_FAILURE;
  }
#endif

#ifdef __APPLE__
  [Application sharedApplication];
#endif

  auto browser = CefRefPtr<CefBrowser>{};

  auto output_buffer = ipc_unmanaged_object<triple_buffer>{argv[argc - 1]};

  auto title = CefString{};
  auto url = CefString{};

  auto reload_clients = std::function<void()>{[] {}};

  auto app = CefRefPtr<App>{
      new App{browser, output_buffer.data(), title, url, reload_clients}};

  if (auto exitCode = CefExecuteProcess(mainArgs, nullptr, nullptr);
      exitCode >= 0) {
    return exitCode;
  }

  auto settings = CefSettings{};

  settings.windowless_rendering_enabled = true;

  CefInitialize(mainArgs, settings, app, nullptr);

#ifdef __APPLE__
  [[NSBundle mainBundle] loadNibNamed:@"MainMenu"
                                owner:NSApp
                      topLevelObjects:nil];
#endif

  /*
  auto refreshTimer = L2D::Timer{
      2000, [&browser](uint32_t milliseconds) -> uint32_t {
        if (browser) {
          auto frame = browser->GetMainFrame();
          frame->GetSource(new StringVisitor{[browser,
                                              frame](CefString const &source)
  { auto source_stdstr = source.ToString(); if (source_stdstr ==
  "<html><head></head><body></body></html>") { browser->Reload();
            }
          }});
        }
        return milliseconds;
      }};
  */

  auto http_delegate_ = std::make_shared<http_delegate>(browser, title, url);
  auto websocket_delegate_ = std::make_shared<websocket_delegate>();
  auto server_ = server{http_delegate_, websocket_delegate_, "0.0.0.0", 0, 4};

  reload_clients = [&] { server_.send(""s); };

  std::cout << server_.port() << '\n' << std::flush;

  CefRunMessageLoop();

  browser = nullptr;

  CefShutdown();

  return EXIT_SUCCESS;
}
