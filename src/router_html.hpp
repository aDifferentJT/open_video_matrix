#ifndef ROUTER_HTML_HPP
#define ROUTER_HTML_HPP

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

#include <fmt/format.h>

using fmt::operator""_a;

using namespace std::literals;

auto ignore_first(auto &&, auto &&x) -> decltype(auto) { return x; }

struct device_header_cell {
public:
  std::string const &name;
  unsigned short port;

private:
  static inline auto const empty_name = ""s;
  device_header_cell(auto const &, int) : name{empty_name}, port{0} {}

public:
  template <typename Device>
  device_header_cell(Device const &dev) : name{dev.name()}, port{dev.port()} {}

  static constexpr auto make =
      []<typename Device>(std::weak_ptr<Device> const &_dev) {
        if (auto dev = _dev.lock()) {
          return device_header_cell{*dev};
        } else {
          return device_header_cell{_dev, 0};
        }
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
    auto iframe_id = fmt::format("header_iframe_{}", dev.name);
    return fmt::format_to(ctx.out(), R"html(
<th>
  <iframe class="header_iframe" id="{iframe_id}">
  </iframe>
  <script>
    document.getElementById("{iframe_id}").src = `http://${{window.location.hostname}}:{port}`;
  </script>
</th>
)html",
                          "iframe_id"_a = iframe_id, "port"_a = dev.port);
  }
};

class matrix_cell {
private:
  struct data_t {
    input_device const &input;
    output_device const &output;

    data_t(input_device const &input, output_device const &output)
        : input{input}, output{output} {}
  };

  std::optional<data_t> data;

public:
  matrix_cell() = default;

  matrix_cell(input_device const &input, output_device const &output)
      : data{std::in_place, input, output} {}

  static constexpr auto make(input_device const &input) {
    return [&input](std::weak_ptr<output_device> const &_output) {
      if (auto output = _output.lock()) {
        return matrix_cell{input, *output};
      } else {
        return matrix_cell{};
      }
    };
  }

  template <typename, typename, typename> friend struct fmt::formatter;
};

template <> struct fmt::formatter<matrix_cell> {
  constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
    if (ctx.begin() != ctx.end() && *ctx.begin() != '}')
      throw format_error("invalid format");
    return ctx.begin();
  }

  auto format(matrix_cell const &cell, auto &ctx) const -> decltype(ctx.out()) {
    if (cell.data) {
      auto checked =
          std::find_if(cell.data->input.outputs.begin(),
                       cell.data->input.outputs.end(),
                       [&](std::weak_ptr<output_device> const &output) {
                         return output.lock().get() == &cell.data->output;
                       }) != cell.data->input.outputs.end();
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
                            "input"_a = cell.data->input.name(),
                            "output"_a = cell.data->output.name());
    } else {
      return fmt::format_to(ctx.out(), "");
    }
  }
};

class input_row {
private:
  struct data_t {
    input_device const &input;
    std::vector<std::weak_ptr<output_device>> const &outputs;

    data_t(input_device const &input,
           std::vector<std::weak_ptr<output_device>> const &outputs)
        : input{input}, outputs{outputs} {}
  };

  std::optional<data_t> data;

public:
  input_row() = default;

  input_row(input_device const &input,
            std::vector<std::weak_ptr<output_device>> const &outputs)
      : data{std::in_place, input, outputs} {}

  template <typename, typename, typename> friend struct fmt::formatter;
};

template <> struct fmt::formatter<input_row> {
  constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
    if (ctx.begin() != ctx.end() && *ctx.begin() != '}')
      throw format_error("invalid format");
    return ctx.begin();
  }

  auto format(input_row const &row, auto &ctx) const -> decltype(ctx.out()) {
    if (row.data) {
      return fmt::format_to(
          ctx.out(),
          R"html(
<tr>
  <th>
    <table>
      <tr>
        <td style="border: none;">
          <button onclick="fetch('/bring_input_backward', {{method: 'POST', body: '{name}'}})">
            &#11165;
          </button>
        </td>
      </tr>
      <tr>
        <td style="border: none;">
          <button onclick="fetch('/bring_input_forward', {{method: 'POST', body: '{name}'}})">
            &#11167;
          </button>
        </td>
      </tr>
    </table>
  </th>
  {header}
  {cells}
</tr>
)html",
          "name"_a = std::string{row.data->input.name()},
          "header"_a = device_header_cell{row.data->input},
          "cells"_a = fmt::join(
              row.data->outputs |
                  ranges::views::transform(matrix_cell::make(row.data->input)),
              ""));
    } else {
      return fmt::format_to(ctx.out(), "");
    }
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
        padding: 0px;
        border: 1px solid;
        text-align: center;
        vertical-align: middle;
      }}

      .header_iframe {{
        width: 300px;
        height: 200px;
        border: none;
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
              <th style="border: none;"></th>
              <th style="border: none;"></th>
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
