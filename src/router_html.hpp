#ifndef ROUTER_HTML_HPP
#define ROUTER_HTML_HPP

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

#include <fmt/format.h>

using fmt::operator""_a;

using namespace std::literals;

struct device_header_cell {
  std::string const &name;
  unsigned short port;

  static constexpr auto make =
      []<typename Device>(std::unique_ptr<Device> const &dev) {
        return device_header_cell{dev->name(), dev->port()};
      };
};

template <> struct fmt::formatter<device_header_cell> {
  constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
    if (ctx.begin() != ctx.end() && *ctx.begin() != '}')
      throw format_error("invalid format");
    return ctx.begin();
  }

  auto format(device_header_cell const &dev, auto &ctx) const
      -> decltype(ctx.out()) {
    auto iframe_id = fmt::format("input_header_iframe_{}", dev.name);
    return fmt::format_to(ctx.out(), R"html(
<th>
  <h3>{name}</h3>
  <iframe id="{iframe_id}">
  </iframe>
  <script>
    document.getElementById("{iframe_id}").src = `http://${{window.location.hostname}}:{port}`;
  </script>
</th>
)html",
                          "iframe_id"_a = iframe_id, "name"_a = dev.name,
                          "port"_a = dev.port);
  }
};

struct matrix_cell {
  input_device const &input;
  output_device const &output;

  static constexpr auto make(input_device const &input) {
    return [&input](std::unique_ptr<output_device> const &output) {
      return matrix_cell{input, *output};
    };
  }
};

template <> struct fmt::formatter<matrix_cell> {
  constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
    if (ctx.begin() != ctx.end() && *ctx.begin() != '}')
      throw format_error("invalid format");
    return ctx.begin();
  }

  auto format(matrix_cell const &cell, auto &ctx) const -> decltype(ctx.out()) {
    auto checked =
        std::find(cell.input.outputs.begin(), cell.input.outputs.end(),
                  &cell.output) != cell.input.outputs.end();
    return fmt::format_to(ctx.out(),
                          R"html(
<td>
  <input
    type="checkbox"
    {checked}
    onclick="fetch('/connect', {{method: 'POST', body: `{input}&{output}&${{event.target.checked}}`}})"
  />
</td>
)html",
                          "checked"_a = checked ? "checked"sv : ""sv,
                          "input"_a = cell.input.name(),
                          "output"_a = cell.output.name());
  }
};

struct input_row {
  input_device const &input;
  std::vector<std::unique_ptr<output_device>> const &outputs;
};

template <> struct fmt::formatter<input_row> {
  constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
    if (ctx.begin() != ctx.end() && *ctx.begin() != '}')
      throw format_error("invalid format");
    return ctx.begin();
  }

  auto format(input_row const &row, auto &ctx) const -> decltype(ctx.out()) {
    return fmt::format_to(
        ctx.out(),
        R"html(
<tr>
  {header}
  {cells}
</tr>
)html",
        "header"_a = device_header_cell{row.input.name(), row.input.port()},
        "cells"_a = fmt::join(row.outputs | ranges::views::transform(
                                                matrix_cell::make(row.input)),
                              ""));
  }
};

constexpr auto router_html = R"html(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="utf-8" />
    <title>Open Video Matrix</title>

    <style>
      table {{
        border-collapse: collapse;
      }}

      th,
      td {{
        padding: 5px;
        border: 1px solid;
        text-align: center;
        vertical-align: middle;
        max-height: 200px;
        overflow-y: scroll;
      }}

      .header_cell {{
        max-height: 200px;
        overflow-y: scroll;
      }}

      #container {{
        height: 100vh;
        width: 100vw;
        position: fixed;
        left: 0px;
        top: 0px;
      }}

      #matrix_view {{
        overflow: scroll;
        margin: 10px;
        padding: 10px;
        background-color: #E0F0F0;
        border-radius: 10px;
      }}

      #detail_view {{
        position: relative;
        width: 0vw;
        opacity: 0%;
        margin: 10px;
        background-color: #E0FFE0;
        border-radius: 10px;
        transition-property: width, opacity;
        transition-duration: 0.5s;
      }}

      #detail_view_iframe {{
        box-sizing: border-box;
        height: 100%;
        width: 100%;
        overflow: scroll;
        padding: 10px;
      }}

      #detail_view_close {{
        position: absolute;
        top: 10px;
        right: 10px;
      }}

      .row {{
        display: flex;
        flex-direction: row;
        align-items: stretch;
      }}

      .col {{
        display: flex;
        flex-direction: column;
        align-items: stretch;
      }}

      .padding {{
        flex-grow: 1;
        flex-shrink: 1;
      }}
    </style>
  </head>

  <body>
    <div id="container" class="row">
      <div class="col" style="max-width: 75%; flex-shrink: 0;">
        <div id="matrix_view">
          <table id="matrix">
            <tr>
              <th style="border:none;"></th>
              {output_headers}
            </tr>
            {input_rows}
          </table>
        </div>
        <div class="padding" style="min-width: 0px;"></div>
      </div>
      <div class="padding"></div>
      <div id="detail_view">
        <iframe id="detail_view_iframe"></iframe>
        <button id="detail_view_close" onclick="hide_detail_view()">Close
      </div>
    </div>
    </div>
    <script>
      function hide_detail_view() {{
        const detail_view = document.getElementById("detail_view");
        detail_view.style.width = "0vw";
        detail_view.style.opacity = "0%";
      }}

      window.addEventListener("message", function (event) {{
        const {{msg: msg, data: data}} = event.data;
        switch (msg) {{
          case "show_detail_view":
            const detail_view = document.getElementById("detail_view");
            const detail_view_iframe = document.getElementById("detail_view_iframe");

            detail_view_iframe.src = data;

            detail_view.style.width = "100vw";
            detail_view.style.opacity = "100%";
            break;
          default:
            console.log(`Unknown message ${{msg}}`);
            break;
        }}
      }});

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
)html"sv;

#endif // ROUTER_HTML_HPP
