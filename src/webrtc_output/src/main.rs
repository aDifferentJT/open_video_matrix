#![feature(async_closure)]

use anyhow::Result;
use dioxus::prelude::*;
use futures::stream::StreamExt;
use ipc_shared_object::IpcUnmanagedObject;
use std::sync::Arc;
use tokio::sync::Mutex;
use tokio::time::Duration;
use triple_buffer::TripleBuffer;
use webrtc::api::media_engine::{MediaEngine, MIME_TYPE_VP8};
use webrtc::api::APIBuilder;
use webrtc::media::Sample;
use webrtc::peer_connection::configuration::RTCConfiguration;
use webrtc::peer_connection::sdp::session_description::RTCSessionDescription;
use webrtc::peer_connection::RTCPeerConnection;
use webrtc::rtp_transceiver::rtp_codec::RTCRtpCodecCapability;
use webrtc::track::track_local::track_local_static_sample::TrackLocalStaticSample;
use webrtc::track::track_local::TrackLocal;

#[macro_use]
extern crate lazy_static;

lazy_static! {
    static ref api: webrtc::api::API = (|| {
        let mut media_engine = MediaEngine::default();
        media_engine.register_default_codecs();
    
        // Create the API object with the MediaEngine
        APIBuilder::new().with_media_engine(media_engine).build()
    })();
}

enum GuiToEncoderMsg {
    AddPeerConnection(std::sync::Arc<RTCPeerConnection>),
}

enum EncoderToGuiMsg {
    SetColour(String),
}

struct AppProps {
    from_renderer: flume::Receiver<EncoderToGuiMsg>,
    to_renderer: flume::Sender<GuiToEncoderMsg>,
}

const PLAYER_HTML: &str = r#"
<div id="remoteVideos"></div>

<script>
  async function makeCall() {
    let pc = new RTCPeerConnection()

    pc.ontrack = function (event) {
      var el = document.createElement(event.track.kind)
      el.srcObject = event.streams[0]
      el.autoplay = true
      el.controls = true
    
      document.getElementById('remoteVideos').appendChild(el)
    }

    pc.oniceconnectionstatechange = e => console.log(pc.iceConnectionState)
    pc.onicecandidate = event => console.log(event)

    // Offer to receive 1 audio, and 1 video track
    pc.addTransceiver('video', {'direction': 'sendrecv'})
    pc.addTransceiver('audio', {'direction': 'sendrecv'})

    const offer = await pc.createOffer();
    pc.setLocalDescription(offer);

    const resp = await fetch(
      '/sdp',
      {method: 'post',
      headers: new Headers({'content-type': 'application/json'}),
      body: JSON.stringify(offer),
      },
    )
    const answer = await resp.json();
    console.log(answer)
    const remoteDesc = new RTCSessionDescription(answer);
    await pc.setRemoteDescription(remoteDesc);
  }

  makeCall();
</script>
"#;

#[tokio::main]
async fn main() -> Result<()> {
    dcv_color_primitives::initialize();

    let Ok(listener) = std::net::TcpListener::bind("0.0.0.0:60000") else {
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
            local_set.run_until(encoder(from_gui, to_gui, port)).await;
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
                <body>
                  <button
                    onclick="window.parent.postMessage({{msg: 'show_detail_view', data: `http://${{window.location.host}}/player`}}, '*')"
                  >
                    Open Player
                  </button>
                  <div id="main"></div>
                </body>
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
                        .launch(
                            dioxus_liveview::axum_socket(socket),
                            app,
                        )
                        .await;
                })
            }),
        )
        .route(
            "/player",
            axum::routing::get(|| async { axum::response::Html(PLAYER_HTML) }),
        )
        .route(
            "/sdp",
            axum::routing::post(
                async move |axum::Json(offer): axum::Json<RTCSessionDescription>| {
                    // TODO this shouldn't panic

                    println!("remote_handler receive from /sdp");

                    let peer_connection = std::sync::Arc::new(
                        api.new_peer_connection(RTCConfiguration {
                            ..Default::default()
                        })
                        .await.unwrap(),
                    );

                    to_renderer.send(GuiToEncoderMsg::AddPeerConnection(peer_connection.clone()));

                    if let Err(err) = peer_connection.set_remote_description(offer).await {
                        panic!("{}", err);
                    }

                    // Create an answer to send to the other process
                    let answer = match peer_connection.create_answer(None).await {
                        Ok(a) => a,
                        Err(err) => panic!("{}", err),
                    };

                    // Create channel that is blocked until ICE Gathering is complete
                    let mut gather_complete = peer_connection.gathering_complete_promise().await;

                    // Sets the LocalDescription, and starts our UDP listeners
                    if let Err(err) = peer_connection.set_local_description(answer).await {
                        panic!("{}", err);
                    }

                    // Block until ICE Gathering is complete, disabling trickle ICE
                    // we do this because we only can exchange one signaling message Candidate
                    // in a production application you should exchange ICE Candidates via OnICECandidate
                    let _ = gather_complete.recv().await;

                    axum::Json(peer_connection.local_description().await)
                },
            ),
        );

    axum::Server::from_tcp(listener)
        .unwrap()
        .serve(app.into_make_service())
        .await
        .unwrap();

    Ok(())
}

