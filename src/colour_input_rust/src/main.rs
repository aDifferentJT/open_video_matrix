use dioxus::prelude::*;
use futures::stream::StreamExt;
use ipc_shared_object::IpcUnmanagedObject;
use triple_buffer::TripleBuffer;

enum GuiToRendererMsg {
    SetColour(String),
}

enum RendererToGuiMsg {
    SetColour(String),
}

struct AppProps {
    from_renderer: flume::Receiver<RendererToGuiMsg>,
    to_renderer: flume::Sender<GuiToRendererMsg>,
}

#[tokio::main]
async fn main() {
    let Ok(listener) = std::net::TcpListener::bind("0.0.0.0:0") else {
        panic!("Could not bind to interface");
    };

    let port = match listener.local_addr().unwrap() {
        std::net::SocketAddr::V4(addr) => addr.port(),
        std::net::SocketAddr::V6(addr) => addr.port(),
    };

    let (to_renderer, from_gui) = flume::unbounded();
    let (to_gui, from_renderer) = flume::unbounded();

    std::thread::spawn(move || {
        tokio::runtime::Runtime::new().unwrap().block_on(async {
            let local_set = tokio::task::LocalSet::new();
            local_set.run_until(renderer(from_gui, to_gui, port)).await;
        });
    });

    let view = dioxus_liveview::LiveViewPool::new();

    let app = axum::Router::new()
        // The root route contains the glue code to connect to the WebSocket
        .route(
            "/",
            axum::routing::get(move || async {
                axum::response::Html(format!(
                    r#"
                <!DOCTYPE html>
                <html>
                <head></head>
                <body> <div id="main"></div> </body>
                {glue}
                </html>
                "#,
                    // TODO this is using an injection attack
                    glue = dioxus_liveview::interpreter_glue(
                        &"\" + `ws://${window.location.host}/ws` + \""
                    )
                ))
            }),
        )
        // The WebSocket route is what Dioxus uses to communicate with the browser
        .route(
            "/ws",
            axum::routing::get(move |ws: axum::extract::WebSocketUpgrade| async move {
                ws.on_upgrade(move |socket| async move {
                    // When the WebSocket is upgraded, launch the LiveView with the app component
                    _ = view
                        .launch_with_props(
                            dioxus_liveview::axum_socket(socket),
                            app,
                            AppProps {
                                from_renderer: from_renderer,
                                to_renderer: to_renderer,
                            },
                        )
                        .await;
                })
            }),
        );

    axum::Server::from_tcp(listener)
        .unwrap()
        .serve(app.into_make_service())
        .await
        .unwrap();
}

fn app(cx: Scope<AppProps>) -> Element {
    let colour = use_state(cx, || "#abcdef".to_string());

    let from_renderer = cx.props.from_renderer.clone();

    use_coroutine(cx, |_: dioxus::prelude::UnboundedReceiver<()>| {
        to_owned![colour];

        async move {
            while let Ok(msg) = from_renderer.recv_async().await {
                match msg {
                    RendererToGuiMsg::SetColour(new_colour) => colour.set(new_colour),
                }
            }
        }
    });

    cx.render(rsx! {
        div {
            "Colour",
            input {
                "type": "color",
                value: "{colour.get()}",
                oninput: move |evt| {
                    let _ = cx.props.to_renderer.send(GuiToRendererMsg::SetColour(evt.value.clone()));
                },
            },
        }
    })
}

async fn renderer(
    from_gui: flume::Receiver<GuiToRendererMsg>,
    to_gui: flume::Sender<RendererToGuiMsg>,
    port: u16,
) {
    let Ok((mut websocket, _)) =
        tokio_tungstenite::connect_async(
            &format!("ws://127.0.0.1:8080/input_{port}")
        ).await
    else {
        panic!("Can't connect to server"); 
    };

    let Some(Ok(tungstenite::protocol::Message::Binary(message))) = websocket.next().await else {
        panic!("no message");
    };
    let Ok(name) = std::str::from_utf8(&message) else { panic!("message not utf8"); };

    // Start thread to listen to the websocket to make sure that ping-pong is handled
    tokio::spawn(async move {
        loop {
            websocket.next().await;
        }
    });

    let mut cpp_output_buffer: IpcUnmanagedObject<triple_buffer::CppTripleBuffer> =
        IpcUnmanagedObject::new(name);
    let mut output_buffer = TripleBuffer::new(cpp_output_buffer.get_mut());

    let mut write_frame = |colour: &str| {
        let parse_channel = |s: &str| u8::from_str_radix(s, 16);

        let Ok(r) = parse_channel(&colour[1..3]) else {return;};
        let Ok(g) = parse_channel(&colour[3..5]) else {return;};
        let Ok(b) = parse_channel(&colour[5..7]) else {return;};

        let buffer = output_buffer.write();
        for i in (0..triple_buffer::SIZE).step_by(4) {
            buffer.video_frame[i + 0] = b;
            buffer.video_frame[i + 1] = g;
            buffer.video_frame[i + 2] = r;
            buffer.video_frame[i + 3] = 255;
        }
        output_buffer.done_writing();
    };

    let mut colour = "#abcdef".to_string();

    write_frame(&colour);

    while let Ok(msg) = from_gui.recv() {
        match msg {
            GuiToRendererMsg::SetColour(new_colour) => {
                colour = new_colour;
                write_frame(&colour);
                let _ = to_gui.send(RendererToGuiMsg::SetColour(colour));
            }
        }
    }
}