fn app(cx: Scope) -> Element {
    cx.render(rsx! {
        div {
            "Hello world",
        }
    })
}

async fn encoder(
    from_gui: flume::Receiver<GuiToEncoderMsg>,
    to_gui: flume::Sender<EncoderToGuiMsg>,
    port: u16,
) {
    let Ok((mut websocket, _)) =
        tokio_tungstenite::connect_async(
            &format!("ws://127.0.0.1:8080/output_{port}")
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

    let mut cpp_input_buffer: IpcUnmanagedObject<triple_buffer::CppTripleBuffer> =
        IpcUnmanagedObject::new(name);
    let mut input_buffer = TripleBuffer::new(cpp_input_buffer.get_mut());

    let Ok(mut encoder) = vpx_encode::Encoder::new(vpx_encode::Config {
        width: triple_buffer::WIDTH as _,
        height: triple_buffer::HEIGHT as _,
        timebase: [1, 25],
        bitrate: 1000,
        codec: vpx_encode::VideoCodecId::VP8,
    }) else {
        panic!("Can't create encoder");
    };

    let video_track = Arc::new(TrackLocalStaticSample::new(
        RTCRtpCodecCapability {
            mime_type: MIME_TYPE_VP8.to_owned(),
            ..Default::default()
        },
        "video".to_owned(),
        "webrtc-rs".to_owned(),
    ));

    let video_track2 = video_track.clone();
    tokio::spawn(async move {
        while let Ok(msg) = from_gui.recv() {
            match msg {
                GuiToEncoderMsg::AddPeerConnection(peer_connection) => {
                    // Add this newly created track to the PeerConnection
                    let Ok(rtp_sender) = peer_connection
                        .add_track(Arc::clone(&video_track2) as Arc<dyn TrackLocal + Send + Sync>)
                        .await else {
                            panic!("Could not add video track to peer connection");
                        };

                    // Read incoming RTCP packets
                    // Before these packets are returned they are processed by interceptors. For things
                    // like NACK this needs to be called.
                    tokio::spawn(async move {
                        let mut rtcp_buf = vec![0u8; 1500];
                        while let Ok((_, _)) = rtp_sender.read(&mut rtcp_buf).await {}
                        Result::<()>::Ok(())
                    });
                }
            }
        }
    });

    let src_format = dcv_color_primitives::ImageFormat {
        pixel_format: dcv_color_primitives::PixelFormat::Bgra,
        color_space: dcv_color_primitives::ColorSpace::Rgb,
        num_planes: 1,
    };

    let src_sizes: &mut [usize] = &mut [0usize; 1];
    match dcv_color_primitives::get_buffers_size(
        triple_buffer::WIDTH as u32,
        triple_buffer::HEIGHT as u32,
        &src_format,
        None,
        src_sizes,
    ) {
        Ok(_) => {}
        Err(e) => {
            println!("Error getting buffer size for I420 data: {}", e);
            return;
        }
    }
    if src_sizes[0] != triple_buffer::SIZE {
        println!(
            "Error, dcv thinks the correct size is {} when triple_buffer is of size {}",
            src_sizes[0],
            triple_buffer::SIZE
        );
    }

    let dst_format = dcv_color_primitives::ImageFormat {
        pixel_format: dcv_color_primitives::PixelFormat::I420,
        color_space: dcv_color_primitives::ColorSpace::Bt601,
        num_planes: 3,
    };

    let i420_data_sizes: &mut [usize] = &mut [0usize; 3];
    match dcv_color_primitives::get_buffers_size(
        triple_buffer::WIDTH as u32,
        triple_buffer::HEIGHT as u32,
        &dst_format,
        None,
        i420_data_sizes,
    ) {
        Ok(_) => {}
        Err(e) => {
            println!("Error getting buffer size for I420 data: {}", e);
            return;
        }
    }

    let mut i420_data: Vec<_> =
        vec![0u8; i420_data_sizes[0] + i420_data_sizes[1] + i420_data_sizes[2]];

    let mut ticker = tokio::time::interval(Duration::from_millis(1000 / 25));
    loop {
        let (i420_data1, i420_data23) = i420_data.split_at_mut(i420_data_sizes[0]);
        let (i420_data2, i420_data3) = i420_data23.split_at_mut(i420_data_sizes[1]);
        input_buffer.about_to_read();
        match dcv_color_primitives::convert_image(
            triple_buffer::WIDTH as u32,
            triple_buffer::HEIGHT as u32,
            &src_format,
            None,
            &[&input_buffer.read().video_frame],
            &dst_format,
            None,
            &mut [i420_data1, i420_data2, i420_data3],
        ) {
            Ok(_) => {}
            Err(e) => {
                println!("Error converting image: {}", e);
                continue;
            }
        }

        let Ok(packets) = encoder.encode(0, &i420_data) else { continue; };

        for frame in packets {
            let Ok(_) = video_track
                .write_sample(&Sample {
                    data: bytes::Bytes::copy_from_slice(frame.data),
                    duration: Duration::from_millis(1000 / 25),
                    ..Default::default()
                })
                .await else { continue; };
        }

        let _ = ticker.tick().await;
    }
}
